import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import checker from 'vite-plugin-checker';

// 路径别名必须与 tsconfig.json 的 paths 完全一致，否则构建与类型检查会出现两套解析结果。
// [FIX 2026-10-31] Windows 下 URL.pathname 产生 "/C:/..." 前导斜杠形式，
// 别名匹配会失配，必须经 fileURLToPath 转成本地路径。
export default defineConfig({
  plugins: [
    react(),
    // [库适配] dev 覆盖层实时类型检查（tsc 错误直接浮在页面上，不再埋到终端）
    checker({ typescript: { tsconfigPath: './tsconfig.json' } }),
  ],
  resolve: {
    alias: {
      '@protocol': fileURLToPath(new URL('./src/protocol', import.meta.url)),
      '@bridge': fileURLToPath(new URL('./src/bridge', import.meta.url)),
      '@state': fileURLToPath(new URL('./src/state', import.meta.url)),
      '@features': fileURLToPath(new URL('./src/features', import.meta.url)),
      '@app': fileURLToPath(new URL('./src/app', import.meta.url)),
      '@components': fileURLToPath(new URL('./src/components', import.meta.url)),
      '@lib': fileURLToPath(new URL('./src/lib', import.meta.url)),
    },
  },
  server: {
    port: 5178,
    strictPort: true,
    proxy: {
      // 浏览器侧只与同机桥接服务通信，不直接触碰驱动设备。
      // [FIX 2026-10-31] 桥接 Hono 路由注册在根路径（/health /rpc），
      // 代理必须剥掉 /rb 前缀，否则 503 语义被 404 掩盖（前端误判为 IOCTL_FAILED）。
      '/rb': {
        target: 'http://127.0.0.1:5199',
        changeOrigin: false,
        rewrite: (p) => p.replace(/^\/rb/, ''),
      },
    },
  },
  build: {
    target: 'es2022',
    sourcemap: true,
  },
});
