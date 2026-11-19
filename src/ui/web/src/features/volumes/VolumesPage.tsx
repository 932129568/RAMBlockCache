import { useState } from 'react';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { RB_WRITEMODE, type RbWriteMode } from '@protocol/constants';
import type { RbVolumeInfo, RbVolumeList } from '@protocol/types';
import { useDriverStore } from '@state/driver-store';
import { confirmDialog } from '@state/confirm-store';
import { toast } from '@state/toast-store';
import { Badge, Button, EmptyState, Hint, Icons, PageHeader, SkeletonCard } from '@components/ui';
import { attachDataVolumesWithL1Profile, selectAttachableDataVolumes } from '@bridge/volume-attach-service';

const WRITEMODE_TEXT: Record<number, string> = {
  [RB_WRITEMODE.THROUGH]: '直写',
  [RB_WRITEMODE.BACK]: '回写',
  [RB_WRITEMODE.GLOBAL]: '跟随全局',
};

/**
 * 加速页（§11.5 迁移顺序第 4 页）：卷枚举 + 热开关（ATTACH）+ 逐卷写模式（SET_VOLUME）。
 * 纪律：写命令一律显式确认；系统卷允许手动热开关（[USER-REQ 2026-11-05] 驱动侧已解除
 * B-10 硬拒绝，rbattach.c "system volume manual opt-in"；开机自动挂载仍跳过系统卷，
 * SafeMode 强制直写，簇重排闸门同样生效）；
 * Bound=0 的卷如实显示"未绑定"，不给可点按钮（驱动闸门之外的前端诚实降级）。
 */
export default function VolumesPage() {
  const transport = useDriverStore((s) => s.transport);
  const queryClient = useQueryClient();
  const [actionError, setActionError] = useState<string | null>(null);
  const [busyLetter, setBusyLetter] = useState<string | null>(null);   /* [PERF-UI] 动作 pending 反馈 */

  const volumesQuery = useQuery<RbVolumeList>({
    queryKey: ['rb', 'volumes'],
    enabled: false, // [适配 2026-10-31] 枚举轮询已收编到 App 级 StatsPoller（5s），本页只读观察 + 手动刷新
    retry: 0,
  });

  async function runCommand(action: () => Promise<{ ok: boolean; error?: { message: string } }>, label: string, letter?: string) {
    setActionError(null);
    if (letter) setBusyLetter(letter);   /* [PERF-UI] 点击即反馈：按钮禁用+文案切换 */
    try {
    const result = await action();
    if (!result.ok) {
      setActionError(result.error?.message ?? '命令执行失败');
      toast('danger', `${label}失败`, result.error?.message ?? '未知错误');
      return;
    }
    toast('ok', `${label}成功`, '驱动返回 Status=0');
    } finally {
      if (letter) setBusyLetter(null);
    }
    await queryClient.invalidateQueries({ queryKey: ['rb', 'volumes'] });
  }

  /* [240 ARCH-06] Empty-state dead end fix: ask GET_VOLUMES directly instead of
   * invalidating an `enabled:false` query (which may never refetch), and never
   * invent a drive letter. The bridge is the only truthful source of letters. */
  async function refreshVolumes() {
    setActionError(null);
    try {
      const r = await transport.getVolumes();
      if (!r.ok) {
        setActionError(r.error.message);
        toast('danger', '卷枚举失败', r.error.message);
        return;
      }
      queryClient.setQueryData(['rb', 'volumes'], r.value);
      toast('ok', '卷枚举已刷新', `驱动返回 ${r.value.Count} 个卷`);
    } catch (e) {
      setActionError((e as Error).message);
    }
  }

  /* [240 module 3] One-click attach of REAL enumerated non-system data volumes.
   * System/boot volumes, unbound volumes and already-attached ones are filtered
   * by the service; write mode is never switched to write-back here (R9). */
  async function attachAllDataVolumes() {
    const candidates = selectAttachableDataVolumes(volumes);
    if (candidates.length === 0) {
      toast('warn', '无可挂载卷', '枚举到的卷均已加速、为系统卷或未绑定');
      return;
    }
    const labels = candidates.map((v) => `${v.Letter}:`).join('、');
    const okGo = await confirmDialog({
      title: '确认批量热开启？',
      body: `将对 ${labels} 启用块级读缓存（免重启，写模式保持「跟随全局」）。系统卷与引导卷已自动排除。`,
    });
    if (!okGo) return;
    setActionError(null);
    try {
      const r = await attachDataVolumesWithL1Profile(volumes, {
        attachVolume: (input) => transport.attachVolume(input),
      });
      if (!r.ok) {
        setActionError(r.error ?? '热开启失败');
        toast('danger', '热开启未完成', r.error ?? '未知错误');
      } else {
        toast('ok', '热开启成功', `已挂载 ${r.attachedLetters.join('、')}`);
      }
    } catch (e) {
      setActionError((e as Error).message);
    } finally {
      await queryClient.invalidateQueries({ queryKey: ['rb', 'volumes'] });
    }
  }

  async function toggleAttach(v: RbVolumeInfo) {
    const turningOn = v.Attached === 0;
    const label = turningOn ? '热开启' : '热停用';
    const detail = turningOn
      ? `将对卷 ${v.Letter}: 启用块级缓存（读缓存开启；写模式保持 ${WRITEMODE_TEXT[v.WriteMode] ?? '跟随全局'}）。` +
        (v.IsSystem === 1
          ? ' ⚠ 这是系统卷：仅手动开启、不进开机自动挂载；若选回写，掉电丢脏窗口自负。'
          : '')
      : `将先刷写卷 ${v.Letter}: 的脏数据再热停用缓存。`;
    if (!(await confirmDialog({ title: `确认${label}？`, body: detail, danger: !turningOn }))) return;
    void runCommand(() =>
      transport.attachVolume({
        Letter: v.Letter,
        Attach: turningOn ? 1 : 0,
        ReadEnabled: 1,
        WriteEnabled: 1,
        WriteMode: RB_WRITEMODE.GLOBAL, // 保持原值
      }), label, v.Letter);
  }

  async function changeWriteMode(v: RbVolumeInfo, mode: RbWriteMode) {
    if (mode === v.WriteMode) return;
    const okGo = await confirmDialog({
      title: `确认将卷 ${v.Letter}: 的写模式切换为「${WRITEMODE_TEXT[mode] ?? mode}」？`,
      body: mode === RB_WRITEMODE.BACK
        ? '回写模式：写入先进缓存即完成，后台按节奏合并落盘，写延迟显著降低；异常断电时最近未落盘的少量数据可能丢失。'
        : mode === RB_WRITEMODE.THROUGH
          ? '直写模式：写入同时落盘，断电不丢数据。'
          : '跟随全局：使用设置页的全局写模式。',
      danger: mode === RB_WRITEMODE.BACK,
    });
    if (!okGo) return;
    void runCommand(() =>
      transport.setVolume({
        Letter: v.Letter,
        ReadEnabled: v.ReadEnabled,
        WriteEnabled: v.WriteEnabled,
        WriteMode: mode,
      }), `写模式切换 ${v.Letter}:`);
  }

  /* [UI-2026-11-12] 加速中的卷排到最前（组内按盘符）。
   * 实测 4 卷时 D: 是唯一"加速中"的，却排在最后 —— 用户要先扫过三张
   * 非活动卡片才看到当前生效的那一张。列表顺序应当表达优先级。 */
  const volumes = [...(volumesQuery.data?.Items ?? [])].sort(
    (a, b) => b.Attached - a.Attached || a.Letter.localeCompare(b.Letter),
  );
  const count = volumesQuery.data?.Count ?? 0;
  /* [UI-2026-11-12] 结论段需要的一个数：有几个卷真的在加速。
   * 改前页面只给"枚举 N 卷"，读者要自己逐张卡数徽标才知道当前态势。 */
  const attachedCount = volumes.filter((v) => v.Attached === 1).length;

  return (
    <div className="flex flex-col gap-6">
      <PageHeader
        title="卷加速"
        desc="卷加速开关与逐卷写模式"
        actions={
          <>
            <Badge tone="neutral">枚举 {count} 卷 · 5s 轮询</Badge>
            <Button
              tone="secondary"
              icon={<Icons.refresh size={14} />}
              onClick={() => void queryClient.invalidateQueries({ queryKey: ['rb', 'volumes'] })}
            >
              立即刷新
            </Button>
          </>
        }
      />

      {/* 结论段：先把"现在什么态势"说清，再看清单 */}
      <p className="rbc-fade-up text-sm" style={{ color: 'var(--rbc-text-secondary)' }}>
        {attachedCount > 0
          ? `${attachedCount} 个卷正在加速（共枚举 ${count} 卷）。逐卷可在下方切换热开关与写模式。`
          : count > 0
            ? `当前没有卷在加速（共枚举 ${count} 卷）。打开任一数据卷的「热开启」即开始收集收益，无需重启。`
            : '正在枚举卷…'}
      </p>

      {/* [240 ARCH-06] Enumerated but idle volumes used to be a second dead end:
          the page told the user to find a switch that was buried in a card list.
          Offer a single explicit, confirmed action over real candidates. */}
      {count > 0 && attachedCount === 0 && selectAttachableDataVolumes(volumes).length > 0 && (
        <div className="rbc-fade-up">
          <Button tone="primary" onClick={() => void attachAllDataVolumes()}>
            一键热开启全部非系统数据卷（{selectAttachableDataVolumes(volumes).length} 个）
          </Button>
        </div>
      )}

      {/* [UX 2026-11-12] 命令失败提示从页底移到页首：改前它排在整张卷列表之后，
          用户点完"热开启"失败后必须先滚到底才看得见原因。 */}
      {actionError && (
        <p role="alert" className="text-sm" style={{ color: 'var(--rbc-danger-fg)' }}>
          命令失败：{actionError}
        </p>
      )}

      {volumesQuery.isPending ? (
        <SkeletonCard rows={3} />
      ) : volumesQuery.isError && !volumesQuery.data ? (
        /* [PERF-UI 2026-11-04] 仅在"从未拿到过数据"时才整页报错。已有数据时
         * （慢命令期间轮询短暂超时）继续展示上一份快照 + 顶部轻提示，而不是把
         * 整页换成红色失败——用户会误以为操作失败（实际命令已成功）。 */
        <p role="alert" className="text-sm" style={{ color: 'var(--rbc-danger-fg)' }}>
          卷枚举失败：{(volumesQuery.error as Error).message}
          <br />
          <span style={{ color: 'var(--rbc-text-secondary)' }}>
            驱动忙碌时属正常现象（回写刷脏期间控制通道排队），稍候会自动恢复；也可点右上角刷新。
          </span>
        </p>
      ) : count === 0 ? (
        <EmptyState
          icon={<Icons.disk size={26} />}
          title="驱动未枚举到卷"
          body="驱动返回 0 个卷。请确认桥接以管理员权限运行且驱动已加载，再在管理员 PowerShell 7 执行 rbfeatctl.exe vols 核对枚举结果；本页不会虚构盘符。"
          action={
            <Button tone="secondary" icon={<Icons.refresh size={14} />} onClick={() => void refreshVolumes()}>
              重新枚举
            </Button>
          }
        />
      ) : (
        <section className="rbc-section rbc-fade-up">
          <div className="rbc-section-head">
            <span className="rbc-kicker">卷清单 · {count} 卷</span>
            <span className="rbc-section-note">每个卷可独立开关；回写模式下写延迟更低</span>
          </div>
          <div className="rbc-section-body gap-3">
            {volumes.map((v) => (
              <VolumeCard
                key={v.Letter}
                v={v}
                busy={busyLetter === v.Letter}
                onToggle={() => void toggleAttach(v)}
                onWriteMode={(m) => void changeWriteMode(v, m)}
              />
            ))}
          </div>
        </section>
      )}
    </div>
  );
}

function VolumeCard({
  v,
  busy,
  onToggle,
  onWriteMode,
}: {
  v: RbVolumeInfo;
  busy: boolean;
  onToggle: () => void;
  onWriteMode: (mode: RbWriteMode) => void;
}) {
  const isSystem = v.IsSystem === 1;
  const attached = v.Attached === 1;
  const bound = v.Bound === 1;
  const canToggle = bound && !busy;   /* [USER-REQ 2026-11-05] 系统卷解除前端禁用：驱动侧 B-10 已放开（手动 opt-in，强制安全边界在驱动） */

  return (
    /* [UI-2026-11-12] section → article：这是一张"卷"实体卡，不是页面分区。
     * 改前它包在 `.rbc-section` 里会变成 section 套 section，读屏器与
     * 文档大纲都会退化。 */
    <article
      className={`rbc-card ${attached ? 'rbc-card-accent' : ''}`}
      aria-label={`卷 ${v.Letter} 卡片`}
    >
      <div className="flex flex-wrap items-center gap-3">
        <span className="rbc-num text-xl" style={{ color: 'var(--rbc-text-primary)' }}>
          {v.Letter}:
        </span>
        {attached ? (
          <Badge tone="ok">加速中</Badge>
        ) : (
          <Badge tone="neutral">未加速</Badge>
        )}
        {isSystem && <Badge tone="warn">系统卷（手动开启）</Badge>}
        {!bound && <Badge tone="info">未绑定</Badge>}
        <span className="flex-1" />

        <label className="flex items-center gap-1.5 text-xs" style={{ color: 'var(--rbc-text-secondary)' }}>
          <Hint text="直写=写即落盘，断电不丢数据；回写=写进缓存后台落盘，写延迟更低；跟随全局=使用设置页的全局写模式。">写模式</Hint>
          <select
            aria-label={`卷 ${v.Letter} 写模式`}
            value={v.WriteMode}
            disabled={!bound || busy}
            onChange={(e) => onWriteMode(Number(e.target.value) as RbWriteMode)}
            className="rbc-input text-xs"
          >
            <option value={RB_WRITEMODE.THROUGH}>直写</option>
            <option value={RB_WRITEMODE.BACK}>回写</option>
            <option value={RB_WRITEMODE.GLOBAL}>跟随全局</option>
          </select>
        </label>

        <Hint text={attached
          ? '热停用：本卷读写立即旁路缓存、直达磁盘（数据不受影响），缓存中该卷内容丢弃。'
          : '热开启：本卷读写进入缓存加速，即时生效，无需重启。回写模式下写延迟更低。'}>
          <Button
            tone={attached ? 'secondary' : 'primary'}
            disabled={!canToggle}
            onClick={onToggle}
            ariaLabel={`卷 ${v.Letter} ${attached ? '热停用' : '热开启'}`}
          >
            {attached ? '热停用' : '热开启'}
          </Button>
        </Hint>
      </div>

      {/* [UX-FIX 2026-11-05] FreeMB 恒为 0：块层过滤器无文件系统语义，拿不到
          "剩余空间"（需 FSCTL 查 FS，属文件系统过滤器职责）。原样显示"可用 0 MB"
          会被误读为"盘已满"——改为显式标注"—"，并把该行信息按"容量/开关/扇区"
          分组降噪。 */}
      <div className="mt-2 flex flex-wrap items-center gap-x-5 gap-y-1 text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
        <span>
          容量 <b className="rbc-num" style={{ color: 'var(--rbc-text-secondary)' }}>{v.CapacityMB.toLocaleString('zh-CN')} MB</b>
        </span>
        <span title="块层过滤器无文件系统语义，无法获取卷剩余空间">
          可用 —
        </span>
        <span>
          {/* [UI-2026-11-12] 未加速卷原样显示"读 开 · 写 开"，读起来像"缓存已开"。
              ReadEnabled/WriteEnabled 描述的是**若接入**时用哪套开关，未接入时
              应显式说"旁路"，否则用户会以为这张卡已经在加速。 */}
          {attached
            ? `读 ${v.ReadEnabled === 1 ? '开' : '关'} · 写 ${v.WriteEnabled === 1 ? '开' : '关'}`
            : '旁路中（未接管）'}
        </span>
        <span>
          扇区 {v.LogicalSectorBytes}/{v.PhysicalSectorBytes} B
        </span>
        {!bound && <span style={{ color: 'var(--rbc-warning-fg)' }}>过滤驱动未绑定该卷栈</span>}
      </div>
    </article>
  );
}
