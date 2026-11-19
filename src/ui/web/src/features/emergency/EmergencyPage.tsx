import { useState } from 'react';
import { useQuery } from '@tanstack/react-query';
import { motion } from 'motion/react';
import type { RbStats } from '@protocol/types';
import { useDriverStore } from '@state/driver-store';
import { confirmDialog } from '@state/confirm-store';
import { toast } from '@state/toast-store';
import { Badge, Card, Icons, PageHeader } from '@components/ui';

/**
 * 应急页：熔断 / 解除熔断 / 立即刷写 / 退出安全模式。
 * 全部写命令经 radix 确认框 + toast 回执；执行结果如实显示（FUNC-03 纪律）。
 */
export default function EmergencyPage() {
  const transport = useDriverStore((s) => s.transport);
  const [busy, setBusy] = useState<string | null>(null);

  const statsQuery = useQuery<RbStats>({
    queryKey: ['rb', 'stats'],
    enabled: false,
    retry: 0,
  });
  const stats = statsQuery.data;
  const panicking = stats?.PanicActive === 1;
  const safeMode = stats?.SafeMode === 1;

  async function run(
    key: string,
    confirmBody: string,
    label: string,
    action: () => Promise<{ ok: boolean; error?: { message: string } }>,
  ) {
    if (!(await confirmDialog({ title: label, body: confirmBody, danger: key === 'panic' }))) return;
    setBusy(key);
    const result = await action();
    setBusy(null);
    if (result.ok) {
      toast('ok', `${label}：已执行`, '驱动返回 Status=0');
    } else {
      toast('danger', `${label}失败`, result.error?.message ?? '未知错误');
    }
  }

  const tiles: Array<{
    key: string;
    label: string;
    hint: string;
    detail: string;
    icon: React.ReactNode;
    tone: 'danger' | 'primary' | 'secondary';
    confirm: string;
    action: () => Promise<{ ok: boolean; error?: { message: string } }>;
  }> = [
    {
      key: 'panic',
      label: '全局熔断',
      hint: '一切 I/O 立即直通磁盘（数据安全优先的保守动作）',
      detail: '熔断=缓存引擎全局旁路：所有读写立即直达磁盘，不再命中缓存。用于排障或紧急避险（例如怀疑磁盘异常时）。完全可逆：解除熔断后缓存恢复工作，期间缓存的统计仍累计。',
      icon: <Icons.shield size={22} />,
      tone: 'danger',
      confirm: '所有 I/O 立即直通磁盘，缓存不再命中（可随时解除）。',
      action: () => transport.panic(),
    },
    {
      key: 'panicClear',
      label: '解除熔断',
      hint: '等自愈世代追平后恢复，超时 fail-closed',
      detail: '恢复正常缓存路径：驱动会等待在飞 I/O 的自愈世代追平后再恢复，确保不撕裂数据；超时则拒绝恢复（fail-closed，安全方向）。',
      icon: <Icons.refresh size={22} />,
      tone: 'secondary',
      confirm: '驱动将等待自愈世代追平后恢复缓存；超时则拒绝恢复（fail-closed）。',
      action: () => transport.panicClear(),
    },
    {
      key: 'flush',
      label: '立即刷写脏数据',
      hint: '把全部脏块落盘（直写模式下通常无脏数据）',
      detail: '把回写模式下尚未落盘的脏数据立即全部写盘（全局 FLUSH）。适合关机前、拔盘前或希望立即落定数据的时刻；直写模式没有脏数据，点击秒回。',
      icon: <Icons.download size={22} />,
      tone: 'secondary',
      confirm: '确认立即刷写全部脏数据到磁盘？',
      action: () => transport.flush(),
    },
    {
      key: 'exitSafeMode',
      label: '退出安全模式',
      hint: safeMode ? '当前处于安全模式' : '当前不在安全模式（驱动按状态响应）',
      detail: '安全模式=强制全局直写直到再次进入正常态（放弃回写收益、换取断电零丢失）。退出后恢复原本的写模式。驱动按当前状态响应：不在安全模式时点击无副作用。',
      icon: <Icons.sliders size={22} />,
      tone: 'secondary',
      confirm: '确认退出安全模式？',
      action: () => transport.exitSafeMode(),
    },
  ];

  return (
    <div className="flex flex-col gap-6">
      <PageHeader
        title="应急"
        desc="紧急操作：立即回写落盘、安全模式（强制直写）、熔断（全局旁路）"
        actions={
          <>
            {panicking && <Badge tone="warn">熔断中</Badge>}
            {safeMode && <Badge tone="danger">安全模式</Badge>}
          </>
        }
      />

      {/* 结论段：先把"现在是不是异常态"说清。改前只有一个徽标，正常态时
          页面和异常态几乎没区别，用户得自己读徽标才知道要不要动手。 */}
      <p className="rbc-fade-up text-sm" style={{ color: 'var(--rbc-text-secondary)' }}>
        {panicking
          ? '当前全局熔断中：所有 I/O 直通磁盘，缓存不再命中。解除前请先确认磁盘健康。'
          : safeMode
            ? '当前处于安全模式：强制全局直写（放弃回写收益换断电零丢失）。退出后恢复原写模式。'
            : '当前运行正常，缓存按配置生效中。以下操作会立即改变 I/O 路径，全部需要二次确认。'}
      </p>

      {panicking && (
        <motion.div
          role="alert"
          initial={{ opacity: 0, y: -6 }}
          animate={{ opacity: 1, y: 0 }}
          className="rounded-[var(--rbc-radius-md)] p-4 text-sm"
          style={{
            background: 'var(--rbc-warning-bg)',
            border: '1px solid var(--rbc-warning-border)',
            color: 'var(--rbc-warning-fg)',
          }}
        >
          全局熔断中：所有 I/O 直通磁盘，缓存统计仍在累计但不再命中。解除前请先确认磁盘健康。
        </motion.div>
      )}
      {safeMode && (
        <div
          role="alert"
          className="rounded-[var(--rbc-radius-md)] p-4 text-sm"
          style={{
            background: 'var(--rbc-danger-bg)',
            border: '1px solid var(--rbc-danger-border)',
            color: 'var(--rbc-danger-fg)',
          }}
        >
          安全模式生效中：缓存完全旁路。
        </div>
      )}

      <section className="rbc-section">
        <div className="rbc-section-head">
          <span className="rbc-kicker">应急操作 · {tiles.length} 项</span>
          <span className="rbc-section-note">每项都会即时改变 I/O 路径，全部走二次确认</span>
        </div>
        <div className="rbc-section-body">
          {/* [UI-2026-11-12] 危险区提示从 tiles 之后移到之前：它是对这些按钮的
              使用说明，放在按钮下面等于"做完才告诉你危险"。
              另：原 fallback 写了 #FCA5A5/#FEF2F2/#B91C1C 三个 Tailwind 红，
              既不在本调色板里、也永远不会生效（令牌存在）——已删。 */}
          <div
            className="rbc-fade-up rounded-[var(--rbc-radius-md)] p-3 text-xs"
            style={{
              border: '1px dashed var(--rbc-danger-border)',
              background: 'var(--rbc-danger-bg)',
              color: 'var(--rbc-danger-fg)',
            }}
            role="note"
          >
            危险操作区 —— 每个开关都会即时改变 I/O 路径。误触时的恢复顺序：
            先「解除熔断」→ 再检查卷页加速态。二次确认框不会绕过。
          </div>

          {/* [UI-2026-11-12] 固定 grid-cols-2 在窄窗下把 4 张大按钮挤成两列小格，
              标题与提示换行严重；改为 1 列起步。 */}
          <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
            {tiles.map((t, i) => (
              <motion.button
                key={t.key}
                type="button"
                disabled={busy !== null}
                title={t.detail}
                onClick={() => void run(t.key, t.confirm, t.label, t.action)}
                {...(busy
                  ? {}
                  : { whileHover: { y: -2 }, whileTap: { scale: 0.97 } })}
                className={`rbc-card rbc-fade-up ${t.tone === 'danger' ? 'rbc-card-danger' : ''}`}
                style={{
                  cursor: busy ? 'wait' : 'pointer',
                  textAlign: 'left',
                  animationDelay: `${i * 50}ms`,
                }}
              >
                <div className="flex items-start gap-3">
                  <span
                    className="flex h-10 w-10 shrink-0 items-center justify-center rounded-[var(--rbc-radius-md)]"
                    style={{
                      background:
                        t.tone === 'danger' ? 'var(--rbc-danger-bg)' : 'var(--rbc-bg-input)',
                      color: t.tone === 'danger' ? 'var(--rbc-danger-fg)' : 'var(--rbc-accent)',
                      boxShadow: 'inset 0 1px 0 rgba(255,255,255,0.06)',
                    }}
                  >
                    {t.icon}
                  </span>
                  <div>
                    <div className="text-sm font-medium" style={{ color: 'var(--rbc-text-primary)' }}>
                      {busy === t.key ? '执行中…' : t.label}
                    </div>
                    <div className="mt-1 text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
                      {t.hint}
                    </div>
                  </div>
                </div>
              </motion.button>
            ))}
          </div>
        </div>
      </section>

      {/* 故障面快照：改前 `stats &&` 为 false 时整块静默消失，读者无法区分
          "没有故障"与"没拿到数据"。补一个显式占位。 */}
      {stats ? (
        <Card title="故障面快照" subtitle="连续非干净启动 / 异常关机脏数据上界 / 已确认丢失">
          <div className="flex flex-wrap gap-x-8 gap-y-2 rbc-num text-sm" style={{ color: 'var(--rbc-text-secondary)' }}>
            <span>
              BootFails <span style={{ color: 'var(--rbc-text-primary)' }}>{stats.BootFails}</span>
            </span>
            <span>
              脏数据上界{' '}
              <span style={{ color: 'var(--rbc-text-primary)' }}>{stats.DirtySnapshotMB} MB</span>
            </span>
            <span>
              LostDirty{' '}
              <span style={{ color: 'var(--rbc-text-primary)' }}>
                {stats.LostDirtyBytes.toLocaleString('zh-CN')} B
              </span>
            </span>
          </div>
        </Card>
      ) : (
        <Card title="故障面快照" subtitle="连续非干净启动 / 异常关机脏数据上界 / 已确认丢失">
          <p className="text-xs" style={{ color: 'var(--rbc-text-tertiary)' }}>
            尚未取到统计（等待首次轮询或驱动未响应），此处不显示 0 值以免被误读为"无故障"。
          </p>
        </Card>
      )}
    </div>
  );
}
