// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { renderHook } from '@testing-library/react';
import { useVisibilityAwarePoll } from './useVisibilityAwarePoll';

let visibility: DocumentVisibilityState;

function setVisibility(next: DocumentVisibilityState): void {
  visibility = next;
  document.dispatchEvent(new Event('visibilitychange'));
}

beforeEach(() => {
  vi.useFakeTimers();
  visibility = 'visible';
  Object.defineProperty(document, 'visibilityState', {
    configurable: true,
    get: () => visibility,
  });
});

afterEach(() => {
  vi.useRealTimers();
});

describe('useVisibilityAwarePoll', () => {
  it('fires once per interval while visible', () => {
    const cb = vi.fn();
    renderHook(() => useVisibilityAwarePoll(cb, { intervalMs: 1000 }));
    expect(cb).not.toHaveBeenCalled();
    vi.advanceTimersByTime(3000);
    expect(cb).toHaveBeenCalledTimes(3);
  });

  it('holds no timer while hidden and refreshes once on resume', () => {
    const cb = vi.fn();
    renderHook(() => useVisibilityAwarePoll(cb, { intervalMs: 1000 }));
    vi.advanceTimersByTime(1000);
    expect(cb).toHaveBeenCalledTimes(1);
    setVisibility('hidden');
    vi.advanceTimersByTime(5000);
    expect(cb).toHaveBeenCalledTimes(1);
    setVisibility('visible');
    expect(cb).toHaveBeenCalledTimes(2);
    vi.advanceTimersByTime(1000);
    expect(cb).toHaveBeenCalledTimes(3);
  });

  it('does not start when enabled=false', () => {
    const cb = vi.fn();
    renderHook(() =>
      useVisibilityAwarePoll(cb, { intervalMs: 1000, enabled: false }),
    );
    vi.advanceTimersByTime(5000);
    expect(cb).not.toHaveBeenCalled();
  });

  it('keeps polling while hidden when pauseWhenHidden=false', () => {
    const cb = vi.fn();
    renderHook(() =>
      useVisibilityAwarePoll(cb, { intervalMs: 1000, pauseWhenHidden: false }),
    );
    setVisibility('hidden');
    vi.advanceTimersByTime(2000);
    expect(cb).toHaveBeenCalledTimes(2);
  });

  it('stops after unmount', () => {
    const cb = vi.fn();
    const { unmount } = renderHook(() =>
      useVisibilityAwarePoll(cb, { intervalMs: 1000 }),
    );
    vi.advanceTimersByTime(1000);
    unmount();
    vi.advanceTimersByTime(5000);
    expect(cb).toHaveBeenCalledTimes(1);
  });
});
