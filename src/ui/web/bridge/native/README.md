# 原生桥接插件（路径 B：已实现，2026-10-31）

> **状态：已实现并实测联通。** Rust + napi-rs 源码在本目录（`Cargo.toml` / `src/lib.rs`），
> 产物 `build/Release/rb_native.node` 已被 `bridge/server` 加载，GET_VERSION / GET_STATS /
> GET_VOLUMES 全链路通过（见 `deliverables/43-web-track-verification-2026-10-31.md`）。

## 为什么必须有这一层

浏览器与 Node 的纯 JS 运行时都无法调用 `DeviceIoControl`。现有 WPF 版通过 P/Invoke 完成（见 `src/ui/controller/RAMBlockCache.Controller/Services/DriverService.cs`）。Web 前端要获得同等能力，三条路：

| 路径 | 状态 |
|---|---|
| A. Tauri（Rust 直接调 Win32） | 未选型，备选 |
| **B. napi-rs 原生插件（本目录）** | **已实现（2026-10-31）** |
| C. 保留 C# sidecar，Node 只做 BFF | 未采用（B 已通，无需回退） |

## 导出函数（napi-derive 自动驼峰化）

```rust
#[napi] pub fn rb_open() -> Result<()>;                       // → JS: rbOpen()
#[napi] pub fn rb_request(r#type: u32, payload: Option<Buffer>) -> Result<Buffer>;  // → rbRequest()
#[napi] pub fn rb_close() -> Result<()>;                      // → rbClose()
```

零第三方 Win 绑定依赖：kernel32（CreateFileW / DeviceIoControl / CloseHandle / GetLastError）直接 `extern "system"` 声明。

## 二进制布局（不可偏移）

| 项 | 值 |
|---|---|
| 设备路径 | `\\.\RAMBlockCache` |
| 控制 IOCTL | `0x00222000` |
| `sizeof(RB_MSG)` | **8224** |
| 信封 | 16 字节：`Type@0` / `Size@4` / `Status@8` / `Reserved@12` |
| 载荷位置 | `p + 16` |
| 应答 Status 读取 | `p[8..12]` 小端 int32（METHOD_BUFFERED 失败时同样可读，[P2-14] 非 0 即错） |

发送前载荷区清零（对应 DriverService [FIX C8]/[警告-6]）；Win32 2/6/21 时作废句柄待重开（[FIX S7-14]）。

## RB2 特性层 IOCTL（第二通道，**尚未接入**）

| 名称 | 值 |
|---|---|
| STATS | `0x00222404` |
| REFRESH | `0x00222408` |
| SHRINK | `0x0022240c` |
| SNAPSHOT | `0x00222410` |
| VOLSTATUS | `0x00222414` |

该通道不使用 RB_MSG 信封，直接读写定长结构，实现时不要套用主通道的 16 字节头。

## 构建方式（实测通过的流程）

```powershell
# 工具链：Rust stable-x86_64-pc-windows-msvc（本机 D:\rust，rustc 1.99.1）+ MSVC 14.44（C:\Program1\VC）+ WinSDK 10.0.26100
$env:PATH = "D:\rust\cargo\bin;C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE = "C:\Program1\VC\Tools\MSVC\14.44.35207\include;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\ucrt;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared"
$env:LIB = "C:\Program1\VC\Tools\MSVC\14.44.35207\lib\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64"
cd web\bridge\native
cargo build --release
# 产物复制（Mimosa 钩子拦 cp 时用 node fs.copyFileSync）
node -e "const fs=require('fs');fs.mkdirSync('build/Release',{recursive:true});fs.copyFileSync('target/release/rb_native.dll','build/Release/rb_native.node')"
```

- 未使用 `@napi-rs/cli` 脚手架：`cargo build --release` 直接产出 cdylib，改扩展名为 `.node` 即可（N-API 本身跨 Node 版本 ABI 稳定；换 Node 大版本后建议重编一次以对照验证）。
- crates.io 走 USTC sparse 镜像（本目录 `.cargo/config.toml`）；rustup 安装走 TUNA（rsproxy/SJTU 的 dist 镜像 2026-10-31 实测无 stable 清单）。

## 运行前提（实测确定）

1. 驱动服务已启动：`sc.exe start RAMBlockCache`（需管理员；本机实测 UAC 提权通过）。
2. **桥接进程必须以管理员运行**——设备对象 SDDL 仅授 SYSTEM + 内置管理员（41 号 §7.6）。
   非提权运行时 `/rpc` 返回 503 + Win32 5（拒绝访问），前端如实显示，属预期行为。
3. 重启桥接后 `/health` 应返回 `{"ok":true,"native":true,"deviceOpen":true,...}`。

## 验收标准（2026-10-31 实测结果）

1. ✅ `GET /health` 返回 `native: true`（且 `deviceOpen: true`）
2. ✅ `POST /rpc { "method": 10 }`（GET_VERSION）返回 `Major=1, Minor=1, Build=19`
   ——驱动上报的是 `rb_common.h` 的 `RB_DRIVER_MAJOR/MINOR` + `version.h` 的 `RB_VERSION_BUILD`，
   与 WPF `DriverService.ExpectedDriverVer = "1.1 Build19"` 逐字段一致。
   （本文档旧版写"与 version.h 的 10.4.1.19 一致"系笔误：10.4.1.19 是机器版本，
   驱动 GET_VERSION 不上报该拼装值。）
3. ✅ `POST /rpc { "method": 1 }`（GET_STATS）返回结构通过前端 zod 校验（状态页已实时渲染）
4. ⬜ 与 CLI `ramboostctl version` 输出逐字段一致——CLI 需先构建（`src/_build_user.cmd`），
   留待 CLI 轨道执行；当前已与 DriverService 常量交叉核对。

前三项已通过，可进入页面联调；第 4 项随 CLI 轨道补验后控制面迁移方可宣称完成。
