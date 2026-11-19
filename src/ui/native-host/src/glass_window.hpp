#pragma once
// ============================================================================
//  src/ui/native-host/src/glass_window.hpp   [295 Wave 1 2026-11-19]
//
//  原生 Win32 D3D11 + DirectComposition 双 Visual 液态玻璃宿主。
//
//  为什么弃 WPF + DWM Desktop Acrylic（293 已查证）：
//    · DWM 材料里只有 Desktop Acrylic(3) 真透「窗口背后内容」，其浊度系统固定
//      不可调 = 物理地板；Mica(2)/MicaAlt(4) 是 opaque，只取壁纸色调，失焦退化纯色。
//    · WPF AllowsTransparency 遇原生子 HWND（WebView2 HwndHost）不绘制 = 全黑。
//    · backdrop-filter 采不到桌面像素（Chromium 只模糊渲染上下文内，窗口下方归 OS）
//      ⇒ 模糊半径唯一来源是 DWM，而 DWM 的材料不可调。
//  唯一正解：自己接管合成，用 DComp 双 Visual —— Visual 0 画折射后的桌面，
//  Visual 1 挂真 WebView2（透明背景），页面内容零重写。
//
//  本文件相对方案原文的修正（见 deliverables/295 §2.1）：
//    · 方案 .hpp 声明了 InitWebView2Composition() / QueryMonitorHdrAndSdrWhite()，
//      但 .cpp 里**没有实现体** ⇒ 照抄即 LNK2019。本落地给出实现（glass_window.cpp
//      + desktop_capture.cpp）。
//    · TRUE/FALSE 不再裸用（某些头序下未定义），统一 TRUE 只出现在 windows.h 之后。
// ============================================================================

#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <wrl/client.h>
// Microsoft::WRL::Callback 定义在 wrl/event.h，wrl.h 是聚合头。
// 只 include wrl/client.h（只有 ComPtr）会让所有 Callback 调用报
// C2039 "Callback 不是 Microsoft::WRL 的成员" —— 这是编译期就暴露的真错。
#include <wrl.h>
#include <WebView2.h>

#include <cstdint>
#include <functional>
#include <string>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// 透镜剖面：决定边缘那 16~28 px 里光线怎么弯。
// ---------------------------------------------------------------------------
enum class GlassProfile : uint32_t {
    ConvexSquircle = 0,  // 4 阶超椭圆：内边界曲率收敛至 0.0003，零折射折痕
    Lip            = 1,  // Smootherstep 复合剖面：顶栏药丸外凸聚光、内凹清透
    Concave        = 2,  // 凹透镜微缩
};

// ---------------------------------------------------------------------------
// 每个玻璃面板的 GPU 参数。layout 必须与 shaders 里的 struct 逐字节一致：
//   5 组 float4 / uint4 = 80 B，alignas(16) 保证 StructuredBuffer stride 对齐。
// 前端 useNativeGlassSync.ts 发的 JSON 由主循环填进这个结构（见 main.cpp）。
// ---------------------------------------------------------------------------
struct alignas(16) GlassPanelGPU {
    float    rect[4];      // x, y, w, h        (物理像素，窗口客户区坐标)
    float    geom[4];      // radius, bezelWidth, thickness, blurPx
    uint32_t meta[4];      // profile, tier(1=nav / 2=card), flags, reserved
    float    adaptive[4];  // Y_bg, variance, polarity(0..1), apcaMinAlpha
    float    state[4];     // holdTimerSec, drmBlackRatio, targetPol, reserved
};

// 显示器 HDR 态与 SDR 白电平 —— 唯一定义在 desktop_capture.h，
// 这里只转发。两处各写一份 struct 会在同一 TU 里重定义（编译期就炸）。
#include "desktop_capture.h"

class LiquidGlassWindow {
public:
    LiquidGlassWindow() = default;
    ~LiquidGlassWindow();

    LiquidGlassWindow(const LiquidGlassWindow&)            = delete;
    LiquidGlassWindow& operator=(const LiquidGlassWindow&) = delete;

    // 调用前必须已 SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)，
    // 否则 GetDpiForWindow 与 WM_DPICHANGED 的换算全错（窗口在 150% 屏上糊成一片）。
    bool Create(HINSTANCE hInst, int width, int height, const std::wstring& initialUrl);

    void Resize(uint32_t physWidth, uint32_t physHeight);
    void SyncMonitorHdrAndRebuildIfNeeded();
    void WaitForFrameLatency();
    bool PresentWithRecovery();
    bool SetCaptureExcluded(bool excluded);
    void PostWebMessageJson(const std::wstring& jsonMsg);

    HWND                   hwnd()          const { return hwnd_; }
    ID3D11Device*          d3dDevice()    const { return d3dDevice_.Get(); }
    ID3D11DeviceContext*   d3dContext()   const { return d3dContext_.Get(); }
    IDXGIFactory4*         dxgiFactory()  const { return dxgiFactory_.Get(); }
    ID3D11RenderTargetView* backBufferRtv() const { return backBufferRtv_.Get(); }
    uint32_t               width()         const { return width_; }
    uint32_t               height()        const { return height_; }
    bool                   isHdr()         const { return isHdr_; }
    float                  sdrWhiteScale() const { return sdrWhiteScale_; }
    bool                   freezeBackground() const { return freezeBackgroundForPresentation_; }
    bool                   isCaptureExcluded() const { return captureExcluded_; }

    std::function<void(const std::wstring& jsonMsg)> onWebMessage;
    std::function<void()>                            onDeviceRebuilt;

private:
    bool InitD3DDevice();
    bool RecreateSwapChainWithFormat(DXGI_FORMAT format);
    bool InitDirectComposition();
    bool InitWebView2Composition(const std::wstring& url);
    void RecreateBackBufferRtv();
    void RebuildDeviceAndCapturePipeline();
    void ApplySystemCornerAndShadow();
    void HandleWebMessageInternal(const std::wstring& json);
    void ForwardMouseToWebView(UINT msg, WPARAM wp, LPARAM lp);
    void TeardownWebView();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND     hwnd_          = nullptr;
    uint32_t width_         = 1280;
    uint32_t height_        = 800;
    uint32_t dpi_           = 96;
    bool     isHdr_         = false;
    float    sdrWhiteScale_ = 1.0f;
    bool     captureExcluded_ = true;
    bool     freezeBackgroundForPresentation_ = false;
    bool     shuttingDown_  = false;   // 析构期标志：回调里一律不得再 Commit/Present

    ComPtr<ID3D11Device>            d3dDevice_;
    ComPtr<ID3D11DeviceContext>     d3dContext_;
    ComPtr<IDXGIFactory4>           dxgiFactory_;
    ComPtr<IDXGISwapChain3>         swapChain_;
    ComPtr<ID3D11RenderTargetView>  backBufferRtv_;
    HANDLE                          frameLatencyWaitable_ = nullptr;

    ComPtr<IDCompositionDevice>     dcompDevice_;
    ComPtr<IDCompositionTarget>     dcompTarget_;
    ComPtr<IDCompositionVisual>     rootVisual_;
    ComPtr<IDCompositionVisual>     glassSwapChainVisual_;
    ComPtr<IDCompositionVisual>     webViewVisual_;

    ComPtr<ICoreWebView2Environment>            wvEnv_;
    ComPtr<ICoreWebView2CompositionController>  wvCompController_;
    ComPtr<ICoreWebView2Controller>             wvController_;
    ComPtr<ICoreWebView2>                       wvCore_;
    // CreateCoreWebView2CompositionController 是异步 API：没有它就无法知道
    // 「何时可以安全 put_RootVisualTarget」。方案原文缺这个事件，照抄会在
    // 控制器就绪前调用并拿到 E_ILLEGAL_METHOD_CALL。
    HANDLE                                     wvCreatedEvent_ = nullptr;
};