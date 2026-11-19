import { describe, expect, it } from 'vitest';
import {
  RB2F_L2,
  RB2F_WBCOAL,
  classifyL2Window,
  formatRbLogStatus,
  type L2TelemetrySample,
} from './l2Diagnostics';

const base: L2TelemetrySample = {
  dCoreMiss: 8,
  dL2Hits: 16,
  dL2Miss: 0,
  l2State: 1,
  l2Halt: 0,
  featureFlags: RB2F_L2,
  attachedVolumeCount: 1,
  blockSize: 4096,
  selfDisabled: false,
};

describe('classifyL2Window', () => {
  it('PASS_SERVING only when the three J6 criteria all hold', () => {
    expect(classifyL2Window(base).state).toBe('PASS_SERVING');
  });

  it('INCONCLUSIVE when a core full hit absorbed the probe', () => {
    expect(classifyL2Window({ ...base, dCoreMiss: 0 }).state).toBe('INCONCLUSIVE_CORE_HIT');
  });

  it('PARTIAL_SERVING when L2 misses appear', () => {
    expect(classifyL2Window({ ...base, dL2Miss: 2 }).state).toBe('PARTIAL_SERVING');
  });

  it('COLD_FILLING when there are no hits', () => {
    expect(classifyL2Window({ ...base, dL2Hits: 0, dL2Miss: 8 }).state).toBe('COLD_FILLING');
  });

  it('LAZY_UNOPENED before the first core read miss', () => {
    expect(classifyL2Window({ ...base, l2State: 0 }).state).toBe('LAZY_UNOPENED');
  });

  it('HALT_GUARDED outranks the opening gates', () => {
    expect(classifyL2Window({ ...base, l2Halt: 1 }).state).toBe('HALT_GUARDED');
  });

  it('SELF_DISABLED has the highest priority', () => {
    expect(classifyL2Window({ ...base, selfDisabled: true, l2Halt: 1 }).state).toBe('SELF_DISABLED');
  });

  it('GATE_BLOCKED_FLAGS when the L2 bit is clear', () => {
    expect(classifyL2Window({ ...base, featureFlags: 0 }).state).toBe('GATE_BLOCKED_FLAGS');
  });

  it('GATE_BLOCKED_FLAGS when WBCOAL is set (red line R5)', () => {
    expect(classifyL2Window({ ...base, featureFlags: RB2F_L2 | RB2F_WBCOAL }).state).toBe('GATE_BLOCKED_FLAGS');
  });

  it('GATE_BLOCKED_MULTI_VOL when more than one volume is attached', () => {
    expect(classifyL2Window({ ...base, attachedVolumeCount: 2 }).state).toBe('GATE_BLOCKED_MULTI_VOL');
  });

  it('GATE_BLOCKED_BLOCKSIZE when BlockSize*16 is not 64KB', () => {
    expect(classifyL2Window({ ...base, blockSize: 8192 }).state).toBe('GATE_BLOCKED_BLOCKSIZE');
  });
});

describe('formatRbLogStatus (legacy boolean + Phase-4 text dual track)', () => {
  const entry = (status: number, text: string) => ({ level: 0, srcLine: 269, status, text });

  it('reads the legacy boolean Status=1', () => {
    expect(formatRbLogStatus(entry(1, 'system volume opt-in ON by user')).label).toBe('OPT-IN ON (1)');
  });

  it('reads ON from text when Phase-4 Status=0', () => {
    expect(formatRbLogStatus(entry(0, 'system volume opt-in ON')).label).toBe('OPT-IN ON (1)');
  });

  it('reads OFF from text when Phase-4 Status=0', () => {
    expect(formatRbLogStatus(entry(0, 'system volume opt-in OFF')).label).toBe('OPT-IN OFF (0)');
  });

  it('renders STATUS_SUCCESS for an ordinary status 0', () => {
    expect(formatRbLogStatus(entry(0, 'l2: close deferred')).label).toBe('STATUS_SUCCESS (0x0)');
  });

  it('renders a non-zero NTSTATUS as 8-digit hex', () => {
    const result = formatRbLogStatus(entry(-1, 'error'));
    expect(result.label).toBe('0xFFFFFFFF');
    expect(result.tone).toBe('rose');
  });
});
