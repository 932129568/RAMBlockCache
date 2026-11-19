import { create } from 'zustand';

/**
 * 全局确认框状态（radix-ui AlertDialog 宿主由 App 挂载）。
 * ask() 返回 Promise<boolean>，调用方 await——替代 window.confirm，
 * 换取键盘可达（Esc=取消/Enter=确认）与读屏语义（P1-5 对齐）。
 */

export interface ConfirmOptions {
  title: string;
  body: string;
  /** 确认键危险态（应急/回写类动作用红色确认键） */
  danger?: boolean | undefined;
}

interface ConfirmState {
  open: boolean;
  title: string;
  body: string;
  danger: boolean;
  resolve: ((v: boolean) => void) | null;
  ask: (opts: ConfirmOptions) => Promise<boolean>;
  answer: (v: boolean) => void;
}

export const useConfirm = create<ConfirmState>((set, get) => ({
  open: false,
  title: '',
  body: '',
  danger: false,
  resolve: null,
  ask(opts) {
    return new Promise<boolean>((resolve) => {
      // 串行化：已有未决确认时直接拒绝新的请求（控制通道纪律：一次一件事）
      if (get().open && get().resolve) {
        resolve(false);
        return;
      }
      set({ open: true, title: opts.title, body: opts.body, danger: opts.danger === true, resolve });
    });
  },
  answer(v) {
    const r = get().resolve;
    set({ open: false, resolve: null });
    r?.(v);
  },
}));

export function confirmDialog(opts: ConfirmOptions): Promise<boolean> {
  return useConfirm.getState().ask(opts);
}
