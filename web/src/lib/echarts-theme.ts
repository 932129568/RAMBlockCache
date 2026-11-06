import * as echarts from 'echarts/core';

/**
 * ECharts 主题（rbc）——一次性从设计令牌取色注册，
 * 图表组件不再各自散写 axis/legend/tooltip 样式（库适配：主题化是 echarts 规范用法）。
 */

let registered = false;

export function ensureRbcTheme(): void {
  if (registered) return;
  const css = getComputedStyle(document.documentElement);
  const v = (name: string, fallback: string): string =>
    css.getPropertyValue(name).trim() || fallback;
  const grid = v('--rbc-chart-grid', '#23232b');
  const axis = v('--rbc-chart-axis-text', '#9a9aa5');
  echarts.registerTheme('rbc', {
    color: [
      v('--rbc-chart-1', '#4cc2ff'),
      v('--rbc-chart-2', '#2e86d6'),
      v('--rbc-chart-heat-3', '#4cc2ff'),
      v('--rbc-success-fg', '#63d6a0'),
      v('--rbc-warning-fg', '#f2b84b'),
      v('--rbc-danger-fg', '#f27a72'),
    ],
    backgroundColor: 'transparent',
    textStyle: { fontFamily: v('--rbc-font-mono', 'Consolas, monospace') },
    axisPointer: {
      lineStyle: { color: axis },
      crossStyle: { color: axis },
    },
    tooltip: {
      backgroundColor: 'rgba(34,34,42,0.95)',
      borderColor: v('--rbc-border-strong', '#3a3a45'),
      textStyle: { color: v('--rbc-text-primary', '#ededf2'), fontSize: 12 },
    },
    categoryAxis: {
      axisLine: { lineStyle: { color: grid } },
      axisTick: { lineStyle: { color: grid } },
      axisLabel: { color: axis },
      splitLine: { show: false },
    },
    valueAxis: {
      axisLine: { show: false },
      axisLabel: { color: axis },
      splitLine: { lineStyle: { color: grid, type: 'dashed' as const } },
    },
    legend: { textStyle: { color: axis } },
  });
  registered = true;
}
