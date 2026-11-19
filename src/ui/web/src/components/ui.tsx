/**
 * 兼容垫片（2026-11-12）。
 * 533 行「组件大杂烩」已按职责拆分到同目录 `./ui/`：
 *   icons.tsx / brand.tsx / value.tsx / surface.tsx / controls.tsx / index.ts
 *
 * 保留此文件是为了让 `@components/ui` 与相对导入 `./ui` 的解析路径**完全不变**，
 * 调用方（6 个页面 + App/StateBar/GlobalAlertBanner）无需改动任何 import。
 * 新增原语请写进 `./ui/*`，不要再往这里堆实现。
 */

export * from './ui/index';
