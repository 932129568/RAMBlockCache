# RAMBlockCache 架构说明

> **事实修正（对照 audit V10.3.4）**：
> - WBCOAL：设计预留，rbwback/rbfeat 未接线，计数恒 0 —— 不要引用为真实遥测
> - rb_engine_flush_parallel：测试专用路径，生产路径是 flush_pass
> - 目录内若与本文冲突，以本文为准并提 issue

## 组件

- `driver/`：内核块过滤驱动（WDM PnP，卷栈上层，非 minifilter）。
- `core/`：缓存引擎（dseq/sflush/zflush/flush_pass 协议）。
- `ctl/` / `ui/`：控制面与 WPF 客户端。
- `common/version.h`：唯一版本事实来源。

## 版本

版本号唯一来源为 `common/version.h`（当前 10.3.4.19）。驱动 `RB_DRIVER_BUILD`
与 WPF `AppInfo.Version` 均由该头派生，改版本只改这一处。
