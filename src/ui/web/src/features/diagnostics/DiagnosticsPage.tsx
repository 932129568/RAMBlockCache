import { useMemo, useRef, useState } from 'react';
import { useQuery } from '@tanstack/react-query';
import { useVirtualizer } from '@tanstack/react-virtual';
import { RB_LOG_LEVEL, RB_OP, RB_PHASE } from '@protocol/constants';
import type { RbEventRec, RbLogRec } from '@protocol/types';
import { useDriverStore } from '@state/driver-store';
import { useChannels } from '@state/trace-store';
import { toast } from '@state/toast-store';
import IoctlMatrix from '@components/IoctlMatrix';
import { Badge, Button, Icons, PageHeader } from '@components/ui';
import { filetimeToTime } from '@lib/time';
import {
  classifyL2Window,
  formatRbLogStatus,
  RB2F_L2,
  RB2F_WBCOAL,
  type L2TelemetrySample,
} from '../../utils/l2Diagnostics';

const OP_TEXT: Record<number, string> = {
  [RB_OP.BENCHMARK]: '基准测试',
  [RB_OP.PRESET_APPLY]: '预设应用',
  [RB_OP.UNMOUNT]: '卸载回写',
  [RB_OP.L2_REBUILD]: 'L2 重建',
};

const PHASE_TEXT: Record<number, string> = {
  [RB_PHASE.START]: '开始',
  [RB_PHASE.PROGRESS]: '进行中',
  [RB_PHASE.DONE]: '完成',
};

const LEVEL_TEXT: Record<number, string> = {
  [RB_LOG_LEVEL.INFO]: '信息',
  [RB_LOG_LEVEL.WARN]: '警告',
  [RB_LOG_LEVEL.ERROR]: '错误',
};

const LEVEL_COLOR: Record<number, string> = {
  [RB_LOG_LEVEL.INFO]: 'var(--rbc-text-secondary)',
  [RB_LOG_LEVEL.WARN]: 'var(--rbc-warning-fg)',
  [RB_LOG_LEVEL.ERROR]: 'var(--rbc-danger-fg)',
};

/** formatRbLogStatus 的 tone 对应的语义色令牌（用于日志状态徽标）。 */
const STATUS_TONE_COLOR: Record<'emerald' | 'cyan' | 'amber' | 'rose', string> = {
  emerald: 'var(--rbc-success-fg)',
  cyan: 'var(--rbc-accent)',
  amber: 'var(--rbc-warning-fg)',
  rose: 'var(--rbc-danger-fg)',
};

/**
 * [235 Phase 2] L2 窗口判读规则自检样本（§5.2.1）。
 * 这些是**规则参考样本，不是实时遥测**：Node Bridge 契约（Gate5 冻结、Session 0）
 * 不暴露 dCoreMiss / dL2Hits / dL2Miss 三项增量，本表仅用于逐条验证并展示
 * classifyL2Window 的判定口径；J6 跨引导验证严格保持 PREREQ / 待验证（S-02 冻结，
 * dCoreMiss>=1 / dL2Hits>0 / dL2Miss=0 三条真判据一字节不改）。
 */
const L2_RULE_SAMPLES: { caption: string; sample: L2TelemetrySample }[] = [
  {
    caption: '引导保护自禁用',
    sample: { dCoreMiss: 0, dL2Hits: 0, dL2Miss: 0, l2State: 0, l2Halt: 0, selfDisabled: true },
  },
  {
    caption: 'HALT 停机门',
    sample: { dCoreMiss: 0, dL2Hits: 0, dL2Miss: 0, l2State: 0, l2Halt: 1 },
  },
  {
    caption: 'L2 位门 / WBCOAL 互斥',
    sample: { dCoreMiss: 0, dL2Hits: 0, dL2Miss: 0, l2State: 0, l2Halt: 0, featureFlags: RB2F_WBCOAL },
  },
  {
    caption: '单卷门拦截',
    sample: { dCoreMiss: 0, dL2Hits: 0, dL2Miss: 0, l2State: 0, l2Halt: 0, featureFlags: RB2F_L2, attachedVolumeCount: 2 },
  },
  {
    caption: '块几何门拦截',
    sample: { dCoreMiss: 0, dL2Hits: 0, dL2Miss: 0, l2State: 0, l2Halt: 0, featureFlags: RB2F_L2, attachedVolumeCount: 1, blockSize: 8192 },
  },
  {
    caption: '懒加载待命',
    sample: { dCoreMiss: 0, dL2Hits: 0, dL2Miss: 0, l2State: 0, l2Halt: 0, featureFlags: RB2F_L2, attachedVolumeCount: 1, blockSize: 4096 },
  },
  {
    caption: 'Core 全命中内联',
    sample: { dCoreMiss: 0, dL2Hits: 0, dL2Miss: 0, l2State: 1, l2Halt: 0, featureFlags: RB2F_L2, attachedVolumeCount: 1, blockSize: 4096 },
  },
  {
    caption: 'J6 三条真判据满足',
    sample: { dCoreMiss: 8, dL2Hits: 16, dL2Miss: 0, l2State: 1, l2Halt: 0, featureFlags: RB2F_L2, attachedVolumeCount: 1, blockSize: 4096 },
  },
  {
    caption: '混合命中',
    sample: { dCoreMiss: 8, dL2Hits: 12, dL2Miss: 4, l2State: 1, l2Halt: 0, featureFlags: RB2F_L2, attachedVolumeCount: 1, blockSize: 4096 },
  },
  {
    caption: '哈希槽冷态填充',
    sample: { dCoreMiss: 8, dL2Hits: 0, dL2Miss: 16, l2State: 1, l2Halt: 0, featureFlags: RB2F_L2, attachedVolumeCount: 1, blockSize: 4096 },
  },
];

/**
 * [UI-2026-11-12 真缺陷] `Status` 字段**不总是** NTSTATUS。
 *
 * 改前渲染规则是 `l.Status !== 0` → 一律当红色错误码打出来。实测日志列表里
 * 连续五行信息级日志（"system volume opt-in ON by user"）后面都挂着红色的
 * `0xFFFFFFFF`，看起来像五条故障——而它们全是正常的开关操作记录。
 *
 * 根因在驱动侧：`rbctl.c:269` 把 `SystemVolumeCached` 这个**布尔配置值**当成
 * `Status` 传给 `RbLog()`：
 *   `RbLog(0, __LINE__, (LONG)msg->Data.Config.SystemVolumeCached, ...)`
 * 配置为 TRUE 时该值是 `0xFFFFFFFF`。这不是错误码，是"本记录不携带状态"。
 *
 * 处置：驱动侧语义瑕疵留待下一轮驱动批次修（改 `.c` 会动 `.sys` 哈希，
 * 而当前源码树与已部署物本就分叉，不宜再叠加）。UI 侧先按 NTSTATUS 的**真实
 * 编码规则**判定，不再谎报红色：
 *   - `0` 与 `0xFFFFFFFF` → 不带状态，不显示；
 *   - 高两位为 `11`（≥0xC0000000）→ 真错误，红色；
 *   - 其余（如 0x80000000 段的 warning、或塞进来的小整数）→ 中性灰。
 */
function statusOf(st: number): { text: string; error: boolean } | null {
  const u = st >>> 0;
  if (u === 0 || u === 0xffffffff) return null;
  return {
    text: `0x${u.toString(16).toUpperCase()}`,
    error: (u & 0xc0000000) === 0xc0000000,
  };
}

/**
 * 诊断页 —— 2026-11-12 五段式重构
 *
 * 本页在全站的职分是回答「**出问题时查什么**」，与状态页（现在健不健康）、
 * 收益页（攒了多少）严格分工。所以它只放"可观测性三件套"：
 *   ① 结论   —— 12 通道里几个失败/几个超 SLO、日志环里几条 ERROR（一句话）
 *   ② 通道   —— IOCTL 点阵（12 通道 × 四态，前端埋点）
 *   ③ 事件   —— 操作进度环（卸载回写 / L2 重建等长操作的阶段）
 *   ④ 日志   —— 驱动环形日志（级别筛选 + 排序 + 虚拟滚动）
 *   ⑤ 落盘   —— 跨重启保留的日志文件（内存环清空后唯一来源）
 *
 * 改前三块都用手写的 `<section class="rbc-card"><header class="mb-3 border-b pb-2">`，
 * 各自定间距和字号，且与状态页/收益页的 `.rbc-section` 骨架不是一套东西。
 * 本轮统一到骨架：标题行在卡**外**、正文在卡**内**。
 *
 * 另一处结构改造：通道数据改由**页面**订阅一次（`useChannels()`）再传给
 * IoctlMatrix。改前矩阵自己轮询，结论段若再写一份就是同页两条 500ms 定时器。
 */
export default function DiagnosticsPage() {
  const transport = useDriverStore((s) => s.transport);
  const reportError = useDriverStore((s) => s.reportError);
  const reportSuccess = useDriverStore((s) => s.reportSuccess);

  /* 唯一一条通道订阅：结论段的判定与矩阵的渲染消费同一份快照，不会出现
   * "结论说全部正常、矩阵里却有一个红点"这种自相矛盾的画面。 */
  const channels = useChannels();

  const cursorRef = useRef(0);
  const [events, setEvents] = useState<RbEventRec[]>([]);

  const eventsQuery = useQuery({
    queryKey: ['rb', 'events'],
    queryFn: async () => {
      const result = await transport.getEvents({
        Cursor: cursorRef.current,
        MaxCount: 64,
      });
      if (!result.ok) {
        reportError(result.error);
        throw new Error(result.error.message);
      }
      reportSuccess();
      const list = result.value;
      if (list.Count > 0) {
        // [功能打磨 2026-10-31] 驱动重启自愈：Seq 回退（新批次首条 ≤ 已消费游标）
        // 说明驱动侧环已重置——丢弃旧缓存、游标归零重放，否则事件永远为空。
        const restarted = (list.Items[0]?.Seq ?? Infinity) <= cursorRef.current;
        if (restarted) setEvents([]);
        cursorRef.current = list.Cursor;
        setEvents((prev) => (restarted ? list.Items : [...prev, ...list.Items]).slice(-200));
      } else if (list.Cursor < cursorRef.current) {
        // 环空且游标回退：同为重启痕迹，游标跟随驱动
        cursorRef.current = list.Cursor;
      }
      return list;
    },
    /* [CPU 2026-11-05] 事件轮询自适应：页面可见 500ms；不可见 5s（后台不占 CPU）；
     * 连续失败退避至 4s（桥接忙时减负），恢复后回 500ms。 */
    refetchInterval: (query) => {
      if (typeof document !== 'undefined' && document.hidden) return 5000;
      const fails = query.state.dataUpdateCount >= 0 && query.state.error ? 1 : 0;
      return fails ? 4000 : 500;
    },
    refetchIntervalInBackground: false,
    retry: 0,
  });

  /* [FEAT-LOGUI 2026-11-05] 落盘日志：驱动日志已由桥接 2s 增量写到
   * %ProgramData%\RAMBlockCache\logs\driver-YYYYMMDD.log（512 条环 × 96 字符的
   * 持久化副本）。此处提供"打开目录 / 列出历史文件 / 查看文件尾部"，
   * 把故障复盘从"手工找路径"降到一次点击。 */
  const logFilesQuery = useQuery({
    queryKey: ['rb', 'logs'],
    queryFn: async () => {
      const r = await fetch('/rb/logs');
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      return (await r.json()) as {
        dir: string;
        exists: boolean;
        files: { name: string; size: number; mtime: number }[];
      };
    },
    refetchInterval: 10000,
    retry: 1,
  });
  const [openFile, setOpenFile] = useState<string | null>(null);
  const logFileQuery = useQuery({
    queryKey: ['rb', 'logs', openFile],
    enabled: !!openFile,
    queryFn: async () => {
      const r = await fetch(`/rb/logs/${encodeURIComponent(openFile!)}`);
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      return (await r.json()) as { name: string; size: number; truncated: boolean; text: string };
    },
    retry: 0,
  });
  async function openLogDir(): Promise<void> {
    try {
      const r = await fetch('/rb/logs/open', { method: 'POST' });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const j = (await r.json()) as { ok: boolean; error?: string };
      /* [FIX 2026-11-13] 失败原因归为 IOCTL_FAILED 而不是 UNREACHABLE：
       * driver-store.reportError 只在 UNREACHABLE / DRIVER_ABSENT 时把全局
       * phase 改成 offline。而「打开资源管理器失败」跟驱动连通性无关——旧代码
       * 会因此把整个控制台闪成离线态（下一次 1s 统计轮询又翻回在线），
       * 用户看到的是莫名其妙的断连横幅。 */
      if (!j.ok) reportError({ code: 'IOCTL_FAILED', message: j.error ?? '打开失败' });
    } catch (e) {
      reportError({ code: 'IOCTL_FAILED', message: `打开日志目录失败：${(e as Error).message}` });
    }
  }

  const logQuery = useQuery({
    queryKey: ['rb', 'log'],
    queryFn: async () => {
      const result = await transport.getLog();
      if (!result.ok) {
        reportError(result.error);
        throw new Error(result.error.message);
      }
      reportSuccess();
      return result.value;
    },
    refetchInterval: false,
    retry: 0,
  });

  const logs = logQuery.data?.Items ?? [];
  const [levelFilter, setLevelFilter] = useState<number | 'all'>('all');
  const filtered = useMemo(
    () => (levelFilter === 'all' ? logs : logs.filter((l) => l.Level === levelFilter)),
    [logs, levelFilter],
  );

  // 排序：默认时间降序（最新在上）。注：@tanstack/react-table 已装 v9
  //（破坏性大版本，API 迁移待官方文档补齐后接入），此处先以手写排序交付同 UX。
  const [sort, setSort] = useState<{ key: keyof RbLogRec; desc: boolean }>({
    key: 'Ticks',
    desc: true,
  });
  const rows = useMemo(() => {
    const arr = [...filtered];
    arr.sort((a, b) => {
      const av = a[sort.key];
      const bv = b[sort.key];
      const c =
        typeof av === 'string' && typeof bv === 'string'
          ? av.localeCompare(bv)
          : Number(av) - Number(bv);
      return sort.desc ? -c : c;
    });
    return arr;
  }, [filtered, sort]);

  function toggleSort(key: keyof RbLogRec): void {
    setSort((s) => (s.key === key ? { key, desc: !s.desc } : { key, desc: true }));
  }

  const [exporting, setExporting] = useState(false);
  async function exportBundle() {
    setExporting(true);
    try {
      const [cfg, stats, log, ev] = await Promise.all([
        transport.getConfig(),
        transport.getStats(),
        transport.getLog(),
        transport.getEvents({ Cursor: 0, MaxCount: 64 }),
      ]);
      const bundle = {
        exportedAt: new Date().toISOString(),
        source: 'web-console/diagnostics-export',
        config: cfg.ok ? cfg.value : { error: cfg.error.message },
        stats: stats.ok ? stats.value : { error: stats.error.message },
        log: log.ok ? log.value : { error: log.error.message },
        events: ev.ok ? ev.value : { error: ev.error.message },
      };
      const blob = new Blob([JSON.stringify(bundle, null, 2)], { type: 'application/json' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `rbc-diagnostics-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, '')}.json`;
      a.click();
      URL.revokeObjectURL(url);
      toast('ok', '诊断快照已导出', 'JSON 已下载到本地');
    } catch (e) {
      toast('danger', '导出失败', e instanceof Error ? e.message : String(e));
    } finally {
      setExporting(false);
    }
  }

  const scrollRef = useRef<HTMLDivElement>(null);
  const virtualizer = useVirtualizer({
    count: rows.length,
    getScrollElement: () => scrollRef.current,
    estimateSize: () => 24,
    overscan: 8,
  });

  /* ① 结论段的判定。四态优先级：失败 > 超 SLO > 单次偏慢 > 正常未调用。
   * "未调用"必须显式说出来 —— 页面刚打开时 12 个格子全是灰的，
   * 不说清楚会被读成"通道全坏了"。 */
  const called = channels.filter((c) => c.state !== 0);
  const failed = channels.filter((c) => c.state === 3);
  const slow = channels.filter((c) => c.state === 2);
  const breached = channels.filter((c) => c.sloBreached);
  const errLogs = logs.filter((l) => l.Level === RB_LOG_LEVEL.ERROR).length;
  const worst: 'ok' | 'warn' | 'danger' | 'neutral' =
    failed.length > 0 ? 'danger' : breached.length > 0 || slow.length > 0 ? 'warn' : called.length > 0 ? 'ok' : 'neutral';

  let verdict: string;
  if (called.length === 0) {
    verdict =
      '通道还没产生调用样本——页面刚打开，或桥接不通。下面几段会是空的，那是「还没问到」，不是「问坏了」。';
  } else if (failed.length > 0) {
    verdict = `${failed.length} 个通道失败（${failed.map((c) => c.name).join('、')}），另有 ${breached.length} 个超 SLO。先看下面日志段的错误行。`;
  } else if (breached.length > 0) {
    verdict = `${called.length} 个通道已产生样本，其中 ${breached.length} 个 P95 超出 SLO（${breached.map((c) => c.name).join('、')}）——功能可用，但这条通道在变慢。`;
  } else if (slow.length > 0) {
    verdict = `${called.length} 个通道已产生样本，全部未超 SLO；其中 ${slow.length} 个单次时延 ≥50ms（${slow.map((c) => c.name).join('、')}）。`;
  } else {
    verdict = `${called.length} 个通道已产生样本，全部正常且未超 SLO。`;
  }
  if (errLogs > 0) verdict += ` 日志环里有 ${errLogs} 条 ERROR。`;

  return (
    <div className="flex flex-col gap-6">
      <PageHeader
        title="诊断"
        desc="通道健康 / 操作事件 / 驱动日志——前端埋点 + 驱动侧双视角"
        actions={
          <>
            <Badge tone={worst}>
              {worst === 'danger'
                ? `${failed.length} 通道失败`
                : worst === 'warn'
                  ? `${breached.length || slow.length} 通道偏慢`
                  : worst === 'ok'
                    ? '通道正常'
                    : '尚无样本'}
            </Badge>
            <Button
              tone="secondary"
              icon={<Icons.download size={14} />}
              onClick={() => void exportBundle()}
              disabled={exporting}
            >
              {exporting ? '导出中…' : '导出诊断快照'}
            </Button>
          </>
        }
      />

      <p className="rbc-fade-up text-sm" style={{ color: 'var(--rbc-text-secondary)' }}>
        {verdict}
      </p>

      {/* ② 通道：12 通道 × 四态点阵 */}
      <section className="rbc-section rbc-fade-up">
        <div className="rbc-section-head">
          <span className="rbc-kicker">通道 · IOCTL 健康点阵</span>
          <span className="rbc-section-note">最近 32 样本 P50/P95 · SLO 违约扫光</span>
        </div>
        <div className="rbc-card">
          <IoctlMatrix items={channels} />
        </div>
      </section>

      {/* ③ 事件：长操作的阶段进度（卸载回写 / L2 重建） */}
      <section className="rbc-section rbc-fade-up">
        <div className="rbc-section-head">
          <span className="rbc-kicker">事件 · 操作进度</span>
          <span className="rbc-section-note">
            500ms 游标增量轮询 · 已缓存 {events.length} 条
          </span>
        </div>
        <div className="rbc-card">
          {eventsQuery.isPending ? (
            <p className="text-sm" style={{ color: 'var(--rbc-text-secondary)' }}>
              正在读取事件环…
            </p>
          ) : eventsQuery.isError ? (
            <p role="alert" className="text-sm" style={{ color: 'var(--rbc-danger-fg)' }}>
              事件读取失败：{(eventsQuery.error as Error).message}
            </p>
          ) : events.length === 0 ? (
            <p className="text-sm" style={{ color: 'var(--rbc-text-tertiary)' }}>
              暂无事件（卸载回写 / L2 重建等操作发生时在此滚动出现）
            </p>
          ) : (
            <ul className="max-h-72 overflow-y-auto font-mono text-xs" aria-label="操作进度事件列表">
              {events.map((e) => (
                <li
                  key={e.Seq}
                  className="flex items-baseline gap-3 border-b py-1"
                  style={{ borderColor: 'var(--rbc-border)' }}
                >
                  <span style={{ color: 'var(--rbc-text-tertiary)' }}>#{e.Seq}</span>
                  <span style={{ color: 'var(--rbc-accent)' }}>
                    {OP_TEXT[e.OpType] ?? `OpType ${e.OpType}`}
                  </span>
                  <span style={{ color: 'var(--rbc-text-secondary)' }}>
                    {PHASE_TEXT[e.Phase] ?? (e.Phase === 0 ? '—' : `Phase ${e.Phase}`)}
                  </span>
                  {e.Percent > 0 && (
                    <span style={{ color: 'var(--rbc-text-secondary)' }}>{e.Percent}%</span>
                  )}
                  <span className="flex-1 truncate" style={{ color: 'var(--rbc-text-primary)' }}>
                    {e.StageText || '（无阶段文案）'}
                  </span>
                </li>
              ))}
            </ul>
          )}
        </div>
      </section>

      {/* ④ 日志：驱动环形缓冲。筛选与动作放在正文顶部的工具条，
          不再挤在标题行里——703px 下标题+下拉+两个按钮会挤成一团。 */}
      <section className="rbc-section rbc-fade-up">
        <div className="rbc-section-head">
          <span className="rbc-kicker">日志 · 驱动环形缓冲</span>
          <span className="rbc-section-note">
            {logs.length > 0 ? `${logs.length} 条 · 内存环` : '内存环（重启即清空）'}
          </span>
        </div>
        <div className="rbc-card">
          <div className="mb-3 flex flex-wrap items-center gap-2">
            <select
              aria-label="日志级别筛选"
              value={String(levelFilter)}
              onChange={(e) =>
                setLevelFilter(e.target.value === 'all' ? 'all' : Number(e.target.value))
              }
              className="rbc-input text-xs"
            >
              <option value="all">全部级别</option>
              <option value={RB_LOG_LEVEL.INFO}>信息</option>
              <option value={RB_LOG_LEVEL.WARN}>警告</option>
              <option value={RB_LOG_LEVEL.ERROR}>错误</option>
            </select>
            <span className="flex-1" />
            <Button
              tone="ghost"
              icon={<Icons.refresh size={14} />}
              onClick={() => void logQuery.refetch()}
              ariaLabel="刷新驱动日志"
            >
              刷新
            </Button>
            <Button
              tone="ghost"
              onClick={() => void openLogDir()}
              ariaLabel="在资源管理器中打开日志目录"
            >
              打开日志目录
            </Button>
          </div>

          {logQuery.isPending ? (
            <p className="text-sm" style={{ color: 'var(--rbc-text-secondary)' }}>
              正在读取环形日志…
            </p>
          ) : logQuery.isError ? (
            <p role="alert" className="text-sm" style={{ color: 'var(--rbc-danger-fg)' }}>
              日志读取失败：{(logQuery.error as Error).message}
            </p>
          ) : rows.length === 0 ? (
            <p className="text-sm" style={{ color: 'var(--rbc-text-tertiary)' }}>
              {logs.length === 0 ? '日志环为空（驱动刚启动、无日志写入）' : '当前筛选下无日志'}
            </p>
          ) : (
            <>
              {/* 排序表头：点击切换升/降序（react-table v9 迁移前的手写排序） */}
              <div
                className="flex items-center gap-3 border-b pb-1 text-xs"
                style={{ borderColor: 'var(--rbc-border)', color: 'var(--rbc-text-tertiary)' }}
              >
                {(
                  [
                    ['Ticks', '时间', 90],
                    ['Level', '级别', 60],
                    ['Text', '内容', 0],
                    ['Status', '状态', 90],
                  ] as const
                ).map(([key, label, w]) => (
                  <button
                    key={key}
                    type="button"
                    onClick={() => toggleSort(key)}
                    className="rbc-btn rbc-btn-ghost !px-1 !py-0"
                    style={w === 0 ? { flex: '1' } : { width: w }}
                  >
                    {label}
                    {sort.key === key ? (sort.desc ? ' ↓' : ' ↑') : ''}
                  </button>
                ))}
              </div>
              <div
                ref={scrollRef}
                className="max-h-72 overflow-y-auto font-mono text-xs"
                aria-label="驱动日志列表"
              >
                <div style={{ height: virtualizer.getTotalSize(), position: 'relative' }}>
                  {virtualizer.getVirtualItems().map((vi) => {
                    const l = rows[vi.index];
                    if (!l) return null;
                    return (
                      <div
                        key={l.Ticks + '-' + vi.index}
                        className="absolute left-0 w-full"
                        style={{
                          height: 24,
                          transform: `translateY(${vi.start}px)`,
                          display: 'flex',
                          alignItems: 'center',
                          gap: 12,
                          borderBottom: '1px solid var(--rbc-border)',
                        }}
                      >
                        <span style={{ color: 'var(--rbc-text-tertiary)', width: 90 }}>
                          {filetimeToTime(l.Ticks)}
                        </span>
                        <span
                          style={{
                            color: LEVEL_COLOR[l.Level] ?? 'var(--rbc-text-secondary)',
                            width: 60,
                          }}
                        >
                          {LEVEL_TEXT[l.Level] ?? `L${l.Level}`}
                        </span>
                        <span
                          className="flex-1 truncate"
                          style={{ color: 'var(--rbc-text-primary)' }}
                          title={l.Text}
                        >
                          {l.Text || '（空）'}
                        </span>
                        {(() => {
                          /* [235 Phase 2 / BUG-NEW-04] SystemVolumeCached 的 opt-in
                           * 日志把布尔值塞在 Status 里；用 formatRbLogStatus 双轨
                           * 解析成明确的 ON/OFF 徽标，避免被误读成 NTSTATUS 故障码。
                           * 其余日志沿用原 statusOf() 的十六进制渲染，行为不变。 */
                          const fmt = formatRbLogStatus({
                            level: l.Level,
                            srcLine: l.Src,
                            status: l.Status,
                            text: l.Text,
                          });
                          if (/system volume opt-in/i.test(l.Text)) {
                            return (
                              <span
                                style={{
                                  width: 90,
                                  textAlign: 'right',
                                  color: STATUS_TONE_COLOR[fmt.tone],
                                }}
                                title="SystemVolumeCached opt-in 状态（双轨兼容解析）"
                              >
                                {fmt.label}
                              </span>
                            );
                          }
                          const s = statusOf(l.Status);
                          if (!s) return null;
                          return (
                            <span
                              style={{
                                width: 90,
                                textAlign: 'right',
                                color: s.error
                                  ? 'var(--rbc-danger-fg)'
                                  : 'var(--rbc-text-tertiary)',
                              }}
                              title={s.error ? 'NTSTATUS 错误码' : '非错误码（记录附带的状态值）'}
                            >
                              {s.text}
                            </span>
                          );
                        })()}
                      </div>
                    );
                  })}
                </div>
              </div>
            </>
          )}
        </div>
      </section>

      {/* ⑤ 落盘历史：内存环重启即空，这是唯一跨重启来源 */}
      <section className="rbc-section rbc-fade-up">
        <div className="rbc-section-head">
          <span className="rbc-kicker">落盘 · 跨重启保留</span>
          <span className="rbc-section-note">
            {/* 目录是 Windows 绝对路径（反斜杠不是断行点），必须截断，否则在 703px 下
                会把整个 section-head 撑出横向滚动。完整路径见 title 提示。 */}
            {logFilesQuery.data ? (
              <span
                className="truncate"
                style={{ display: 'inline-block', maxWidth: '42vw', verticalAlign: 'bottom' }}
                title={logFilesQuery.data.dir}
              >
                {logFilesQuery.data.exists ? logFilesQuery.data.dir : '目录尚未创建'}
              </span>
            ) : (
              '桥接 2s 增量写入'
            )}
          </span>
        </div>
        <div className="rbc-card">
          {logFilesQuery.isPending ? (
            <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
              正在读取…
            </p>
          ) : (logFilesQuery.data?.files.length ?? 0) === 0 ? (
            <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
              暂无落盘日志文件。桥接启动后会自动把驱动日志增量写入该目录。
            </p>
          ) : (
            <ul className="mb-3 flex flex-wrap gap-2" aria-label="落盘日志文件列表">
              {logFilesQuery.data!.files.map((f) => (
                <li key={f.name}>
                  <button
                    type="button"
                    onClick={() => setOpenFile(openFile === f.name ? null : f.name)}
                    className="rbc-input text-xs"
                    aria-pressed={openFile === f.name}
                    style={
                      openFile === f.name
                        ? { borderColor: 'var(--rbc-accent)', color: 'var(--rbc-accent)' }
                        : undefined
                    }
                  >
                    {f.name}
                    <span style={{ color: 'var(--rbc-text-tertiary)', marginLeft: 6 }}>
                      {(f.size / 1024).toFixed(0)} KB
                    </span>
                  </button>
                </li>
              ))}
            </ul>
          )}
          {openFile && logFileQuery.isPending && (
            <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
              正在读取 {openFile}…
            </p>
          )}
          {openFile && logFileQuery.isError && (
            <p role="alert" className="text-xs" style={{ color: 'var(--rbc-danger-fg)' }}>
              读取失败：{(logFileQuery.error as Error).message}
            </p>
          )}
          {openFile && logFileQuery.data && (
            <>
              {logFileQuery.data.truncated && (
                <p className="mb-1 text-xs" style={{ color: 'var(--rbc-warning-fg)' }}>
                  文件较大（{(logFileQuery.data.size / 1024).toFixed(0)} KB），仅显示尾部 512 KB。
                </p>
              )}
              <pre
                className="max-h-64 overflow-auto rounded p-2 font-mono text-xs"
                style={{
                  /* [F-UI1 2026-11-11] --rbc-surface-2 从未定义 → 裸 var() 整条失效，
                     日志块底色实为透明（与页面底色重合，层次丢失）。改用输入层底色。 */
                  background: 'var(--rbc-bg-input)',
                  color: 'var(--rbc-text-secondary)',
                  whiteSpace: 'pre-wrap',
                  wordBreak: 'break-all',
                }}
                aria-label={`日志文件 ${openFile} 内容`}
              >
                {logFileQuery.data.text}
              </pre>
            </>
          )}
        </div>
      </section>

      {/* ⑥ L2 窗口判读规则自检（§5.2.1）：以标准样本驱动 classifyL2Window，
          把 s_selfDisable / HALT / 三道开启门 / J6 三条真判据的判定逐条列出。
          语义边界：规则参考，不是实时遥测；J6 仍为 PREREQ / 待验证（S-02 冻结）。 */}
      <section className="rbc-section rbc-fade-up">
        <div className="rbc-section-head">
          <span className="rbc-kicker">L2 · 窗口判读规则自检</span>
          <span className="rbc-section-note">
            示例样本驱动 · 非实时遥测 · J6 = PREREQ / 待验证（S-02 冻结）
          </span>
        </div>
        <div className="rbc-card">
          <ul className="flex flex-col gap-1.5 text-xs" aria-label="L2 窗口判读规则对照表">
            {L2_RULE_SAMPLES.map(({ caption, sample }) => {
              const verdict = classifyL2Window(sample);
              return (
                <li
                  key={caption}
                  className="flex flex-wrap items-baseline gap-x-3 gap-y-0.5 border-b pb-1"
                  style={{ borderColor: 'var(--rbc-border)' }}
                >
                  <span style={{ color: 'var(--rbc-text-tertiary)', minWidth: 132 }}>
                    {caption}
                  </span>
                  <span style={{ color: 'var(--rbc-accent)' }}>{verdict.badgeText}</span>
                  <span className="flex-1" style={{ color: 'var(--rbc-text-secondary)' }}>
                    {verdict.detailText}
                  </span>
                  <span className="font-mono" style={{ color: 'var(--rbc-text-tertiary)' }}>
                    {verdict.state}
                  </span>
                </li>
              );
            })}
          </ul>
        </div>
      </section>
    </div>
  );
}
