import { format } from 'date-fns';
import { zhCN } from 'date-fns/locale';

/**
 * 时间工具（库适配：date-fns v4）。
 * 驱动 Ticks 为 NT FILETIME（100ns / 1601-01-01 起，rbdriver.c KeQuerySystemTimePrecise）。
 */

const FILETIME_EPOCH_DIFF_MS = 11644473600000;

export function filetimeToDate(ticks: number): Date | null {
  if (ticks <= 0) return null;
  const ms = ticks / 10000 - FILETIME_EPOCH_DIFF_MS;
  const d = new Date(ms);
  return Number.isFinite(d.getTime()) ? d : null;
}

export function filetimeToTime(ticks: number): string {
  const d = filetimeToDate(ticks);
  return d ? format(d, 'HH:mm:ss', { locale: zhCN }) : '—';
}

export function filetimeToFull(ticks: number): string {
  const d = filetimeToDate(ticks);
  return d ? format(d, 'yyyy-MM-dd HH:mm:ss.SSS', { locale: zhCN }) : '—';
}
