import { useEffect, useState, type ReactNode } from 'react';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { Controller, useForm } from 'react-hook-form';
import type { RbConfig } from '@protocol/types';
import { useDriverStore } from '@state/driver-store';
import { confirmDialog } from '@state/confirm-store';
import { toast } from '@state/toast-store';
import { Badge, Button, Hint, Icons, PageHeader, Skeleton, Toggle } from '@components/ui';
import { describeDiff, diffConfig } from './config-diff';
import {
  commitSequentialEngineSettings,
  PartialSettingsCommitError,
} from '../../bridge/config-orchestrator';

/** 合法缓存块大小（驱动 rbctl.c rb_legal_bs[] / rbrebuild.c 五档白名单同款） */
// [2026-11-07 BugFix] 79/83 号卷级免重启几何重建已落地：切档由驱动在卷级重建
// 引擎，无需整机重启；S2 全链重放真机验收 FAIL=0 / WARN=0。UI 恢复开放
// {4K,8K,16K,32K} 四档，与 WPF MemoryView.xaml.cs:20 及驱动白名单对齐。
// [64K-UNBAN 2026-11-08] 64K 恢复（59-64k-audit §5 前置清单满足，S4 五档矩阵
// 真机验证）；128K 依 76 号仍封禁，不出现在任何 UI。
const BLOCK_SIZES = [4096, 8192, 16384, 32768, 65536];

/**
 * 设置页 —— 2026-11-12 分段重构（六页收尾）
 *
 * 纪律（与功能无关，必须保住）：
 *  - SET_CONFIG 是**全量结构回写**：以 GET_CONFIG 原文为基底做读改写，
 *    只放开安全字段的编辑；未知/危险字段只读展示，防"表单没渲染的字段被 0 覆盖"。
 *  - [适配 2026-10-31] 表单迁移到 react-hook-form：dirty 跟踪/注册式校验/
 *    还原 = reset(defaults)，替代手写受控状态。
 *  - VolumeFilter 驱动明确忽略，仅展示并注明。
 *
 * 为什么改结构
 * ------------
 * 改前是一个 `grid-cols-2` 装 7 个 `Group` 卡，阅读顺序是"左-右-左-右"，
 * 于是：缓存开关 → 容量预算 → 回写回收 → 开机行为 → 预取分页 → 只读字段 →
 * 特性开关。**"开机行为"被夹在"回写回收"和"预取分页"中间**，只因为它恰好
 * 是第 4 个写的；而"特性开关"（含 L2 与排除集，最危险的一块）在最后一格右下角。
 *
 * 改后按**用户心智顺序**分段，每段一个 `.rbc-section`：
 *   ① 结论   —— 现在这套配置的实际姿态（一句话：开关/写模式/块大小/上限）
 *   ② 开关   —— 谁能用缓存
 *   ③ 容量   —— 内存预算与块粒度
 *   ④ 回写   —— 落盘节奏与回收
 *   ⑤ 预取   —— 顺序读与映射 I/O
 *   ⑥ 开机   —— 自动加速策略
 *   ⑦ 特性   —— RB2 热开关 / L2 / 排除集（改动最"重"，单独成段）
 *   ⑧ 只读   —— 驱动回报的内部状态，排查用，降到末尾
 *
 * 另两处：`grid-cols-2` 写死 → `grid-cols-1 xl:grid-cols-2`（703px 下两列
 * 各 340px，标签+控件挤在一起）；保存条改 sticky 贴底（600 行表单改完一半
 * 还要滚到底才能保存，是这一页最实际的痛点）。
 *
 * 还有一处删减：8 处说明文字与 Hint 提示**逐字重复**（BlockSize / ParallelFlush /
 * TinyLFU / CachePagingIo / ReadaheadWindowKB / L2 各一条），既占版面又不加信息。
 * 只保留真正携带**动态值**的说明（上次加速记录、当前排除集、L2 状态行）。
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
  const [rbfeat, setRbfeat] = useState<{
    stats: Record<string, number> | null;
    l2: string;
    excl: string;
    exe: string;
  } | null>(null);
  const [rbfeatBusy, setRbfeatBusy] = useState<string | undefined>(undefined);
  const loadRbfeat = (): void => {
    fetch('/rb/rbfeat')
      .then((r) => r.json())
      .then((d) =>
        setRbfeat({
          stats: d.stats ?? null,
          l2: String(d.l2 ?? ''),
          excl: String(d.excl ?? ''),
          exe: String(d.exe ?? ''),
        }),
      )
      .catch(() => setRbfeat(null));
  };
  useEffect(() => {
    loadRbfeat();
  }, []);
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
    WBCOAL:
      '回写合写调度：回写模式下按每卷限速节奏把脏数据合并成顺序写落盘，平滑写延迟、减少随机写碎片。与 L2 二级缓存互斥，二者只能开一个。',
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
    // [240 / 239 sec 4.3] R6 boundary gate. The picker only offers the five
    // legal sizes, but SET_CONFIG is a full struct write -- a stale form value,
    // a replayed request or a future field must never push 131072 to the driver.
    // Gate23 (check_ts_proto_sync.py) keeps BLOCK_SIZES itself honest.
    if (!BLOCK_SIZES.includes(values.BlockSize)) {
      form.setError('BlockSize', {
        message: '块大小仅允许 4K/8K/16K/32K/64K；131072 按红线 R6 永久禁用',
      });
      return;
    }
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
    /* [243 / C01] 单点串行提交：engine config -> 开机策略 -> 特性位。
     * 旧实现只发 SET_CONFIG，开机策略与特性位靠用户在别处另点，界面显示的
     * “已保存”并不等于三处一致。bootCfg 只在查询已就绪时传：查询未加载时
     * 传 {0,0} 会把用户已开启的「开机恢复上次加速的卷」静默清掉。 */
    try {
      await commitSequentialEngineSettings((fullCfg) => transport.setConfig(fullCfg), {
        config: payload,
        bootCfg: bootCfgQuery.data
          ? {
              AutoAttach: bootCfgQuery.data.AutoAttach ? 1 : 0,
              ResumeOnBoot: bootCfgQuery.data.ResumeOnBoot ? 1 : 0,
            }
          : undefined,
      });
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err);
      const partialApplied = err instanceof PartialSettingsCommitError && err.engineConfigApplied;
      const latest = await transport.getConfig().catch(() => null);
      if (latest?.ok) {
        form.reset(latest.value);
      }
      const detail = partialApplied
        ? `引擎核心配置已写入生效，但后续同步失败：${msg}`
        : `保存失败：${msg}`;
      setResult({ ok: false, text: detail });
      toast('danger', partialApplied ? '部分配置同步失败' : '设置保存失败', detail);
      return;
    } finally {
      setSaving(false);
    }
    /* [R19 2026-11-13] 回读核对「请求值 vs 驱动实际生效值」。
     * Status=0 只说明驱动收下了这次写入；驱动对多处字段**静默** clamp
     * （L2 容量 >128GB 截断、BlockSize 白名单回落、脏页水位纠正…）。
     * 旧实现无条件提示「已保存」，等于替驱动担保了它没承诺的事。
     * 这里显式重查一次配置，逐字段比对：不一致就点名，回读失败也不许说"已生效"。 */
    let applied: RbConfig | null = null;
    try {
      const rb = await transport.getConfig();
      if (rb.ok) applied = rb.value;
    } catch {
      /* 下面统一处理：applied 仍为 null ⇒ 判定为"无法确认" */
    }
    await queryClient.invalidateQueries({ queryKey: ['rb', 'config'] });

    if (!applied) {
      setResult({
        ok: false,
        text: '驱动已接受写入（Status=0），但配置回读失败：无法确认实际生效值，请点「刷新」核对后再依赖这些设置',
      });
      toast('warn', '设置已写入，生效值未确认', '回读失败，请手动核对');
      return;
    }

    const diffs = diffConfig(payload, applied);
    if (diffs.length === 0) {
      setResult({ ok: true, text: '已保存，回读核对通过：驱动实际生效值与请求完全一致' });
      toast('ok', '设置已保存', '回读核对一致');
      return;
    }
    setResult({
      ok: false,
      text: `已写入，但驱动实际生效值与请求不一致（${diffs.length} 项）：\n${describeDiff(diffs)}`,
    });
    toast('warn', `设置已写入，但有 ${diffs.length} 项被驱动调整`, '详见下方逐项对照');
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

  /* ① 结论：这套配置现在实际的姿态。四个字段决定 90% 的行为，
   * 让用户不用在一屏 20 个控件里自己拼。 */
  const posture = cfg
    ? [
        cfg.Enabled === 1 ? '缓存已启用' : '缓存已关闭（全局旁路）',
        cfg.ReadCacheEnabled === 1 ? '读缓存开' : '读缓存关',
        cfg.WriteCacheEnabled === 1 ? '写缓存开' : '写缓存关',
        cfg.WriteBack === 1 ? '回写' : '直写',
        `块 ${cfg.BlockSize / 1024} KB`,
        /* CacheSizeMB=0 是"完全交给自动预算"的合法取值（驱动按可用内存伸缩）。
         * 改前无条件拼成「上限 0 MB」，读起来像"缓存上限是 0"，与紧邻的
         * 「缓存已启用」自相矛盾。 */
        cfg.AutoMemory === 1
          ? cfg.CacheSizeMB > 0
            ? `自动预算（上限 ${cfg.CacheSizeMB} MB）`
            : '自动预算（不设上限）'
          : `固定 ${cfg.CacheSizeMB} MB`,
      ].join(' · ')
    : null;

  if (configQuery.isPending) {
    return (
      <div className="flex flex-col gap-6" aria-busy>
        <PageHeader title="设置" desc="缓存引擎全局设置" />
        {[0, 1, 2].map((i) => (
          <div key={i} className="rbc-card flex flex-col gap-3">
            <Skeleton w="30%" h={12} />
            <Skeleton w="65%" h={14} />
            <Skeleton w="55%" h={14} />
          </div>
        ))}
      </div>
    );
  }

  if (configQuery.isError) {
    return (
      <div className="flex flex-col gap-4">
        <PageHeader title="设置" desc="缓存引擎全局设置" />
        <div role="alert" className="rbc-card flex flex-col items-start gap-2 p-5" style={{ borderColor: 'var(--rbc-danger-border)' }}>
          <span className="text-sm" style={{ color: 'var(--rbc-danger-fg)' }}>
            配置读取失败
          </span>
          <span className="text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
            {(configQuery.error as Error).message}
          </span>
          <Button tone="secondary" icon={<Icons.refresh size={14} />} onClick={() => void configQuery.refetch()}>
            重试
          </Button>
        </div>
      </div>
    );
  }

  return (
    <div className="flex flex-col gap-6">
      <PageHeader
        title="设置"
        desc="缓存引擎全局设置——改动需点底部「保存设置」才写入驱动"
        actions={
          <>
            {dirty && <Badge tone="warn">有未保存修改</Badge>}
            <Badge tone="neutral">{configQuery.isFetching ? '查询中…' : '每 5s 查询'}</Badge>
          </>
        }
      />

      {posture && (
        <p className="rbc-fade-up text-sm" style={{ color: 'var(--rbc-text-secondary)' }}>
          {posture}
        </p>
      )}

      <form onSubmit={onSaved} className="flex flex-col gap-6">
        {/* ② 开关 */}
        <Section kicker="开关 · 谁能用缓存" note="关闭总开关 = 全局旁路，其余开关全部失效">
          <Row
            label="总开关 Enabled"
            hint="缓存引擎总开关：关闭后所有读写直接访问磁盘（全局旁路），所有缓存功能停用；重新开启即时生效，无需重启。"
          >
            <Controller
              control={form.control}
              name="Enabled"
              render={({ field }) => (
                <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="总开关 Enabled" />
              )}
            />
          </Row>
          <Row
            label="读缓存 ReadCacheEnabled"
            hint="读缓存：磁盘读到的数据存入内存，同一位置再次读取时直接从内存返回。重复读、程序启动、常用文件访问提速最明显，顺序大文件读取可达数倍提升。"
          >
            <Controller
              control={form.control}
              name="ReadCacheEnabled"
              render={({ field }) => (
                <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="读缓存 ReadCacheEnabled" />
              )}
            />
          </Row>
          <Row
            label="写缓存 WriteCacheEnabled"
            hint="写缓存：写请求经过缓存引擎。直写模式下写入即落盘；回写模式下写入先进缓存、后台落盘。关闭后写 I/O 完全旁路、不经过缓存。"
          >
            <Controller
              control={form.control}
              name="WriteCacheEnabled"
              render={({ field }) => (
                <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="写缓存 WriteCacheEnabled" />
              )}
            />
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
        </Section>

        {/* ③ 容量 */}
        <Section kicker="容量 · 内存预算" note="缓存越大命中越高，但会与其它程序抢内存">
          <Num
            label="缓存上限 CacheSizeMB (MB)"
            hint="缓存可占用的最大内存（MB）。「自动内存预算」开启时此值仅作为上限兜底，实际容量随系统可用内存动态伸缩；关闭时按此值固定分配。"
            min={0}
            max={1048576}
            {...form.register('CacheSizeMB', { valueAsNumber: true, min: 0, max: 1048576 })}
          />
          <Row
            label="自动内存预算 AutoMemory"
            hint="按系统实际可用内存自动伸缩缓存容量：内存紧张时自动让出内存给其他程序，空闲时自动扩展。日常使用推荐开启；专用缓存机可用固定值。"
          >
            <Controller
              control={form.control}
              name="AutoMemory"
              render={({ field }) => (
                <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="自动内存预算 AutoMemory" />
              )}
            />
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
            options={[
              ...(BLOCK_SIZES.includes(cfg!.BlockSize)
                ? []
                : [{ value: cfg!.BlockSize, text: `${cfg!.BlockSize / 1024} KB（当前值）` }]),
              ...BLOCK_SIZES.map((b) => ({ value: b, text: `${b / 1024} KB` })),
            ]}
            {...form.register('BlockSize', { valueAsNumber: true })}
          />
        </Section>

        {/* ④ 回写 */}
        <Section kicker="回写 · 落盘与回收" note="直写模式下这一段的多数项不生效">
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
          <Row
            label="空闲回收 IdleReclaimEnabled"
            hint="系统空闲时自动回收长期没有被访问的缓存块，把内存还给其他程序。"
          >
            <Controller
              control={form.control}
              name="IdleReclaimEnabled"
              render={({ field }) => (
                <Toggle checked={field.value === 1} onChange={(b) => field.onChange(b ? 1 : 0)} ariaLabel="空闲回收 IdleReclaimEnabled" />
              )}
            />
          </Row>
          <Num
            label="空闲回收阈值 IdleReclaimSec"
            hint="缓存块多久未被访问后视为「长期未使用」（秒），是空闲回收的判定阈值。"
            min={0}
            max={86400}
            {...form.register('IdleReclaimSec', { valueAsNumber: true, min: 0, max: 86400 })}
          />
          <Select
            label="TinyLFU 频率准入"
            hint="准入过滤器：内存压力下按「访问频率」决定新数据是否进缓存。开启后，一次性的大文件拷贝、杀毒扫描等顺序流量不再把常用热数据挤出缓存。"
            options={[
              { value: 0, text: '关闭' },
              { value: 1, text: '开启' },
            ]}
            {...form.register('TinyLFU', { valueAsNumber: true })}
          />
        </Section>

        {/* ⑤ 预取 */}
        <Section kicker="预取 · 顺序读与映射 I/O" note="这两项对拷贝大文件、程序启动影响最大">
          <Row
            label="分页 IO 进缓存 CachePagingIo"
            hint="内存映射文件的读写（程序加载 DLL、映射大文件）也纳入缓存：提高这类文件的读取命中率。关闭时映射 I/O 直接读写磁盘、不占缓存。"
          >
            <Toggle
              checked={!!form.watch('CachePagingIo')}
              onChange={(b) => form.setValue('CachePagingIo', b ? 1 : 0, { shouldDirty: true })}
              ariaLabel="CachePagingIo"
            />
          </Row>
          <Row
            label="预取引擎 ReadaheadEnabled"
            hint="预取引擎：检测到顺序读取模式时，提前把后续数据读入缓存。大文件拷贝、连续读取场景显著提速。"
          >
            <Toggle
              checked={!!form.watch('ReadaheadEnabled')}
              onChange={(b) => form.setValue('ReadaheadEnabled', b ? 1 : 0, { shouldDirty: true })}
              ariaLabel="ReadaheadEnabled"
            />
          </Row>
          <Num
            label="预取窗口 ReadaheadWindowKB (KB)"
            hint="每次预取向前多读的窗口大小（KB）。窗口越大顺序读越快，占用内存也越多。"
            min={64}
            max={4096}
            {...form.register('ReadaheadWindowKB', { valueAsNumber: true })}
          />
        </Section>

        {/* ⑥ 开机 */}
        <Section kicker="开机 · 自动加速策略" note="走独立 IOCTL 0x80A，不属于 RB_CONFIG">
          <Row
            label="开机自动加速所有数据卷 AutoAttach"
            hint="开机自动加速所有数据卷（Windows 系统盘除外）。关闭时每次开机手动打开需要的卷。"
          >
            <Toggle
              checked={bootCfgQuery.data?.AutoAttach === 1}
              onChange={(b) => void setBootPolicy({ AutoAttach: b ? 1 : 0 })}
              ariaLabel="开机自动加速所有数据卷"
              disabled={bootBusy}
            />
          </Row>
          <Row
            label="开机恢复上次加速的卷 ResumeOnBoot"
            hint="开机自动恢复上次加速过的卷（下方显示当前记录）。比「开机自动加速所有数据卷」更可控：只恢复你明确开启过的。"
          >
            <Toggle
              checked={bootCfgQuery.data?.ResumeOnBoot === 1}
              onChange={(b) => void setBootPolicy({ ResumeOnBoot: b ? 1 : 0 })}
              ariaLabel="开机恢复上次加速的卷"
              disabled={bootBusy}
            />
          </Row>
          <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
            上次加速记录：
            {bootCfgQuery.data?.ResumeLetters
              ? bootCfgQuery.data.ResumeLetters.split('').join('、') + ' 盘'
              : '（暂无——手动开启任一卷后自动记录）'}
          </p>
        </Section>

        {/* ⑦ 特性：改动最重的一块，单独成段 */}
        <Section
          kicker="特性 · RB2 热开关 / L2 / 排除集"
          note="热生效写注册表，不经过 SET_CONFIG"
        >
          {(
            [
              ['PREFETCH', '预取（提升连续读）'],
              ['HOT', '热区预热（冷启动更快命中）'],
              ['THERMAL', '温度护栏（过热暂停预取）'],
              ['ETW', 'ETW 遥测'],
              ['WBCOAL', '回写合写（平滑写延迟）'],
            ] as const
          ).map(([k, label]) => (
            <label
              key={k}
              className="flex items-center justify-between gap-3 text-xs"
              style={{ color: 'var(--rbc-text-secondary)' }}
            >
              <Hint text={RB2_HINTS[k]}>
                {label} · {k}
              </Hint>
              <Toggle
                checked={flagOn(k)}
                disabled={rbfeatBusy === k || rbfeat?.exe !== 'found'}
                onChange={(b) => setFlag(k, b)}
                ariaLabel={`RB2 ${k}`}
              />
            </label>
          ))}
          <div className="mt-1 border-t pt-3" style={{ borderColor: 'var(--rbc-border)' }}>
            <L2Manager rbfeat={rbfeat} reload={loadRbfeat} />
          </div>
          <div className="mt-1 border-t pt-3" style={{ borderColor: 'var(--rbc-border)' }}>
            <ExclManager rbfeat={rbfeat} reload={loadRbfeat} />
          </div>
          {rbfeat?.exe !== 'found' && (
            <p className="text-xs" style={{ color: 'var(--rbc-warning-fg)' }}>
              rbfeatctl.exe 不可达（payload 布局异常？）——本区开关停用，缓存功能本体不受影响。
            </p>
          )}
        </Section>

        {/* ⑧ 只读：排查用，降到末尾 */}
        <Section kicker="只读 · 驱动回报的内部状态" note="不随保存写回；由驱动维护">
          <ReadOnly
            label="SystemVolumeCached"
            value={cfg!.SystemVolumeCached}
            hint="Windows 系统盘是否被缓存。出于安全默认不缓存；可在「卷加速」页手动开启。"
          />
          <ReadOnly label="Frozen" value={cfg!.Frozen} hint="1=引擎处于冻结/重建窗口，此时 I/O 走旁路。" />
          <ReadOnly label="IdleFlush" value={cfg!.IdleFlush} hint="空闲回写开关的当前状态（驱动内部语义）。" />
          <ReadOnly label="WriteRelease" value={cfg!.WriteRelease} hint="写释放模式状态（驱动内部语义）。" />
          <ReadOnly
            label="VolumeFilter"
            value={JSON.stringify(cfg!.VolumeFilter)}
            hint="卷过滤白名单（本版本驱动不消费此字段；逐卷策略在「卷加速」页设置）。"
            wrap
          />
        </Section>

        {/* 保存条：sticky 贴底。600 行表单改完一半不用再滚到底。 */}
        <div
          className="sticky bottom-0 -mx-1 flex flex-wrap items-center gap-3 border-t px-1 py-3"
          style={{
            background: 'var(--rbc-bg-app)',
            borderColor: 'var(--rbc-border)',
            /* 粘在 main 的滚动边界上时，下方内容会从缝隙里露出来；
               加一层内阴影制造"浮起"的层次，而不是靠阴影外扩（会被裁）。 */
            boxShadow: '0 -8px 16px -12px rgba(20, 30, 50, 0.28)',
          }}
        >
          <Button type="submit" tone="primary" disabled={!dirty || saving}>
            {saving ? '保存中…' : '保存设置'}
          </Button>
          <Button
            tone="secondary"
            disabled={!dirty || saving}
            onClick={() => form.reset(configQuery.data)}
          >
            还原
          </Button>
          {dirty ? (
            <span className="text-xs" style={{ color: 'var(--rbc-warning-fg)' }}>
              有未保存修改
            </span>
          ) : (
            <span className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
              表单与驱动当前值一致
            </span>
          )}
          {result && (
            <span
              role="status"
              className="text-xs"
              style={{
                color: result.ok ? 'var(--rbc-success-fg)' : 'var(--rbc-danger-fg)',
                /* 回读 diff 是多行文本（每项一行），必须保留换行 */
                whiteSpace: 'pre-line',
              }}
            >
              {result.text}
            </span>
          )}
        </div>
      </form>
    </div>
  );
}

/** 统一分区骨架：标题行在卡外，正文在卡内（与状态页/收益页/诊断页同一套） */
function Section({
  kicker,
  note,
  children,
}: {
  kicker: string;
  note?: string;
  children: ReactNode;
}) {
  return (
    <section className="rbc-section rbc-fade-up">
      <div className="rbc-section-head">
        <span className="rbc-kicker">{kicker}</span>
        {note && <span className="rbc-section-note">{note}</span>}
      </div>
      <div className="rbc-card flex flex-col gap-3">{children}</div>
    </section>
  );
}

function Row({ label, hint, children }: { label: string; hint?: string; children: ReactNode }) {
  return (
    <div
      className="flex items-center justify-between gap-3 text-xs"
      style={{ color: 'var(--rbc-text-secondary)' }}
    >
      <span>{hint ? <Hint text={hint}>{label}</Hint> : label}</span>
      {children}
    </div>
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
    <label
      className="flex items-center justify-between gap-3 text-xs"
      style={{ color: 'var(--rbc-text-secondary)' }}
    >
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
    <label
      className="flex items-center justify-between gap-3 text-xs"
      style={{ color: 'var(--rbc-text-secondary)' }}
    >
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

type RbfeatState = {
  stats: Record<string, number> | null;
  l2: string;
  excl: string;
  exe: string;
} | null;

/**
 * L2 容器容量上限，必须与驱动 `RB2_L2_MAX_GB`（src/driver/rbL2.c:59 = 128）
 * 和 WPF `MainViewModel.L2SizeGB`（clamp 1–128）三端一致。
 * 驱动对越界值是**静默截断**，所以前端不设上限就等于骗用户。
 */
const L2_MAX_GB = 128;

function L2Manager({ rbfeat, reload }: { rbfeat: RbfeatState; reload: () => void }) {
  const [letter, setLetter] = useState('');
  /* 缺省 32 与驱动 Rb2GetDW(L"L2SizeGB", 32) 及 WPF _l2SizeGb = 32 对齐
   * （原为 16：用户在 Web 上首次启用会拿到与另两端不同的默认容量）。 */
  const [gb, setGb] = useState(32);
  const [busy, setBusy] = useState(false);
  /* rbfeatctl 不可达时，`rbfeat.l2` 里装的是工具自身的英文报错（实测为
   * "rbfeatctl.exe not found (payload layout?)"），直接显示会和本段末尾那条
   * 中文告警**重复两遍同一件事**，还让用户以为 L2 有独立故障。这里统一成「不可达」。 */
  const l2Line =
    rbfeat?.exe !== 'found'
      ? '不可达'
      : (rbfeat?.l2 ?? '').split(String.fromCharCode(13, 10)).filter(Boolean)[0] || '（无输出）';
  const call = (op: string) => {
    setBusy(true);
    fetch('/rb/l2', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ op, letter, gb }),
    })
      .then((r) => r.json())
      .then((d) => {
        if (d.code !== 0) toast('danger', `L2 ${op} 失败`, String(d.out ?? '').slice(0, 160));
        reload();
      })
      .catch((e) => toast('danger', 'L2 请求失败', String(e)))
      .finally(() => {
        setBusy(false);
        reload();
      });
  };
  const disabled = busy || rbfeat?.exe !== 'found';
  return (
    <>
      <div className="flex flex-wrap items-baseline justify-between gap-3 text-xs">
        <span style={{ color: 'var(--rbc-text-secondary)' }}>
          <Hint text="L2 二级读缓存：内存缓存淘汰下来的热数据写入卷上的容器文件，再次读取时先查 L2、未中再读盘。内存是第一级、L2 是第二级，适合「工作集大于内存但重复读多」的场景。仅 4K 块几何可用、单加速卷、与 WBCOAL 互斥。">
            L2 容器状态
          </Hint>
        </span>
        <span className="rbc-num truncate text-xs" style={{ color: 'var(--rbc-text-tertiary)', maxWidth: '60%' }}>
          {l2Line}
        </span>
      </div>
      <div
        className="flex flex-wrap items-center gap-2 text-xs"
        style={{ color: 'var(--rbc-text-secondary)' }}
      >
        <span>容器卷</span>
        <input
          className="rbc-input"
          style={{ width: 64 }}
          maxLength={1}
          value={letter}
          onChange={(e) => setLetter(e.target.value.toUpperCase().replace(/[^A-Z]/g, ''))}
          placeholder="X"
        />
        <span>容量 GB</span>
        <input
          type="number"
          className="rbc-input"
          style={{ width: 88 }}
          min={1}
          /* [FIX 2026-11-13] 上限由 512 收到 128：驱动侧 RB2_L2_MAX_GB=128
           * （rbL2.c:642 `if (s_SizeGB > RB2_L2_MAX_GB) s_SizeGB = RB2_L2_MAX_GB;`）
           * 是**静默截断**，WPF 侧也 clamp 在 1–128。Web 允许 512 会让用户以为
           * 配了 256GB、实际只拿到 128GB，且没有任何提示——三端契约漂移。 */
          max={L2_MAX_GB}
          value={gb}
          onChange={(e) =>
            setGb(Math.max(1, Math.min(L2_MAX_GB, Number(e.target.value) || 1)))
          }
        />
        <span className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
          1–{L2_MAX_GB}
        </span>
        <Button
          tone="secondary"
          disabled={disabled || !letter}
          onClick={() => call('enable')}
        >
          启用
        </Button>
        <Button tone="ghost" disabled={disabled} onClick={() => call('disable')}>
          停用
        </Button>
      </div>
      <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
        「停用」会先保存索引再关闭，容器文件保留，可再次启用。
      </p>
    </>
  );
}

function ExclManager({ rbfeat, reload }: { rbfeat: RbfeatState; reload: () => void }) {
  const [name, setName] = useState('');
  const [busy, setBusy] = useState(false);
  const call = (op: string, extra?: Record<string, unknown>) => {
    setBusy(true);
    fetch('/rb/excl', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ op, name, ...extra }),
    })
      .then((r) => r.json())
      .then((d) => {
        if (d.code !== 0) toast('danger', `排除集 ${op} 失败`, String(d.out ?? '').slice(0, 160));
        reload();
      })
      .catch((e) => toast('danger', '排除集请求失败', String(e)))
      .finally(() => setBusy(false));
  };
  const useDef = /UseDefaultExcl=(1)/.test(rbfeat?.excl ?? '');
  const entries = (rbfeat?.excl ?? '')
    .split(String.fromCharCode(13, 10))
    .filter((l) => /^  \S/.test(l))
    .map((l) => l.trim());
  const disabled = busy || rbfeat?.exe !== 'found';
  return (
    <>
      <div className="flex items-baseline justify-between gap-3 text-xs">
        <span style={{ color: 'var(--rbc-text-secondary)' }}>
          <Hint text="进程排除集：被排除进程（如杀毒扫描、磁盘碎片整理）的读写绕过缓存直达磁盘，避免它们的海量顺序流量污染缓存、挤掉热数据。内置默认排除：defrag/dfrgui/wbengine/vssadmin/msmpeng。">
            内置排除（defrag/dfrgui/wbengine/vssadmin/msmpeng）
          </Hint>
        </span>
        <Toggle checked={useDef} disabled={disabled} onChange={(b) => call('def', { on: b })} ariaLabel="UseDefaultExcl" />
      </div>
      <div
        className="flex flex-wrap items-center gap-2 text-xs"
        style={{ color: 'var(--rbc-text-secondary)' }}
      >
        <span>进程排除</span>
        <input
          className="rbc-input"
          style={{ width: 180 }}
          value={name}
          maxLength={60}
          onChange={(e) => setName(e.target.value)}
          placeholder="如 Weixin.exe"
        />
        <Button
          tone="secondary"
          disabled={disabled || !name.trim()}
          onClick={() => {
            call('add');
            setName('');
          }}
        >
          加入
        </Button>
      </div>
      <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
        当前自定义排除：{entries.length ? entries.join('、') : '（空）'}——命中进程的读写不进缓存。
      </p>
    </>
  );
}

function ReadOnly({
  label,
  value,
  hint,
  wrap,
}: {
  label: string;
  value: string | number;
  hint?: string;
  /** 长值（如 VolumeFilter 的 JSON）允许折行，否则会顶出卡片 */
  wrap?: boolean;
}) {
  return (
    <div className="flex items-baseline justify-between gap-3 text-xs">
      <span style={{ flex: '0 0 auto', color: 'var(--rbc-text-secondary)' }}>
        {hint ? <Hint text={hint}>{label}</Hint> : label}
      </span>
      <span
        className="rbc-num"
        style={{
          color: 'var(--rbc-text-tertiary)',
          textAlign: 'right',
          ...(wrap ? { wordBreak: 'break-all', flex: '1 1 auto' } : {}),
        }}
      >
        {value}
      </span>
    </div>
  );
}
