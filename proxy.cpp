// Drop-in transport only. All BCrypt APIs forward to the genuine System32 DLL.
#include "api.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <sddl.h>
#include "system_exports.h"

static HMODULE self_module;
static INIT_ONCE system_once = INIT_ONCE_STATIC_INIT;
static FARPROC system_functions[_countof(system_exports)];
// One file shared by the SYSTEM host and the user-session worker, so users can
// send it instead of running DebugView: %ProgramData%\ParsecHdrShim\hdr-shim.log.
static SRWLOCK log_lock = SRWLOCK_INIT;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static wchar_t log_path[MAX_PATH], log_old[MAX_PATH];
static ULONGLONG log_checked;
static bool log_blocked; // Folder is not a plain directory: file logging stays off.
// Each distinct message is written at most once per minute; retry loops (e.g.
// behind a lock screen for hours) only add a repeat count.
struct LOG_RECENT { char text[160]; ULONGLONG shown; ULONG repeats; };
static LOG_RECENT log_recent[16];
static unsigned log_next;

static void log_rotate(ULONGLONG now)
{
    if (!log_path[0]) {
        wchar_t dir[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"ProgramData", dir, _countof(dir));
        if (!n || n >= _countof(dir) - 32) wcscpy_s(dir, L"C:\\ProgramData");
        wcscat_s(dir, L"\\ParsecHdrShim");
        // SYSTEM and admins: full. Users (the WGC worker runs with the user's token)
        // may create, write, rotate and delete files, but not remove the folder.
        PSECURITY_DESCRIPTOR security = nullptr;
        SECURITY_ATTRIBUTES sa {sizeof(sa), nullptr, FALSE};
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;0x1201bf;;;BU)(A;OIIO;0x1301bf;;;BU)",
                SDDL_REVISION_1, &security, nullptr))
            sa.lpSecurityDescriptor = security;
        CreateDirectoryW(dir, &sa);
        if (security) LocalFree(security);
        swprintf_s(log_path, L"%s\\hdr-shim.log", dir); swprintf_s(log_old, L"%s\\hdr-shim.old.log", dir);
        // A SYSTEM host must not follow a user-planted junction out of this folder.
        DWORD attributes = GetFileAttributesW(dir);
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            log_blocked = true;
    }
    if (log_blocked) return;
    // Checked by path, not handle: another process may already have rotated the
    // file this handle points to, so the handle is reopened on every check.
    log_checked = now;
    WIN32_FILE_ATTRIBUTE_DATA info {};
    if (GetFileAttributesExW(log_path, GetFileExInfoStandard, &info) && (info.nFileSizeHigh || info.nFileSizeLow > 1024 * 1024))
        MoveFileExW(log_path, log_old, MOVEFILE_REPLACE_EXISTING);
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    log_file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    // Refuse symlinks and hardlinks that would redirect a SYSTEM append elsewhere.
    BY_HANDLE_FILE_INFORMATION opened {};
    if (log_file != INVALID_HANDLE_VALUE && (!GetFileInformationByHandle(log_file, &opened) ||
        opened.nNumberOfLinks != 1 || (opened.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(log_file); log_file = INVALID_HANDLE_VALUE;
    }
}
// Returns the repeats to report, or -1 when the message is suppressed.
static long log_throttle(const char *message, ULONGLONG now)
{
    for (auto &recent : log_recent) {
        if (!recent.shown || strncmp(recent.text, message, sizeof(recent.text) - 1)) continue;
        if (now - recent.shown < 60000) { ++recent.repeats; return -1; }
        long repeats = long(recent.repeats); recent.shown = now; recent.repeats = 0; return repeats;
    }
    auto &slot = log_recent[log_next++ % _countof(log_recent)];
    strncpy_s(slot.text, message, _TRUNCATE); slot.shown = now; slot.repeats = 0;
    return 0;
}
extern "C" void core_log(int32_t, const char *fmt, ...)
{
    DWORD saved = GetLastError();
    char message[2048]; va_list args; va_start(args, fmt);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args); va_end(args);
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&log_lock);
    long repeats = log_throttle(message, now);
    if (repeats >= 0) {
        OutputDebugStringA("[HDR API shim v3] "); OutputDebugStringA(message); OutputDebugStringA("\n");
        if (log_file == INVALID_HANDLE_VALUE || now - log_checked >= 10000) log_rotate(now);
        if (log_file != INVALID_HANDLE_VALUE) {
            SYSTEMTIME t; GetLocalTime(&t);
            char line[2300], suffix[64] = "";
            if (repeats) _snprintf_s(suffix, sizeof(suffix), _TRUNCATE, " (and %ld identical since last shown)", repeats);
            int length = _snprintf_s(line, sizeof(line), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] %s%s\r\n",
                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), message, suffix);
            DWORD written;
            // A single append is atomic, so both processes can share the file.
            if (length > 0) WriteFile(log_file, line, DWORD(length), &written, nullptr);
        }
    }
    ReleaseSRWLockExclusive(&log_lock);
    SetLastError(saved);
}
void shim_pin()
{
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(self_module), &pinned);
}
static BOOL CALLBACK load_system(PINIT_ONCE, void *, void **)
{
    wchar_t path[MAX_PATH]; UINT length = GetSystemDirectoryW(path, _countof(path));
    if (!length || length + 12 >= _countof(path)) return FALSE;
    wcscat_s(path, L"\\bcrypt.dll");
    HMODULE system = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!system || system == self_module) return FALSE;
    for (size_t i = 0; i < _countof(system_exports); ++i) {
        const auto &entry = system_exports[i];
        system_functions[i] = GetProcAddress(system, entry.name ? entry.name : MAKEINTRESOURCEA(entry.ordinal));
    }
    return TRUE;
}
extern "C" FARPROC shim_resolve(unsigned index)
{
    DWORD saved = GetLastError();
    if (!InitOnceExecuteOnce(&system_once, load_system, nullptr, nullptr) ||
        index >= _countof(system_functions) || !system_functions[index]) {
        RaiseFailFastException(nullptr, nullptr, 0); return nullptr;
    }
    FARPROC result = system_functions[index]; SetLastError(saved); return result;
}
extern "C" __declspec(dllexport) BOOL HdrShimGetStats(HDR_SHIM_STATS *stats)
{
    if (!stats || stats->size != sizeof(*stats)) return FALSE;
    *stats = {sizeof(*stats), 3, hdr_factories, hdr_duplication_calls, hdr_sessions, hdr_fallbacks, hdr_frames};
    return TRUE;
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void *)
{
    if (reason == DLL_PROCESS_ATTACH) { self_module = module; api_bootstrap(module); }
    // Only named IAT writes in the entry point. No threads, COM, or graphics work.
    return TRUE;
}
