/**
 * [FIX 2026-10-31] tokens.css 的副作用导入需要模块声明，
 * 否则 TS2882（strict 下无 vite/client 类型注入时的最小解）。
 */
declare module '*.css';
