#pragma once
// ============================================================================
//  src/ui/native-host/src/desktop_capture.h   [295 Wave 1 2026-11-19]
//
//  方案原文（blk 172 / blk 184）只有：
//      extern CaptureMonitorInfo QueryMonitorHdrAndSdrWhite(IDXGIFactory4*, HWND);
//  —— 一个 extern 声明 + 一段说明注释，**没有实现**。照抄即 LNK2019。
//  本文件把它变成可实现的接口，把「捕获」与「HDR 探测」分成两件独立的事：
//
//    · HDR 探测（QueryMonitorHdrAndSdrWhite）是**必须**的：决定交换链用
//      R16G16B16A16_FLOAT 还是 B8G8R8A8_UNORM。做错就是整屏泛白（SDR 被当 HDR
//      做了 pow(x,1/2.2)）或整屏发灰（HDR 被当 SDR 直通）。这条能纯静态实现。
//    · 实时桌面捕获（DesktopCapture）需要 WGC（Win11 24H2 ReportAndRender）或
//      DXGI DuplicateOutput1（Win10 22H2）。**本文件只定义接口与降级契约**，
//      实现见 desktop_capture.cpp 的 WGC/DXGI 双路；其中 WGC 路径依赖
//      winrt/windows.graphics.capture.h，需要 /std:c++20 + C++/WinRT 头序。
//
//  降级契约（三级，方案 §3.4 的三态）：捕获不可用时**不得**让窗口变黑 ——
//  退回「纯色底 + 页面自身 scrim」，即 292/293 已验收的那条路径。
// ============================================================================

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>

using Microsoft::WRL::ComPtr;

// 显示器 HDR 态与 SDR 白电平（与 glass_window.hpp 里的同名结构必须逐字段一致；
// 二者共用同一份定义由 glass_window.cpp 的 include 顺序保证）。
struct CaptureMonitorInfo {
    HMONITOR hmon          = nullptr;
    RECT     physRect      { 0, 0, 0, 0 };
    bool     isHdr         = false;
    float    sdrWhiteScale = 1.0f;   // HDR: sdrWhiteLevel/1000 ; SDR: 恒 1.0
};

// 查询窗口所在显示器的 HDR 态与 SDR 白电平。
// 实现要点（desktop_capture.cpp）：
//   · 走 IDXGIOutput6::GetDesc1() 拿 DXGI_OUTPUT_DESC1 的 ColorSpace 与
//     MaxLuminance/MinLuminance —— HDR 判据是 ColorSpace == RGB_FULL_G2084_NONE_P2020。
//   · SDR 白电平走 DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL)。
//   · 任何一步失败都**不得**猜：返回 isHdr=false / sdrWhiteScale=1.0（SDR 是安全侧，
//     因为 SDR 路径不做 Gamma 变换，画错只是颜色偏，不会像误判 HDR 那样整屏泛白）。
CaptureMonitorInfo QueryMonitorHdrAndSdrWhite(IDXGIFactory4* factory, HWND hwnd);

// ---------------------------------------------------------------------------
//  实时桌面捕获：把「窗口背后那块桌面」变成一张 D3D11 纹理。
// ---------------------------------------------------------------------------
class DesktopCapture {
public:
    enum class Backend {
        None = 0,       // 不可用（RDP / 独占全屏 / 捕获配额满）→ 走纯色降级
        Wgc,            // Windows.Graphics.Capture（Win11 24H2 优先）
        DxgiDuplication // IDXGIOutput5::DuplicateOutput1（Win10 22H2 回退）
    };

    DesktopCapture() = default;
    ~DesktopCapture();

    DesktopCapture(const DesktopCapture&)            = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;

    // device 由宿主传入（与玻璃渲染共用同一个 D3D11 设备，纹理才能零拷贝进管线）。
    bool Initialize(ID3D11Device* device, IDXGIAdapter* adapter, HWND hwnd, Backend preferred);
    void Shutdown();

    // 抓一帧到内部纹理。返回 false = 本帧无新内容（不是错误）。
    bool AcquireFrame();

    // 供着色器采样：sharp = 原图，blur = 预模糊（0..4px，由 Python 侧… 不，由
    // 宿主的降采样链生成）。二者都可能为 nullptr（首帧之前）。
    ID3D11ShaderResourceView* sharpSrv() const { return sharpSrv_.Get(); }
    ID3D11ShaderResourceView* blurSrv()  const { return blurSrv_.Get(); }

    Backend backend()        const { return backend_; }
    bool    isDeviceLost()   const { return deviceLost_; }
    // DRM 保护内容会返回纯黑帧。连续黑帧占比 >85% ⇒ 着色器把折射权重归零，
    // 否则整块玻璃变成一个黑洞（方案 RT-U8）。
    float   blackRatio()     const { return blackRatio_; }

private:
    bool InitWgc(ID3D11Device* device, IDXGIAdapter* adapter, HWND hwnd);
    bool InitDxgiDuplication(ID3D11Device* device, IDXGIAdapter* adapter, HWND hwnd);
    bool EnsurePersistentTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt);

    Backend backend_     = Backend::None;
    bool    deviceLost_  = false;
    float   blackRatio_  = 0.0f;

    ComPtr<ID3D11Device>         device_;
    ComPtr<ID3D11DeviceContext>  context_;
    ComPtr<ID3D11Texture2D>      sharpTex_;
    ComPtr<ID3D11ShaderResourceView> sharpSrv_;
    ComPtr<ID3D11Texture2D>      blurTex_;
    ComPtr<ID3D11ShaderResourceView> blurSrv_;

    uint32_t texWidth_  = 0;
    uint32_t texHeight_ = 0;

    // DXGI 路径
    ComPtr<IDXGIOutputDuplication> duplication_;

    // WGC 路径的状态用 void* 持有，避免把 winrt 头暴露到这个头文件里
    // （C++/WinRT 的 include 顺序极其挑剔，混进 windows.h 之后必炸）。
    void* wgcState_ = nullptr;
};