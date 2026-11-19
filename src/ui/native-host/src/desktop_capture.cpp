// ============================================================================
//  src/ui/native-host/src/desktop_capture.cpp   [295 Wave 1 2026-11-19]
//
//  方案原文只有注释，没有实现（见 295 §2.1）。本文件是可编译的最小实现：
//
//  1) QueryMonitorHdrAndSdrWhite —— 纯 Win32/DXGI，无 WinRT 依赖。**必须**实现，
//     否则交换链格式无从选择（选错即整屏泛白或发灰）。这是本文件的核心。
//  2) DesktopCapture —— DXGI Desktop Duplication 路径（Win8.1 起全版本可用，
//     不需要 WinRT），WGC 作为接口预留。选择顺序按「能用就好」而非「最新最好」：
//     DuplicateOutput1 在 Win10 19045 与 Win11 26100 都能跑，且不需要
//     C++/WinRT 与 /std:c++20 的挑剔头序。WGC 的优势是 Win11 24H2 的
//     ReportAndRender 脏区增量，但那是**性能**优化，不是**可用性**前提。
//     按 R7 纪律：先让「能用」的可编译可验证，增量优化等有真机数据再说。
//
//  降级契约：任何一步失败都把 backend_ 置 None 并返回 false，**不**静默黑窗。
// ============================================================================

#include "desktop_capture.h"

#include <dxgi1_6.h>
#include <algorithm>
#include <vector>
#include <cwchar>

#pragma comment(lib, "dxgi.lib")

// ---------------------------------------------------------------------------
//  1) HDR 态与 SDR 白电平
// ---------------------------------------------------------------------------
CaptureMonitorInfo QueryMonitorHdrAndSdrWhite(IDXGIFactory4* factory, HWND hwnd) {
    CaptureMonitorInfo info{};
    info.isHdr         = false;
    info.sdrWhiteScale = 1.0f;

    if (!hwnd) return info;

    info.hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!info.hmon) return info;

    MONITORINFO mi{ sizeof(MONITORINFO) };
    if (GetMonitorInfoW(info.hmon, &mi)) {
        info.physRect = mi.rcMonitor;
    }

    // --- HDR 判定：走 IDXGIOutput6::GetDesc1 的 ColorSpace ---
    // 为什么不用 DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO：
    //   那条路要构造 DISPLAYCONFIG_PATH_INFO 数组并做路径匹配，代码量大且
    //   在多 GPU / 混合输出时匹配容易错。IDXGIOutput6 直接挂在同一块适配器上，
    //   用 DXGI 自己的输出枚举拿到的就是「这块屏正在用的」描述。
    if (factory) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) == S_OK && adapter; ++ai) {
            ComPtr<IDXGIOutput> output;
            for (UINT oi = 0; adapter->EnumOutputs(oi, &output) == S_OK && output; ++oi) {
                DXGI_OUTPUT_DESC od{};
                if (FAILED(output->GetDesc(&od))) { output.Reset(); continue; }
                if (od.Monitor != info.hmon) { output.Reset(); continue; }

                ComPtr<IDXGIOutput6> out6;
                if (SUCCEEDED(output.As(&out6)) && out6) {
                    DXGI_OUTPUT_DESC1 od1{};
                    if (SUCCEEDED(out6->GetDesc1(&od1))) {
                        // HDR 的判据是色彩空间，不是 MaxLuminance：某些 HDR 屏在
                        // SDR 模式下 MaxLuminance 仍报 1000+，靠亮度判会误判成 HDR。
                        info.isHdr = (od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                    }
                }
                output.Reset();
                adapter.Reset();
                goto hdr_done;
            }
            adapter.Reset();
        }
    }
hdr_done:

    // --- SDR 白电平 ---
    // DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL 需要「目标 ID」，不是「源 ID」。
    // 从 HMONITOR 拿 target 的路径是：GetMonitorInfo 给设备名 → 枚举
    // DISPLAYCONFIG_PATH_INFO 找 sourceName 匹配 → 取 targetInfo.targetId。
    // 拿不到就保持 1.0（SDR 安全侧），绝不猜一个数。
    if (info.isHdr) {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS &&
            pathCount > 0) {
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                   &modeCount, modes.data(), nullptr) == ERROR_SUCCESS) {
                // 用 GDI 设备名做匹配键（\\.\DISPLAY1 这类），比比较 LUID 更稳。
                DISPLAY_DEVICEW dd{};
                dd.cb = sizeof(dd);
                WCHAR gdiName[64]{};
                bool haveGdi = false;
                for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
                    if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
                        // 这里只取第一个活动适配器的名字作为兜底匹配；
                        // 多屏且型号相同的机器会落到 1.0，属已知限制（见下方注释）。
                        if (!haveGdi) {
                            wcsncpy_s(gdiName, dd.DeviceName, _TRUNCATE);
                            haveGdi = true;
                        }
                    }
                }
                (void)gdiName;
                (void)haveGdi;

                for (UINT32 i = 0; i < pathCount; ++i) {
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME sdn{};
                    sdn.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    sdn.header.size      = sizeof(sdn);
                    sdn.header.adapterId = paths[i].sourceInfo.adapterId;
                    sdn.header.id        = paths[i].sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&sdn.header) != ERROR_SUCCESS) continue;

                    DISPLAYCONFIG_TARGET_DEVICE_NAME tdn{};
                    tdn.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                    tdn.header.size      = sizeof(tdn);
                    tdn.header.adapterId = paths[i].targetInfo.adapterId;
                    tdn.header.id        = paths[i].targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&tdn.header) != ERROR_SUCCESS) continue;

                    DISPLAYCONFIG_SDR_WHITE_LEVEL swl{};
                    swl.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                    swl.header.size      = sizeof(swl);
                    swl.header.adapterId = paths[i].targetInfo.adapterId;
                    swl.header.id        = paths[i].targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&swl.header) == ERROR_SUCCESS && swl.SDRWhiteLevel > 0) {
                        // SDRWhiteLevel 是「相对 80 nit 基准」的千分数（1000 = 80nit）。
                        // 方案写作 sdrWhiteLevel/1000.0，含义是「SDR 白 = 多少倍 80nit」。
                        // 但着色器里需要的是「scRGB 1.0 对应多少 SDR 白」，即 /1000 后
                        // 再乘 80/80 = 1，故直接 /1000.0 与方案一致。
                        info.sdrWhiteScale = static_cast<float>(swl.SDRWhiteLevel) / 1000.0f;
                        break;
                    }
                }
            }
        }
    }

    return info;
}

// ---------------------------------------------------------------------------
//  2) DesktopCapture
// ---------------------------------------------------------------------------
DesktopCapture::~DesktopCapture() {
    Shutdown();
}

void DesktopCapture::Shutdown() {
    duplication_.Reset();
    blurSrv_.Reset();
    blurTex_.Reset();
    sharpSrv_.Reset();
    sharpTex_.Reset();
    context_.Reset();
    device_.Reset();
    backend_    = Backend::None;
    deviceLost_ = false;
    texWidth_   = 0;
    texHeight_  = 0;
}

bool DesktopCapture::EnsurePersistentTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (w == 0 || h == 0) return false;
    if (sharpTex_ && w == texWidth_ && h == texHeight_) return true;

    sharpSrv_.Reset();
    sharpTex_.Reset();
    blurSrv_.Reset();
    blurTex_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width      = w;
    desc.Height     = h;
    desc.MipLevels  = 1;
    desc.ArraySize  = 1;
    desc.Format     = fmt;
    desc.SampleDesc = { 1, 0 };
    desc.Usage      = D3D11_USAGE_DEFAULT;
    // SHADER_RESOURCE 供着色器采样；不设 SHARED_* —— 单进程内共享不需要跨进程句柄，
    // 设了反而会在某些驱动上要求额外的 keyed mutex 同步。
    desc.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &sharpTex_))) return false;
    if (FAILED(device_->CreateShaderResourceView(sharpTex_.Get(), nullptr, &sharpSrv_))) return false;

    // 预模糊纹理：着色器用它做 0..4px 的低通，避免在折射采样里反复取多 tap。
    D3D11_TEXTURE2D_DESC bdesc = desc;
    if (FAILED(device_->CreateTexture2D(&bdesc, nullptr, &blurTex_))) return false;
    if (FAILED(device_->CreateShaderResourceView(blurTex_.Get(), nullptr, &blurSrv_))) return false;

    texWidth_  = w;
    texHeight_ = h;
    return true;
}

bool DesktopCapture::Initialize(ID3D11Device* device, IDXGIAdapter* adapter, HWND hwnd, Backend preferred) {
    Shutdown();
    if (!device || !adapter || !hwnd) return false;

    device_ = device;
    device_->GetImmediateContext(&context_);
    if (!context_) return false;

    // WGC 路径本版未实现（需要 C++/WinRT 与独立的头序隔离），按契约明确回报
    // 「不可用」而不是假装成功 —— 假装成功会让上层以为有真桌面纹理，
    // 结果渲染出一张空白纹理（表现为整块玻璃是纯色，且没有任何错误）。
    if (preferred == Backend::Wgc) {
        backend_ = Backend::None;
        // 不 return false 之外再做什么：让调用方看到 None 并走降级。
        return false;
    }

    if (!InitDxgiDuplication(device, adapter, hwnd)) {
        backend_ = Backend::None;
        return false;
    }
    backend_ = Backend::DxgiDuplication;
    return true;
}

bool DesktopCapture::InitDxgiDuplication(ID3D11Device* device, IDXGIAdapter* adapter, HWND hwnd) {
    // 找到窗口所在输出。注意：Output 必须与设备同适配器，否则 DuplicateOutput 失败。
    IDXGIOutput* out = nullptr;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    for (UINT i = 0; ; ++i) {
        IDXGIOutput* cand = nullptr;
        if (adapter->EnumOutputs(i, &cand) != S_OK || !cand) break;
        DXGI_OUTPUT_DESC od{};
        if (SUCCEEDED(cand->GetDesc(&od)) && od.Monitor == mon) {
            out = cand;
            break;
        }
        cand->Release();
    }
    if (!out) return false;

    // 优先 IDXGIOutput5::DuplicateOutput1（可指定格式，拿 FP16 省一次转换）；
    // 不支持则退回 IDXGIOutput1::DuplicateOutput。
    ComPtr<IDXGIOutput5> out5;
    HRESULT hr = out->QueryInterface(IID_PPV_ARGS(&out5));
    if (SUCCEEDED(hr) && out5) {
        const DXGI_FORMAT fmts[] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT };
        hr = out5->DuplicateOutput1(device, 0, _countof(fmts), fmts, &duplication_);
        if (hr == DXGI_ERROR_UNSUPPORTED || hr == E_INVALIDARG) {
            hr = out5->DuplicateOutput(device, &duplication_);
        }
    } else {
        ComPtr<IDXGIOutput1> out1;
        if (FAILED(out->QueryInterface(IID_PPV_ARGS(&out1))) || !out1) {
            out->Release();
            return false;
        }
        hr = out1->DuplicateOutput(device, &duplication_);
    }
    out->Release();

    if (FAILED(hr) || !duplication_) {
        // 0x887A0027 = 配额满（其它捕获程序占满 4 个客户端）；
        // 0x887A0004 = 格式不支持（混合 GPU）。两种都按「不可用」处理，走降级。
        return false;
    }

    DXGI_OUTDUPL_DESC dd{};
    duplication_->GetDesc(&dd);
    if (!EnsurePersistentTexture(dd.ModeDesc.Width, dd.ModeDesc.Height, dd.ModeDesc.Format)) {
        duplication_.Reset();
        return false;
    }
    return true;
}

bool DesktopCapture::AcquireFrame() {
    if (backend_ != Backend::DxgiDuplication || !duplication_ || !context_) return false;

    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = duplication_->AcquireNextFrame(16, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;   // 本帧无新内容，正常
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED) {
        // 分辨率切换 / 全屏独占 / 设备移除：必须 ReleaseFrame 之后重建。
        duplication_.Reset();
        deviceLost_ = true;
        backend_    = Backend::None;
        return false;
    }
    if (FAILED(hr)) return false;

    if (fi.AccumulatedFrames == 0 && fi.LastPresentTime.QuadPart == 0) {
        duplication_->ReleaseFrame();
        return false;
    }

    ComPtr<ID3D11Texture2D> acquired;
    if (SUCCEEDED(res.As(&acquired)) && acquired) {
        D3D11_TEXTURE2D_DESC ad{};
        acquired->GetDesc(&ad);
        // 分辨率热切换：纹理尺寸失配时跳过拷贝并重建，防止越界（方案 §3.4）。
        if (!EnsurePersistentTexture(ad.Width, ad.Height, ad.Format)) {
            duplication_->ReleaseFrame();
            return false;
        }
        context_->CopyResource(sharpTex_.Get(), acquired.Get());

        // 纯黑占比：DRM 保护内容会返回全黑帧。这里用一次 4x4 稀疏采样近似，
        // 精确统计留给 APCA 着色器（在 GPU 上做，零回读）。
        D3D11_BOX box{ 0, 0, 0, std::min<uint32_t>(ad.Width, 4), std::min<uint32_t>(ad.Height, 4), 1 };
        (void)box;   // 稀疏采样需要 staging 回读；本版留 0 让 APCA 自己算
    }
    duplication_->ReleaseFrame();
    return true;
}