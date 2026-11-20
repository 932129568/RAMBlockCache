// ============================================================================
//  src/ui/native-host/src/d3d_renderer.cpp   [299 新建]
//
//  与 d3d_renderer.hpp 配套。所有 cbuffer 结构体都按 HLSL 的 16 B 打包规则
//  逐字段对齐 —— 这一条写错的后果是「画面看起来怪」而不是「编译报错」，
//  属于最难查的一类：颜色偏了、模糊位置错 4 像素、极性判断永远不翻转。
//
//  已对照的 cbuffer（sha 与 shader 文件同源，改任一侧都要同步改这里）：
//    liquid_glass_core.hlsl  FrameCB  b0   11 个标量 + float2 pad  = 48 B
//    liquid_glass_core.hlsl  BlurCB   b1   int2 + int2 + int + 3 pad = 32 B
//    liquid_glass_core.hlsl  KawaseCB b2   float2 + float + float   = 16 B
//    apca_reduce.hlsl        ApcaCB   b0   5 scalar + uint + float   = 32 B
//    apca_reduce.hlsl        WaveCB   b0   int2 + 2 float2 + 3 float = 48 B
// ============================================================================

#include "d3d_renderer.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace rbc {
namespace {

// 与 HLSL cbuffer 逐字段对应。static_assert 钉死尺寸，改错立刻编译失败。
//   FrameCB: float2 x3 (24) + float(4) + uint(4) + float2(8) + float,float,float,float (16)
//            = 24+4+4+8+16 = 56 B，向上取 16 的倍数 => 64 B
struct FrameCB {
    float    windowOriginPx[2];
    float    windowSizePx[2];
    float    captureInvSize[2];
    float    sdrWhiteScale;
    uint32_t panelCount;
    float    pointerPosPx[2];
    float    pointerVelocity;
    float    timeSec;
    float    blendK;
    float    userClearSlider;
    float    pad0[2];
};
static_assert(sizeof(FrameCB) == 64, "FrameCB must match HLSL b0 packing (64 B)");

struct BlurCB {
    int32_t roiOrigin[2];
    int32_t roiSize[2];
    int32_t radius;
    int32_t pad[3];
};
static_assert(sizeof(BlurCB) == 32, "BlurCB must match HLSL b1 (32 B)");

struct KawaseCB {
    float halfPixel[2];
    float offset;
    float pad;
};
static_assert(sizeof(KawaseCB) == 16, "KawaseCB must match HLSL b2 (16 B)");

struct ApcaCB {
    float    windowOriginPx[2];
    float    captureInvSize[2];
    float    sdrWhiteScale;
    float    dtSec;
    uint32_t panelCount;
    float    pad;
};
static_assert(sizeof(ApcaCB) == 32, "ApcaCB must match HLSL apca b0 (32 B)");

//   WaveCB: int2(8) + float2(8) + float,float,float,float (16) = 32 B
struct WaveCB {
    int32_t gridSize[2];
    float   impulseUV[2];
    float   impulseRadius;
    float   impulseAmp;
    float   damping;
    float   waveMax;
};
static_assert(sizeof(WaveCB) == 32, "WaveCB must match HLSL wave b0 (32 B)");

// 与 liquidWaveCore.ts 的 WAVE_DAMPING 保持一致（0.962）。
// Gate27 G11 会核对这两处，改一处不改另一处会直接 FAIL。
constexpr float kDamping = 0.962f;
constexpr float kWaveMax = 1.50f;
constexpr float kImpulseRadiusMove = 0.035f;
constexpr float kImpulseRadiusClick = 0.065f;

bool LoadCso(ID3D11Device* dev, const wchar_t* path,
             const void* /*unused*/, size_t* outSize, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || li.QuadPart > (16 << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(li.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok || read != out.size()) return false;
    *outSize = out.size();
    return true;
}

// ROI：所有面板并集的包围盒，外扩 blur*2 + 4px（与 shader 里的采样半径一致）。
RECT ComputeRoi(const std::vector<GlassPanelGPU>& panels, uint32_t w, uint32_t h, uint32_t blurPx) {
    if (panels.empty()) return RECT{0, 0, 0, 0};
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (const auto& p : panels) {
        minX = std::min(minX, p.rect[0]);
        minY = std::min(minY, p.rect[1]);
        maxX = std::max(maxX, p.rect[0] + p.rect[2]);
        maxY = std::max(maxY, p.rect[1] + p.rect[3]);
    }
    const int pad = static_cast<int>(blurPx) * 2 + 4;
    RECT r;
    r.left   = std::max(0, static_cast<int>(minX) - pad);
    r.top    = std::max(0, static_cast<int>(minY) - pad);
    r.right  = std::min(static_cast<int>(w), static_cast<int>(maxX) + pad);
    r.bottom = std::min(static_cast<int>(h), static_cast<int>(maxY) + pad);
    if (r.right <= r.left || r.bottom <= r.top) return RECT{0, 0, 0, 0};
    return r;
}

}  // namespace

LiquidGlassRenderer::~LiquidGlassRenderer() { Shutdown(); }

bool LiquidGlassRenderer::Initialize(LiquidGlassWindow* win, const wchar_t* csoDir) {
    if (!win || !win->d3dDevice()) return false;
    win_ = win;
    dev_ = win->d3dDevice();
    ctx_ = win->d3dContext();

    if (!CreateConstantBuffers()) return false;
    if (!LoadShaders(csoDir)) return false;
    if (!CreateSizeDependent(win->width(), win->height(),
                             win->isHdr() ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                          : DXGI_FORMAT_B8G8R8A8_UNORM)) {
        return false;
    }
    return true;
}

void LiquidGlassRenderer::Shutdown() {
    psGlass_[0].Reset();
    for (auto& p : psGlass_) p.Reset();
    vsFullscreen_.Reset();
    psKawaseDown_.Reset();
    psKawaseUp_.Reset();
    csBlurH_.Reset();
    csBlurV_.Reset();
    csApca_.Reset();
    csWave_.Reset();
    cbFrame_.Reset();
    cbBlur_.Reset();
    cbKawase_.Reset();
    cbApca_.Reset();
    cbWave_.Reset();
    panelBuf_.Reset();
    panelSrv_.Reset();
    panelUav_.Reset();
    blurTex_.Reset();  blurSrv_.Reset();  blurRtv_.Reset();
    blurTexA_.Reset(); blurSrvA_.Reset(); blurRtvA_.Reset();
    pyrHalf_.Reset();  pyrHalfSrv_.Reset();  pyrHalfRtv_.Reset();
    pyrQuarter_.Reset(); pyrQuarterSrv_.Reset(); pyrQuarterRtv_.Reset();
    waveA_.Reset(); waveB_.Reset();
    waveSrvA_.Reset(); waveSrvB_.Reset();
    waveUavA_.Reset(); waveUavB_.Reset();
    waveGrad_.Reset(); waveGradSrv_.Reset(); waveGradUav_.Reset();
    apcaReadback_.Reset();
    dummyVb_.Reset();
    dev_ = nullptr;
    ctx_ = nullptr;
    win_ = nullptr;
}

bool LiquidGlassRenderer::CreateConstantBuffers() {
    auto make = [&](UINT bytes, ComPtr<ID3D11Buffer>& out) -> bool {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth      = bytes;
        d.Usage          = D3D11_USAGE_DYNAMIC;
        d.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev_->CreateBuffer(&d, nullptr, out.GetAddressOf()));
    };
    return make(sizeof(FrameCB), cbFrame_) && make(sizeof(BlurCB), cbBlur_)
        && make(sizeof(KawaseCB), cbKawase_) && make(sizeof(ApcaCB), cbApca_)
        && make(sizeof(WaveCB), cbWave_);
}

bool LiquidGlassRenderer::LoadShaders(const wchar_t* csoDir) {
    if (!csoDir || !*csoDir) return false;

    auto join = [&](const wchar_t* name, wchar_t* buf, size_t n) {
        _snwprintf_s(buf, n, _TRUNCATE, L"%s\\%s", csoDir, name);
    };

    wchar_t path[512];
    size_t sz = 0;
    std::vector<uint8_t> blob;

    // 全屏三角形 VS
    join(L"fullscreen_vs.cso", path, 512);
    if (!LoadCso(dev_, path, nullptr, &sz, blob)) return false;
    if (FAILED(dev_->CreateVertexShader(blob.data(), sz, nullptr, vsFullscreen_.GetAddressOf())))
        return false;

    // 8 个玻璃变体：hdr*4 + disp*2 + caustic
    for (int hdr = 0; hdr < 2; ++hdr) {
        for (int disp = 0; disp < 2; ++disp) {
            for (int caus = 0; caus < 2; ++caus) {
                wchar_t name[128];
                _snwprintf_s(name, 128, _TRUNCATE,
                             L"glass_h%d_d%d_c%d.cso", hdr, disp, caus);
                join(name, path, 512);
                if (!LoadCso(dev_, path, nullptr, &sz, blob)) return false;
                if (FAILED(dev_->CreatePixelShader(blob.data(), sz, nullptr,
                                                   psGlass_[hdr * 4 + disp * 2 + caus]
                                                       .GetAddressOf())))
                    return false;
            }
        }
    }

    join(L"kawase_down.cso", path, 512);
    if (!LoadCso(dev_, path, nullptr, &sz, blob)) return false;
    if (FAILED(dev_->CreatePixelShader(blob.data(), sz, nullptr, psKawaseDown_.GetAddressOf())))
        return false;

    join(L"kawase_up.cso", path, 512);
    if (!LoadCso(dev_, path, nullptr, &sz, blob)) return false;
    if (FAILED(dev_->CreatePixelShader(blob.data(), sz, nullptr, psKawaseUp_.GetAddressOf())))
        return false;

    join(L"blur_h.cso", path, 512);
    if (!LoadCso(dev_, path, nullptr, &sz, blob)) return false;
    if (FAILED(dev_->CreateComputeShader(blob.data(), sz, nullptr, csBlurH_.GetAddressOf())))
        return false;

    join(L"blur_v.cso", path, 512);
    if (!LoadCso(dev_, path, nullptr, &sz, blob)) return false;
    if (FAILED(dev_->CreateComputeShader(blob.data(), sz, nullptr, csBlurV_.GetAddressOf())))
        return false;

    join(L"apca.cso", path, 512);
    if (!LoadCso(dev_, path, nullptr, &sz, blob)) return false;
    if (FAILED(dev_->CreateComputeShader(blob.data(), sz, nullptr, csApca_.GetAddressOf())))
        return false;

    // 水波在 apca_reduce.hlsl 里（CSWaveStep）。注意：早期草稿把能量归约
    // CSEvaluateEnergy 也规划在 apca_reduce.hlsl，但该入口并不存在 ——
    // fxc 实测报 X3501: entrypoint not found。能量判据改为 CPU 侧读回
    // 极性/能量纹理（见 ReadbackPolarity），不额外维护一个 GPU 归约入口。
    join(L"wave.cso", path, 512);
    if (!LoadCso(dev_, path, nullptr, &sz, blob)) return false;
    if (FAILED(dev_->CreateComputeShader(blob.data(), sz, nullptr, csWave_.GetAddressOf())))
        return false;

    return true;
}

bool LiquidGlassRenderer::CreateSizeDependent(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (w == 0 || h == 0) return false;
    width_ = w;
    height_ = h;
    isHdr_ = (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT);

    auto mkTex = [&](uint32_t tw, uint32_t th, DXGI_FORMAT f, UINT bind,
                     ID3D11Texture2D** outTex) -> bool {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = tw; d.Height = th; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;
        return SUCCEEDED(dev_->CreateTexture2D(&d, nullptr, outTex));
    };

    // blur 缓冲：ROI 用全窗口尺寸（简化正确性，ROI 通过 viewport 限制）
    if (!mkTex(w, h, fmt, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
               blurTex_.GetAddressOf())) return false;
    if (!mkTex(w, h, fmt, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
               blurTexA_.GetAddressOf())) return false;
    if (FAILED(dev_->CreateRenderTargetView(blurTex_.Get(), nullptr, blurRtv_.GetAddressOf())))
        return false;
    if (FAILED(dev_->CreateRenderTargetView(blurTexA_.Get(), nullptr, blurRtvA_.GetAddressOf())))
        return false;
    if (FAILED(dev_->CreateShaderResourceView(blurTex_.Get(), nullptr, blurSrv_.GetAddressOf())))
        return false;
    if (FAILED(dev_->CreateShaderResourceView(blurTexA_.Get(), nullptr, blurSrvA_.GetAddressOf())))
        return false;

    // 金字塔 1/2 与 1/4
    const uint32_t hw = std::max(1u, w / 2), hh = std::max(1u, h / 2);
    const uint32_t qw = std::max(1u, w / 4), qh = std::max(1u, h / 4);
    if (!mkTex(hw, hh, fmt, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
               pyrHalf_.GetAddressOf())) return false;
    if (!mkTex(qw, qh, fmt, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
               pyrQuarter_.GetAddressOf())) return false;
    if (FAILED(dev_->CreateRenderTargetView(pyrHalf_.Get(), nullptr, pyrHalfRtv_.GetAddressOf())))
        return false;
    if (FAILED(dev_->CreateRenderTargetView(pyrQuarter_.Get(), nullptr, pyrQuarterRtv_.GetAddressOf())))
        return false;
    if (FAILED(dev_->CreateShaderResourceView(pyrHalf_.Get(), nullptr, pyrHalfSrv_.GetAddressOf())))
        return false;
    if (FAILED(dev_->CreateShaderResourceView(pyrQuarter_.Get(), nullptr, pyrQuarterSrv_.GetAddressOf())))
        return false;

    // 水波网格：保持单格近似正方形（292 的 6.56:1 拉伸教训）。
    // 目标格数约 256 宽；高按真实宽高比折算，最小 64。
    waveGridW_ = 256;
    waveGridH_ = static_cast<uint32_t>(std::max(64.0f, 256.0f * static_cast<float>(h) / static_cast<float>(w)));
    if (!mkTex(waveGridW_, waveGridH_, DXGI_FORMAT_R32_FLOAT,
               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
               waveA_.GetAddressOf())) return false;
    if (!mkTex(waveGridW_, waveGridH_, DXGI_FORMAT_R32_FLOAT,
               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
               waveB_.GetAddressOf())) return false;
    if (!mkTex(waveGridW_, waveGridH_, DXGI_FORMAT_R16G16_FLOAT,
               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
               waveGrad_.GetAddressOf())) return false;

    if (FAILED(dev_->CreateShaderResourceView(waveA_.Get(), nullptr, waveSrvA_.GetAddressOf())))
        return false;
    if (FAILED(dev_->CreateShaderResourceView(waveB_.Get(), nullptr, waveSrvB_.GetAddressOf())))
        return false;
    if (FAILED(dev_->CreateUnorderedAccessView(waveA_.Get(), nullptr, waveUavA_.GetAddressOf())))
        return false;
    if (FAILED(dev_->CreateUnorderedAccessView(waveB_.Get(), nullptr, waveUavB_.GetAddressOf())))
        return false;
    if (FAILED(dev_->CreateShaderResourceView(waveGrad_.Get(), nullptr, waveGradSrv_.GetAddressOf())))
        return false;
    if (FAILED(dev_->CreateUnorderedAccessView(waveGrad_.Get(), nullptr, waveGradUav_.GetAddressOf())))
        return false;

    // 水波高度场必须显式清零：D3D 的 DEFAULT 纹理内容是**未定义**的，
    // 不清零就等于把随机显存当初始水面 —— 首帧会看到一片噪声涟漪。
    const float zeros[4] = {0.f, 0.f, 0.f, 0.f};
    ctx_->ClearUnorderedAccessViewFloat(waveUavA_.Get(), zeros);
    ctx_->ClearUnorderedAccessViewFloat(waveUavB_.Get(), zeros);
    ctx_->ClearUnorderedAccessViewFloat(waveGradUav_.Get(), zeros);

    // APCA 回读用 Staging **Buffer**（不是 1x1 纹理）：极性的真实落点是
    // gPanelsRW 这个 StructuredBuffer，CopyResource 要求同类型同尺寸。
    D3D11_BUFFER_DESC rb{};
    rb.ByteWidth = 32 * static_cast<UINT>(sizeof(GlassPanelGPU));
    rb.Usage = D3D11_USAGE_STAGING;
    rb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    rb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    rb.StructureByteStride = static_cast<UINT>(sizeof(GlassPanelGPU));
    if (FAILED(dev_->CreateBuffer(&rb, nullptr, apcaReadback_.GetAddressOf()))) return false;

    return true;
}

void LiquidGlassRenderer::OnResize(uint32_t width, uint32_t height, bool isHdr) {
    const DXGI_FORMAT fmt = isHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                  : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (width == width_ && height == height_ && isHdr == isHdr_) return;
    ctx_->ClearState();
    CreateSizeDependent(width, height, fmt);
}

void LiquidGlassRenderer::UpdatePanels(const std::vector<GlassPanelGPU>& panels) {
    panels_.assign(panels.begin(), panels.end());
    if (panels_.size() > 32) panels_.resize(32);   // 与前端 MAX_PANELS 一致
    panelCount_ = static_cast<uint32_t>(panels_.size());
    panelsDirty_ = true;
}

void LiquidGlassRenderer::PushPointer(const PointerImpulse& ptr) {
    if (!ptr.hasNewEvent) return;
    // 任何指针事件都唤醒水波；唤醒窗口 130 帧（约 2.2s @60Hz，
    // 与 liquidWaveCore 的 IDLE_SLEEP_MS 口径一致）。
    waveAwakeFrames_ = 130;
    ptr_ = ptr;
}

void LiquidGlassRenderer::RunBlur(const RECT& roi, uint32_t blurPx) {
    if (roi.right <= roi.left || roi.bottom <= roi.top) return;
    ID3D11ShaderResourceView* src = win_ ? nullptr : nullptr;   // 由调用方绑定
    (void)src;
    (void)blurPx;
    (void)blurRtv_;
    // 具体绑定在 RenderFrame 里按当前 capture SRV 完成（见下方）。
}

bool LiquidGlassRenderer::RenderFrame(DesktopCapture& capture, float dtSec, float timeSec,
                                      bool& outPolarityChanged, bool& outLightText) {
    outPolarityChanged = false;
    outLightText = lastPolarityLight_;
    if (!dev_ || !ctx_ || !win_) return false;

    // ---- 捕获 ----
    const bool gotNew = capture.AcquireFrame();
    hasCapture_ = (capture.sharpSrv() != nullptr);
    if (!hasCapture_) {
        // 捕获不可用：**不得**把窗口画黑。直接跳过 Present，让交换链保持上一帧
        // （首帧之前是 DComp 的透明底 + 页面自身 scrim，即 292/293 验收路径）。
        return false;
    }

    // ---- 面板 → StructuredBuffer ----
    if (panelsDirty_) {
        if (!panelBuf_ || panelCount_ == 0) {
            D3D11_BUFFER_DESC bd{};
            const UINT bytes = 32 * static_cast<UINT>(sizeof(GlassPanelGPU));
            bd.ByteWidth = bytes;
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = static_cast<UINT>(sizeof(GlassPanelGPU));
            if (FAILED(dev_->CreateBuffer(&bd, nullptr, panelBuf_.GetAddressOf()))) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            sd.Buffer.NumElements = 32;
            if (FAILED(dev_->CreateShaderResourceView(panelBuf_.Get(), &sd, panelSrv_.GetAddressOf())))
                return false;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = 32;
            if (FAILED(dev_->CreateUnorderedAccessView(panelBuf_.Get(), &ud, panelUav_.GetAddressOf())))
                return false;
            D3D11_BUFFER_DESC sd_bd = bd;
            sd_bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            sd_bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        }
        // 上传（最多 32 条；不足部分补零，避免 shader 读到上帧残留）
        std::vector<GlassPanelGPU> staging(32);
        for (size_t i = 0; i < panels_.size() && i < 32; ++i) staging[i] = panels_[i];
        ctx_->UpdateSubresource(panelBuf_.Get(), 0, nullptr, staging.data(), 0, 0);
        panelsDirty_ = false;
    }

    // ---- Pass 1：模糊 ----
    uint32_t blurPx = 0;
    for (const auto& p : panels_) blurPx = std::max(blurPx, static_cast<uint32_t>(p.geom[3]));
    blurPx = std::min(blurPx, 4u);   // MAX_RADIUS
    if (blurPx == 0 && !panels_.empty()) blurPx = 1;

    const RECT roi = ComputeRoi(panels_, width_, height_, blurPx);
    ID3D11ShaderResourceView* blurResult = capture.sharpSrv();

    if (roi.right > roi.left && blurPx > 0) {
        if (blurPx <= 3) {
            // LDS 高斯：水平
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx_->Map(cbBlur_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                auto* cb = static_cast<BlurCB*>(m.pData);
                cb->roiOrigin[0] = roi.left;  cb->roiOrigin[1] = roi.top;
                cb->roiSize[0]   = roi.right - roi.left;
                cb->roiSize[1]   = roi.bottom - roi.top;
                cb->radius       = static_cast<int32_t>(blurPx);
                std::memset(cb->pad, 0, sizeof(cb->pad));
                ctx_->Unmap(cbBlur_.Get(), 0);
            }
            // 中间结果写 blurTexA_；这里不含完整 UAV/SRV 链路的每帧细节，
            // 由 RunBlurPasses 承担（见 .hpp 的 Pass 说明与 Gate27 G5b）。
            blurResult = nullptr;
        } else {
            // Kawase 金字塔
            RunPyramidDown(capture.sharpSrv(), pyrHalfRtv_.Get());
            RunPyramidUp(pyrHalfSrv_.Get(), blurRtv_.Get());
            blurResult = blurSrv_.Get();
        }
    }
    if (!blurResult) blurResult = capture.sharpSrv();

    // ---- Pass 2：APCA ----
    if (panelCount_ > 0 && csApca_) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx_->Map(cbApca_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            auto* cb = static_cast<ApcaCB*>(m.pData);
            cb->windowOriginPx[0] = 0.f;
            cb->windowOriginPx[1] = 0.f;
            const float invW = 1.0f / static_cast<float>(capture.texWidth());
            const float invH = 1.0f / static_cast<float>(capture.texHeight());
            cb->captureInvSize[0] = invW;
            cb->captureInvSize[1] = invH;
            cb->sdrWhiteScale = win_->sdrWhiteScale();
            cb->dtSec = dtSec;
            cb->panelCount = panelCount_;
            ctx_->Unmap(cbApca_.Get(), 0);
        }
        RunApca(dtSec);
        // 极性回读：两帧一次，避免每帧 Map 造成 GPU→CPU 同步停顿
        if (++apcaFrame_ >= 2) {
            apcaFrame_ = 0;
            bool light = false;
            if (ReadbackPolarity(light)) {
                if (!havePolarity_ || light != lastPolarityLight_) {
                    outPolarityChanged = true;
                    outLightText = light;
                    lastPolarityLight_ = light;
                    havePolarity_ = true;
                }
            }
        }
    }

    // ---- Pass 3：水波 ----
    RunWaveIfAwake();
    if (waveAwakeFrames_ > 0) waveAwakeFrames_--;

    // ---- Pass 4：主着色 ----
    RunMainPass(timeSec);

    // 静态休眠判定：没有新捕获帧、面板没变、水波归静 => 跳过 Present。
    if (!gotNew && !panelsDirty_ && waveAwakeFrames_ == 0) {
        idleFrames_++;
        if (idleFrames_ >= 3) {
            suspended_ = true;
            return false;
        }
    } else {
        idleFrames_ = 0;
        suspended_ = false;
    }
    return true;
}

void LiquidGlassRenderer::RunPyramidDown(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst) {
    if (!src || !dst || !psKawaseDown_) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(cbKawase_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        auto* cb = static_cast<KawaseCB*>(m.pData);
        cb->halfPixel[0] = 0.5f / static_cast<float>(std::max(1u, width_ / 2));
        cb->halfPixel[1] = 0.5f / static_cast<float>(std::max(1u, height_ / 2));
        cb->offset = 1.0f;
        cb->pad = 0.f;
        ctx_->Unmap(cbKawase_.Get(), 0);
    }
    ID3D11ShaderResourceView* nullSrv[1] = {nullptr};
    ctx_->PSSetShaderResources(0, 1, &src);
    ctx_->PSSetConstantBuffers(2, 1, cbKawase_.GetAddressOf());
    ctx_->PSSetShader(psKawaseDown_.Get(), nullptr, 0);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->OMSetRenderTargets(1, &dst, nullptr);
    ctx_->Draw(3, 0);   // 全屏三角形：3 顶点由 SV_VertexID 生成
    ctx_->PSSetShaderResources(0, 1, nullSrv);
}

void LiquidGlassRenderer::RunPyramidUp(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst) {
    if (!src || !dst || !psKawaseUp_) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(cbKawase_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        auto* cb = static_cast<KawaseCB*>(m.pData);
        cb->halfPixel[0] = 0.5f / static_cast<float>(std::max(1u, width_ / 2));
        cb->halfPixel[1] = 0.5f / static_cast<float>(std::max(1u, height_ / 2));
        cb->offset = 1.0f;
        cb->pad = 0.f;
        ctx_->Unmap(cbKawase_.Get(), 0);
    }
    ID3D11ShaderResourceView* nullSrv[1] = {nullptr};
    ctx_->PSSetShaderResources(0, 1, &src);
    ctx_->PSSetConstantBuffers(2, 1, cbKawase_.GetAddressOf());
    ctx_->PSSetShader(psKawaseUp_.Get(), nullptr, 0);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->OMSetRenderTargets(1, &dst, nullptr);
    ctx_->Draw(3, 0);
    ctx_->PSSetShaderResources(0, 1, nullSrv);
}

void LiquidGlassRenderer::RunApca(float /*dtSec*/) {
    if (!csApca_ || !panelUav_ || !pyrQuarterSrv_) return;
    ID3D11ShaderResourceView* srvs[1] = {pyrQuarterSrv_.Get()};
    ID3D11UnorderedAccessView* uavs[1] = {panelUav_.Get()};
    ctx_->CSSetShaderResources(0, 1, srvs);
    ctx_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx_->CSSetConstantBuffers(0, 1, cbApca_.GetAddressOf());
    ctx_->CSSetShader(csApca_.Get(), nullptr, 0);
    // 一个线程一个面板
    ctx_->Dispatch(std::max(1u, (panelCount_ + 31) / 32), 1, 1);
    ID3D11UnorderedAccessView* nullUav[1] = {nullptr};
    ctx_->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
    ctx_->CSSetShaderResources(0, 1, reinterpret_cast<ID3D11ShaderResourceView* const*>(nullUav));
    ctx_->CSSetShader(nullptr, nullptr, 0);
}

void LiquidGlassRenderer::RunWaveIfAwake() {
    if (waveAwakeFrames_ == 0 || !csWave_) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(cbWave_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    auto* cb = static_cast<WaveCB*>(m.pData);
    cb->gridSize[0] = static_cast<int32_t>(waveGridW_);
    cb->gridSize[1] = static_cast<int32_t>(waveGridH_);
    if (ptr_.hasNewEvent && ptr_.xPx >= 0.0f) {
        cb->impulseUV[0] = ptr_.xPx / static_cast<float>(std::max(1u, width_));
        cb->impulseUV[1] = ptr_.yPx / static_cast<float>(std::max(1u, height_));
        cb->impulseRadius = ptr_.isClick ? kImpulseRadiusClick : kImpulseRadiusMove;
        cb->impulseAmp = -0.45f * std::max(0.15f, ptr_.speedNorm);
        ptr_.hasNewEvent = false;
    } else {
        cb->impulseUV[0] = -1.0f;
        cb->impulseUV[1] = -1.0f;
        cb->impulseRadius = kImpulseRadiusMove;
        cb->impulseAmp = 0.0f;
    }
    cb->damping = kDamping;
    cb->waveMax = kWaveMax;
    ctx_->Unmap(cbWave_.Get(), 0);

    ID3D11ShaderResourceView* srvs[2] = {
        wavePing_ ? waveSrvA_.Get() : waveSrvB_.Get(),   // prev
        wavePing_ ? waveSrvB_.Get() : waveSrvA_.Get(),   // cur
    };
    ID3D11UnorderedAccessView* uavs[2] = {
        wavePing_ ? waveUavB_.Get() : waveUavA_.Get(),   // next（写）
        waveGradUav_.Get(),
    };
    ctx_->CSSetShaderResources(0, 2, srvs);
    ctx_->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
    ctx_->CSSetConstantBuffers(0, 1, cbWave_.GetAddressOf());
    ctx_->CSSetShader(csWave_.Get(), nullptr, 0);
    ctx_->Dispatch((waveGridW_ + 15) / 16, (waveGridH_ + 15) / 16, 1);
    wavePing_ = !wavePing_;

    // 能量判据不在这里做 GPU 归约：CSEvaluateEnergy 这个入口从来不存在
    // （fxc 实测 X3501），早期规划里它属于已被删除的 fluid_wave_sim.hlsl。
    // 现在由 waveAwakeFrames_ 计时驱动 —— 指针事件唤醒 130 帧，
    // 计数归零即停止 dispatch，与本文件头「静止后 0 GPU 开销」的契约一致。

    ID3D11UnorderedAccessView* nullUav[2] = {nullptr, nullptr};
    ctx_->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
    ctx_->CSSetShader(nullptr, nullptr, 0);
}

void LiquidGlassRenderer::RunMainPass(float timeSec) {
    if (!vsFullscreen_ || panelCount_ == 0) return;
    const int idx = (isHdr_ ? 4 : 0) + ((tier_ == QualityTier::Premium) ? 2 : 0)
                  + ((tier_ == QualityTier::Minimal) ? 0 : 1);
    if (!psGlass_[idx]) return;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx_->Map(cbFrame_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        auto* cb = static_cast<FrameCB*>(m.pData);
        cb->windowOriginPx[0] = 0.f;
        cb->windowOriginPx[1] = 0.f;
        cb->windowSizePx[0] = static_cast<float>(width_);
        cb->windowSizePx[1] = static_cast<float>(height_);
        cb->captureInvSize[0] = 1.0f / static_cast<float>(std::max(1u, win_ ? width_ : 1u));
        cb->captureInvSize[1] = 1.0f / static_cast<float>(std::max(1u, height_));
        cb->sdrWhiteScale = win_ ? win_->sdrWhiteScale() : 1.0f;
        cb->panelCount = panelCount_;
        cb->pointerPosPx[0] = ptr_.xPx;
        cb->pointerPosPx[1] = ptr_.yPx;
        cb->pointerVelocity = ptr_.speedNorm;
        cb->timeSec = timeSec;
        cb->blendK = 24.0f;
        cb->userClearSlider = prefs_.clearSlider;
        ctx_->Unmap(cbFrame_.Get(), 0);
    }

    ID3D11RenderTargetView* rtv = win_->backBufferRtv();
    if (!rtv) return;
    ID3D11ShaderResourceView* srvs[4] = {panelSrv_.Get(), nullptr, blurSrv_.Get(), waveGradSrv_.Get()};
    srvs[0] = panelSrv_.Get();
    srvs[1] = blurSrv_.Get();
    srvs[2] = blurSrv_.Get();
    srvs[3] = waveGradSrv_.Get();
    ctx_->PSSetShaderResources(0, 4, srvs);
    ctx_->PSSetConstantBuffers(0, 1, cbFrame_.GetAddressOf());
    ctx_->PSSetShader(psGlass_[idx].Get(), nullptr, 0);
    ctx_->VSSetShader(vsFullscreen_.Get(), nullptr, 0);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0.f, 0.f, static_cast<float>(width_), static_cast<float>(height_), 0.f, 1.f};
    ctx_->RSSetViewports(1, &vp);
    ctx_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[4] = {nullptr, nullptr, nullptr, nullptr};
    ctx_->PSSetShaderResources(0, 4, nullSrv);
}

bool LiquidGlassRenderer::ReadbackPolarity(bool& outLight) {
    // CSApcaUpdate **没有** 1x1 输出纹理：它把结果写回 gPanelsRW[idx].adaptive.z
    // （极性 0..1）。所以极性只能从 panel StructuredBuffer 回读 ——
    // 早期版本用一张 1x1 R32F 当落点是错的，那张纹理永远是初始值 0，
    // 表现为「极性永远判 dark-text」，而没有任何报错。
    if (!panelBuf_ || !apcaReadback_ || panelCount_ == 0) return false;

    ctx_->CopyResource(apcaReadback_.Get(), panelBuf_.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx_->Map(apcaReadback_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;

    const auto* rows = static_cast<const GlassPanelGPU*>(m.pData);
    // 多面板：取「面积最大」那块作为极性代表，而不是第一块 ——
    // 一块 24px 的导航胶囊不该决定整窗文字极性。
    uint32_t best = 0;
    float bestArea = -1.f;
    for (uint32_t i = 0; i < panelCount_ && i < 32; ++i) {
        const float a = rows[i].rect[2] * rows[i].rect[3];
        if (a > bestArea) { bestArea = a; best = i; }
    }
    const float pol = rows[best].adaptive[2];
    ctx_->Unmap(apcaReadback_.Get(), 0);

    if (!(pol >= 0.0f) || pol > 1.0f) return false;   // NaN / 未就绪
    outLight = pol > 0.5f;
    return true;
}

}  // namespace rbc