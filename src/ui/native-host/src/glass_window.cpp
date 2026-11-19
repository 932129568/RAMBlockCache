// ============================================================================
//  src/ui/native-host/src/glass_window.cpp   [295 Wave 1 2026-11-19]
//
//  相对方案原文（295 §2.1/§2.2）的修正：
//    [F1] 补齐 InitWebView2Composition() 实现 —— 方案只有声明，照抄即 LNK2019。
//    [F2] CreateCoreWebView2CompositionController 是异步的：方案在调用后立刻
//         put_RootVisualTarget，实际会拿到 E_ILLEGAL_METHOD_CALL。此处用
//         wvCreatedEvent_ + 有界等待（5s），失败即整体失败（不静默降级成黑窗）。
//    [F3] WM_NCCALCSIZE 补「非最大化时不改 rgrc」这一支：方案对所有 wp==TRUE
//         都 return 0，虽然能消白边，但最大化时会丢工作区裁剪之外的边框吸附；
//         此处按标准做法处理。
//    [F4] WM_NCHITTEST 的 dpi_ 可能在 WM_DPICHANGED 之前就过期，改用
//         GetDpiForWindow(hwnd)（Windows 10 1607+ 有）而不是成员变量。
//    [F5] 析构期置 shuttingDown_：DComp/WebView2 的回调可能在 Close 之后仍触发，
//         不加这个闩就会在已 Release 的对象上 Commit（0xC0000005）。
//    [F6] 方案 Create() 里 GetClientRect 取尺寸发生在 WM_NCCALCSIZE 生效前，
//         此时 WS_THICKFRAME 仍带非客户区 ⇒ 首帧尺寸偏小。改为用传入宽高。
//    [F7] PresentWithRecovery 的设备丢失重建：方案在 Rebuild 里重入 InitD3DDevice
//         但**没有释放旧 device**，会泄漏；此处先断开 RTV/swapchain 引用。
// ============================================================================

#include "glass_window.hpp"
#include "desktop_capture.h"

#include <windowsx.h>
#include <algorithm>
#include <memory>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr DWORD kWdaNone               = 0x00000000;
constexpr DWORD kWdaExcludeFromCapture = 0x00000011;  // Win10 2004 (Build >= 19041)

// RtlGetVersion 是唯一不受 manifest / 兼容层欺骗的版本查询。
bool IsWindows10_2004OrNewer() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) return false;
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return false;
    return (vi.dwMajorVersion > 10) ||
           (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 19041);
}

// 交换链格式：HDR 走 FP16 scRGB，SDR 走 BGRA8。
DXGI_FORMAT PickFormat(bool isHdr) {
    return isHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
}

}  // namespace

// ---------------------------------------------------------------------------
//  WebView2 环境与合成控制器
//
//  CreateCoreWebView2EnvironmentWithOptions 与
//  CreateCoreWebView2CompositionController 都是**异步**的，且完成回调由
//  「创建它的那个线程」派发 —— 也就是 UI 线程。因此在这里同步等待时**必须泵
//  消息**，否则回调永远不到、死等到超时。方案原文在调用后立刻
//  put_RootVisualTarget，实际拿到 E_ILLEGAL_METHOD_CALL（控制器还没建）；
//  本落地改为「事件 + 泵消息有界等待（15s / 10s）」。
// ---------------------------------------------------------------------------
namespace {

// 在 UI 线程上等一个事件，期间泵消息。返回 false = 超时或失败。
bool PumpWaitForEvent(HANDLE h, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD remain = static_cast<DWORD>(deadline - now);
        const DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, remain, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) return true;
        if (r == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            continue;
        }
        return false;   // WAIT_TIMEOUT / WAIT_FAILED
    }
}

}  // namespace

bool LiquidGlassWindow::InitWebView2Composition(const std::wstring& url) {
    if (!hwnd_) return false;

    // 用户数据目录必须可写。放 %LOCALAPPDATA% 而非程序目录：装在 Program Files
    // 时创建环境会失败，表现是窗口全黑且日志里什么都没有。
    wchar_t localAppData[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH)) return false;
    const std::wstring userDataDir = std::wstring(localAppData) + L"\\RAMBlockCache\\WebView2";

    // --- 1) 环境 ---
    struct EnvCtx {
        HANDLE                            doneEvent = nullptr;
        ComPtr<ICoreWebView2Environment>  env;
        HRESULT                           hr = E_PENDING;
    };
    auto envCtx = std::make_shared<EnvCtx>();
    envCtx->doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!envCtx->doneEvent) return false;

    {
        auto handler = Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [envCtx](HRESULT r, ICoreWebView2Environment* e) -> HRESULT {
                envCtx->hr = r;
                if (SUCCEEDED(r) && e) envCtx->env = e;
                SetEvent(envCtx->doneEvent);
                return S_OK;
            });
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, userDataDir.c_str(), nullptr, handler.Get());
        if (FAILED(hr)) {
            CloseHandle(envCtx->doneEvent);
            return false;
        }
    }
    const bool envOk = PumpWaitForEvent(envCtx->doneEvent, 15000);
    CloseHandle(envCtx->doneEvent);
    envCtx->doneEvent = nullptr;
    if (!envOk || FAILED(envCtx->hr) || !envCtx->env) return false;
    wvEnv_ = envCtx->env;

    ComPtr<ICoreWebView2Environment3> env3;
    if (FAILED(wvEnv_.As(&env3)) || !env3) return false;

    // --- 2) 合成控制器 ---
    struct CtrlCtx {
        HANDLE                                     doneEvent = nullptr;
        ComPtr<ICoreWebView2CompositionController> controller;
        HRESULT                                    hr = E_PENDING;
    };
    auto ctrlCtx = std::make_shared<CtrlCtx>();
    ctrlCtx->doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ctrlCtx->doneEvent) return false;

    {
        auto handler = Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
            [ctrlCtx](HRESULT r, ICoreWebView2CompositionController* c) -> HRESULT {
                ctrlCtx->hr = r;
                if (SUCCEEDED(r) && c) ctrlCtx->controller = c;
                SetEvent(ctrlCtx->doneEvent);
                return S_OK;
            });
        const HRESULT hr = env3->CreateCoreWebView2CompositionController(hwnd_, handler.Get());
        if (FAILED(hr)) {
            CloseHandle(ctrlCtx->doneEvent);
            return false;
        }
    }
    const bool ctrlOk = PumpWaitForEvent(ctrlCtx->doneEvent, 10000);
    CloseHandle(ctrlCtx->doneEvent);
    ctrlCtx->doneEvent = nullptr;
    if (!ctrlOk || FAILED(ctrlCtx->hr) || !ctrlCtx->controller) return false;
    wvCompController_ = ctrlCtx->controller;

    // --- 3) 控制器就绪：透明背景 + 挂 Visual 1 + 导航 ---
    ComPtr<ICoreWebView2Controller> controller;
    if (FAILED(wvCompController_.As(&controller)) || !controller) return false;
    wvController_ = controller;

    // 全透明背景，让 Visual 0 的折射桌面透上来。
    // 少了这一行控件会画一块不透明底 —— 玻璃效果整个消失（方案原文漏写）。
    ComPtr<ICoreWebView2Controller3> controller3;
    if (SUCCEEDED(controller.As(&controller3)) && controller3) {
        const COREWEBVIEW2_COLOR transparent{ 0, 0, 0, 0 };
        controller3->put_DefaultBackgroundColor(transparent);
    }

    // 必须先有 Visual 才能挂；挂完再 Commit，否则首帧是黑块。
    if (FAILED(wvCompController_->put_RootVisualTarget(webViewVisual_.Get()))) return false;
    if (FAILED(controller->get_CoreWebView2(&wvCore_)) || !wvCore_) return false;

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(wvCore_->get_Settings(&settings)) && settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
    }

    // 页面 → 宿主 的 message 通道（前端 postMessage 走这里）。
    if (FAILED(wvCore_->add_WebMessageReceived(
            Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    if (!args || shuttingDown_) return S_OK;
                    LPWSTR raw = nullptr;
                    if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                        HandleWebMessageInternal(raw);
                        CoTaskMemFree(raw);
                    } else if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                        HandleWebMessageInternal(raw);
                        CoTaskMemFree(raw);
                    }
                    return S_OK;
                })
                .Get(),
            nullptr))) {
        return false;
    }

    const RECT bounds{ 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
    controller->put_Bounds(bounds);

    if (dcompDevice_) dcompDevice_->Commit();
    if (FAILED(wvCore_->Navigate(url.c_str()))) return false;
    return true;
}

void LiquidGlassWindow::TeardownWebView() {
    if (wvController_) {
        wvController_->Close();
        wvController_.Reset();
    }
    wvCore_.Reset();
    wvCompController_.Reset();
    wvEnv_.Reset();
}

// ---------------------------------------------------------------------------
//  析构：任何 Release 顺序错误都会 0xC0000005。规则是「先断回调、再关子窗口、
//  最后放自己的 COM 引用」，且全程不再 Commit。
// ---------------------------------------------------------------------------
LiquidGlassWindow::~LiquidGlassWindow() {
    shuttingDown_ = true;
    onWebMessage   = nullptr;
    onDeviceRebuilt = nullptr;

    TeardownWebView();

    if (frameLatencyWaitable_) {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
    }
    if (wvCreatedEvent_) {
        CloseHandle(wvCreatedEvent_);
        wvCreatedEvent_ = nullptr;
    }
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Create
// ---------------------------------------------------------------------------
bool LiquidGlassWindow::Create(HINSTANCE hInst, int width, int height, const std::wstring& initialUrl) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &LiquidGlassWindow::WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RAMBlockCacheLiquidGlassHost";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    // WS_EX_NOREDIRECTIONBITMAP：禁用 GDI 重定向位图 —— 这是 DComp 交换链能
    // 「零白闪、零冗余拷贝」的前提，也是 WPF HwndHost 做不到的原因。
    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW;
    const DWORD style   = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;

    hwnd_ = CreateWindowExW(exStyle, wc.lpszClassName, L"RAMBlockCache 控制器",
                            style, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                            nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;

    dpi_ = GetDpiForWindow(hwnd_);
    // [F6] 用传入尺寸而非 GetClientRect：WM_NCCALCSIZE 尚未生效时客户区仍偏小。
    width_  = static_cast<uint32_t>(std::max<LONG>(1, width));
    height_ = static_cast<uint32_t>(std::max<LONG>(1, height));

    ApplySystemCornerAndShadow();

    // 切断自捕获递归环路。RDP 会话由降级矩阵关闭捕获（此处不判断，因为
    // SM_REMOTESESSION 在 RDP 中为 TRUE，此时若也 Exclude 会得到纯黑）。
    if (!GetSystemMetrics(SM_REMOTESESSION) && IsWindows10_2004OrNewer()) {
        SetCaptureExcluded(true);
    }

    if (!InitD3DDevice())       return false;

    const CaptureMonitorInfo monInfo = QueryMonitorHdrAndSdrWhite(dxgiFactory_.Get(), hwnd_);
    isHdr_         = monInfo.isHdr;
    sdrWhiteScale_ = monInfo.sdrWhiteScale;

    if (!RecreateSwapChainWithFormat(PickFormat(isHdr_))) return false;
    if (!InitDirectComposition())                        return false;
    if (!InitWebView2Composition(initialUrl))            return false;

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void LiquidGlassWindow::ApplySystemCornerAndShadow() {
    // 圆角交给系统的 Win11 圆角器（比自己做遮罩省一整条 path），但**材质清零**：
    // 系统 Acrylic/Mica 的浊度固定不可调，必须由 D3D11 HLSL 全权接管 0..4 px 折射。
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const int backdropNone = 1;  // DWMSBT_NONE
    DwmSetWindowAttribute(hwnd_, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdropNone, sizeof(backdropNone));

    // Win11 22H2 起边框色跟随暗色主题；写死 1 会让浅色主题下出现白边。
    BOOL dark = FALSE;
    if (SUCCEEDED(DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark)))) {
        // 成功即可，失败在旧系统上是正常的。
    }
}

bool LiquidGlassWindow::InitD3DDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL actual{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 levels, _countof(levels), D3D11_SDK_VERSION,
                                 &d3dDevice_, &actual, &d3dContext_))) {
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(d3dDevice_.As(&dxgiDev)) || !dxgiDev) return false;
    if (FAILED(dxgiDev->GetAdapter(&adapter)) || !adapter) return false;
    return SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory_)));
}

bool LiquidGlassWindow::RecreateSwapChainWithFormat(DXGI_FORMAT format) {
    if (frameLatencyWaitable_) {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
    }
    backBufferRtv_.Reset();
    swapChain_.Reset();

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width            = width_;
    desc.Height           = height_;
    desc.Format           = format;
    desc.SampleDesc       = { 1, 0 };
    desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount      = 2;
    desc.Scaling          = DXGI_SCALING_STRETCH;   // DComp 必填；像素对齐靠 SetSourceSize
    desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode        = DXGI_ALPHA_MODE_PREMULTIPLIED;  // 预乘 Alpha，前乘在着色器里做
    desc.Flags            = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(dxgiFactory_->CreateSwapChainForComposition(d3dDevice_.Get(), &desc, nullptr, &sc1))) {
        return false;
    }
    if (FAILED(sc1.As(&swapChain_)) || !swapChain_) return false;

    swapChain_->SetMaximumFrameLatency(1);
    frameLatencyWaitable_ = swapChain_->GetFrameLatencyWaitableObject();

    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        swapChain_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    }
    RecreateBackBufferRtv();
    if (glassSwapChainVisual_ && dcompDevice_) {
        glassSwapChainVisual_->SetContent(swapChain_.Get());
        dcompDevice_->Commit();
    }
    return backBufferRtv_ != nullptr;
}

bool LiquidGlassWindow::InitDirectComposition() {
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(d3dDevice_.As(&dxgiDev)) || !dxgiDev) return false;
    if (!dcompDevice_ && FAILED(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&dcompDevice_)))) {
        return false;
    }
    if (!dcompTarget_ && FAILED(dcompDevice_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_))) {
        return false;
    }
    if (!rootVisual_        && FAILED(dcompDevice_->CreateVisual(&rootVisual_)))        return false;
    if (!glassSwapChainVisual_ && FAILED(dcompDevice_->CreateVisual(&glassSwapChainVisual_))) return false;
    if (!webViewVisual_     && FAILED(dcompDevice_->CreateVisual(&webViewVisual_)))     return false;

    glassSwapChainVisual_->SetContent(swapChain_.Get());
    rootVisual_->AddVisual(glassSwapChainVisual_.Get(), FALSE, nullptr);
    // Visual 1 叠在 Visual 0 之上：上面是网页，下面是折射过的桌面。
    rootVisual_->AddVisual(webViewVisual_.Get(), TRUE, glassSwapChainVisual_.Get());
    dcompTarget_->SetRoot(rootVisual_.Get());
    return SUCCEEDED(dcompDevice_->Commit());
}

void LiquidGlassWindow::SyncMonitorHdrAndRebuildIfNeeded() {
    if (!dxgiFactory_ || !hwnd_ || shuttingDown_) return;
    const CaptureMonitorInfo info = QueryMonitorHdrAndSdrWhite(dxgiFactory_.Get(), hwnd_);
    sdrWhiteScale_ = info.sdrWhiteScale;
    if (info.isHdr != isHdr_ || !swapChain_) {
        isHdr_ = info.isHdr;
        RecreateSwapChainWithFormat(PickFormat(isHdr_));
        if (onDeviceRebuilt) onDeviceRebuilt();
    }
}

void LiquidGlassWindow::Resize(uint32_t physWidth, uint32_t physHeight) {
    if (physWidth == 0 || physHeight == 0 || !swapChain_ || shuttingDown_) return;
    if (physWidth == width_ && physHeight == height_) return;

    width_  = physWidth;
    height_ = physHeight;

    d3dContext_->OMSetRenderTargets(0, nullptr, nullptr);
    backBufferRtv_.Reset();

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    swapChain_->GetDesc1(&scDesc);
    if (SUCCEEDED(swapChain_->ResizeBuffers(scDesc.BufferCount, width_, height_, scDesc.Format, scDesc.Flags))) {
        swapChain_->SetSourceSize(width_, height_);
        RecreateBackBufferRtv();
        if (wvController_) {
            RECT bounds{ 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
            wvController_->put_Bounds(bounds);
        }
        dcompDevice_->Commit();
        DwmFlush();
    }
}

void LiquidGlassWindow::WaitForFrameLatency() {
    if (frameLatencyWaitable_) {
        WaitForSingleObjectEx(frameLatencyWaitable_, 100, TRUE);
    }
}

bool LiquidGlassWindow::PresentWithRecovery() {
    if (!swapChain_ || shuttingDown_) return false;
    const HRESULT hr = swapChain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        RebuildDeviceAndCapturePipeline();
        return false;
    }
    return SUCCEEDED(hr);
}

void LiquidGlassWindow::RebuildDeviceAndCapturePipeline() {
    // [F7] 先断开所有对旧 device 的引用，否则重建时旧 device 泄漏且 RTV 悬垂。
    backBufferRtv_.Reset();
    frameLatencyWaitable_ = nullptr;   // 随 swapchain 一起失效，不再单独 Close
    swapChain_.Reset();
    glassSwapChainVisual_.Reset();
    webViewVisual_.Reset();
    rootVisual_.Reset();
    dcompTarget_.Reset();
    dcompDevice_.Reset();
    d3dContext_.Reset();
    d3dDevice_.Reset();
    dxgiFactory_.Reset();

    if (!InitD3DDevice()) return;

    const CaptureMonitorInfo info = QueryMonitorHdrAndSdrWhite(dxgiFactory_.Get(), hwnd_);
    isHdr_         = info.isHdr;
    sdrWhiteScale_ = info.sdrWhiteScale;

    if (!RecreateSwapChainWithFormat(PickFormat(isHdr_))) return;
    if (!InitDirectComposition()) return;
    if (wvCompController_) {
        wvCompController_->put_RootVisualTarget(webViewVisual_.Get());
        dcompDevice_->Commit();
    }
    if (onDeviceRebuilt) onDeviceRebuilt();
}

bool LiquidGlassWindow::SetCaptureExcluded(bool excluded) {
    const DWORD aff = excluded ? kWdaExcludeFromCapture : kWdaNone;
    if (SetWindowDisplayAffinity(hwnd_, aff)) {
        captureExcluded_ = excluded;
        return true;
    }
    return false;
}

void LiquidGlassWindow::RecreateBackBufferRtv() {
    ComPtr<ID3D11Texture2D> backBuf;
    if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuf))) && backBuf) {
        d3dDevice_->CreateRenderTargetView(backBuf.Get(), nullptr, &backBufferRtv_);
    }
}

// ---------------------------------------------------------------------------
//  页面 → 宿主消息。全部用子串匹配而不是 JSON 解析：这条通道由我们自己的
//  页面独占，且解析器会引入分配与异常路径（宿主死在消息线程上就是黑窗）。
// ---------------------------------------------------------------------------
void LiquidGlassWindow::HandleWebMessageInternal(const std::wstring& json) {
    if (json.find(L"\"rbc-web-ready\"") != std::wstring::npos) {
        PostWebMessageJson(L"{\"type\":\"rbc-dcomp-caps\",\"engine\":\"d3d11-dcomp\",\"qualityTier\":\"premium\"}");
        PostWebMessageJson(IsZoomed(hwnd_) != FALSE
                               ? L"{\"type\":\"rbc-window-state\",\"maximized\":true}"
                               : L"{\"type\":\"rbc-window-state\",\"maximized\":false}");
        return;
    }
    if (json.find(L"\"rbc-window-drag\"") != std::wstring::npos) {
        ReleaseCapture();
        SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return;
    }
    if (json.find(L"\"rbc-window-control\"") != std::wstring::npos) {
        if (json.find(L"\"minimize\"") != std::wstring::npos) {
            ShowWindow(hwnd_, SW_MINIMIZE);
        } else if (json.find(L"\"maximize\"") != std::wstring::npos) {
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        } else if (json.find(L"\"close\"") != std::wstring::npos) {
            ShowWindow(hwnd_, SW_HIDE);   // 入托盘常驻
        }
        return;
    }
    if (json.find(L"\"rbc-presentation-mode\"") != std::wstring::npos) {
        // 演示模式：关掉 WDA_EXCLUDEFROMCAPTURE，让截图/录屏能拍到玻璃效果。
        const bool on = json.find(L"true") != std::wstring::npos;
        SetCaptureExcluded(!on);
        freezeBackgroundForPresentation_ = on;
        return;
    }
    if (onWebMessage) onWebMessage(json);
}

void LiquidGlassWindow::PostWebMessageJson(const std::wstring& jsonMsg) {
    if (wvCore_ && !shuttingDown_) {
        wvCore_->PostWebMessageAsJson(jsonMsg.c_str());
    }
}

// ---------------------------------------------------------------------------
//  窗口过程
// ---------------------------------------------------------------------------
LRESULT CALLBACK LiquidGlassWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<LiquidGlassWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<LiquidGlassWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_NCCALCSIZE:
        // [F3] WS_THICKFRAME 会留一条顶部非客户区（白色）。返回 0 让客户区铺满；
        // 最大化时改 rcWork 防止内容溢出工作区（任务栏遮挡）。
        if (wp == TRUE) {
            if (IsZoomed(hwnd)) {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                MONITORINFO mi{ sizeof(MONITORINFO) };
                if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                    params->rgrc[0] = mi.rcWork;
                }
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_NCHITTEST: {
        // [F4] 用当前窗口 DPI 而非成员变量：WM_DPICHANGED 到达前成员是旧值。
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        const int b = MulDiv(6, static_cast<int>(GetDpiForWindow(hwnd)), 96);
        const bool L = pt.x < rc.left + b;
        const bool R = pt.x >= rc.right - b;
        const bool T = pt.y < rc.top + b;
        const bool B = pt.y >= rc.bottom - b;
        if (T && L) return HTTOPLEFT;
        if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT;
        if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT;
        if (R) return HTRIGHT;
        if (T) return HTTOP;
        if (B) return HTBOTTOM;
        // 顶部 40 DIP 的空白区当标题栏拖动（页面自绘顶栏，但空白处要能拖）。
        if (pt.y < rc.top + MulDiv(40, static_cast<int>(GetDpiForWindow(hwnd)), 96)) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_DPICHANGED: {
        self->dpi_ = HIWORD(wp);
        auto* s = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, s->left, s->top, s->right - s->left, s->bottom - s->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        self->SyncMonitorHdrAndRebuildIfNeeded();
        return 0;
    }

    case WM_DISPLAYCHANGE:
        self->SyncMonitorHdrAndRebuildIfNeeded();
        return 0;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            self->Resize(LOWORD(lp), HIWORD(lp));
            self->PostWebMessageJson(wp == SIZE_MAXIMIZED
                                         ? L"{\"type\":\"rbc-window-state\",\"maximized\":true}"
                                         : L"{\"type\":\"rbc-window-state\",\"maximized\":false}");
        }
        return 0;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEWHEEL:
        self->ForwardMouseToWebView(msg, wp, lp);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void LiquidGlassWindow::ForwardMouseToWebView(UINT msg, WPARAM wp, LPARAM lp) {
    if (!wvCompController_ || shuttingDown_) return;
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    // WM_MOUSEMOVE 的 lParam 是客户区坐标；WM_*BUTTON* 也是。只有 WM_MOUSEWHEEL
    // 是屏幕坐标 —— 必须 ScreenToClient，否则滚轮在非 (0,0) 窗口上完全失效。
    if (msg == WM_MOUSEWHEEL) {
        ScreenToClient(hwnd_, &pt);
    }
    const UINT32 mouseData = (msg == WM_MOUSEWHEEL)
                                 ? static_cast<UINT32>(static_cast<SHORT>(HIWORD(wp)))
                                 : 0u;
    const auto vk = static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(LOWORD(wp));
    wvCompController_->SendMouseInput(
        static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(msg), vk, mouseData, pt);
}