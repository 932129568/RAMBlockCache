# RAMBlockCache Web 前端

> 状态：**六页全部实装、全链路联通驱动（2026-10-31）**——Node 27.0.0-alpha.0.0.0 +
> npm 12.2.1 + TS 7.0.2 + napi-rs 原生插件；状态/加速/收益/诊断/应急/设置六页 +
> 全部读写命令编码（SET_CONFIG/SET_VOLUME/ATTACH/INVALIDATE_RANGE）。
> 验证记录见 `deliverables/43-web-track-verification-2026-10-31.md`。
> 运行前提：`sc.exe start RAMBlockCache` + 桥接以管理员运行（见 `bridge/native/README.md`）。

## 依赖方向（单向，禁止反向）

```
src/main.tsx
  └─ app/（壳 + 路由）
       └─ features/（页面）
            ├─ state/driver-store.ts        会话级状态
            │    └─ bridge/http-transport.ts
            │         └─ bridge/transport.ts     接口
            │              └─ protocol/types.ts  类型
            │                   └─ protocol/constants.ts
            └─ styles/tokens.css            纯样式，无 TS 依赖

bridge/server（Hono，独立进程）──HTTP──> 被 http-transport 调用
bridge/native（napi-rs，未实现）──被──> bridge/server 加载
```

`src/` 与 `bridge/` 之间**没有任何 import 关系**，只有 HTTP。因此不存在循环依赖。

## 命令

```bash
node -v && npm -v          # Node 27 Alpha / npm 12.2.1
npm install                # 首次：生成 package-lock.json
npm run bridge             # 起本机桥接（当前 /rpc 返回 503，属预期）
npm run dev                # 起前端，http://localhost:5178
```

## 目录

| 路径 | 职责 |
|---|---|
| `src/protocol/` | 协议常量与类型，唯一与 `rb_proto.h` 对应的 TS 层 |
| `src/bridge/` | 传输接口与 HTTP 实现，含 zod 运行时校验 |
| `src/state/` | 会话级状态（连接阶段、版本、最近错误） |
| `src/features/` | 页面。已迁移：状态 / 诊断 / 收益；剩余：加速 / 应急 / 设置 |
| `src/app/` | 壳、路由、导航 |
| `src/styles/` | 设计令牌（映射 `Theme/Dark.xaml`） |
| `bridge/server/` | 本机 HTTP 桥接，唯一触碰驱动的 Node 进程（须管理员运行） |
| `bridge/native/` | napi-rs 原生插件（**已实现**），`cargo build --release` 构建 |

## 当前边界

- 读命令 GET_VERSION/GET_STATS/GET_VOLUMES/GET_CONFIG 已结构化解码；
  GET_LOG/GET_EVENTS 解码与写命令 params 编码随页面迁移补齐（`bridge/server/index.ts` TODO）。
- 未挂载任何卷时统计为诚实全零、命中率「—」、延迟「未采样」，这是正确行为。
