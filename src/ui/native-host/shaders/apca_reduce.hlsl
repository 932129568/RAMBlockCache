// ============================================================================
//  src/ui/native-host/shaders/apca_reduce.hlsl   [295 Wave 1 2026-11-19]
//
//  APCA（Accessible Perceptual Contrast Algorithm, 0.0.98G-4g）自适应极性 +
//  120 ms 滞回。目的：玻璃背后的桌面可能是深色终端也可能是浅色文档，
//  正文文字颜色必须跟着翻，且不能跟着视频画面闪。
//
//  相对方案原文的修正：
//    [F1] 方案把 apca 的 cbuffer 与 liquid_glass_core.hlsl 的 FrameCB 都放
//         register(b0)，两个文件分开编译没问题，但合到同一个 effect 里会撞。
//         这里改成(b0)+独立文件，并在 Gate27 里禁止把两者合进同一个 .hlsl。
//    [F2] 方案用 4x4=16 个采样点做均值与方差。对 1080p 面板来说 16 点太少，
//         纯色文档会稳定、但带纹理的壁纸方差被低估 ⇒ 遮罩alpha 偏低 ⇒
//         文字在花纹处不可读。改为 8x8=64 点（仍是一次 dispatch 内完成）。
//    [F3] 120 ms 滞回的状态机原样保留（这是方案里少见写对的部分）：双阈值
//         [0.30, 0.43] + 持续时间累积，避免在阈值附近抖动。
// ============================================================================

Texture2D<float4> gPyramidTop  : register(t0);
SamplerState      gLinearClamp : register(s0);

struct GlassPanelGPU {
    float4 rect;
    float4 geom;
    uint4  meta;
    float4 adaptive;
    float4 state;
};
RWStructuredBuffer<GlassPanelGPU> gPanelsRW : register(u0);

cbuffer ApcaCB : register(b0) {
    float2 gWindowOriginPx;
    float2 gCaptureInvSize;
    float  gSdrWhiteScale;
    float  gDtSec;
    uint   gPanelCount;
    float  _p;
};

// APCA 的亮度模型：sRGB → 线性（2.4 而非 2.2），黑端额外加一个 1.414 指数补偿。
float ApcaLuminance(float3 srgb) {
    float3 p = pow(saturate(srgb), 2.4);
    float  y = 0.2126729 * p.r + 0.7151522 * p.g + 0.0721750 * p.b;
    return (y < 0.022) ? (y + pow(0.022 - y, 1.414)) : y;
}

#define APCA_GRID 8   // 8x8 = 64 采样点（[F2]）

[numthreads(32, 1, 1)]
void CSApcaUpdate(uint3 id : SV_DispatchThreadID) {
    const uint idx = id.x;
    if (idx >= gPanelCount) return;

    GlassPanelGPU p = gPanelsRW[idx];

    const float2 minUv = (gWindowOriginPx + p.rect.xy) * gCaptureInvSize;
    const float2 maxUv = (gWindowOriginPx + p.rect.xy + p.rect.zw) * gCaptureInvSize;

    float sumY = 0.0, sumY2 = 0.0, blackCount = 0.0;
    const float inv = 1.0 / float(APCA_GRID * APCA_GRID);

    [unroll]
    for (int gy = 0; gy < APCA_GRID; ++gy) {
        [unroll]
        for (int gx = 0; gx < APCA_GRID; ++gx) {
            const float2 t  = (float2(gx, gy) + 0.5) * (1.0 / float(APCA_GRID));
            const float2 uv = lerp(minUv, maxUv, t);
            const float3 c  = gPyramidTop.SampleLevel(gLinearClamp, uv, 0).rgb;

#if HDR_SCRGB
            const float3 srgb = pow(saturate(c / max(gSdrWhiteScale, 1.0)), 1.0 / 2.2);
#else
            const float3 srgb = saturate(c);   // [RT-U3] SDR 直通，严禁二次 Gamma
#endif
            // 零熵纯黑（DRM 保护内容）：统计占比，供液面着色器把折射权重归零。
            if (max(srgb.r, max(srgb.g, srgb.b)) < 0.004) blackCount += 1.0;

            const float y = ApcaLuminance(srgb);
            sumY  += y;
            sumY2 += y * y;
        }
    }

    const float meanY = sumY * inv;
    const float varY  = max(sumY2 * inv - meanY * meanY, 0.0);

    // [RT-U7/F3] 双阈值 + 120 ms 持续。单阈值会在视频画面亮度起伏时
    // 让文字颜色高频翻转 —— 那是无法阅读的，比对比度不足更糟。
    const float curTarget = p.state.z;
    float wantPol = curTarget;
    if (curTarget < 0.5 && meanY > 0.43)      wantPol = 1.0;
    else if (curTarget >= 0.5 && meanY < 0.30) wantPol = 0.0;

    float hold = (wantPol != curTarget) ? (p.state.x + gDtSec) : 0.0;
    if (hold >= 0.12) {
        p.state.z = wantPol;
        hold = 0.0;
    }

    // 极性平滑：不直接跳变，用 8/s 的指数趋近（约 125 ms 到 71%）。
    const float newPol = lerp(p.adaptive.z, p.state.z, 1.0 - exp(-gDtSec * 8.0));

    // 遮罩强度：背景越接近中灰（0.365），需要的遮罩越少（中灰对两种文字都不友好，
    // 实际上更该加遮罩，所以这里用 |meanY-0.365| 越大 → 遮罩越少的反向曲线，
    // 配合上方方差项补偿花纹背景）。
    const float baseNeed = lerp(0.25, 0.055, saturate(abs(meanY - 0.365) / 0.32));
    const float minAlpha = clamp(baseNeed + saturate(sqrt(varY) * 0.65) * 0.14, 0.04, 0.42);

    p.adaptive = float4(meanY, varY, newPol, minAlpha);
    p.state    = float4(hold, blackCount * inv, p.state.z, 0.0);
    gPanelsRW[idx] = p;
}

// ============================================================================
//  GPU Ping-Pong 浅水波。阻尼 0.962 + Neumann 边界（镜像反射）。
//
//  为什么用 GPU：CPU 版（liquidWaveCore.ts）是 160x90 Float32Array + Canvas 2D
//  putImageData，只能叠 2D 亮斑，弯不了底层桌面；而这里的高度梯度直接喂给
//  PSLiquidGlass 的法线，桌面图像会真的被"水"推动。
//
//  Neumann 边界：越界索引钳到边界（max/min），等价于镜像反射 —— 波撞边不消失。
// ============================================================================

Texture2D<float>    gHeightPrev : register(t0);
Texture2D<float>    gHeightCur  : register(t1);
RWTexture2D<float>  gHeightNext : register(u0);
RWTexture2D<float2> gWaveGrad   : register(u1);

cbuffer WaveCB : register(b0) {
    int2   gGridSize;
    float2 gImpulseUV;
    float  gImpulseRadius;
    float  gImpulseAmp;
    float  gDamping;
    float  gWaveMax;
};

[numthreads(16, 16, 1)]
void CSWaveStep(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (p.x >= gGridSize.x || p.y >= gGridSize.y) return;

    // Neumann：越界钳到边界
    const float hL = gHeightCur[int2(max(p.x - 1, 0), p.y)];
    const float hR = gHeightCur[int2(min(p.x + 1, gGridSize.x - 1), p.y)];
    const float hU = gHeightCur[int2(p.x, max(p.y - 1, 0))];
    const float hD = gHeightCur[int2(p.x, min(p.y + 1, gGridSize.y - 1))];

    float nextH = ((hL + hR + hU + hD) * 0.5 - gHeightPrev[p]) * gDamping;

    // 指针脉冲：cos 剖面 + 平方 → 中心最陡，边缘平滑归零（不会激起方波）。
    if (gImpulseUV.x >= 0.0 && abs(gImpulseAmp) > 1e-4) {
        const float2 gsize = float2(gGridSize);
        const float  dist  = length((float2(p) + 0.5) / gsize - gImpulseUV);
        if (dist < gImpulseRadius) {
            const float f = cos((dist / gImpulseRadius) * 1.5707963);
            nextH += gImpulseAmp * f * f;
        }
    }

    nextH = clamp(nextH, -gWaveMax, gWaveMax);
    if (abs(nextH) < 1e-4) nextH = 0.0;   // 归零死区：防止浮点噪声永远不会停
    gHeightNext[p] = nextH;

    // 梯度：中心差分。低于 0.018 视为平 -> 归零，避免法线被数值噪声推动。
    const float2 g = float2(hR - hL, hD - hU) * 0.5;
    gWaveGrad[p] = (length(g) < 0.018) ? float2(0.0, 0.0) : g;
}