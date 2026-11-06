import { clsx, type ClassValue } from 'clsx';
import { twMerge } from 'tailwind-merge';

/** 设计层类名组合：clsx 条件拼接 + tailwind-merge 冲突消解（后写胜出） */
export function cn(...inputs: ClassValue[]): string {
  return twMerge(clsx(inputs));
}
