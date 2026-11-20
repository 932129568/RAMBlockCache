// @vitest-environment jsdom
// src/ui/web/src/features/settings/SettingsPage.contract.test.tsx
// [300 2026-11-19]
//
// 判据：SettingsPage 的**字段绑定契约**。
//
// 为什么需要这一条
// ----------------
// 这个文件原先 966 行，是前端最大的单文件，而它**零测试覆盖**：
// `grep -rn "SettingsPage" src --include=*.test.ts*` 只命中 config-diff.test.ts
// 对 config-diff.ts 的引用，没有任何一条测试渲染过这个页面。唯一被测试的是
// 纯函数 diffConfig / describeDiff。
//
// 本轮要把 966 行按段落拆成子组件。拆分对 TypeScript 几乎是透明的：
// 少一个 prop、多一层解构都是类型正确的；但有一类错误 tsc **完全看不见**：
//
//   某个字段的 register()/Controller 绑定在搬运中丢了。
//   后果不是编译失败，而是这个字段**永远不进表单**：
//     - formState.isDirty 感知不到它的改动 → 「保存设置」按钮一直是灰的；
//     - handleSubmit(values) 里没有这个 key → payload 缺字段；
//     - 好在 [BUG-3] 用 GET_CONFIG 原文做了基底合并，所以驱动不会被清零，
//       于是**故障表现为「这个开关点了没反应」**，比报错更难排查。
//
// 所以判据必须钉「绑定是否存在」，而不是「页面渲染出来了」。后者对拆分毫无
// 证据力：拆成 7 个空壳组件也能渲染出标题。
//
// 字段清单的来源：src/protocol/rb-config.generated.ts 的 RbConfig 共 24 个字段，
// 页面把它分成两类 ——
//   19 个可编辑（本文件 EDITABLE）：必须存在 name= 或 aria-label 绑定；
//    5 个只读（本文件 READONLY）：只展示、不写回，不得出现在任何绑定里。
// 24 = 19 + 5。这条等式本身也是判据：将来协议加字段，这里必须先改，改的时候
// 就被迫回答「新字段归哪一类」—— 而不是让它悄悄不出现在界面上。

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { cleanup, render, screen, waitFor } from '@testing-library/react';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import type { ReactNode } from 'react';
import type { RbConfig } from '@protocol/types';
import SettingsPage from './SettingsPage';
import { useDriverStore } from '@state/driver-store';

/** 一份「驱动原文」：全部 24 字段齐备，避免回读 diff 把缺失当漂移。 */
const FULL_CFG: RbConfig = {
  Enabled: 1,
  ReadCacheEnabled: 1,
  WriteCacheEnabled: 1,
  WriteBack: 0,
  CacheSizeMB: 0,
  FlushIntervalMs: 1000,
  DirtyHighPct: 70,
  DirtyLowPct: 50,
  SystemVolumeCached: 0,
  IdleReclaimEnabled: 1,
  IdleReclaimSec: 30,
  ParallelFlush: 4,
  CachePagingIo: 0,
  WriteRelease: 0,
  Frozen: 0,
  IdleFlush: 0,
  VolumeFilter: ['', '', '', '', '', '', '', ''],
  ReadaheadEnabled: 1,
  ReadaheadWindowKB: 256,
  AutoMemory: 1,
  MemReserveMB: 1024,
  AutoUsePct: 50,
  BlockSize: 65536,
  TinyLFU: 1,
};

/** 19 个可编辑字段：必须在 DOM 里留下 register 的 name= 或 Controller 的 aria-label。 */
const EDITABLE = [
  'Enabled',
  'ReadCacheEnabled',
  'WriteCacheEnabled',
  'WriteBack',
  'CacheSizeMB',
  'AutoMemory',
  'MemReserveMB',
  'AutoUsePct',
  'BlockSize',
  'FlushIntervalMs',
  'DirtyHighPct',
  'DirtyLowPct',
  'ParallelFlush',
  'IdleReclaimEnabled',
  'IdleReclaimSec',
  'TinyLFU',
  'CachePagingIo',
  'ReadaheadEnabled',
  'ReadaheadWindowKB',
] as const;

/** 5 个只读字段：由驱动维护，不随保存写回，只出现在可读文本里。 */
const READONLY = [
  'SystemVolumeCached',
  'Frozen',
  'IdleFlush',
  'WriteRelease',
  'VolumeFilter',
] as const;

const ALL_FIELDS: string[] = Object.keys(FULL_CFG);

/** 页面按用户心智顺序分 7 段；拆分必须整段搬走，不能丢段或丢字段。 */
const SECTIONS = [
  '开关 · 谁能用缓存',
  '容量 · 内存预算',
  '回写 · 落盘与回收',
  '预取 · 顺序读与映射 I/O',
  '开机 · 自动加速策略',
  '特性 · RB2 热开关 / L2 / 排除集',
  '只读 · 驱动回报的内部状态',
] as const;

function jsonOk(body: unknown): Response {
  return { ok: true, status: 200, json: async () => body } as unknown as Response;
}

const stubFetch = (): void => {
  vi.stubGlobal('fetch', (input: RequestInfo | URL) => {
    const url = String(input);
    if (url.includes('/rb/rbfeat')) {
      // exe:'found' 才让特性区可用（否则 Toggle 全 disabled，但仍在 DOM 里）
      return Promise.resolve(jsonOk({ stats: { flags: 0 }, l2: '', excl: '', exe: 'found' }));
    }
    if (url.includes('/rb/bootcfg')) {
      return Promise.resolve(jsonOk({ AutoAttach: 0, ResumeOnBoot: 1, ResumeLetters: 'D' }));
    }
    return Promise.resolve(jsonOk({ code: 0, out: '' }));
  });
};

const stubTransport = (): void => {
  useDriverStore.setState({
    transport: {
      getConfig: async () => ({ ok: true, value: FULL_CFG }),
      setConfig: async () => ({ ok: true, value: undefined }),
    } as unknown as ReturnType<typeof useDriverStore.getState>['transport'],
  });
};

const renderPage = () => {
  const qc = new QueryClient({
    defaultOptions: { queries: { retry: false, gcTime: 0, staleTime: 0 } },
  });
  const Wrapper = ({ children }: { children: ReactNode }) => (
    <QueryClientProvider client={qc}>{children}</QueryClientProvider>
  );
  return render(<SettingsPage />, { wrapper: Wrapper });
};

beforeEach(() => {
  stubFetch();
  stubTransport();
});

afterEach(() => {
  cleanup();
  vi.unstubAllGlobals();
});

/** 字段绑定存在性：register 产物是 [name]，Controller 产物是含字段名的 aria-label。 */
const bound = (field: string): boolean => {
  if (document.querySelector(`[name="${field}"]`)) return true;
  return Array.from(document.querySelectorAll('[aria-label]')).some((el) =>
    (el.getAttribute('aria-label') ?? '').includes(field),
  );
};

describe('SettingsPage 字段绑定契约（拆分回归网）', () => {
  it('S0 对照：页面确实渲染出了表单（否则下面每条都是空跑）', async () => {
    const { container } = renderPage();
    await waitFor(() => expect(screen.getByRole('button', { name: /保存设置/ })).toBeTruthy());
    // 强判据：至少一个可编辑控件已进 DOM。若页面停在骨架/错误分支，
    // 下面 19 条会在同一处集体失败 —— 那时 S0 单独点名，不会误判成"字段丢了"。
    expect(container.querySelectorAll('input, select').length).toBeGreaterThan(10);
  });

  it('S1 19 个可编辑字段全部有绑定（漏一个 = 该开关点了没反应）', async () => {
    renderPage();
    await waitFor(() => expect(screen.getByRole('button', { name: /保存设置/ })).toBeTruthy());
    const missing = EDITABLE.filter((f) => !bound(f));
    expect(missing, `这些字段在页面上没有 register/Controller 绑定：${missing.join(', ')}`).toEqual([]);
  });

  it('S2 5 个只读字段只展示、不写回（不得出现绑定）', async () => {
    renderPage();
    await waitFor(() => expect(screen.getByRole('button', { name: /保存设置/ })).toBeTruthy());
    const leaked = READONLY.filter((f) => bound(f));
    expect(leaked, `只读字段被误做成可编辑：${leaked.join(', ')}`).toEqual([]);
    // 但必须仍然可见 —— 只读不等于从界面消失
    const text = document.body.textContent ?? '';
    const invisible = READONLY.filter((f) => !text.includes(f));
    expect(invisible, `只读字段从界面上消失了：${invisible.join(', ')}`).toEqual([]);
  });

  it('S3 字段分类是完备的：24 = 19 可编辑 + 5 只读，无遗漏无重复', () => {
    expect(ALL_FIELDS.length).toBe(24);
    expect(EDITABLE.length).toBe(19);
    expect(READONLY.length).toBe(5);
    expect([...EDITABLE, ...READONLY].sort()).toEqual([...ALL_FIELDS].sort());
  });

  it('S4 七个分段全部存在（拆分不得丢段）', async () => {
    renderPage();
    await waitFor(() => expect(screen.getByRole('button', { name: /保存设置/ })).toBeTruthy());
    const text = document.body.textContent ?? '';
    const missing = SECTIONS.filter((k) => !text.includes(k));
    expect(missing, `分段丢失：${missing.join(' / ')}`).toEqual([]);
  });

  it('S5 块大小只给五档白名单（131072 按红线 R6 永不出现在 UI）', async () => {
    renderPage();
    await waitFor(() => expect(screen.getByRole('button', { name: /保存设置/ })).toBeTruthy());
    const sel = document.querySelector<HTMLSelectElement>('[name="BlockSize"]');
    expect(sel, 'BlockSize 不是原生 select，取值来源变了，这条判据需要重写').not.toBeNull();
    const values = Array.from(sel!.options).map((o) => Number(o.value));
    expect(values).toEqual([4096, 8192, 16384, 32768, 65536]);
    expect(values).not.toContain(131072);
  });
});