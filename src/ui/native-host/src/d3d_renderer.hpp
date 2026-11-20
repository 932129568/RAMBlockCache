#pragma once
// ============================================================================
//  src/ui/native-host/src/d3d_renderer.hpp   [299 新建]
//
//  四阶段零拷贝 D3D11 渲染管线。这一层补的是 295 剩下的「编译散件 → 可执行」缺口：
//  glass_window.cpp 已经有了交换链、DComp 双 Visual、WebView2 composition controller，
//  desktop_capture.cpp 已经有了 DuplicateOutput1 捕获，两个着色器也已经能过 fxc；
//  但**没有任何代码把它们串起来**，所以 295 的状态是「分开都能编，合起来不成形」。
//
//  每帧四个 GPU Pass（全部驻留 VRAM，零 CPU 像素回读）：
//    Pass 1  ROI 混合模糊：r<=3 走 CSBlurLds{Horizontal,VerticalTile}（LDS 高斯）；
//            r>3 走 PSKawase{Down,Up} 金字塔。同时产出 1/4 与 1/16 顶层给 APCA。
//    Pass 2  APCA 归约：CSApcaUpdate 在 1/16 顶层上做 8x8 采样，更新 gPanelsRW 的
//            adaptive/state（含 120ms 滞回）。仅在极性真正翻转时回读 80 B 通知前端。
//    Pass 3  GPU 水波：仅当 waveAwakeFrames_ > 0 时 CSWaveStep 并轮换 Ping-Pong；
//            归静后不 dispatch，0 GPU 开销。
//    Pass 4  全屏三角形 PSLiquidGlass：以 3 顶点 SV_VertexID 绘制到 DComp 预乘
//            Alpha 后缓冲。
//
//  「失败就退回 292/293 已验收路径」是本文件的一等契约：任何一步资源创建失败都
//  返回 false 并让 main.cpp 走纯色降级，**不允许**把窗口画黑。
// ============================================================================

#include "glass_window.hpp"
#include "desktop_capture.h"

#include <cstdint>
#include <vector>

namespace rbc {

// 画质档：决定模糊路径与金字塔层数。
enum class QualityTier : uint32_t {
    Premium  = 0,   // 全特效（HDR 下 2 级 Kawase + 色散 + 焦散）
    Standard = 1,   // 关色散
    Minimal  = 2,   // 只用 1 级模糊，关焦散（电池 / 核显）
};

// 指针脉冲。由页面 rbc-glass-pointer 消息填充。
struct PointerImpulse {
    float xPx        = -1.0f;
    float yPx        = -1.0f;
    float speedNorm  = 0.0f;
    bool  isClick    = false;
    bool  hasNewEvent = false;
};

// 页面 rbc-glass-prefs 消息带来的外观偏好。
struct GlassPrefs {
    float clearSlider = 0.0f;   // [0..1] 连续通透度
    bool  reducedMotion = false;
};

class LiquidGlassRenderer {
public:
    LiquidGlassRenderer() = default;
    ~LiquidGlassRenderer();

    LiquidGlassRenderer(const LiquidGlassRenderer&) = delete;
    LiquidGlassRenderer& operator=(const LiquidGlassRenderer&) = delete;

    // 必须在 LiquidGlassWindow::Create 成功之后调用（需要它的 d3dDevice / swapChain）。
    // csoDir 是 .cso 预编译着色器所在目录；为空则退回运行期 fxc 不可用时的 D3DCompile
    // —— 但本项目**只允许**从 .cso 加载（见 Build-Shaders 说明：首帧编译会卡顿 200ms+）。
    bool Initialize(LiquidGlassWindow* win, const wchar_t* csoDir);

    void Shutdown();
    void OnResize(uint32_t width, uint32_t height, bool isHdr);

    void UpdatePanels(const std::vector<GlassPanelGPU>& panels);
    void PushPointer(const PointerImpulse& ptr);
    void SetPrefs(const GlassPrefs& prefs) { prefs_ = prefs; }
    void SetQualityTier(QualityTier t) { tier_ = t; }

    // 单帧。返回 true = 本次真的提交了新画面；false = 静态休眠跳过 Present。
    // outPolarityChanged / outLightText 仅在 APCA 判定极性翻转时置位（由调用方
    // 回发 rbc-apca-polarity 给页面翻字色）。
    bool RenderFrame(DesktopCapture& capture, float dtSec, float timeSec,
                     bool& outPolarityChanged, bool& outLightText);

    // 是否已经处在「静态休眠」——main.cpp 用它决定要不要 Sleep(100)。
    bool isSuspended() const { return suspended_; }

private:
    bool LoadShaders(const wchar_t* csoDir);
    bool CreateSizeDependent(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    bool CreateConstantBuffers();

    void RunBlur(const RECT& roiPx, uint32_t blurPx);
    void RunPyramidDown(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst);
    void RunPyramidUp(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst);
    void RunApca(float dtSec);
    void RunWaveIfAwake();
    void RunMainPass(float timeSec);

    bool ReadbackPolarity(bool& outLight);

    LiquidGlassWindow* win_ = nullptr;
    ID3D11Device*        dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;

    QualityTier tier_ = QualityTier::Premium;
    GlassPrefs  prefs_{};

    // 8 个像素着色器变体：[HDR(0/1)] x [Dispersion(0/1)] x [Caustics(0/1)]
    // 索引 = hdr*4 + disp*2 + caustic
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  psGlass_[8];
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vsFullscreen_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  psKawaseDown_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>  psKawaseUp_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> csBlurH_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> csBlurV_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> csApca_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> csWave_;

    // 常量缓冲（必须按 16 B 对齐，与 HLSL cbuffer 逐字段一致）
    Microsoft::WRL::ComPtr<ID3D11Buffer> cbFrame_;    // FrameCB  b0 (liquid_glass_core)
    Microsoft::WRL::ComPtr<ID3D11Buffer> cbBlur_;     // BlurCB   b1
    Microsoft::WRL::ComPtr<ID3D11Buffer> cbKawase_;   // KawaseCB b2
    Microsoft::WRL::ComPtr<ID3D11Buffer> cbApca_;     // ApcaCB   b0 (apca_reduce)
    Microsoft::WRL::ComPtr<ID3D11Buffer> cbWave_;     // WaveCB   b0 (apca_reduce 水波段)

    // 面板 StructuredBuffer（GPU 侧）+ SRV，以及 APCA 需要的 UAV 版本
    Microsoft::WRL::ComPtr<ID3D11Buffer>             panelBuf_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> panelSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> panelUav_;
    std::vector<GlassPanelGPU> panels_;
    uint32_t panelCount_ = 0;
    bool     panelsDirty_ = true;

    // ROI 纹理与 RTV/SRV
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          blurTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> blurSrv_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   blurRtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          blurTexA_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> blurSrvA_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   blurRtvA_;

    // 金字塔（1/4 与 1/16），供 APCA 采样
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          pyrHalf_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pyrHalfSrv_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   pyrHalfRtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          pyrQuarter_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pyrQuarterSrv_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   pyrQuarterRtv_;

    // 水波 Ping-Pong（R32F 两张）+ 梯度（R16G16F）
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          waveA_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          waveB_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> waveSrvA_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> waveSrvB_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> waveUavA_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> waveUavB_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          waveGrad_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> waveGradSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> waveGradUav_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          energyTex_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> energyUav_;
    bool wavePing_ = true;
    uint32_t waveGridW_ = 0;
    uint32_t waveGridH_ = 0;
    uint32_t waveAwakeFrames_ = 0;   // >0 才 dispatch；归静后不跑，零功耗

    // APCA 极性回读（80 B，两帧一读，避免 Staging 争用）
    // StructuredBuffer 的 STAGING 拷贝（极性落点就是 gPanelsRW 本身）
    Microsoft::WRL::ComPtr<ID3D11Buffer>            apcaReadback_;
    bool lastPolarityLight_ = false;
    bool havePolarity_ = false;
    uint32_t apcaFrame_ = 0;

    // 帧状态
    PointerImpulse ptr_{};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool     isHdr_ = false;
    bool     hasCapture_ = false;
    bool     suspended_ = false;
    uint32_t idleFrames_ = 0;
    RECT     lastRoi_{};
    uint32_t lastBlurPx_ = 0;

    // 全屏三角形用空 input layout（顶点由 SV_VertexID 生成）
    Microsoft::WRL::ComPtr<ID3D11Buffer> dummyVb_;
};

}  // namespace rbc