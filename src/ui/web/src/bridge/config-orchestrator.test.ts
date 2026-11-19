import { afterEach, describe, expect, it, vi } from 'vitest';
import type { RbConfig, RbVolumeAttach } from '@protocol/types';
import type { Result } from './transport';
import {
  VALID_BLOCK_SIZES,
  PartialSettingsCommitError,
  commitSequentialEngineSettings,
  commitUnifiedConfig,
  validateAndSanitizeIntent,
  type BootPolicy,
  type CommandResult,
  type OrchestratorPorts,
  type RbfFeatName,
  type UnifiedEngineIntent,
} from './config-orchestrator';

const ok: CommandResult = { ok: true };
const cfg = (blockSize: number) => ({ BlockSize: blockSize }) as unknown as RbConfig;
const intent = (over: Partial<UnifiedEngineIntent> = {}): UnifiedEngineIntent => ({
  config: cfg(4096),
  featureFlags: 0x1 | 0x2,
  ...over,
});

function ports() {
  const calls: string[] = [];
  const p: OrchestratorPorts = {
    setConfig: vi.fn(async (_input: RbConfig) => { calls.push('SET_CONFIG'); return ok; }),
    setBootPolicy: vi.fn(async (_next: BootPolicy) => { calls.push('BOOTCFG'); return ok; }),
    attachVolume: vi.fn(async (_input: RbVolumeAttach) => { calls.push('ATTACH'); return ok; }),
    setFeature: vi.fn(async (_name: RbfFeatName, _on: boolean) => { calls.push('RBFEAT'); return ok; }),
  };
  return { p, calls };
}

describe('validateAndSanitizeIntent (R4 / R5 / R6 boundary)', () => {
  it('rejects 131072 and any size outside the whitelist', () => {
    const r = validateAndSanitizeIntent(intent({ config: cfg(131072) }));
    expect(r.valid).toBe(false);
    expect(r.error).toContain('131072');
    expect(VALID_BLOCK_SIZES).toEqual([4096, 8192, 16384, 32768, 65536]);
  });

  it('strips WBCOAL (R4) and L2 while keeping PREFETCH|HOT', () => {
    const r = validateAndSanitizeIntent(intent({ featureFlags: 0x1 | 0x2 | 0x4 | 0x8 | 0x10 }));
    expect(r.valid).toBe(true);
    expect(r.sanitizedFlags).toBe(0x3);
    expect(r.warnings.join(' ')).toContain('R4');
  });

  it('normalizes letters: drops C:, de-duplicates, sorts, keeps D-Z only', () => {
    const r = validateAndSanitizeIntent(intent({ attachLetters: ['e:', 'D:\\', 'C:', 'e', '1'] }));
    expect(r.normalizedLetters).toEqual(['D', 'E']);
  });

  it('produces one /rb/rbfeat step per exposed feature name', () => {
    const r = validateAndSanitizeIntent(intent({ featureFlags: 0x1 | 0x20 }));
    expect(r.featureSteps).toEqual([
      { name: 'PREFETCH', on: true },
      { name: 'HOT', on: false },
      { name: 'THERMAL', on: false },
      { name: 'ETW', on: true },
    ]);
  });
});

describe('commitUnifiedConfig (real channels only)', () => {
  it('commits SET_CONFIG -> bootcfg -> method-8 attach -> per-name rbfeat in order', async () => {
    const { p, calls } = ports();
    const r = await commitUnifiedConfig(intent({
      config: cfg(65536),
      bootPolicy: { AutoAttach: 1, ResumeOnBoot: 1 },
      attachLetters: ['D:', 'E'],
    }), p);

    expect(r.ok).toBe(true);
    expect(r.stepsExecuted[0]).toBe('SET_CONFIG');
    expect(r.stepsExecuted[1]).toBe('POST /rb/bootcfg');
    expect(r.stepsExecuted).toContain('ATTACH_VOLUME D:');
    expect(r.stepsExecuted).toContain('/rb/rbfeat PREFETCH=on');
    expect(calls[0]).toBe('SET_CONFIG');

    // /rb/bootcfg does NOT accept ResumeLetters; letters go through ATTACH_VOLUME.
    const policy = vi.mocked(p.setBootPolicy).mock.calls[0]![0];
    expect(policy).toEqual({ AutoAttach: 1, ResumeOnBoot: 1 });
    expect(policy).not.toHaveProperty('ResumeLetters');
    expect(vi.mocked(p.attachVolume).mock.calls[0]![0]).toMatchObject({ Letter: 'D', Attach: 1 });
  });

  it('stops at the first failing step and never retries a side-effecting command', async () => {
    const { p } = ports();
    vi.mocked(p.setConfig).mockResolvedValueOnce({ ok: false, error: { message: 'denied' } });
    const r = await commitUnifiedConfig(intent({ attachLetters: ['D'] }), p);
    expect(r.ok).toBe(false);
    expect(r.stepsExecuted).toEqual([]);
    expect(p.setBootPolicy).not.toHaveBeenCalled();
    expect(p.attachVolume).not.toHaveBeenCalled();
    expect(p.setFeature).not.toHaveBeenCalled();
  });
});

/* [243 2026-11-15] The sequential coordinator that SettingsPage.onSaved now calls.
 * It must stay fail-closed, must not invent routes, and must not report a
 * /rb/rbfeat CLI failure (HTTP 200 + code != 0) as success. */
describe('commitSequentialEngineSettings (SettingsPage save path)', () => {
  const okVoid: Result<void> = { ok: true, value: undefined };
  const seq = (blockSize: number) => ({ BlockSize: blockSize }) as unknown as RbConfig;

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('rejects 131072 before any byte leaves the control plane (R6)', async () => {
    const push = vi.fn(async () => okVoid);
    await expect(
      commitSequentialEngineSettings(push, { config: seq(131072) }),
    ).rejects.toThrow(/R6/);
    expect(push).not.toHaveBeenCalled();
  });

  it('rejects WBCOAL at the boundary (R4)', async () => {
    const push = vi.fn(async () => okVoid);
    await expect(
      commitSequentialEngineSettings(push, { config: seq(4096), features: { WBCOAL: false } }),
    ).rejects.toThrow(/R4/);
    expect(push).not.toHaveBeenCalled();
  });

  it('commits only SET_CONFIG when no boot policy / features are supplied', async () => {
    const fetchMock = vi.fn();
    vi.stubGlobal('fetch', fetchMock);
    const push = vi.fn(async () => okVoid);
    const r = await commitSequentialEngineSettings(push, { config: seq(4096) });
    expect(r.steps).toEqual(['SET_CONFIG(method=2)']);
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it('sends exactly AutoAttach/ResumeOnBoot to /rb/bootcfg', async () => {
    const fetchMock = vi.fn(async () => ({ ok: true, status: 200, json: async () => ({ code: 0 }) }));
    vi.stubGlobal('fetch', fetchMock);
    const r = await commitSequentialEngineSettings(async () => okVoid, {
      config: seq(4096),
      bootCfg: { AutoAttach: 1, ResumeOnBoot: 0 },
    });
    expect(r.steps).toEqual(['SET_CONFIG(method=2)', 'POST /rb/bootcfg']);
    const [, init] = fetchMock.mock.calls[0] as unknown as [string, RequestInit];
    expect(JSON.parse(String(init.body))).toEqual({ AutoAttach: 1, ResumeOnBoot: 0 });
  });

  it('treats /rb/rbfeat HTTP 200 with code != 0 as a partial failure', async () => {
    const fetchMock = vi.fn(async () => ({
      ok: true,
      status: 200,
      json: async () => ({ code: -1, out: 'rbfeatctl.exe not found' }),
    }));
    vi.stubGlobal('fetch', fetchMock);
    const err = await commitSequentialEngineSettings(async () => okVoid, {
      config: seq(4096),
      features: { PREFETCH: true },
    }).catch((e: unknown) => e);

    expect(err).toBeInstanceOf(PartialSettingsCommitError);
    const partial = err as PartialSettingsCommitError;
    expect(partial.completedSteps).toEqual(['SET_CONFIG(method=2)']);
    expect(partial.engineConfigApplied).toBe(true);
    expect(partial.message).toContain('code=-1');
  });

  it('reports an empty step list when SET_CONFIG itself fails', async () => {
    const push = vi.fn(async (): Promise<Result<void>> => ({
      ok: false,
      error: { code: 'IOCTL_FAILED', message: 'denied' },
    }));
    const err = await commitSequentialEngineSettings(push, { config: seq(4096) }).catch(
      (e: unknown) => e,
    );
    expect(err).toBeInstanceOf(PartialSettingsCommitError);
    expect((err as PartialSettingsCommitError).engineConfigApplied).toBe(false);
    expect((err as PartialSettingsCommitError).message).toBe('denied');
  });
});
