import { useEffect, useRef } from 'react';

export interface VisibilityPollOptions {
  /** 前台轮询间隔（毫秒），默认 1000ms */
  intervalMs?: number;
  /** 页面隐藏时是否完全暂停，默认 true */
  pauseWhenHidden?: boolean;
  /** 关闭订阅但保留 hook，默认 true */
  enabled?: boolean;
}

/**
 * 可见性感知轮询：窗口隐藏/最小化时撤掉全部定时器，恢复可见时立即刷新一次
 * 并重启。消费点只有两处：App.tsx 的 5s 离线重连、trace-store.ts
 * useChannels 的 500ms 通道快照；两处都是同步回调，故不等待 Promise。
 * enabled=false 用于 useChannels 的受控场景，避免同一页面开第二条定时器。
 */
export function useVisibilityAwarePoll(
  callback: () => void | Promise<void>,
  options: VisibilityPollOptions = {},
): void {
  const { intervalMs = 1000, pauseWhenHidden = true, enabled = true } = options;
  const savedCallback = useRef(callback);

  useEffect(() => {
    savedCallback.current = callback;
  }, [callback]);

  useEffect(() => {
    if (!enabled) return;
    let timerId: number | null = null;
    let cancelled = false;

    const clearTimer = () => {
      if (timerId !== null) {
        window.clearTimeout(timerId);
        timerId = null;
      }
    };

    const scheduleNext = () => {
      clearTimer();
      if (cancelled) return;
      if (pauseWhenHidden && document.visibilityState === 'hidden') return;
      timerId = window.setTimeout(() => {
        timerId = null;
        if (cancelled) return;
        if (pauseWhenHidden && document.visibilityState === 'hidden') return;
        void savedCallback.current();
        scheduleNext();
      }, intervalMs);
    };

    const onVisibilityChange = () => {
      if (!pauseWhenHidden) return;
      if (document.visibilityState === 'hidden') {
        clearTimer();
      } else {
        void savedCallback.current();
        scheduleNext();
      }
    };

    scheduleNext();
    document.addEventListener('visibilitychange', onVisibilityChange);
    return () => {
      cancelled = true;
      clearTimer();
      document.removeEventListener('visibilitychange', onVisibilityChange);
    };
  }, [intervalMs, pauseWhenHidden, enabled]);
}
