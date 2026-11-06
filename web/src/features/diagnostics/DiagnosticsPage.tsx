import { useMemo, useRef, useState } from 'react';
import { useQuery } from '@tanstack/react-query';
import { useVirtualizer } from '@tanstack/react-virtual';
import { RB_LOG_LEVEL, RB_OP, RB_PHASE } from '@protocol/constants';
import type { RbEventRec, RbLogRec } from '@protocol/types';
import { useDriverStore } from '@state/driver-store';
import { toast } from '@state/toast-store';
import IoctlMatrix from '@components/IoctlMatrix';
import { Badge, Button, Icons, PageHeader } from '@components/ui';
import { filetimeToTime } from '@lib/time';

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

/**
 * 诊断页：
 *  - IOCTL 通道健康点阵（前端埋点 + P1-7 SLO）
 *  - 操作进度事件（500ms 游标增量轮询）
 *  - 驱动环形日志：级别筛选 + react-table v9 排序 + react-virtual 虚拟滚动
 *  - 诊断导出（一键抓 配置/统计/日志/事件 JSON 快照）
 */
export default function DiagnosticsPage() {
  const transport = useDriverStore((s) => s.transport);
  const reportError = useDriverStore((s) => s.reportError);
  const reportSuccess = useDriverStore((s) => s.reportSuccess);

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
      return (await r.json()) as { dir: string; exists: boolean; files: { name: string; size: number; mtime: number }[] };
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
      const j = (await r.json()) as { ok: boolean; error?: string };
      if (!j.ok) reportError({ code: 'UNREACHABLE', message: j.error ?? '打开失败' });
    } catch (e) {
      reportError({ code: 'UNREACHABLE', message: `打开日志目录失败：${(e as Error).message}` });
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
      const c = typeof av === 'string' && typeof bv === 'string'
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

  return (
    <div className="flex flex-col gap-4">
      <PageHeader
        title="诊断"
        desc="通道健康 / 操作事件 / 驱动日志——前端埋点 + 驱动侧双视角"
        actions={
          <>
            <Badge tone="neutral">12 通道 · P1-7 SLO</Badge>
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

      <section className="rbc-card">
        <header className="mb-3 flex items-center justify-between border-b pb-2" style={{ borderColor: 'var(--rbc-border)' }}>
          <h2 className="text-sm font-medium" style={{ color: 'var(--rbc-text-primary)' }}>
            IOCTL 通道健康
          </h2>
          <span className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
            前端埋点 · 最近 32 样本 P50/P95 · SLO 违约扫光
          </span>
        </header>
        <IoctlMatrix />
      </section>

      <section className="rbc-card rbc-fade-up-late">
        <header className="mb-2 flex items-baseline justify-between">
          <h2 className="text-sm font-medium" style={{ color: 'var(--rbc-text-primary)' }}>
            操作进度事件
          </h2>
          <span className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
            500ms 游标增量轮询 · 已缓存 {events.length} 条
          </span>
        </header>
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
      </section>

      <section className="rbc-card rbc-fade-up-late">
        <header className="mb-2 flex items-center gap-3">
          <h2 className="text-sm font-medium" style={{ color: 'var(--rbc-text-primary)' }}>
            驱动环形日志
          </h2>
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
          <Button tone="ghost" onClick={() => void openLogDir()} ariaLabel="在资源管理器中打开日志目录">
            打开日志目录
          </Button>
        </header>

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
              {([
                ['Ticks', '时间', 90],
                ['Level', '级别', 60],
                ['Text', '内容', 0],
                ['Status', '状态', 90],
              ] as const).map(([key, label, w]) => (
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
                      <span className="flex-1 truncate" style={{ color: 'var(--rbc-text-primary)' }} title={l.Text}>
                        {l.Text || '（空）'}
                      </span>
                      {l.Status !== 0 && (
                        <span style={{ color: 'var(--rbc-danger-fg)' }}>
                          0x{(l.Status >>> 0).toString(16).toUpperCase()}
                        </span>
                      )}
                    </div>
                  );
                })}
              </div>
            </div>
          </>
        )}

        {/* [FEAT-LOGUI] 落盘日志历史：重启后仍可回看（驱动内存环已清空时唯一来源） */}
        <div className="mt-4 border-t pt-3" style={{ borderColor: 'var(--rbc-border)' }}>
          <div className="mb-2 flex items-center gap-2">
            <h3 className="text-xs font-medium" style={{ color: 'var(--rbc-text-secondary)' }}>
              落盘历史（跨重启保留）
            </h3>
            {logFilesQuery.data && (
              <span className="truncate text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
                {logFilesQuery.data.exists ? logFilesQuery.data.dir : '目录尚未创建（驱动未产生日志）'}
              </span>
            )}
          </div>
          {logFilesQuery.isPending ? (
            <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>正在读取…</p>
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
            <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>正在读取 {openFile}…</p>
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
                  background: 'var(--rbc-surface-2)',
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
    </div>
  );
}
