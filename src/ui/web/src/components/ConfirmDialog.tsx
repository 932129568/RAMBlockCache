import { AlertDialog } from 'radix-ui';
import { useConfirm } from '@state/confirm-store';

/**
 * 全局确认框宿主（radix-ui AlertDialog）——挂载于 App，全局唯一实例。
 * 键盘：Esc 取消 / Enter 确认（radix 原生焦点管理）；读屏：role=alertdialog +
 * Title/Description 语义（P1-5 可访问性对齐，替代 window.confirm 的不可达方案）。
 */
export default function ConfirmDialog() {
  const { open, title, body, danger, answer } = useConfirm();

  return (
    <AlertDialog.Root open={open} onOpenChange={(o) => !o && answer(false)}>
      <AlertDialog.Portal>
        <AlertDialog.Overlay
          /* [A7 2026-11-11] rbc-overlay：与弹层同步的遮罩淡入淡出。
             原实现遮罩是瞬时出现的，弹层才开始淡入 —— 视觉上"先黑一下再弹框"。
             另：亮色主题下 rgba(8,8,12,0.62) 偏黑，压暗过度，改为冷灰蓝。 */
          className="rbc-overlay"
          style={{
            position: 'fixed',
            inset: 0,
            background: 'rgba(16, 24, 40, 0.42)',
            backdropFilter: 'blur(3px)',
            zIndex: 50,
          }}
        />
        <AlertDialog.Content
          role="alertdialog"
          className="rbc-pop-in"
          style={{
            position: 'fixed',
            top: '50%',
            left: '50%',
            transform: 'translate(-50%, -50%)',
            zIndex: 51,
            width: 'min(440px, calc(100vw - 32px))',
            background: 'var(--rbc-elevated-glass)',
            backdropFilter: 'blur(16px) saturate(150%)',
            border: `1px solid ${danger ? 'var(--rbc-danger-border)' : 'var(--rbc-border-strong)'}`,
            borderRadius: 'var(--rbc-radius-lg)',
            padding: 'var(--rbc-pad-card)',
            boxShadow: 'var(--rbc-shadow-pop)',
          }}
        >
          <AlertDialog.Title
            className="text-sm font-medium"
            style={{ color: danger ? 'var(--rbc-danger-fg)' : 'var(--rbc-text-primary)' }}
          >
            {title}
          </AlertDialog.Title>
          <AlertDialog.Description
            className="mt-2 whitespace-pre-line text-xs"
            style={{ color: 'var(--rbc-text-secondary)', lineHeight: 1.8 }}
          >
            {body}
          </AlertDialog.Description>
          <div className="mt-4 flex justify-end gap-2">
            <AlertDialog.Cancel asChild>
              <button type="button" onClick={() => answer(false)} className="rbc-btn rbc-btn-secondary">
                取消
              </button>
            </AlertDialog.Cancel>
            <AlertDialog.Action asChild>
              <button
                type="button"
                onClick={() => answer(true)}
                className={`rbc-btn ${danger ? 'rbc-btn-danger' : 'rbc-btn-primary'}`}
              >
                确认
              </button>
            </AlertDialog.Action>
          </div>
        </AlertDialog.Content>
      </AlertDialog.Portal>
    </AlertDialog.Root>
  );
}
