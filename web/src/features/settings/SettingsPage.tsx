import { useEffect, useState } from 'react';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { Controller, useForm } from 'react-hook-form';
import type { RbConfig } from '@protocol/types';
import { useDriverStore } from '@state/driver-store';
import { confirmDialog } from '@state/confirm-store';
import { toast } from '@state/toast-store';
import { Badge, PageHeader, Toggle } from '@components/ui';

/** 合法缓存块大小（驱动 rbctl.c rb_legal_bs[] / rbrebuild.c 五档白名单同款） */
// [2026-11-07 BugFix] 79/83 号卷级免重启几何重建已落地：切档由驱动在卷级重建
// 引擎，无需整机重启；S2 全链重放真机验收 FAIL=0 / WARN=0。UI 恢复开放
// {4K,8K,16K,32K} 四档，与 WPF MemoryView.xaml.cs:20 及驱动白名单对齐。
// [64K-UNBAN 2026-11-08] 64K 恢复（59-64k-audit §5 前置清单满足，S4 五档矩阵
// 真机验证）；128K 依 76 号仍封禁，不出现在任何 UI。
const BLOCK_SIZES = [4096, 8192, 16384, 32768, 65536];

/**
 * 设置页（§11.5 迁移顺序第 5 页，六页收尾）。
 * 纪律：
 *  - SET_CONFIG 是**全量结构回写**：以 GET_CONFIG 原文为基底做读改写，
 *    只放开安全字段的编辑；未知/危险字段只读展示，防"表单没渲染的字段被 0 覆盖"。
 *  - [适配 2026-10-31] 表单迁移到 react-hook-form：dirty 跟踪/注册式校验/
 *    还原 = reset(defaults)，替代手写受控状态。
 *  - VolumeFilter 驱动明确忽略，仅展示并注明。
 */
export default function SettingsPage() {
  const transport = useDriverStore((s) => s.transport);
  const queryClient = useQueryClient();
  const [result, setResult] = useState<{ ok: boolean; text: string } | null>(null);
  const [saving, setSaving] = useState(false);

  const configQuery = useQuery({
    queryKey: ['rb', 'config'],
    queryFn: async () => {
      const r = await transport.getConfig();
      if (!r.ok) throw new Error(r.error.message);
      return r.value;
    },
    refetchInterval: 5000,
    refetchIntervalInBackground: false,
    retry: 0,
  });

  const form = useForm<RbConfig>();

  // [99 §3] RB2 Features\Flags 热开关（经 /rb/rbfeat：flags on/off 即时写注册表
  // 并由 rbfeatctl 内建 refresh 热载，无需重启）。
  const [rbfeat, setRbfeat] = useState<{ stats: Record<string, number> | null; l2: string; excl: string; exe: string } | null>(null);
  const [rbfeatBusy, setRbfeatBusy] = useState<string | undefined>(undefined);
  const loadRbfeat = () => {
    fetch('/rb/rbfeat')
      .then((r) => r.json())
      .then((d) => setRbfeat({ stats: d.stats ?? null, l2: String(d.l2 ?? ''), excl: String(d.excl ?? ''), exe: String(d.exe ?? '') }))
      .catch(() => setRbfeat(null));
  };
  useEffect(() => { loadRbfeat(); }, []);
  const setFlag = (name: string, on: boolean) => {
    setRbfeatBusy(name);
    fetch('/rb/rbfeat', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ name, on }),
    })
      .then((r) => r.json())
      .then((d) => {
        if (d.code !== 0) toast('danger', `RB2 ${name} 切换失败`, String(d.out ?? '').slice(0, 120));
        loadRbfeat();
      })
      .catch((e) => toast('danger', 'RB2 切换请求失败', String(e)))
      .finally(() => setRbfeatBusy(undefined));
  };
  const RB2_BITS: Record<string, number> = { PREFETCH: 1, HOT: 2, ETW: 32, THERMAL: 64 };
  const flagOn = (k: 'PREFETCH' | 'HOT' | 'THERMAL' | 'ETW') => {
    const f = rbfeat?.stats?.flags;
    const bit = RB2_BITS[k] ?? 0;
    return typeof f === 'number' ? (f & bit) !== 0 : false;
  };

  // 配置首次到达（或刷新）→ reset 进 RHF（dirty 基线随之更新）
  useEffect(() => {
    // [BUG-1 修复 2026-10-31] 轮询数据仅在"无未保存修改"时灌入表单：
    // 原实现每 5s 无条件 reset，用户编辑到一半会被轮询清空（实测复现路径）。
    if (configQuery.data && !form.formState.isDirty) form.reset(configQuery.data);
  }, [configQuery.data, form]);

  const dirty = form.formState.isDirty;

  const onSaved = form.handleSubmit(async (values) => {
    // 跨字段校验（RHF validate）：低水位不得超过高水位
    if (values.DirtyLowPct > values.DirtyHighPct) {
      form.setError('DirtyLowPct', { message: '低水位必须 ≤ 高水位' });
      return;
    }
    const okGo = await confirmDialog({
      title: '确认保存设置？',
      body: 'SET_CONFIG 为全量结构回写：未在表单中放开的字段将按当前驱动值原样写回。',
    });
    if (!okGo) return;
    setSaving(true);
    setResult(null);
    // [BUG-3 修复 2026-10-31] RHF 只包含已注册字段；未注册的 8 个字段
    // （Frozen/IdleFlush/SystemVolumeCached/VolumeFilter/Readahead…）若缺失，
    // 桥接 u32(undefined)=0 会把它们全部清零。以当前驱动值为基底合并，
    // 真正兑现"未放开字段原样写回"。
    const payload: RbConfig = { ...(configQuery.data as RbConfig), ...values };
    const r = await transport.setConfig(payload);
    setSaving(false);
    if (!r.ok) {
      setResult({ ok: false, text: `保存失败：${r.error.message}` });
      toast('danger', '设置保存失败', r.error.message);
      return;
    }
    setResult({ ok: true, text: '已保存（驱动返回 Status=0），配置查询将自动刷新' });
    toast('ok', '设置已保存', '配置查询将自动刷新');
    await queryClient.invalidateQueries({ queryKey: ['rb', 'config'] });
  });

  /* [USER-REQ 2026-11-05] 开机行为：默认不自动挂载任何卷（AutoAttach=0），
   * 但恢复用户上次手动加速过的盘（ResumeOnBoot=1）。走独立 IOCTL 0x80A，
   * 不属于 RB_CONFIG（协议冻结）。 */
  const bootCfgQuery = useQuery({
    queryKey: ['rb', 'bootcfg'],
    queryFn: async () => {
      const r = await fetch('/rb/bootcfg');
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      return (await r.json()) as { AutoAttach: number; ResumeOnBoot: number; ResumeLetters: string };
    },
    retry: 1,
  });
  const [bootBusy, setBootBusy] = useState(false);
  async function setBootPolicy(next: { AutoAttach?: number; ResumeOnBoot?: number }): Promise<void> {
    setBootBusy(true);
    try {
      const r = await fetch('/rb/bootcfg', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(next),
      });
      const j = (await r.json()) as { ok?: boolean; error?: string };
      if (j.ok) toast('ok', '开机策略已更新', '下次开机生效');
      else toast('danger', '更新失败', j.error ?? '未知错误');
      await bootCfgQuery.refetch();
    } catch (e) {
      toast('danger', '更新失败', (e as Error).message);
    } finally {
      setBootBusy(false);
    }
  }

  const cfg = configQuery.data;
  /* [UI-FIX 2026-11-05] 缓存上限标签联动：真正决定"动态/固定"的是 AutoMemory
   * 开关，而非 CacheSizeMB 是否为 0。原标签"（0=动态自动）"会让用户以为
   * 填 4096 就是动态模式——实际 AutoMemory=1 时该值仅作兜底。 */
  const autoMem = form.watch('AutoMemory');

  return (
    <div className="flex flex-col gap-4">
      <PageHeader
        title="设置"
        desc="SET_CONFIG 全量回写：未放开的字段按当前驱动值原样写回；危险字段只读"
        actions={
          <Badge tone="neutral">
            {configQuery.isFetching ? '查询中…' : '每 5s 查询'}
          </Badge>
        }
      />

      {configQuery.isPending && <p className="text-sm">正在读取配置…</p>}
      {configQuery.isError && (
        <p role="alert" className="text-sm" style={{ color: 'var(--rbc-danger-fg)' }}>
          配置读取失败：{(configQuery.error as Error).message}
        </p>
      )}

      {cfg && (
        <form onSubmit={onSaved}>
          <div className="grid grid-cols-2 gap-3">
            <Group title="缓存开关">
                            <Row label="总开关 Enabled">
                <Controller control={form.control} name="Enabled" render={({ field }) => (
                  <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="总开关 Enabled" />
                )} />
              </Row>
                            <Row label="读缓存 ReadCacheEnabled">
                <Controller control={form.control} name="ReadCacheEnabled" render={({ field }) => (
                  <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="读缓存 ReadCacheEnabled" />
                )} />
              </Row>
                            <Row label="写缓存 WriteCacheEnabled">
                <Controller control={form.control} name="WriteCacheEnabled" render={({ field }) => (
                  <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="写缓存 WriteCacheEnabled" />
                )} />
              </Row>
              <Select
                label="全局写模式 WriteBack（D2 红线：默认直写）"
                options={[
                  { value: 0, text: '直写（默认，安全）' },
                  { value: 1, text: '回写（性能，需知情）' },
                ]}
                {...form.register('WriteBack', { valueAsNumber: true })}
              />
            </Group>

            <Group title="容量与内存预算">
              <Num
                label={autoMem === 1 ? '缓存上限 CacheSizeMB（当前为动态，此值仅兜底）' : '缓存上限 CacheSizeMB（固定上限，MB）'}
                min={0}
                max={1048576}
                {...form.register('CacheSizeMB', { valueAsNumber: true, min: 0, max: 1048576 })}
              />
                            <Row label="自动内存预算 AutoMemory">
                <Controller control={form.control} name="AutoMemory" render={({ field }) => (
                  <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="自动内存预算 AutoMemory" />
                )} />
              </Row>
              <Num
                label="保留内存 MemReserveMB（0=自动按物理内存容量适配；手动 ≤65536）"
                min={0}
                max={65536}
                {...form.register('MemReserveMB', { valueAsNumber: true, min: 0, max: 65536 })}
              />
              <Num
                label="可用内存占用上限 AutoUsePct %（10-95）"
                min={10}
                max={95}
                {...form.register('AutoUsePct', { valueAsNumber: true, min: 10, max: 95 })}
              />
              <Select
                label="缓存块大小 BlockSize"
                options={[...(BLOCK_SIZES.includes(cfg.BlockSize) ? [] : [{ value: cfg.BlockSize, text: `${cfg.BlockSize / 1024} KB（当前值）` }]), ...BLOCK_SIZES.map((b) => ({ value: b, text: `${b / 1024} KB` }))]}
                {...form.register('BlockSize', { valueAsNumber: true })}
              />
              <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
                切换块大小会触发驱动内部引擎重建（无需重启整机）：重建期间缓存中的干净块会被丢弃，命中率短时归零；默认直写（WriteBack=0）下无脏数据、不丢数据。重建窗口内 I/O 回落至磁盘、性能会短暂下降，建议避开高负载时段切换。开放档位：4 KB / 8 KB / 16 KB / 32 KB / 64 KB（64K 档经 59 号前置审计+测试包后解禁，大块顺序场景 per-chunk 开销最低，小随机场景建议 4K）。
              </p>
            </Group>

            <Group title="回写与回收">
              <Num
                label="回写间隔 FlushIntervalMs（0=关闭周期回写）"
                min={0}
                max={600000}
                {...form.register('FlushIntervalMs', { valueAsNumber: true, min: 0, max: 600000 })}
              />
              <Num
                label="脏水位高 DirtyHighPct %（15-99）"
                min={15}
                max={99}
                {...form.register('DirtyHighPct', { valueAsNumber: true, min: 15, max: 99 })}
              />
              <Num
                label="脏水位低 DirtyLowPct %（5-89，须低于高水位）"
                min={5}
                max={89}
                {...form.register('DirtyLowPct', { valueAsNumber: true, min: 5, max: 89 })}
              />
                            <Row label="空闲回收 IdleReclaimEnabled">
                <Controller control={form.control} name="IdleReclaimEnabled" render={({ field }) => (
                  <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="空闲回收 IdleReclaimEnabled" />
                )} />
              </Row>
              <Num
                label="空闲回收阈值 IdleReclaimSec"
                min={0}
                max={86400}
                {...form.register('IdleReclaimSec', { valueAsNumber: true, min: 0, max: 86400 })}
              />
              <Select
                label="回写并行度 ParallelFlush（1–16 路 · 运行期生效）"
                options={[
                  { value: 1, text: '串行（1 路 · 默认）' },
                  { value: 2, text: '并行 2 路' },
                  { value: 4, text: '并行 4 路（历史档）' },
                  { value: 8, text: '并行 8 路' },
                  { value: 16, text: '并行 16 路（上限）' },
                ]}
                {...form.register('ParallelFlush', { valueAsNumber: true })}
              />
              <Select
                label="TinyLFU 频率准入（防顺序扫描冲掉热集 · 默认关）"
                options={[
                  { value: 0, text: '关闭（默认，行为与旧版一致）' },
                  { value: 1, text: '开启（内存压力态按访问频率准入）' },
                ]}
                {...form.register('TinyLFU', { valueAsNumber: true })}
              />
            </Group>

            <Group title="开机行为（默认：不自动加速 · 恢复上次选择）">
              <Row label="开机自动加速所有数据卷 AutoAttach">
                <Toggle
                  checked={bootCfgQuery.data?.AutoAttach === 1}
                  onChange={(b) => void setBootPolicy({ AutoAttach: b ? 1 : 0 })}
                  ariaLabel="开机自动加速所有数据卷"
                  disabled={bootBusy}
                />
              </Row>
              <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
                关闭时（默认）开机不加速任何卷，全部手动开启。系统卷始终只在手动操作下加速。
              </p>
              <Row label="开机恢复上次加速的卷 ResumeOnBoot">
                <Toggle
                  checked={bootCfgQuery.data?.ResumeOnBoot === 1}
                  onChange={(b) => void setBootPolicy({ ResumeOnBoot: b ? 1 : 0 })}
                  ariaLabel="开机恢复上次加速的卷"
                  disabled={bootBusy}
                />
              </Row>
              <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
                上次加速记录：{bootCfgQuery.data?.ResumeLetters
                  ? bootCfgQuery.data.ResumeLetters.split('').join('、') + ' 盘'
                  : '（暂无——手动开启任一卷后自动记录）'}
              </p>
            </Group>

            <Group title="预取与分页（真实生效开关）">
              <label className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
                <span>分页 IO 进缓存 CachePagingIo（运行期即时生效）</span>
                <Toggle
                  checked={!!form.watch('CachePagingIo')}
                  onChange={(b) => form.setValue('CachePagingIo', b ? 1 : 0, { shouldDirty: true })}
                  ariaLabel="CachePagingIo"
                />
              </label>
              <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
                驱动自 2026-11-06 起逐 IO 消费此开关（映射节读写/加载器读 DLL 走此门）；默认 0 = 分页 IO 直读盘不进缓存（11-06 微信 DLL 事故加固）。开启可提高映射文件读命中率，但分页流进入缓存爆炸半径。
              </p>
              <label className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
                <span>预取引擎 ReadaheadEnabled（创建期冻结）</span>
                <Toggle
                  checked={!!form.watch('ReadaheadEnabled')}
                  onChange={(b) => form.setValue('ReadaheadEnabled', b ? 1 : 0, { shouldDirty: true })}
                  ariaLabel="ReadaheadEnabled"
                />
              </label>
              <Num
                label="预取窗口 ReadaheadWindowKB（64–4096）"
                min={64}
                max={4096}
                {...form.register('ReadaheadWindowKB', { valueAsNumber: true })}
              />
              <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
                core 无运行期 setter：两项在下次几何重建/卷重挂（引擎重建）后生效，与 BlockSize 同纪律；保存即持久化。
              </p>
            </Group>

            <Group title="只读字段（高级/驱动侧语义，不在 Web 表单放开）">
              <ReadOnly label="SystemVolumeCached" value={cfg.SystemVolumeCached} note="系统卷已可在「卷加速」页手动开启（含回写，风险自负）" />
              <ReadOnly label="Frozen" value={cfg.Frozen} />
              <ReadOnly label="IdleFlush" value={cfg.IdleFlush} />
              <ReadOnly label="WriteRelease" value={cfg.WriteRelease} />
              <ReadOnly
                label="VolumeFilter"
                value={JSON.stringify(cfg.VolumeFilter)}
                note="驱动明确忽略（逐卷策略走 ATTACH_VOLUME）"
              />
            </Group>

            <Group title="特性开关（RB2 · Features\Flags · 热生效）">
              {([['PREFETCH', '预取（读 miss 窗口回填）'], ['HOT', '热区预热（Prewarm/衰减）'], ['THERMAL', '温度护栏（过热暂停预取）'], ['ETW', 'ETW 遥测']] as const).map(([k, label]) => (
                <label key={k} className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
                  <span>{label} · {k}</span>
                  <Toggle
                    checked={flagOn(k)}
                    disabled={rbfeatBusy === k || rbfeat?.exe !== 'found'}
                    onChange={(b) => setFlag(k, b)}
                    ariaLabel={`RB2 ${k}`}
                  />
                </label>
              ))}
              <ReadOnly label="WBCOAL（bit8）" value="已封禁" note="红线：M2 预留未接线；CLI 与 Web 双层拦截" />
              <ReadOnly label="L2 容器" value={(rbfeat?.l2 ?? '').split(String.fromCharCode(13, 10))[0] || '（不可达）'} note="启用走 CLI：rbfeatctl l2 enable <X:> <GB>（需卷/几何条件满足）" />
              <ReadOnly label="排除集" value={(rbfeat?.excl ?? '').replace(/\s+/g, ' ').slice(0, 60) || '（空）'} note="完整管理走 CLI：rbfeatctl excl" />
              {rbfeat?.exe !== 'found' && (
                <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
                  rbfeatctl.exe 不可达（payload 布局异常？）——本区开关停用，功能本体不受影响。
                </p>
              )}
            </Group>
          </div>

          <div className="mt-3 flex items-center gap-3">
            <button
              type="submit"
              disabled={!dirty || saving}
              className="rounded-[var(--rbc-radius-sm)] text-sm"
              style={{
                padding: 'var(--rbc-pad-btn-y) var(--rbc-pad-btn-x)',
                background: dirty && !saving ? 'var(--rbc-accent-subtle)' : 'var(--rbc-bg-input)',
                color: dirty && !saving ? 'var(--rbc-accent)' : 'var(--rbc-text-disabled)',
                border: '1px solid var(--rbc-border-strong)',
                cursor: dirty && !saving ? 'pointer' : 'not-allowed',
              }}
            >
              {saving ? '保存中…' : '保存设置'}
            </button>
            <button
              type="button"
              onClick={() => form.reset(configQuery.data)}
              disabled={!dirty || saving}
              className="rounded-[var(--rbc-radius-sm)] text-sm"
              style={{
                padding: 'var(--rbc-pad-btn-y) var(--rbc-pad-btn-x)',
                background: 'var(--rbc-bg-elevated)',
                color: dirty ? 'var(--rbc-text-secondary)' : 'var(--rbc-text-disabled)',
                border: '1px solid var(--rbc-border-strong)',
                cursor: dirty ? 'pointer' : 'not-allowed',
              }}
            >
              还原
            </button>
            {dirty && (
              <span className="text-xs" style={{ color: 'var(--rbc-warning-fg)' }}>
                有未保存修改
              </span>
            )}
          </div>

          {result && (
            <p
              role="status"
              className="mt-2 text-sm"
              style={{ color: result.ok ? 'var(--rbc-success-fg)' : 'var(--rbc-danger-fg)' }}
            >
              {result.text}
            </p>
          )}
        </form>
      )}
    </div>
  );
}

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
      <span>{label}</span>
      {children}
    </div>
  );
}

function Group({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <section
      className="rbc-card"
    >
      <h2 className="mb-2 text-sm" style={{ color: 'var(--rbc-text-primary)' }}>
        {title}
      </h2>
      <div className="flex flex-col gap-2">{children}</div>
    </section>
  );
}

function Num({
  label,
  min,
  max,
  ...rest
}: {
  label: string;
  min?: number | string;
  max?: number | string;
} & React.InputHTMLAttributes<HTMLInputElement>) {
  return (
    <label className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
      <span>{label}</span>
      <input type="number" min={min} max={max} className="rbc-input" style={{ width: 120 }} {...rest} />
    </label>
  );
}

function Select({
  label,
  options,
  ...rest
}: {
  label: string;
  options: Array<{ value: number; text: string }>;
} & React.SelectHTMLAttributes<HTMLSelectElement>) {
  return (
    <label className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
      <span>{label}</span>
      <select className="rbc-input" style={{ width: 160 }} {...rest}>
        {options.map((o) => (
          <option key={o.value} value={o.value}>
            {o.text}
          </option>
        ))}
      </select>
    </label>
  );
}

function ReadOnly({ label, value, note }: { label: string; value: string | number; note?: string }) {
  return (
    <div className="flex items-baseline justify-between gap-3 text-xs">
      <span style={{ color: 'var(--rbc-text-secondary)' }}>{label}</span>
      <span className="font-mono" style={{ color: 'var(--rbc-text-tertiary)' }} title={note}>
        {value}
        {note ? ' *' : ''}
      </span>
    </div>
  );
}
