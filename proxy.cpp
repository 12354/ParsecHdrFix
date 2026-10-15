// Drop-in transport only. All BCrypt APIs forward to the genuine System32 DLL.
#include "api.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include "system_exports.h"

static HMODULE self_module;
static INIT_ONCE system_once = INIT_ONCE_STATIC_INIT;
static FARPROC system_functions[_countof(system_exports)];

extern "C" void core_log(int32_t, const char *fmt, ...)
{
    char message[2048]; va_list args; va_start(args, fmt);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args); va_end(args);
    OutputDebugStringA("[HDR API shim v3] "); OutputDebugStringA(message); OutputDebugStringA("\n");
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
