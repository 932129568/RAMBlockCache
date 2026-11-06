import { Toast } from 'radix-ui';
import { useToastStore, type ToastItem } from '@state/toast-store';

/**
 * 命令回执宿主（radix Toast，App 挂载一次）。
 * 右下角堆叠；swipe 右滑关闭；键盘 focusable（P1-5）。
 * 语义色映射：ok=success / warn / danger / info。
 */
export default function ToastHost() {
  const { items, dismiss } = useToastStore();
  const toneStyle: Record<ToastItem['tone'], { bg: string; fg: string; bd: string }> = {
    ok: { bg: 'var(--rbc-success-bg)', fg: 'var(--rbc-success-fg)', bd: 'var(--rbc-success-border)' },
    warn: { bg: 'var(--rbc-warning-bg)', fg: 'var(--rbc-warning-fg)', bd: 'var(--rbc-warning-border)' },
    danger: { bg: 'var(--rbc-danger-bg)', fg: 'var(--rbc-danger-fg)', bd: 'var(--rbc-danger-border)' },
    info: { bg: 'var(--rbc-info-bg)', fg: 'var(--rbc-info-fg)', bd: 'var(--rbc-info-border)' },
  };
  return (
    <Toast.Provider swipeDirection="right" duration={6000}>
      {items.map((t) => {
        const s = toneStyle[t.tone];
        return (
          <Toast.Root
            key={t.id}
            onOpenChange={(o) => !o && dismiss(t.id)}
            className="rounded-[var(--rbc-radius-md)]"
            style={{
              background: s.bg,
              border: `1px solid ${s.bd}`,
              padding: '10px 14px',
              minWidth: 280,
              boxShadow: '0 8px 24px rgba(0,0,0,0.45)',
            }}
          >
            <Toast.Title className="text-sm" style={{ color: s.fg }}>
              {t.title}
            </Toast.Title>
            {t.body && (
              <Toast.Description
                className="mt-0.5 text-xs"
                style={{ color: 'var(--rbc-text-secondary)' }}
              >
                {t.body}
              </Toast.Description>
            )}
          </Toast.Root>
        );
      })}
      <Toast.Viewport
        className="outline-none"
        style={{
          position: 'fixed',
          right: 16,
          bottom: 52,
          display: 'flex',
          flexDirection: 'column',
          gap: 8,
          zIndex: 60,
          width: 340,
          listStyle: 'none',
        }}
      />
    </Toast.Provider>
  );
}
