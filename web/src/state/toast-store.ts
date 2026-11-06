import { create } from 'zustand';

/**
 * 命令回执 Toast（radix Toast 承载）——应急/加速/设置/导出的统一反馈面。
 * 替代各页内联 result 段落：反馈全局可见、自动消失、可手动关闭。
 */

export interface ToastItem {
  id: number;
  tone: 'ok' | 'warn' | 'danger' | 'info';
  title: string;
  body?: string | undefined;
}

interface ToastState {
  items: ToastItem[];
  push: (t: Omit<ToastItem, 'id'>) => void;
  dismiss: (id: number) => void;
}

let nextId = 1;

export const useToastStore = create<ToastState>((set) => ({
  items: [],
  push(t) {
    const id = nextId++;
    set((s) => ({ items: [...s.items.slice(-4), { id, ...t }] }));
    // 控制台场景：回执无需常驻，6s 自动出列（swipe/手动关闭走 radix 事件）
    setTimeout(() => set((s) => ({ items: s.items.filter((i) => i.id !== id) })), 6000);
  },
  dismiss(id) {
    set((s) => ({ items: s.items.filter((i) => i.id !== id) }));
  },
}));

export function toast(tone: ToastItem['tone'], title: string, body?: string): void {
  useToastStore.getState().push({ tone, title, body });
}
