#include "cap_wgc.h"
#include "log.h"
#include <d3d11_4.h>
#include <dxgi.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <memory>
#include <new>

namespace capture = winrt::Windows::Graphics::Capture;
namespace direct3d = winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

static INIT_ONCE cap_wgc_mta_once = INIT_ONCE_STATIC_INIT;
static HRESULT cap_wgc_mta_result = E_FAIL;
static BOOL CALLBACK cap_wgc_keep_mta(PINIT_ONCE, void *, void **)
{
    // WGC's compositor callbacks can outlive Session.Close(). Keep the system MTA
    // alive across capture stop/restart, including when the host uses STA threads.
    // This single OS-owned usage reference intentionally lasts until process exit;
    // it retains no capture sessions, devices, frames, or callbacks into this DLL.
    CO_MTA_USAGE_COOKIE cookie {};
    cap_wgc_mta_result = CoIncrementMTAUsage(&cookie);
    return TRUE;
}

struct WGC_SIGNAL {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~WGC_SIGNAL() { if (event) CloseHandle(event); }
};

struct CAP_WGC {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11Texture2D> texture;
    HMONITOR monitor = nullptr;
    HANDLE stop = nullptr, request = nullptr, done = nullptr;
    HANDLE thread = nullptr;
    UINT timeout = 0;
    HRESULT result = E_FAIL;
    bool borderless = false, require_borderless = false;
};

static bool cap_wgc_request_borderless(capture::GraphicsCaptureSession const &session, HANDLE stop)
{
    try {
        auto border = session.try_as<capture::IGraphicsCaptureSession3>();
        if (!border) {
            core_log(0, "Host HDR capture: this Windows version does not support borderless capture");
            return false;
        }
        auto access = winrt::get_activation_factory<capture::IGraphicsCaptureAccessStatics>(winrt::name_of<capture::GraphicsCaptureAccess>());
        auto request = access.RequestAccessAsync(capture::GraphicsCaptureAccessKind::Borderless);
        auto signal = std::make_shared<WGC_SIGNAL>();
        if (!signal->event) winrt::throw_last_error();
        request.Completed([signal](auto const &, auto) { SetEvent(signal->event); });
        HANDLE waits[] = {stop, signal->event};
        // Do not trap host shutdown indefinitely behind an unanswered OS prompt.
        if (WaitForMultipleObjects(2, waits, FALSE, 30000) != WAIT_OBJECT_0 + 1) {
            request.Cancel();
            core_log(0, "Host HDR capture: borderless permission request did not complete");
            return false;
        }
        auto status = request.GetResults();
        if (status != winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed) {
            core_log(0, "Host HDR capture: Windows denied borderless permission (status %d)", int(status));
            return false;
        }
        border.IsBorderRequired(false);
        bool enabled = !border.IsBorderRequired();
        core_log(enabled ? 1 : 0, "Host HDR capture: borderless permission granted, session border %s", enabled ? "disabled" : "still required");
        return enabled;
    } catch (...) {
        core_log(0, "Host HDR capture: borderless request unavailable (0x%08lx)", long(winrt::to_hresult()));
        return false;
    }
}

// A dedicated MTA owns all WinRT objects. Capture may be torn down from a
// different host thread; it never uninitializes another thread's apartment.
static DWORD WINAPI cap_wgc_worker(void *opaque)
{
    auto *p = static_cast<CAP_WGC *>(opaque);
    HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(apartment)) { p->result = apartment; SetEvent(p->done); return 0; }
    {
        capture::Direct3D11CaptureFramePool pool {nullptr};
        capture::GraphicsCaptureSession session {nullptr};
        capture::Direct3D11CaptureFrame held {nullptr};
        winrt::event_token arrival {};
        bool subscribed = false;
        const char *stage = "support factory";
        try {
            // Use local factories rather than projected static helpers: process-wide
            // WinRT factory caches can outlive the last MTA during capture recreation.
            auto support = winrt::get_activation_factory<capture::IGraphicsCaptureSessionStatics>(winrt::name_of<capture::GraphicsCaptureSession>());
            stage = "support query";
            if (!support.IsSupported()) winrt::throw_hresult(DXGI_ERROR_UNSUPPORTED);
            stage = "item factory";
            auto interop = winrt::get_activation_factory<IGraphicsCaptureItemInterop>(winrt::name_of<capture::GraphicsCaptureItem>());
            stage = "monitor item";
            capture::GraphicsCaptureItem item {nullptr};
            winrt::check_hresult(interop->CreateForMonitor(p->monitor,
                winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
            auto size = item.Size();
            if (size.Width <= 0 || size.Height <= 0) winrt::throw_hresult(DXGI_ERROR_ACCESS_LOST);
            auto dxgi = p->device.as<IDXGIDevice>();
            stage = "D3D interop";
            winrt::com_ptr<IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
            auto device = inspectable.as<direct3d::IDirect3DDevice>();
            auto pools = winrt::get_activation_factory<capture::IDirect3D11CaptureFramePoolStatics2>(winrt::name_of<capture::Direct3D11CaptureFramePool>());
            stage = "FP16 frame pool";
            pool = pools.CreateFreeThreaded(device,
                DirectXPixelFormat::R16G16B16A16Float, 2, size);
            auto signal = std::make_shared<WGC_SIGNAL>();
            if (!signal->event) winrt::throw_last_error();
            // The callback owns its signal, never the capture context. Late callbacks
            // during teardown cannot touch a freed context or a closed event handle.
            arrival = pool.FrameArrived([signal](auto const &, auto const &) { SetEvent(signal->event); });
            subscribed = true;
            stage = "capture session";
            session = pool.CreateCaptureSession(item);
            stage = "borderless access";
            p->borderless = cap_wgc_request_borderless(session, p->stop);
            if (p->require_borderless && !p->borderless) {
                core_log(0, "Host HDR capture: refusing to start with a capture border");
                winrt::throw_hresult(E_ACCESSDENIED);
            }
            // Keep the cursor out of the video; the duplication reports it as a DXGI pointer instead.
            stage = "cursor capture";
            if (auto cursor = session.try_as<capture::IGraphicsCaptureSession2>())
                cursor.IsCursorCaptureEnabled(false);
            else
                core_log(0, "Host HDR capture: this Windows version cannot hide the cursor");
            stage = "StartCapture";
            session.StartCapture();
            p->result = S_OK; SetEvent(p->done);
            HANDLE requests[] = {p->stop, p->request};
            while (WaitForMultipleObjects(2, requests, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
                p->texture = nullptr;
                if (held) { held.Close(); held = nullptr; }
                HRESULT result = DXGI_ERROR_WAIT_TIMEOUT;
                const ULONGLONG start = GetTickCount64();
                for (;;) {
                    auto frame = pool.TryGetNextFrame();
                    if (frame) {
                        // Bound draining: keep newest queued frame without starving
                        // when the compositor is continuously producing new frames.
                        auto newer = pool.TryGetNextFrame();
                        if (newer) { frame.Close(); frame = std::move(newer); }
                        auto content = frame.ContentSize();
                        if (content.Width != size.Width || content.Height != size.Height) {
                            frame.Close(); result = DXGI_ERROR_ACCESS_LOST; break;
                        }
                        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), p->texture.put_void()));
                        D3D11_TEXTURE2D_DESC desc {}; p->texture->GetDesc(&desc);
                        if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Width != UINT(size.Width) || desc.Height != UINT(size.Height)) {
                            p->texture = nullptr; frame.Close(); result = DXGI_ERROR_UNSUPPORTED; break;
                        }
                        // Keep the frame checked out until the consumer's next call.
                        held = std::move(frame); result = S_OK; break;
                    }
                    ULONGLONG elapsed = GetTickCount64() - start;
                    if (elapsed >= p->timeout) break;
                    HANDLE frames[] = {p->stop, signal->event};
                    DWORD wait = WaitForMultipleObjects(2, frames, FALSE, DWORD(p->timeout - elapsed));
                    if (wait == WAIT_OBJECT_0) { result = E_ABORT; break; }
                    if (wait == WAIT_TIMEOUT) break;
                    if (wait == WAIT_FAILED) winrt::throw_last_error();
                }
                p->result = result; SetEvent(p->done);
            }
        } catch (...) {
            p->result = winrt::to_hresult();
            core_log(0,"Host HDR capture: WGC %s failed (0x%08lx)",stage,long(p->result));
            SetEvent(p->done);
        }
        // Close explicitly before leaving the MTA; exceptions must not escape a thread.
        try { if (session) session.Close(); } catch (...) {}
        try { if (subscribed) pool.FrameArrived(arrival); } catch (...) {}
        p->texture = nullptr;
        try { if (held) held.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
    }
    RoUninitialize();
    return 0;
}

HRESULT cap_wgc_create(CAP_WGC **out, ID3D11Device *device, HMONITOR monitor, bool require_borderless)
{
    if (!out || !device || !monitor) return E_INVALIDARG;
    *out = nullptr;
    InitOnceExecuteOnce(&cap_wgc_mta_once, cap_wgc_keep_mta, nullptr, nullptr);
    if (FAILED(cap_wgc_mta_result)) return cap_wgc_mta_result;
    auto *p = new (std::nothrow) CAP_WGC;
    if (!p) return E_OUTOFMEMORY;
    p->require_borderless = require_borderless;
    p->device.copy_from(device); p->monitor = monitor;
    p->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    p->request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    p->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = S_OK;
    if (!p->stop || !p->request || !p->done) hr = HRESULT_FROM_WIN32(GetLastError());
    try {
        if (SUCCEEDED(hr)) {
            // WGC also uses the capture device from its internal worker threads.
            auto protection = p->device.as<ID3D11Multithread>();
            protection->SetMultithreadProtected(TRUE);
            p->thread = CreateThread(nullptr, 0, cap_wgc_worker, p, 0, nullptr);
            if (!p->thread) winrt::throw_last_error();
            WaitForSingleObject(p->done, INFINITE); hr = p->result;
        }
    } catch (...) { hr = winrt::to_hresult(); }
    if (FAILED(hr)) { cap_wgc_destroy(&p); return hr; }
    *out = p;
    core_log(1, "Host HDR capture: Windows Graphics Capture, FP16 scRGB (full HDR before tone mapping)");
    return S_OK;
}

void cap_wgc_destroy(CAP_WGC **out)
{
    if (!out || !*out) return;
    auto *p = *out; *out = nullptr;
    if (p->stop) SetEvent(p->stop);
    if (p->thread) { WaitForSingleObject(p->thread, INFINITE); CloseHandle(p->thread); }
    if (p->request) CloseHandle(p->request);
    if (p->done) CloseHandle(p->done);
    if (p->stop) CloseHandle(p->stop);
    delete p;
}

HRESULT cap_wgc_acquire(CAP_WGC *p, UINT timeout, ID3D11Texture2D **out)
{
    if (!p || !out) return E_INVALIDARG;
    *out = nullptr; p->timeout = timeout;
    SetEvent(p->request);
    // A failed worker must not leave the host blocked on its next acquisition.
    HANDLE waits[] = {p->done, p->thread};
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) return E_FAIL;
    if (SUCCEEDED(p->result) && p->texture) { *out = p->texture.get(); (*out)->AddRef(); }
    return p->result;
}

bool cap_wgc_borderless(CAP_WGC *p) { return p && p->borderless; }
