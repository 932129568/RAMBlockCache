import { useEffect, useState } from 'react';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { Controller, useForm } from 'react-hook-form';
import type { RbConfig } from '@protocol/types';
import { useDriverStore } from '@state/driver-store';
import { confirmDialog } from '@state/confirm-store';
import { toast } from '@state/toast-store';
import { Badge, Hint, PageHeader, Toggle } from '@components/ui';

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
  const loadRbfeat = (): void => {
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
  const RB2_BITS: Record<string, number> = { PREFETCH: 1, HOT: 2, ETW: 32, THERMAL: 64, WBCOAL: 8 };
  const RB2_HINTS: Record<'PREFETCH' | 'HOT' | 'THERMAL' | 'ETW' | 'WBCOAL', string> = {
    PREFETCH: '预取引擎开关：读缓存未命中时按窗口向前回填，连续读场景显著提速。实时热生效。',
    HOT: '热区追踪与预热：统计高频访问的磁盘区域，冷启动/重启后优先预热这些区域，让缓存更快进入高命中状态。',
    THERMAL: '温度护栏：监测磁盘温度，过热时自动暂停预取，避免长时间满速预取导致磁盘过热降速。',
    ETW: 'ETW 内核事件遥测：输出驱动内部事件跟踪，供性能分析与问题诊断使用；日常使用无需开启。',
    WBCOAL: '回写合写调度：回写模式下按每卷限速节奏把脏数据合并成顺序写落盘，平滑写延迟、减少随机写碎片。与 L2 二级缓存互斥，二者只能开一个。',
  };
  const flagOn = (k: 'PREFETCH' | 'HOT' | 'THERMAL' | 'ETW' | 'WBCOAL') => {
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

  return (
    <div className="flex flex-col gap-4">
      <PageHeader
        title="设置"
        desc="缓存引擎全局设置"
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
                            <Row label="总开关 Enabled" hint="缓存引擎总开关：关闭后所有读写直接访问磁盘（全局旁路），所有缓存功能停用；重新开启即时生效，无需重启。">
                <Controller control={form.control} name="Enabled" render={({ field }) => (
                  <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="总开关 Enabled" />
                )} />
              </Row>
                            <Row label="读缓存 ReadCacheEnabled" hint="读缓存：磁盘读到的数据存入内存，同一位置再次读取时直接从内存返回。重复读、程序启动、常用文件访问提速最明显，顺序大文件读取可达数倍提升。">
                <Controller control={form.control} name="ReadCacheEnabled" render={({ field }) => (
                  <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="读缓存 ReadCacheEnabled" />
                )} />
              </Row>
                            <Row label="写缓存 WriteCacheEnabled" hint="写缓存：写请求经过缓存引擎。直写模式下写入即落盘；回写模式下写入先进缓存、后台落盘。关闭后写 I/O 完全旁路、不经过缓存。">
                <Controller control={form.control} name="WriteCacheEnabled" render={({ field }) => (
                  <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="写缓存 WriteCacheEnabled" />
                )} />
              </Row>
              <Select
                label="全局写模式 WriteBack"
                hint="直写：写入缓存的同时立即写盘，任何时刻断电/崩溃都不丢数据，写延迟与裸盘相当。回写：写入只需进入缓存即完成，后台按节奏合并落盘，写延迟大幅降低、写吞吐提升；代价是异常断电时最近几秒未落盘的数据可能丢失。每个卷可在「卷加速」页单独覆盖此全局设置。"
                options={[
                  { value: 0, text: '直写（写入即落盘）' },
                  { value: 1, text: '回写（写延迟更低）' },
                ]}
                {...form.register('WriteBack', { valueAsNumber: true })}
              />
            </Group>

            <Group title="容量与内存预算">
              <Num
                label="缓存上限 CacheSizeMB (MB)"
                hint="缓存可占用的最大内存（MB）。「自动内存预算」开启时此值仅作为上限兜底，实际容量随系统可用内存动态伸缩；关闭时按此值固定分配。缓存越大，能装下的热数据越多，命中率越高。"
                min={0}
                max={1048576}
                {...form.register('CacheSizeMB', { valueAsNumber: true, min: 0, max: 1048576 })}
              />
                            <Row label="自动内存预算 AutoMemory" hint="按系统实际可用内存自动伸缩缓存容量：内存紧张时自动让出内存给其他程序，空闲时自动扩展。日常使用推荐开启；专用缓存机可用固定值。">
                <Controller control={form.control} name="AutoMemory" render={({ field }) => (
                  <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="自动内存预算 AutoMemory" />
                )} />
              </Row>
              <Num
                label="保留内存 MemReserveMB (MB)"
                hint="为缓存引擎预留的内存池大小（MB），0=按物理内存容量自动适配。预留可避免高负载时缓存内存分配失败导致性能波动。"
                min={0}
                max={65536}
                {...form.register('MemReserveMB', { valueAsNumber: true, min: 0, max: 65536 })}
              />
              <Num
                label="可用内存占用上限 AutoUsePct %"
                hint="允许缓存占用的「系统可用内存」百分比上限。调高=缓存更大、命中更多；调低=给其他程序留更多内存余量。"
                min={10}
                max={95}
                {...form.register('AutoUsePct', { valueAsNumber: true, min: 10, max: 95 })}
              />
              <Select
                label="缓存块大小 BlockSize"
                hint="缓存引擎内部的数据块粒度（实际块=此值×16）。4K 小块适合随机小 I/O（数据库、程序加载、大量小文件）；64K 大块适合顺序大文件（视频剪辑、大文件拷贝）。切换免重启：引擎后台重建，重建期间缓存性能短暂回落。"
                options={[...(BLOCK_SIZES.includes(cfg.BlockSize) ? [] : [{ value: cfg.BlockSize, text: `${cfg.BlockSize / 1024} KB（当前值）` }]), ...BLOCK_SIZES.map((b) => ({ value: b, text: `${b / 1024} KB` }))]}
                {...form.register('BlockSize', { valueAsNumber: true })}
              />
              <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
                缓存引擎的数据块粒度：小块利于随机读写，大块利于顺序读写，切换免重启。
              </p>
            </Group>

            <Group title="回写与回收">
              <Num
                label="回写落盘间隔 FlushIntervalMs（毫秒，0=关闭）"
                hint="回写模式下，后台把缓存中的脏数据写回磁盘的周期（毫秒）。调小=落盘更频繁、断电时可能丢失的数据更少；调大=合并更充分、写入效率更高。0=关闭周期落盘（仅按脏水位触发）。直写模式没有脏数据，此项不生效。"
                min={0}
                max={600000}
                {...form.register('FlushIntervalMs', { valueAsNumber: true, min: 0, max: 600000 })}
              />
              <Num
                label="脏水位高 DirtyHighPct %"
                hint="脏数据占缓存容量的比例上限（%）。达到后驱动加速落盘，防止脏数据堆积过多、缩小断电丢失窗口。"
                min={15}
                max={99}
                {...form.register('DirtyHighPct', { valueAsNumber: true, min: 15, max: 99 })}
              />
              <Num
                label="脏水位低 DirtyLowPct %"
                hint="加速落盘进行到脏比例回落到此值（%）即停止冲刺（须低于高水位）。一冲一落形成滞后区间，避免落盘频繁启停。"
                min={5}
                max={89}
                {...form.register('DirtyLowPct', { valueAsNumber: true, min: 5, max: 89 })}
              />
                            <Row label="空闲回收 IdleReclaimEnabled" hint="系统空闲时自动回收长期没有被访问的缓存块，把内存还给其他程序。">
                <Controller control={form.control} name="IdleReclaimEnabled" render={({ field }) => (
                  <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="空闲回收 IdleReclaimEnabled" />
                )} />
              </Row>
              <Num
                label="空闲回收阈值 IdleReclaimSec"
                hint="缓存块多久未被访问后视为「长期未使用」（秒），是空闲回收的判定阈值。"
                min={0}
                max={86400}
                {...form.register('IdleReclaimSec', { valueAsNumber: true, min: 0, max: 86400 })}
              />
              <Select
                label="回写落盘并行度 ParallelFlush"
                hint="回写脏数据落盘的并发路数。并行越高回写吞吐越高（最终受磁盘写入能力限制）；直写模式没有脏数据、不消费此参数；运行期修改即时生效。"
                options={[
                  { value: 1, text: '1 路' },
                  { value: 2, text: '2 路' },
                  { value: 4, text: '4 路' },
                  { value: 8, text: '8 路' },
                  { value: 16, text: '16 路' },
                  { value: 20, text: '20 路' },
                  { value: 32, text: '32 路' },
                ]}
                {...form.register('ParallelFlush', { valueAsNumber: true })}
              />
              <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
                回写脏数据并发落盘的路数：并行越高，回写吞吐越高。
              </p>
              <Select
                label="TinyLFU 频率准入"
                hint="准入过滤器：内存压力下按「访问频率」决定新数据是否进缓存。开启后，一次性的大文件拷贝、杀毒扫描等顺序流量不再把常用热数据挤出缓存。"
                options={[
                  { value: 0, text: '关闭' },
                  { value: 1, text: '开启' },
                ]}
                {...form.register('TinyLFU', { valueAsNumber: true })}
              />
              <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
                按访问频率决定数据是否进缓存：顺序扫描不再冲掉热数据。
              </p>
            </Group>

            <Group title="开机行为">
              <Row label="开机自动加速所有数据卷 AutoAttach" hint="开机自动加速所有数据卷（Windows 系统盘除外）。关闭时每次开机手动打开需要的卷。">
                <Toggle
                  checked={bootCfgQuery.data?.AutoAttach === 1}
                  onChange={(b) => void setBootPolicy({ AutoAttach: b ? 1 : 0 })}
                  ariaLabel="开机自动加速所有数据卷"
                  disabled={bootBusy}
                />
              </Row>
              <Row label="开机恢复上次加速的卷 ResumeOnBoot" hint="开机自动恢复上次加速过的卷（下方显示当前记录）。比「开机自动加速所有卷」更可控：只恢复你明确开启过的。">
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

            <Group title="预取与分页">
              <label className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
                <span><Hint text="内存映射文件的读写（程序加载 DLL、映射大文件）也纳入缓存：提高这类文件的读取命中率。关闭时映射 I/O 直接读写磁盘、不占缓存。">分页 IO 进缓存 CachePagingIo</Hint></span>
                <Toggle
                  checked={!!form.watch('CachePagingIo')}
                  onChange={(b) => form.setValue('CachePagingIo', b ? 1 : 0, { shouldDirty: true })}
                  ariaLabel="CachePagingIo"
                />
              </label>
              <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
                映射文件读写（如加载 DLL）进入缓存：提高映射文件读取命中率。
              </p>
              <label className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
                <span><Hint text="预取引擎：检测到顺序读取模式时，提前把后续数据读入缓存。大文件拷贝、连续读取场景显著提速。">预取引擎 ReadaheadEnabled</Hint></span>
                <Toggle
                  checked={!!form.watch('ReadaheadEnabled')}
                  onChange={(b) => form.setValue('ReadaheadEnabled', b ? 1 : 0, { shouldDirty: true })}
                  ariaLabel="ReadaheadEnabled"
                />
              </label>
              <Num
                label="预取窗口 ReadaheadWindowKB (KB)"
                hint="每次预取向前多读的窗口大小（KB）。窗口越大顺序读越快，占用内存也越多。"
                min={64}
                max={4096}
                {...form.register('ReadaheadWindowKB', { valueAsNumber: true })}
              />
              <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
                提前读入即将访问的数据：提升连续读速度。
              </p>
            </Group>

            <Group title="只读字段">
              <ReadOnly label="SystemVolumeCached" value={cfg.SystemVolumeCached} hint="Windows 系统盘是否被缓存。出于安全默认不缓存；可在「卷加速」页手动开启。" />
              <ReadOnly label="Frozen" value={cfg.Frozen} hint="1=引擎处于冻结/重建窗口，此时 I/O 走旁路。" />
              <ReadOnly label="IdleFlush" value={cfg.IdleFlush} hint="空闲回写开关的当前状态（驱动内部语义）。" />
              <ReadOnly label="WriteRelease" value={cfg.WriteRelease} hint="写释放模式状态（驱动内部语义）。" />
              <ReadOnly
                label="VolumeFilter"
                value={JSON.stringify(cfg.VolumeFilter)}
                hint="卷过滤白名单（本版本驱动不消费此字段；逐卷策略在「卷加速」页设置）。"
              />
            </Group>

            <Group title="特性开关">
              {([['PREFETCH', '预取（提升连续读）'], ['HOT', '热区预热（冷启动更快命中）'], ['THERMAL', '温度护栏（过热暂停预取）'], ['ETW', 'ETW 遥测'], ['WBCOAL', '回写合写（平滑写延迟）']] as const).map(([k, label]) => (
                <label key={k} className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
                  <Hint text={RB2_HINTS[k]}>{label} · {k}</Hint>
                  <Toggle
                    checked={flagOn(k)}
                    disabled={rbfeatBusy === k || rbfeat?.exe !== 'found'}
                    onChange={(b) => setFlag(k, b)}
                    ariaLabel={`RB2 ${k}`}
                  />
                </label>
              ))}
              <L2Manager rbfeat={rbfeat} reload={loadRbfeat} />
              <ExclManager rbfeat={rbfeat} reload={loadRbfeat} />
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

function Row({ label, hint, children }: { label: string; hint?: string; children: React.ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
      <span>{hint ? <Hint text={hint}>{label}</Hint> : label}</span>
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
  hint,
  min,
  max,
  ...rest
}: {
  label: string;
  hint?: string;
  min?: number | string;
  max?: number | string;
} & React.InputHTMLAttributes<HTMLInputElement>) {
  return (
    <label className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
      <span>{hint ? <Hint text={hint}>{label}</Hint> : label}</span>
      <input type="number" min={min} max={max} className="rbc-input" style={{ width: 120 }} {...rest} />
    </label>
  );
}

function Select({
  label,
  hint,
  options,
  ...rest
}: {
  label: string;
  hint?: string;
  options: Array<{ value: number; text: string }>;
} & React.SelectHTMLAttributes<HTMLSelectElement>) {
  return (
    <label className="flex items-center justify-between gap-3 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
      <span>{hint ? <Hint text={hint}>{label}</Hint> : label}</span>
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

type RbfeatState = { stats: Record<string, number> | null; l2: string; excl: string; exe: string } | null;

function L2Manager({ rbfeat, reload }: { rbfeat: RbfeatState; reload: () => void }) {
  const [letter, setLetter] = useState('');
  const [gb, setGb] = useState(16);
  const [busy, setBusy] = useState(false);
  const l2Line = (rbfeat?.l2 ?? '').split(String.fromCharCode(13, 10)).filter(Boolean)[0] || '（不可达）';
  const call = (op: string) => {
    setBusy(true);
    fetch('/rb/l2', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ op, letter, gb }) })
      .then((r) => r.json())
      .then((d) => { if (d.code !== 0) toast('danger', `L2 ${op} 失败`, String(d.out ?? '').slice(0, 160)); reload(); })
      .catch((e) => toast('danger', 'L2 请求失败', String(e)))
      .finally(() => { setBusy(false); reload(); });
  };
  return (
    <>
      <div className="flex items-baseline justify-between gap-3 text-xs">
        <span style={{ color: 'var(--rbc-text-secondary)' }}>
          <Hint text="L2 二级读缓存：内存缓存淘汰下来的热数据写入卷上的容器文件，再次读取时先查 L2、未中再读盘。内存是第一级、L2 是第二级，适合「工作集大于内存但重复读多」的场景。仅 4K 块几何可用、单加速卷、与 WBCOAL 互斥。">L2 容器状态</Hint>
        </span>
        <span className="font-mono" style={{ color: 'var(--rbc-text-tertiary)' }}>{l2Line}</span>
      </div>
      <div className="flex items-center gap-2 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
        <span>容器卷</span>
        <input className="rbc-input" style={{ width: 64 }} maxLength={1} value={letter}
               onChange={(e) => setLetter(e.target.value.toUpperCase().replace(/[^A-Z]/g, ''))} placeholder="X" />
        <span>容量 GB</span>
        <input type="number" className="rbc-input" style={{ width: 88 }} min={1} max={512} value={gb}
               onChange={(e) => setGb(Math.max(1, Math.min(512, Number(e.target.value) || 1)))} />
        <button type="button" className="rbc-input" disabled={busy || !letter || rbfeat?.exe !== 'found'}
                title="把当前容器卷/容量写入配置并立即生效"
                onClick={() => call('enable')}>启用</button>
        <button type="button" className="rbc-input" disabled={busy || rbfeat?.exe !== 'found'}
                title="关闭 L2 并保存索引（容器文件保留，可再次启用）"
                onClick={() => call('disable')}>停用</button>
      </div>
      <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
        L2 二级读缓存：淘汰的热数据存入卷上容器文件，重复读取直接命中，提升二次读速度。
      </p>
    </>
  );
}

function ExclManager({ rbfeat, reload }: { rbfeat: RbfeatState; reload: () => void }) {
  const [name, setName] = useState('');
  const [busy, setBusy] = useState(false);
  const call = (op: string, extra?: Record<string, unknown>) => {
    setBusy(true);
    fetch('/rb/excl', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ op, name, ...extra }) })
      .then((r) => r.json())
      .then((d) => { if (d.code !== 0) toast('danger', `排除集 ${op} 失败`, String(d.out ?? '').slice(0, 160)); reload(); })
      .catch((e) => toast('danger', '排除集请求失败', String(e)))
      .finally(() => setBusy(false));
  };
  const useDef = /UseDefaultExcl=(1)/.test(rbfeat?.excl ?? '');
  const entries = (rbfeat?.excl ?? '').split(String.fromCharCode(13, 10)).filter((l) => /^  \S/.test(l)).map((l) => l.trim());
  return (
    <>
      <div className="flex items-baseline justify-between gap-3 text-xs">
        <span style={{ color: 'var(--rbc-text-secondary)' }}>
          <Hint text="进程排除集：被排除进程（如杀毒扫描、磁盘碎片整理）的读写绕过缓存直达磁盘，避免它们的海量顺序流量污染缓存、挤掉热数据。内置默认排除：defrag/dfrgui/wbengine/vssadmin/msmpeng。">内置排除（defrag/dfrgui/wbengine/vssadmin/msmpeng）</Hint>
        </span>
        <Toggle checked={useDef} disabled={busy || rbfeat?.exe !== 'found'}
                onChange={(b) => call('def', { on: b })} ariaLabel="UseDefaultExcl" />
      </div>
      <div className="flex items-center gap-2 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
        <span>进程排除</span>
        <input className="rbc-input" style={{ width: 180 }} value={name} maxLength={60}
               onChange={(e) => setName(e.target.value)} placeholder="如 Weixin.exe" />
        <button type="button" className="rbc-input" disabled={busy || !name.trim() || rbfeat?.exe !== 'found'}
                onClick={() => { call('add'); setName(''); }}>加入</button>
      </div>
      <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)', margin: '-4px 0 8px' }}>
        当前自定义排除：{entries.length ? entries.join('、') : '（空）'}——命中进程的读写不进缓存。
      </p>
    </>
  );
}

function ReadOnly({ label, value, hint }: { label: string; value: string | number; hint?: string }) {
  return (
    <div className="flex items-baseline justify-between gap-3 text-xs">
      <span style={{ color: 'var(--rbc-text-secondary)' }}>{hint ? <Hint text={hint}>{label}</Hint> : label}</span>
      <span className="font-mono" style={{ color: 'var(--rbc-text-tertiary)' }}>
        {value}
      </span>
    </div>
  );
}
