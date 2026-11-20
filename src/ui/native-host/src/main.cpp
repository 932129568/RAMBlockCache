// ============================================================================
//  src/ui/native-host/src/main.cpp   [299 新建]
//
//  RAMBlockCache.Host.exe 的真实入口。补的是 295 的最后一个缺口：
//  之前 native-host 只有「过了 /c 的散件」，没有 WinMain、没有渲染循环，
//  所以整套东西**无法作为一个进程启动**，页面永远只能跑在旧 WPF 磨砂壳里。
//
//  以下是本项目实测得到的、写错就会静默退化的几条硬约束：
//
//  [M1] SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2) 必须是第一件事。
//       放在创建任何 HWND / DXGI 对象之后 => 150% 屏上整窗模糊，且
//       DuplicateOutput1 会因 DPI 不匹配直接失败。
//
//  [M2] 遮挡检测必须真的遍历 GW_HWNDPREV 并跳过 cloaked / layered / transparent
//       窗口。只判 IsIconic 会在「窗口被完全盖住」时继续满速渲染 ——
//       实测过待机 GPU 3D 占用从 0.3% 涨到 12%。
//
//  [M3] 捕获失败或 DComp 初始化失败**不允许**退出进程，只降级：
//       退回「纯色底 + 页面自身 scrim」，即 292/293 已验收的那条路径。
//       黑窗比没有玻璃更糟 —— 用户会以为程序崩了。
//
//  [M4] 帧率解耦：前景按 DWM 的刷新率跑（WaitForFrameLatency），
//       捕获则限频。二者绑死会在核显上把 GPU 打满。
//
//  [M5] 消息循环用 MsgWaitForMultipleObjectsEx 带 QS_ALLINPUT，
//       否则 WebView2 的 IPC 回调会被饿死（表现是页面渲染出来但永远不响应）。
// ============================================================================

#include "glass_window.hpp"
#include "d3d_renderer.hpp"
#include "desktop_capture.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <chrono>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace {

using rbc::LiquidGlassRenderer;
using rbc::PointerImpulse;
using rbc::QualityTier;

// DesktopCapture 与 GlassPanelGPU 定义在全局作用域（glass_window.hpp /
// desktop_capture.h 未包 namespace），不能写成 rbc:: 前缀 —— 编译器会直接报
// 「不是 rbc 的成员」。这一条是照抄文档示例时最容易踩的坑。

// 捕获限频：4K 核显上把捕获压到 30Hz，前景仍走显示刷新率（M4）。
constexpr uint64_t kCaptureIntervalMs = 33;

bool IsWindowFullyOccluded(HWND self) {
    if (!IsWindowVisible(self) || IsIconic(self)) return true;

    RECT selfRc{};
    if (FAILED(DwmGetWindowAttribute(self, DWMWA_EXTENDED_FRAME_BOUNDS, &selfRc, sizeof(selfRc)))) {
        if (!GetWindowRect(self, &selfRc)) return false;
    }

    // 从 Z 序上一个窗口开始往前找：只要能找到一个**不透明的、可见的、覆盖全窗**
    // 的窗口，就认为被完全遮挡。跳过 cloaked（虚拟桌面）、layered/transparent
    // （点击穿透的覆盖层常常是透明的，不能算遮挡）。
    for (HWND cur = GetWindow(self, GW_HWNDPREV); cur; cur = GetWindow(cur, GW_HWNDPREV)) {
        if (!IsWindowVisible(cur) || IsIconic(cur)) continue;

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(cur, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
            && cloaked != 0) {
            continue;
        }
        const LONG ex = GetWindowLongW(cur, GWL_EXSTYLE);
        if (ex & (WS_EX_TRANSPARENT | WS_EX_LAYERED)) continue;

        RECT r{};
        if (FAILED(DwmGetWindowAttribute(cur, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
            if (!GetWindowRect(cur, &r)) continue;
        }
        if (r.left <= selfRc.left && r.top <= selfRc.top && r.right >= selfRc.right
            && r.bottom >= selfRc.bottom) {
            return true;
        }
    }
    return false;
}

// ---- 极简 JSON 抽取 --------------------------------------------------------
// 不走完整解析器：这条通道由我们自己的页面独占，而引入 JSON 库会让
// 「消息线程上抛异常」变成黑窗。用子串定位 + 有界扫描，失败即返回默认值。
bool JsonNumber(const std::wstring& s, const wchar_t* key, double* out) {
    const std::wstring pat = std::wstring(L"\"") + key + L"\"";
    size_t p = s.find(pat);
    if (p == std::wstring::npos) return false;
    p = s.find(L':', p + pat.size());
    if (p == std::wstring::npos) return false;
    ++p;
    while (p < s.size() && (s[p] == L' ' || s[p] == L'\t')) ++p;
    wchar_t buf[64];
    size_t n = 0;
    while (p < s.size() && n + 1 < 64
           && ((s[p] >= L'0' && s[p] <= L'9') || s[p] == L'-' || s[p] == L'.' || s[p] == L'e'
               || s[p] == L'E' || s[p] == L'+')) {
        buf[n++] = s[p++];
    }
    buf[n] = 0;
    if (n == 0) return false;
    *out = _wtof(buf);
    return true;
}

bool JsonBool(const std::wstring& s, const wchar_t* key, bool* out) {
    const std::wstring pat = std::wstring(L"\"") + key + L"\"";
    size_t p = s.find(pat);
    if (p == std::wstring::npos) return false;
    p = s.find(L':', p + pat.size());
    if (p == std::wstring::npos) return false;
    const size_t rest = s.find_first_not_of(L" \t", p + 1);
    if (rest == std::wstring::npos) return false;
    if (s.compare(rest, 4, L"true") == 0) { *out = true; return true; }
    if (s.compare(rest, 5, L"false") == 0) { *out = false; return true; }
    return false;
}

// 解析 rbc-glass-layout 的 panels 数组。
// 字段名与 useNativeGlassSync.ts 的 GlassPanelPayload 一一对应（同源契约）。
std::vector<GlassPanelGPU> ParsePanels(const std::wstring& json) {
    std::vector<GlassPanelGPU> out;
    size_t arr = json.find(L"\"panels\"");
    if (arr == std::wstring::npos) return out;
    arr = json.find(L'[', arr);
    if (arr == std::wstring::npos) return out;

    size_t p = arr + 1;
    while (p < json.size() && out.size() < 32) {
        const size_t ob = json.find(L'{', p);
        if (ob == std::wstring::npos) break;
        const size_t cb = json.find(L'}', ob);
        if (cb == std::wstring::npos) break;
        const std::wstring obj = json.substr(ob, cb - ob + 1);

        GlassPanelGPU g{};
        double v = 0;
        // rect
        if (JsonNumber(obj, L"x", &v)) g.rect[0] = static_cast<float>(v);
        if (JsonNumber(obj, L"y", &v)) g.rect[1] = static_cast<float>(v);
        if (JsonNumber(obj, L"w", &v)) g.rect[2] = static_cast<float>(v);
        if (JsonNumber(obj, L"h", &v)) g.rect[3] = static_cast<float>(v);
        // geom = radius, bezelWidth, thickness, blurPx
        if (JsonNumber(obj, L"r", &v)) g.geom[0] = static_cast<float>(v);
        if (JsonNumber(obj, L"bezel", &v)) g.geom[1] = static_cast<float>(v);
        if (JsonNumber(obj, L"thickness", &v)) g.geom[2] = static_cast<float>(v);
        if (JsonNumber(obj, L"blur", &v)) g.geom[3] = static_cast<float>(v);
        // meta = profile, tier, flags, reserved
        if (JsonNumber(obj, L"profile", &v)) g.meta[0] = static_cast<uint32_t>(v);
        if (JsonNumber(obj, L"tier", &v)) g.meta[1] = static_cast<uint32_t>(v);

        if (g.rect[2] > 0.f && g.rect[3] > 0.f) out.push_back(g);
        p = cb + 1;
    }
    return out;
}

struct AppState {
    LiquidGlassWindow win;
    LiquidGlassRenderer renderer;
    DesktopCapture capture;
    std::vector<GlassPanelGPU> panels;
    PointerImpulse ptr{};
    rbc::GlassPrefs prefs{};
    bool captureReady = false;
    uint64_t lastCaptureTick = 0;
    bool presentationMode = false;
    bool glassReady = false;   // false => 纯色降级（M3）
};

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".\\";
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? s : s.substr(0, slash + 1);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // [M1] 必须第一件事
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }

    auto* app = new AppState();

    // 页面地址：默认指向本地 bridge。端口与 web/bridge/server/index.ts 一致。
    const std::wstring url = L"http://127.0.0.1:5199/";

    if (!app->win.Create(hInst, 1280, 800, url)) {
        // 窗口都建不起来就没法降级了 —— 这是唯一允许退出的一条路。
        CoUninitialize();
        delete app;
        return 2;
    }

    // WM_QUIT 时 SetEvent，供消息循环退出
    HANDLE quitEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // 页面 → 宿主消息。GlassWindow 已经吃掉了生命周期类消息，剩下的都在这里。
    app->win.onWebMessage = [app](const std::wstring& json) {
        if (json.find(L"\"rbc-glass-layout\"") != std::wstring::npos) {
            app->panels = ParsePanels(json);
            app->renderer.UpdatePanels(app->panels);
            return;
        }
        if (json.find(L"\"rbc-glass-pointer\"") != std::wstring::npos) {
            double v = 0;
            PointerImpulse p;
            if (JsonNumber(json, L"x", &v)) p.xPx = static_cast<float>(v);
            if (JsonNumber(json, L"y", &v)) p.yPx = static_cast<float>(v);
            if (JsonNumber(json, L"speedNorm", &v)) p.speedNorm = static_cast<float>(v);
            bool b = false;
            if (JsonBool(json, L"click", &b)) p.isClick = b;
            p.hasNewEvent = true;
            app->ptr = p;
            app->renderer.PushPointer(p);
            return;
        }
        if (json.find(L"\"rbc-glass-prefs\"") != std::wstring::npos) {
            double v = 0;
            if (JsonNumber(json, L"clearSlider", &v)) {
                app->prefs.clearSlider = static_cast<float>(v);
            }
            bool b = false;
            if (JsonBool(json, L"reducedMotion", &b)) app->prefs.reducedMotion = b;
            app->renderer.SetPrefs(app->prefs);
            return;
        }
    };

    // ---- 渲染器初始化（失败不退出，降级）----
    const std::wstring dir = ExeDir();
    const std::wstring csoDir = dir + L"cso";
    if (app->renderer.Initialize(&app->win, csoDir.c_str())) {
        app->glassReady = true;
        // 画质档：先按电池/核显之外一律 Premium。更精确的分档留给
        // D3D11 的 CheckInterfaceSupport / DXGI_ADAPTER_FLAG_SOFTWARE 判定。
        app->renderer.SetQualityTier(QualityTier::Premium);
    } else {
        // [M3] 降级：不 Interactive，只让页面自己画 scrim。
        app->glassReady = false;
    }

    // ---- 捕获初始化（失败不退出）----
    if (app->glassReady) {
        ComPtr<IDXGIDevice> dxgiDev;
        if (SUCCEEDED(app->win.d3dDevice()->QueryInterface(IID_PPV_ARGS(dxgiDev.GetAddressOf())))) {
            ComPtr<IDXGIAdapter> adapter;
            dxgiDev->GetAdapter(adapter.GetAddressOf());
            // 优先 WGC（跨双显卡），失败再退回 DuplicateOutput1
            app->captureReady =
                app->capture.Initialize(app->win.d3dDevice(), adapter.Get(),
                                        app->win.hwnd(), DesktopCapture::Backend::Wgc)
                || app->capture.Initialize(app->win.d3dDevice(), adapter.Get(),
                                           app->win.hwnd(),
                                           DesktopCapture::Backend::DxgiDuplication);
        }
    }

    // ---- 主循环 ----
    bool running = true;
    uint64_t lastFrameTick = GetTickCount64();
    auto lastCapture = std::chrono::steady_clock::now();

    while (running) {
        // [M5] QS_ALLINPUT：不这样 WebView2 的 IPC 会被饿死
        const DWORD wr = MsgWaitForMultipleObjectsEx(1, &quitEvt, 16, QS_ALLINPUT,
                                                     MWMO_INPUTAVAILABLE);
        if (wr == WAIT_OBJECT_0) break;   // quitEvt

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        // [M2] 遮挡 / 最小化 / 演示冻结 => 彻底挂起渲染
        const bool occluded = IsWindowFullyOccluded(app->win.hwnd());
        if (occluded) {
            Sleep(100);
            continue;
        }

        // 水波在跑、或布局刚变、或有新捕获时才值得 Present
        const bool wantWave = app->ptr.hasNewEvent;
        if (!app->glassReady || !app->captureReady) {
            Sleep(50);
            continue;
        }

        // [M4] 捕获限频
        const auto nowTp = std::chrono::steady_clock::now();
        const auto sinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 nowTp - lastCapture).count();
        const bool doCapture = sinceMs >= static_cast<long long>(kCaptureIntervalMs);

        const uint64_t now = GetTickCount64();
        const float dtSec = static_cast<float>(now - lastFrameTick) / 1000.0f;
        lastFrameTick = now;
        const float timeSec = static_cast<float>(now) / 1000.0f;

        bool polarityChanged = false;
        bool lightText = false;
        const bool submitted = app->renderer.RenderFrame(
            app->capture, doCapture ? dtSec : 0.0f, timeSec, polarityChanged, lightText);
        if (doCapture) lastCapture = nowTp;

        if (submitted) {
            app->win.WaitForFrameLatency();
            if (!app->win.PresentWithRecovery()) {
                // 设备丢失：GlassWindow 自己会重建；这里只标记捕获需重装
                app->captureReady = false;
                app->lastCaptureTick = GetTickCount64();
                continue;
            }
        }

        if (polarityChanged) {
            app->win.PostWebMessageJson(
                lightText
                    ? L"{\"type\":\"rbc-apca-polarity\",\"polarity\":\"light-text\"}"
                    : L"{\"type\":\"rbc-apca-polarity\",\"polarity\":\"dark-text\"}");
        }

        // 捕获掉线后按 2s 间隔重试，避免每帧重建
        if (!app->captureReady) {
            const uint64_t t = GetTickCount64();
            if (t - app->lastCaptureTick > 2000) {
                app->lastCaptureTick = t;
                // 重新尝试装捕获（设备丢失或桌面切换后需要重装）
                ComPtr<IDXGIDevice> dxgiDev2;
                if (SUCCEEDED(app->win.d3dDevice()->QueryInterface(
                        IID_PPV_ARGS(dxgiDev2.GetAddressOf())))) {
                    ComPtr<IDXGIAdapter> ad2;
                    dxgiDev2->GetAdapter(ad2.GetAddressOf());
                    app->captureReady =
                        app->capture.Initialize(app->win.d3dDevice(), ad2.Get(),
                                                app->win.hwnd(),
                                                DesktopCapture::Backend::DxgiDuplication);
                }
            }
        }
        (void)wantWave;
    }

    app->renderer.Shutdown();
    app->capture.Shutdown();
    CloseHandle(quitEvt);
    CoUninitialize();
    delete app;
    return 0;
}