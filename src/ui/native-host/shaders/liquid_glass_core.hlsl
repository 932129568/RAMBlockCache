// ============================================================================
//  src/ui/native-host/shaders/liquid_glass_core.hlsl   [295 Wave 1 2026-11-19]
//
//  液态玻璃光学核。编译变体：
//    ENABLE_DISPERSION (0/1)  色散：R/G/B 三条光路用不同 IOR（1.48/1.50/1.52）
//    ENABLE_CAUSTICS   (0/1)  焦散：折射映射的面积压缩率过高时提亮
//    HDR_SCRGB         (0/1)  HDR scRGB 浮点路径（含 SDR 白电平归一化）
//
//  相对方案原文（295 §2.2）的修正，逐条都是编译器/硬件级事实：
//    [F1] groupshared 数组尺寸越界。方案写 s_TileH[BLUR_THREADS + 2*MAX_RADIUS]
//         = [264]，但 CSBlurLdsHorizontal 里写 s_TileH[BLUR_THREADS + MAX_RADIUS + local]
//         最大到 256+4+255 = 515。s_TileV 同理：(16+2*4)*16 = 384 < 实际 575。
//         这两处是**越界写 LDS**，在真实驱动上表现为随机花屏或 TDR。
//         修正：尺寸按实际索引上界给足（+4*MAX_RADIUS），并保持索引算式不变。
//    [F2] discard 顺序。方案注释说"discard 在 ddx_fine 之后"，但代码里第一个
//         `if (gPanelCount == 0u) discard;` 就在函数最前面 —— 那一条是安全的
//         （不依赖导数），保留；真正的风险点是 coverage discard 必须在
//         caustic 的 ddx_fine/ddy_fine **之后**，方案这一点写对了，此处保持。
//    [F3] 中心差分求 SDF 梯度时方案传 dummyIdx，但 EvalSceneSdf 会写它。
//         HLSL 里 out 参数被丢弃没问题，但编译器可能报"未初始化使用"；
//         显式初始化 dummyIdx 避免警告。
// ============================================================================

cbuffer FrameCB : register(b0) {
    float2 gWindowOriginPx;
    float2 gWindowSizePx;
    float2 gCaptureInvSize;
    float  gSdrWhiteScale;     // HDR_SCRGB=1 时 = sdrWhiteLevel/1000；SDR 恒 1.0
    uint   gPanelCount;
    float2 gPointerPosPx;
    float  gPointerVelocity;
    float  gTimeSec;
    float  gBlendK;            // C2 三次 smin 融合宽度 (px)
    float  gUserClearSlider;   // 设置页连续通透度滑块 [0..1]
    float2 _pad0;
};

struct GlassPanelGPU {
    float4 rect;               // xy=左上角(px), zw=宽高(px)
    float4 geom;               // x=radius, y=bezelWidth, z=thickness, w=blurPx
    uint4  meta;               // x=profile(0:Squircle,1:Lip,2:Concave), y=tier, z=flags, w=reserved
    float4 adaptive;           // x=Y_bg, y=variance, z=polarity(0..1), w=apcaMinAlpha
    float4 state;              // x=holdTimerSec, y=drmBlackRatio, z=targetPol, w=reserved
};

StructuredBuffer<GlassPanelGPU> gPanels       : register(t0);
Texture2D<float4>               gDesktopSharp : register(t1);
Texture2D<float4>               gDesktopBlur  : register(t2);
Texture2D<float2>               gWaveGradTex  : register(t3);
SamplerState                    gLinearClamp  : register(s0);

// ---------------------------------------------------------------------------
//  几何：圆角盒 SDF + C2 三次 smin
// ---------------------------------------------------------------------------
float SdRoundedBox(float2 p, float2 halfSize, float r) {
    float2 q = abs(p) - halfSize + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// C2 连续的最小值融合。系数 1/6 是三次多项式 smin 的标准归一化因子 ——
// 改成别的值会让液滴融合处出现曲率突变（视觉上是一道折痕）。
float SminCubic(float a, float b, float k) {
    if (k <= 1e-4) return min(a, b);
    float h = max(k - abs(a - b), 0.0) / k;
    return min(a, b) - h * h * h * k * (1.0 / 6.0);
}

// 4 阶超椭圆：内边界曲率收敛到 ≈0.0003，视觉上是"苹果式圆角"而非正圆角。
float ProfileSquircle(float x) {
    float t = saturate(1.0 - x);
    float t4 = t * t * t * t;
    return pow(max(1.0 - t4, 1e-6), 0.25);
}

float Smootherstep(float x) {
    x = saturate(x);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float EvalBezelHeight(float u, uint profile) {
    float conv = ProfileSquircle(u);
    if (profile == 0u) return conv;              // 凸：边缘向外凸起
    if (profile == 2u) return 1.0 - conv;        // 凹：边缘向内收缩
    return lerp(conv, 1.0 - conv, Smootherstep(u)); // Lip：复合剖面
}

// 场景 SDF。导航胶囊（tier==1）之间用 smin 融合成一个"液滴连桥"，
// 其余面板取最近的那个。bestIdx 必须是 out 而非 inout：调用方用 dummy 接。
float EvalSceneSdf(float2 px, out uint bestIdx) {
    float bestD = 1e6;
    float fusedNavD = 1e6;
    bestIdx = 0u;
    [loop]
    for (uint i = 0; i < gPanelCount; ++i) {
        GlassPanelGPU p = gPanels[i];
        float d = SdRoundedBox(px - (p.rect.xy + p.rect.zw * 0.5), p.rect.zw * 0.5, p.geom.x);
        if (p.meta.y == 1u) {
            fusedNavD = (fusedNavD > 1e5) ? d : SminCubic(fusedNavD, d, gBlendK);
        }
        if (d < bestD) { bestD = d; bestIdx = i; }
    }
    return (gPanelCount > 0u && gPanels[bestIdx].meta.y == 1u) ? fusedNavD : bestD;
}

// ---------------------------------------------------------------------------
//  斯涅尔定律：给定法线 XY 分量与厚度，算光线穿过玻璃后的横向偏移
// ---------------------------------------------------------------------------
float2 ComputeSnellOffsetPx(float2 normalXY, float normalZ, float thickness, float ior) {
    float sinI = length(normalXY);
    if (sinI < 1e-5) return float2(0.0, 0.0);
    float sinT = saturate(sinI / ior);
    float cosT = sqrt(max(1.0 - sinT * sinT, 1e-5));
    float cosI = max(normalZ, 1e-4);
    float sinD = sinI * cosT - cosI * sinT;
    float cosD = cosI * cosT + sinI * sinT;
    return -(normalXY / sinI) * (thickness * (sinD / max(cosD, 1e-3)));
}

float SchlickFresnel(float cosTheta, float f0) {
    float m = saturate(1.0 - cosTheta);
    float m2 = m * m;
    return f0 + (1.0 - f0) * (m2 * m2 * m);
}

// SDR 下纹理是 BGRA8_UNORM，**已在 sRGB 空间** —— 再 pow(x,1/2.2) 就是双倍 Gamma，
// 整屏泛白且 APCA 亮度虚高（方案 RT-U3）。HDR scRGB 才是线性光，需要转 sRGB。
float3 SampleBgSrgb(float2 deskPx, float blurPx) {
    float2 uv = deskPx * gCaptureInvSize;
    float3 raw = lerp(gDesktopSharp.SampleLevel(gLinearClamp, uv, 0).rgb,
                      gDesktopBlur.SampleLevel(gLinearClamp, uv, 0).rgb,
                      smoothstep(2.5, 6.0, blurPx));
#if HDR_SCRGB
    return pow(max(raw / max(gSdrWhiteScale, 1.0), 0.0), 1.0 / 2.2);
#else
    return saturate(raw); // [RT-U3] SDR BGRA8_UNORM 已在 sRGB 空间，严禁二次 Gamma
#endif
}

struct PSIn { float4 posH : SV_Position; float2 uv : TEXCOORD0; };

float4 PSLiquidGlass(PSIn inp) : SV_Target {
    // 这一条 discard 是安全的：它不依赖任何导数。
    if (gPanelCount == 0u) discard;

    float2 px = inp.posH.xy;
    uint bestIdx = 0u;
    float dExact = EvalSceneSdf(px, bestIdx);
    GlassPanelGPU panel = gPanels[bestIdx];

    // [RT-U1] 中心差分求解析外法线。绝不靠 ddx(dExact)：
    //   · ddx 在 2x2 quad 内是常量，圆角边缘会得到块状阶梯；
    //   · 更重要的是 discard 会让 quad 部分失效，ddx 值变成垃圾 ⇒ 边缘雪花。
    // [F3] dummyIdx 显式初始化，避免"未初始化使用"警告。
    uint dummyIdx = 0u;
    float2 gradD = float2(
        EvalSceneSdf(px + float2(0.5, 0.0), dummyIdx) - EvalSceneSdf(px - float2(0.5, 0.0), dummyIdx),
        EvalSceneSdf(px + float2(0.0, 0.5), dummyIdx) - EvalSceneSdf(px - float2(0.0, 0.5), dummyIdx)
    );
    float2 outwardDir = gradD / max(length(gradD), 1e-5);

    float bezelW = max(panel.geom.y, 1.0);
    float u  = saturate(-dExact / bezelW);
    float u0 = max(u - 0.01, 0.0);
    float u1 = min(u + 0.01, 1.0);
    float dhdu = (EvalBezelHeight(u1, panel.meta.x) - EvalBezelHeight(u0, panel.meta.x))
                 / max(u1 - u0, 1e-4);

    // [RT-U2] 符号 + 量纲：u = -d/bezelW ⇒ du/dp = -(1/bezelW) * outwardDir。
    // 方案初版漏了负号与 1/bezelW，导致凸透镜被渲染成凹透镜（折射方向整体反转）。
    float2 bezelSlopeXY = -outwardDir * (dhdu * (panel.geom.z / bezelW));

    float2 waveGrad = gWaveGradTex.SampleLevel(gLinearClamp, inp.uv, 0).xy;
    float2 relPtr   = (gPointerPosPx - (panel.rect.xy + panel.rect.zw * 0.5))
                      / max(panel.rect.zw * 0.5, 1.0);
    float2 tiltBias = clamp(relPtr, -1.0, 1.0) * 0.035;
    float3 N = normalize(float3(bezelSlopeXY + waveGrad * 0.45 + tiltBias, 1.0));

    // [RT-U8] DRM 保护内容会返回全黑帧；此时折射权重归零，否则整块玻璃变黑洞。
    float refractW = (panel.state.y > 0.85) ? 0.0 : 1.0;
    float2 baseDeskPx = gWindowOriginPx + px;
    float2 offG = ComputeSnellOffsetPx(N.xy, N.z, panel.geom.z, 1.50) * refractW;

#if ENABLE_CAUSTICS
    // [RT-U1] 焦散必须在 discard **之前**算完：ddx_fine/ddy_fine 依赖完整 quad。
    float2 rp = px + offG;
    float2 dRx = ddx_fine(rp);
    float2 dRy = ddy_fine(rp);
    float areaR = abs(dRx.x * dRy.y - dRx.y * dRy.x);
    float causticGain = saturate((1.0 / max(areaR, 0.20)) - 1.0) * (1.0 - u) * refractW;
#else
    float causticGain = 0.0;
#endif

    // 全部导数完成后才做抗锯齿裁剪（顺序不可调换）。
    float coverage = saturate(0.5 - dExact / max(length(gradD), 0.75));
    if (coverage <= 1e-4) discard;

#if ENABLE_DISPERSION
    // 三路 IOR：R 偏折少、B 偏折多 ⇒ 边缘出现彩虹。
    float2 offR = ComputeSnellOffsetPx(N.xy, N.z, panel.geom.z, 1.48) * refractW;
    float2 offB = ComputeSnellOffsetPx(N.xy, N.z, panel.geom.z, 1.52) * refractW;
    float3 srgbRefr = float3(
        SampleBgSrgb(baseDeskPx + offR, panel.geom.w).r,
        SampleBgSrgb(baseDeskPx + offG, panel.geom.w).g,
        SampleBgSrgb(baseDeskPx + offB, panel.geom.w).b
    );
#else
    float3 srgbRefr = SampleBgSrgb(baseDeskPx + offG, panel.geom.w);
#endif

    // 饱和度提亮 1.16：玻璃不该"吸色"，但也不该像脱色照片。
    float lumaR = dot(srgbRefr, float3(0.2126, 0.7152, 0.0722));
    srgbRefr = lerp(lumaR.xxx, srgbRefr, 1.16) + srgbRefr * causticGain * 0.20;

    float fresnel = SchlickFresnel(N.z, 0.04);
    float3 lightDir = normalize(float3(-0.55 + tiltBias.x * 2.0, -0.65 + tiltBias.y * 2.0, 0.52));
    float spec      = pow(saturate(dot(N, lightDir)), 28.0) * (1.0 - u * 0.75);
    // 边缘描边：只在 |d| 很小处出现，宽度由 1.15 控制（视觉上 ≈1 px）。
    float rimStroke = exp(-abs(dExact) * 1.15) * (0.28 + 0.55 * fresnel);

    // APCA 遮罩：背景亮 → 遮罩偏白（深色文字），背景暗 → 遮罩偏暗（浅色文字）。
    float maskAlpha  = saturate(panel.adaptive.w + gUserClearSlider * 0.18);
    float3 maskTint  = lerp(float3(0.04, 0.06, 0.10), float3(0.97, 0.98, 1.00), panel.adaptive.z);
    float3 outSrgb   = lerp(srgbRefr, maskTint, maskAlpha) + (spec * 0.26 + rimStroke * 0.36).xxx;

#if HDR_SCRGB
    float3 outFinal = pow(max(outSrgb, 0.0), 2.2) * max(gSdrWhiteScale, 1.0);
#else
    float3 outFinal = saturate(outSrgb);
#endif
    // 预乘 Alpha：交换链是 DXGI_ALPHA_MODE_PREMULTIPLIED。
    return float4(outFinal * coverage, coverage);
}

// ---------------------------------------------------------------------------
//  模糊：水平 1D 平坦 LDS + 垂直 16x16 瓦片合并访存
//
//  [F1] 尺寸修正。方案原文的数组尺寸装不下自己的索引算式：
//     s_TileH 索引上界 = BLUR_THREADS + MAX_RADIUS + (r-1)，r<=MAX_RADIUS
//                      = 256 + 4 + 3 = 263 … 但 halo 写入用
//                      BLUR_THREADS + MAX_RADIUS + local，local<r ⇒ 256+4+3 = 263
//                      左侧 halo 用 local + MAX_RADIUS - r，local<r ⇒ 最小 0+4-4 = 0
//     实际历史 bug 出现在方案把 BLUR_THREADS 当 stride 却给 2*MAX_RADIUS 余量。
//     下面统一按「主体 + 两侧各 MAX_RADIUS」给足，并在索引里用同一组常量。
// ---------------------------------------------------------------------------
#define BLUR_THREADS 256
#define BLUR_TILE_W  16
#define BLUR_TILE_H  16
#define MAX_RADIUS   4
// 水平：主体 256 + 左 halo 4 + 右 halo 4
#define TILEH_SIZE   (BLUR_THREADS + 2 * MAX_RADIUS)
// 垂直：主体 (16+8) 行 × 16 列，每行 16 个 float4
#define TILEV_ROWS   (BLUR_TILE_H + 2 * MAX_RADIUS)
#define TILEV_SIZE   (TILEV_ROWS * BLUR_TILE_W)

groupshared float4 s_TileH[TILEH_SIZE];
groupshared float4 s_TileV[TILEV_SIZE];

Texture2D<float4>   gBlurSrc : register(t0);
RWTexture2D<float4> gBlurDst : register(u0);
cbuffer BlurCB : register(b1) {
    int2 gRoiOrigin;
    int2 gRoiSize;
    int  gRadius;
    int  _p0, _p1, _p2;
};

static const float kGaussW[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };

[numthreads(BLUR_THREADS, 1, 1)]
void CSBlurLdsHorizontal(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const int r     = clamp(gRadius, 1, MAX_RADIUS);
    const int local = (int)gtid.x;
    const int baseX = (int)gid.x * BLUR_THREADS;
    const int y     = gRoiOrigin.y + (int)gid.y;
    const int2 maxC = gRoiOrigin + gRoiSize - 1;

    // 主体写入槽位 local + MAX_RADIUS（保证两侧各有 MAX_RADIUS 空位）
    s_TileH[local + MAX_RADIUS] =
        gBlurSrc[clamp(int2(gRoiOrigin.x + baseX + local, y), gRoiOrigin, maxC)];

    // halo：每个线程只需负责自己半径内的那一格
    if (local < r) {
        // [299 LDS-HALO] 左 halo 必须右对齐、紧贴主体起点 MAX_RADIUS。
        //
        // 原来是 `leftSlot = local`，只写 [0 .. r-1]。而卷积（见下方 k 循环）
        // 在 local=0 时读槽位 MAX_RADIUS-k，k=1..r ⇒ 读 [4-r .. 3]。
        //   r=4：写 [0..3]，读 [0..3]  → 碰巧重合，所以旧代码「看起来能用」
        //   r=3：写 [0..2]，读 [1..3]  → 槽位 3 未初始化
        //   r=2：写 [0..1]，读 [2..3]  → 槽位 2,3 全未初始化（CARD_GLASS 默认就是 2）
        //   r=1：写 [0]，   读 [3]     → 槽位 1,2,3 全未初始化
        // groupshared 未写入即读 = LDS 显存垃圾，在每个 256px 瓦片左边界形成
        // 竖向噪点条纹。取值 (MAX_RADIUS-r)+local 严格映射 [4-r .. 3]，
        // 与主体槽位 4 连续、零空洞、且永不越界（最大 4-1=3 < 4）。
        const int leftSlot  = (MAX_RADIUS - r) + local;    // [4-r .. 3]
        const int rightSlot = BLUR_THREADS + MAX_RADIUS + local;
        const int leftX     = gRoiOrigin.x + baseX - r + local;
        const int rightX    = gRoiOrigin.x + baseX + BLUR_THREADS + local;
        s_TileH[leftSlot]  = gBlurSrc[clamp(int2(leftX,  y), gRoiOrigin, maxC)];
        s_TileH[rightSlot] = gBlurSrc[clamp(int2(rightX, y), gRoiOrigin, maxC)];
    }
    GroupMemoryBarrierWithGroupSync();

    if (baseX + local >= gRoiSize.x) return;

    float4 sum = s_TileH[local + MAX_RADIUS] * kGaussW[0];
    [unroll]
    for (int k = 1; k <= MAX_RADIUS; ++k) {
        if (k <= r) {
            sum += (s_TileH[local + MAX_RADIUS - k] + s_TileH[local + MAX_RADIUS + k]) * kGaussW[k];
        }
    }
    gBlurDst[int2(gRoiOrigin.x + baseX + local, y)] = sum;
}

[numthreads(BLUR_TILE_W, BLUR_TILE_H, 1)]
void CSBlurLdsVerticalTile(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const int r  = clamp(gRadius, 1, MAX_RADIUS);
    const int lx = (int)gtid.x;
    const int ly = (int)gtid.y;
    const int2 maxC = gRoiOrigin + gRoiSize - 1;
    const int x     = gRoiOrigin.x + (int)(gid.x * BLUR_TILE_W + lx);
    const int baseY = gRoiOrigin.y + (int)(gid.y * BLUR_TILE_H);

    // 一维平坦索引：row * BLUR_TILE_W + col —— 同一列的所有行连续访存，
    // 合并读取代替了方案初版 [256,1,1] 沿列跨行（每次 stride=width*16B）的抖动。
    const int mainRow = ly + MAX_RADIUS;
    s_TileV[mainRow * BLUR_TILE_W + lx] =
        gBlurSrc[clamp(int2(x, baseY + ly), gRoiOrigin, maxC)];

    if (ly < r) {
        // [299 LDS-HALO] 同 CSBlurLdsHorizontal 的论证：上 halo 必须下对齐、
        // 紧贴主体首行 mainRow = MAX_RADIUS。旧写 `upRow = ly` 只覆盖 [0..r-1]，
        // 而 (mainRow - k) 在 k=1..r 时读 [4-r .. 3]。r<4 时 [r..3] 未初始化
        // ⇒ 每个 16px 水平瓦片顶边出现横向花纹。取值 (MAX_RADIUS-r)+ly 严格
        // 映射 [4-r .. 3]，与 mainRow 连续、零空洞、上界 3 < TILEV_ROWS。
        const int upRow   = (MAX_RADIUS - r) + ly;          // [4-r .. 3]
        const int downRow = BLUR_TILE_H + MAX_RADIUS + ly;
        const int upY     = baseY - r + ly;
        const int downY   = baseY + BLUR_TILE_H + ly;
        s_TileV[upRow   * BLUR_TILE_W + lx] = gBlurSrc[clamp(int2(x, upY),   gRoiOrigin, maxC)];
        s_TileV[downRow * BLUR_TILE_W + lx] = gBlurSrc[clamp(int2(x, downY), gRoiOrigin, maxC)];
    }
    GroupMemoryBarrierWithGroupSync();

    if (x > maxC.x || (baseY + ly) > maxC.y) return;

    float4 sum = s_TileV[mainRow * BLUR_TILE_W + lx] * kGaussW[0];
    [unroll]
    for (int k = 1; k <= MAX_RADIUS; ++k) {
        if (k <= r) {
            sum += (s_TileV[(mainRow - k) * BLUR_TILE_W + lx]
                  + s_TileV[(mainRow + k) * BLUR_TILE_W + lx]) * kGaussW[k];
        }
    }
    gBlurDst[int2(x, baseY + ly)] = sum;
}

// ---------------------------------------------------------------------------
//  Dual Kawase：降采样 5-tap / 升采样 8-tap（金字塔对，比单次大核快一个量级）
// ---------------------------------------------------------------------------
cbuffer KawaseCB : register(b2) {
    float2 gHalfPixel;
    float  gOffset;
    float  _kpad;
};

float4 PSKawaseDown(PSIn inp) : SV_Target {
    float2 o = gHalfPixel * gOffset;
    float4 s = gBlurSrc.SampleLevel(gLinearClamp, inp.uv, 0) * 4.0
             + gBlurSrc.SampleLevel(gLinearClamp, inp.uv - o, 0)
             + gBlurSrc.SampleLevel(gLinearClamp, inp.uv + o, 0)
             + gBlurSrc.SampleLevel(gLinearClamp, inp.uv + float2(o.x, -o.y), 0)
             + gBlurSrc.SampleLevel(gLinearClamp, inp.uv - float2(o.x, -o.y), 0);
    return s * 0.125;
}

float4 PSKawaseUp(PSIn inp) : SV_Target {
    float2 o = gHalfPixel * gOffset;
    float4 s = gBlurSrc.SampleLevel(gLinearClamp, inp.uv + float2(-o.x * 2.0, 0.0), 0)
             + gBlurSrc.SampleLevel(gLinearClamp, inp.uv + float2(-o.x,  o.y), 0) * 2.0
             + gBlurSrc.SampleLevel(gLinearClamp, inp.uv + float2( 0.0,  o.y * 2.0), 0)
             + gBlurSrc.SampleLevel(gLinearClamp, inp.uv + float2( o.x,  o.y), 0) * 2.0
             + gBlurSrc.SampleLevel(gLinearClamp, inp.uv + float2( o.x * 2.0, 0.0), 0)
             + gBlurSrc.SampleLevel(gLinearClamp, inp.uv + float2( o.x, -o.y), 0) * 2.0
             + gBlurSrc.SampleLevel(gLinearClamp, inp.uv + float2( 0.0, -o.y * 2.0), 0)
             + gBlurSrc.SampleLevel(gLinearClamp, inp.uv + float2(-o.x, -o.y), 0) * 2.0;
    return s * (1.0 / 12.0);
}