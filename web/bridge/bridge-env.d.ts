/**
 * bridge 侧类型补丁：@hono/node-server/serve-static 无内建类型声明时的最小兜底。
 * （库适配：真实类型随上游更新后删除本文件。）
 */
declare module '@hono/node-server/serve-static' {
  import type { MiddlewareHandler } from 'hono';
  export function serveStatic(options: {
    root: string;
    path?: string;
    index?: string;
    rewriteRequestPath?: (path: string) => string;
    onNotFound?: (path: string, c: unknown) => Response | Promise<Response>;
  }): MiddlewareHandler;
}
