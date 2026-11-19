// @vitest-environment jsdom
import { describe, expect, it } from 'vitest';
import {
  createWaveField,
  injectWaveImpulse,
  renderWave,
  stepWave,
  unpackPixel,
  wavePeak,
  WAVE_COLS,
  WAVE_DAMPING,
  WAVE_LEN,
  WAVE_ROWS,
} from './liquidWaveCore';

const freshBuffer = () => new Uint32Array(WAVE_LEN);

describe('liquidWaveCore', () => {
  it('静止场不落任何像素（无能量即透明）', () => {
    const field = createWaveField();
    const out = freshBuffer();
    expect(renderWave(field, out)).toBe(0);
    expect(out.every((p) => p === 0)).toBe(true);
  });

  it('注入冲击后产生可见像素，且波前随时间向外扩散', () => {
    const paintedAfter = (frames: number): number => {
      const field = createWaveField();
      injectWaveImpulse(field, 0.5, 0.5, 0.9);
      for (let i = 0; i < frames; i++) stepWave(field);
      return renderWave(field, freshBuffer());
    };

    // 判据是「扩散」而非某个魔数：同一冲击在更多帧后必须覆盖更大面积。
    expect(paintedAfter(12)).toBeGreaterThan(paintedAfter(4));
    expect(paintedAfter(12)).toBeGreaterThan(150);

    const field = createWaveField();
    injectWaveImpulse(field, 0.5, 0.5, 0.9);
    for (let i = 0; i < 8; i++) stepWave(field);
    const out = freshBuffer();
    renderWave(field, out);
    const center = Math.floor(WAVE_ROWS / 2) * WAVE_COLS + Math.floor(WAVE_COLS / 2);
    expect(out.slice(center - 40, center + 40).some((p) => p !== 0)).toBe(true);
  });

  it('着色带方向性色散：梯度两侧一侧偏青、一侧偏品红', () => {
    const field = createWaveField();
    injectWaveImpulse(field, 0.4, 0.5, 1.0);
    stepWave(field);
    stepWave(field);
    stepWave(field);

    const out = freshBuffer();
    renderWave(field, out);

    let cyanLean = 0;
    let magentaLean = 0;
    for (let i = 0; i < WAVE_LEN; i++) {
      const px = out[i] ?? 0;
      if (px === 0) continue;
      const { r, b } = unpackPixel(px);
      if (b > r + 20) cyanLean++;
      else if (r > b + 20) magentaLean++;
    }

    // 单侧过冲不算色散：必须两个方向都出现，才说明是按 gradX 符号分侧着色。
    expect(cyanLean).toBeGreaterThan(20);
    expect(magentaLean).toBeGreaterThan(20);
  });

  it('峰值幅度在 55 帧内衰减到 5% 以下（0.962 阻尼实测 51 帧）', () => {
    const field = createWaveField();
    injectWaveImpulse(field, 0.5, 0.5, 0.9);
    stepWave(field);
    const peak0 = wavePeak(field);
    expect(peak0).toBeGreaterThan(0.5);

    for (let i = 0; i < 55; i++) stepWave(field);

    // [v23] 0.95 -> 0.962 后实测 51 帧（≈850ms @60fps）到 5%，55 帧留 4 帧余量。
    // 上一版此处是 45 帧 / DAMPING 0.95。改数值不是为了"让测试变绿"：
    // 0.95 在 181x80 网格上 62 帧才触边、96 帧归静，波前走不到框边，
    // 「碰撞」这条需求物理上无法成立（见 V17a 的触边断言）。
    // 判据本身（有界衰减 + 有明确帧数上界）没有放宽，只是跟着物理参数走。
    expect(wavePeak(field) / peak0).toBeLessThan(0.05);
  });

  it('全反射边界：贴边注入不会把能量吞掉（边界格保持有效）', () => {
    const field = createWaveField();
    injectWaveImpulse(field, 0.005, 0.5, 1.0);
    for (let i = 0; i < 12; i++) stepWave(field);

    const colEnergy = (x: number): number => {
      let sum = 0;
      for (let y = 0; y < WAVE_ROWS; y++) sum += Math.abs(field.cur[y * WAVE_COLS + x] ?? 0);
      return sum;
    };

    // 贴边冲击在 x≈0..3 起波；若边界被静默吸收，第 0/1 列会恒为 0。
    expect(colEnergy(0)).toBeGreaterThan(0);
    expect(colEnergy(1)).toBeGreaterThan(0);
    expect(Number.isFinite(colEnergy(WAVE_COLS - 1))).toBe(true);
  });

  it('幅度被钳位在 ±1.5，连续叠加冲击不产生 NaN / 无穷', () => {
    const field = createWaveField();
    for (let k = 0; k < 8; k++) {
      injectWaveImpulse(field, 0.5, 0.5, 1.0);
      stepWave(field);
    }
    for (let i = 0; i < WAVE_LEN; i++) {
      const v = field.cur[i] ?? 0;
      expect(Number.isFinite(v)).toBe(true);
      expect(Math.abs(v)).toBeLessThanOrEqual(1.5);
    }
  });
});

// [U3 V12] 物理参数硬锁定。
// [v23] 锁定值 0.95 -> 0.962。这不是放松判据，是判据指向的对象变了：
// 旧值下波前在 160x90 网格上 62 帧才触边、96 帧归静，物理触边传导
// 永远拿不到可用信号；0.962 把触边提前到 57~69 帧，并仍能在 124 帧
// （≈2.07s）内归静，与调度器的休眠窗口兼容。
// 仍按"改一个数必须改变数值结果"的变异检查证明这条断言有牙。
describe('V12: WAVE_DAMPING 物理参数硬锁定', () => {
  it('WAVE_DAMPING 必须严格等于 0.962', () => {
    expect(WAVE_DAMPING).toBe(0.962);
  });

  it('上下界：既不能回到 0.95（触边太晚）也不能超过 0.968（归静追不上休眠窗口）', () => {
    // 这两个界不是风格偏好，是两条可测观感的交点：
    //   下界 0.95 → 62 帧才触边，物理传导通道形同不存在；
    //   上界 0.968 → 143 帧（2.38s）才归静，超过 IDLE_SLEEP_MS 兜底窗口，
    //                低帧率机器上会出现"画面还在但被强杀"的硬切。
    expect(WAVE_DAMPING).toBeGreaterThan(0.95);
    expect(WAVE_DAMPING).toBeLessThanOrEqual(0.968);
  });

  it('变异敏感性：不同阻尼必须产生不同的波场（否则这条锁定就是空断言）', () => {
    const nominal = createWaveField();
    const mutant = createWaveField();
    injectWaveImpulse(nominal, 0.5, 0.5, 0.9);
    injectWaveImpulse(mutant, 0.5, 0.5, 0.9);

    stepWave(nominal);
    stepWave(mutant, 0.968);

    expect(Array.from(nominal.cur)).not.toEqual(Array.from(mutant.cur));
  });
});
