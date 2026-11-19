import { fileURLToPath } from 'node:url';
// [U3 2026-11-19] 从 'vitest/config' 取 defineConfig：vite 官方的 defineConfig 类型里
// 没有 test 字段，直接塞 test 会 TS2769 报错（"test does not exist in UserConfigExport"）。
// 这是本轮改配置时踩到的真错，不是风格偏好。
import { defineConfig } from 'vitest/config';
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
  /* [U3 2026-11-19] 测试池显式指定 threads，不用默认 forks。
   *
   * 实测（本机 Windows / Node v27.0.0-alpha）：
   *   - forks 池：**既有** src/lib/liquidWaveCore.test.ts 单文件 10 次里 2 次卡死，
   *     报 "[vitest-pool] Timeout terminating forks worker"，进程退出码 124；
   *   - threads 池：同一文件 10/10 通过，全量 16 文件 113 用例 6/6 稳定 3-4 s。
   * 排除本轮新增的 4 个测试文件后仍能复现 ⇒ 这是**既有基础设施缺陷**，与 U3 无关。
   *
   * 危害不只是慢：worker 拆解超时会让 CI 随机变红，把"改动引入回归"的判断污染成
   * 噪声，比不跑测试更危险。因此在配置层钉死，而不是靠每次命令行加 --pool。
   * 若将来要回去用 forks，必须先在 Windows 上跑够 30 次无挂起再改。 */
  test: {
    pool: 'threads',
  },
  build: {
    target: 'es2022',
    sourcemap: true,
    /* [A11 2026-11-11] 手动分包。此前 6 页 lazy 化后仍有 2 个 chunk 超 500KB：
     *   - BenefitPage 547KB：echarts 全量被打进该页（只有它用图表）；
     *   - index 524KB：react + router + motion + react-query 全在入口。
     * 后果不是"慢"（本地 HTTP 快），而是**缓存失效粒度粗**：任何一行业务代码
     * 改动都会让 500KB 的 vendor 一起失效，用户每次升级都要重下全部。
     * 拆成 3 个 vendor chunk 后，业务代码频繁变动、vendor 长期稳定，
     * 浏览器缓存命中率显著提高。 */
    rollupOptions: {
      output: {
        manualChunks: (id) => {
          if (!id.includes('node_modules')) return undefined;
          /* echarts 单独成块：只有收益页用，且体积最大（约 400KB）。
             它若留在业务 chunk 里，会让"改一个文字"变成"重下整个 echarts"。 */
          if (id.includes('echarts') || id.includes('zrender')) return 'vendor-echarts';
          /* 动效库：跨页共用，独立成块便于长期缓存 */
          if (id.includes('motion') || id.includes('framer-motion')) return 'vendor-motion';
          /* 其余 UI 基础设施：react / router / query / radix / form / 工具库 */
          return 'vendor';
        },
      },
    },
  },
});
