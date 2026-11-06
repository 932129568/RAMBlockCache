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
    icon: React.ReactNode;
    tone: 'danger' | 'primary' | 'secondary';
    confirm: string;
    action: () => Promise<{ ok: boolean; error?: { message: string } }>;
  }> = [
    {
      key: 'panic',
      label: '全局熔断',
      hint: '一切 I/O 立即直通磁盘（数据安全优先的保守动作）',
      icon: <Icons.shield size={22} />,
      tone: 'danger',
      confirm: '所有 I/O 立即直通磁盘，缓存不再命中（可随时解除）。',
      action: () => transport.panic(),
    },
    {
      key: 'panicClear',
      label: '解除熔断',
      hint: '等自愈世代追平后恢复，超时 fail-closed',
      icon: <Icons.refresh size={22} />,
      tone: 'secondary',
      confirm: '驱动将等待自愈世代追平后恢复缓存；超时则拒绝恢复（fail-closed）。',
      action: () => transport.panicClear(),
    },
    {
      key: 'flush',
      label: '立即刷写脏数据',
      hint: '把全部脏块落盘（直写模式下通常无脏数据）',
      icon: <Icons.download size={22} />,
      tone: 'secondary',
      confirm: '确认立即刷写全部脏数据到磁盘？',
      action: () => transport.flush(),
    },
    {
      key: 'exitSafeMode',
      label: '退出安全模式',
      hint: safeMode ? '当前处于安全模式' : '当前不在安全模式（驱动按状态响应）',
      icon: <Icons.sliders size={22} />,
      tone: 'secondary',
      confirm: '确认退出安全模式？',
      action: () => transport.exitSafeMode(),
    },
  ];

  return (
    <div className="flex flex-col gap-4">
      <PageHeader
        title="应急"
        desc="这些按钮直接改变驱动数据面行为。确认框是最后一道人为闸门，驱动侧闸门（panic 世代追平、BootLoopGuard）不因前端绕过。"
        actions={
          <>
            {panicking && <Badge tone="warn">熔断中</Badge>}
            {safeMode && <Badge tone="danger">安全模式</Badge>}
          </>
        }
      />

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

      <div className="grid grid-cols-2 gap-3">
        {tiles.map((t, i) => (
          <motion.button
            key={t.key}
            type="button"
            disabled={busy !== null}
            onClick={() => void run(t.key, t.confirm, t.label, t.action)}
            {...(busy
              ? {}
              : { whileHover: { y: -2 }, whileTap: { scale: 0.97 } })}
            className={`rbc-card rbc-fade-up ${t.tone === 'danger' ? 'rbc-card-accent' : ''}`}
            style={{
              borderColor: t.tone === 'danger' ? 'var(--rbc-danger-border)' : undefined,
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

      {/* [UI-UP 2026-11-04] 断路器箱视觉锚点：本页所有操作都会立刻改变数据面行为。
          危险度分层：熔断=全局透传（可逆）；安全模式=直写（可逆）；FLUSH=落盘（安全）。 */}
      <div
        className="rbc-fade-up rounded-[var(--rbc-radius-md)] p-3 text-xs"
        style={{
          border: '1px dashed var(--rbc-danger-border, #FCA5A5)',
          background: 'var(--rbc-danger-bg, #FEF2F2)',
          color: 'var(--rbc-danger-fg, #B91C1C)',
        }}
        role="note"
      >
        危险操作区 —— 每个开关都会即时改变 I/O 路径。误触时的恢复顺序：
        先「PANIC_CLEAR」解除熔断 → 再检查卷页加速态。二次确认框不会绕过。
      </div>

      {stats && (
        <Card title="故障面快照" subtitle="连续非干净启动 / 异常关机脏数据上界 / 已确认丢失">
          <div className="flex gap-8 font-mono text-sm" style={{ color: 'var(--rbc-text-secondary)' }}>
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
      )}
    </div>
  );
}
