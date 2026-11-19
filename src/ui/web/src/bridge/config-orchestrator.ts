/**
 * Control-plane orchestration for the three REAL channels in this repository.
 *
 * Origin: deliverables/240-full-stack-architecture-guidance-2026-11-14.md
 * module 2. The sample in that document was NOT copied: it posted to
 * `/rb/config` and `/rb/features` (neither route exists) and described
 * `method: 8` as "bootcfg restore". The real wiring is:
 *   - SET_CONFIG            -> transport.setConfig()        -> /rb/rpc method 2
 *   - GET_VOLUMES           -> transport.getVolumes()       -> /rb/rpc method 7
 *   - ATTACH_VOLUME         -> transport.attachVolume()     -> /rb/rpc method 8
 *   - boot policy           -> POST /rb/bootcfg {AutoAttach, ResumeOnBoot}
 *     (the route does NOT accept ResumeLetters; letters are attached per volume)
 *   - feature bits          -> POST /rb/rbfeat {name, on}   (per-name whitelist)
 *
 * All side effects are injected through ports, so this module is testable
 * without a live driver and never guesses a route or a drive letter.
 */
import { RB_WRITEMODE } from '@protocol/constants';
import type { RbConfig, RbVolumeAttach } from '@protocol/types';
import type { Result } from './transport';

export const VALID_BLOCK_SIZES = [4096, 8192, 16384, 32768, 65536] as const;
export type ValidBlockSize = (typeof VALID_BLOCK_SIZES)[number];

/** Mirrors src/driver/rb_common2.h; checked by Gate23 check_ts_proto_sync.py. */
export const RB2_FLAGS = {
  PREFETCH: 0x1,
  HOT: 0x2,
  L2: 0x4,
  WBCOAL: 0x8, // R4: never enable
  MEMGUARD: 0x10,
  ETW: 0x20,
  THERMAL: 0x40,
} as const;

/** Names accepted by POST /rb/rbfeat (bridge/server/index.ts whitelist). */
export const RBFEAT_SETTABLE = ['PREFETCH', 'HOT', 'THERMAL', 'ETW'] as const;
export type RbfFeatName = (typeof RBFEAT_SETTABLE)[number];

export interface CommandResult {
  ok: boolean;
  error?: { message: string } | undefined;
}

export interface BootPolicy {
  AutoAttach?: number | undefined;
  ResumeOnBoot?: number | undefined;
}

export interface UnifiedEngineIntent {
  config: RbConfig;
  featureFlags: number;
  bootPolicy?: BootPolicy | undefined;
  attachLetters?: string[] | undefined;
}

export interface OrchestratorPorts {
  setConfig(input: RbConfig): Promise<CommandResult>;
  setBootPolicy(next: BootPolicy): Promise<CommandResult>;
  attachVolume(input: RbVolumeAttach): Promise<CommandResult>;
  setFeature(name: RbfFeatName, on: boolean): Promise<CommandResult>;
}

export interface SanitizedIntent {
  valid: boolean;
  error?: string | undefined;
  sanitizedFlags: number;
  warnings: string[];
  normalizedLetters: string[];
  featureSteps: Array<{ name: RbfFeatName; on: boolean }>;
}

/** Red-line gate at the boundary (R4 / R5 / R6) before anything is written. */
export function validateAndSanitizeIntent(intent: UnifiedEngineIntent): SanitizedIntent {
  const warnings: string[] = [];
  const blockSize = intent.config.BlockSize;
  if (!VALID_BLOCK_SIZES.includes(blockSize as ValidBlockSize)) {
    return {
      valid: false,
      error: `BlockSize=${blockSize} rejected: only ${VALID_BLOCK_SIZES.join('/')} are allowed (131072 is permanently banned, R6).`,
      sanitizedFlags: 0,
      warnings,
      normalizedLetters: [],
      featureSteps: [],
    };
  }

  let flags = intent.featureFlags >>> 0;
  if ((flags & RB2_FLAGS.WBCOAL) !== 0) {
    flags &= ~RB2_FLAGS.WBCOAL;
    warnings.push('R4: WBCOAL (0x8) stripped; never enabled from the control plane.');
  }
  if ((flags & RB2_FLAGS.MEMGUARD) !== 0) {
    flags &= ~RB2_FLAGS.MEMGUARD;
    warnings.push('MEMGUARD (0x10) stripped; the kernel discards it and /rb/rbfeat does not expose it.');
  }
  if ((flags & RB2_FLAGS.L2) !== 0) {
    flags &= ~RB2_FLAGS.L2;
    warnings.push('L2 (0x4) not settable via /rb/rbfeat; Phase 4 kernel freeze keeps it off.');
  }

  // System/boot volume C: is always excluded; only real D-Z letters survive.
  const normalizedLetters = Array.from(
    new Set(
      (intent.attachLetters ?? [])
        .map((raw) => raw.trim().replace(/[:\\/]+$/g, '').toUpperCase())
        .filter((letter) => /^[D-Z]$/.test(letter)),
    ),
  ).sort();

  const featureSteps = RBFEAT_SETTABLE.map((name) => ({
    name,
    on: (flags & RB2_FLAGS[name as keyof typeof RB2_FLAGS]) !== 0,
  }));

  return {
    valid: true,
    sanitizedFlags: flags,
    warnings,
    normalizedLetters,
    featureSteps,
  };
}

export interface OrchestratorResult {
  ok: boolean;
  sanitizedFeatureFlags: number;
  stepsExecuted: string[];
  warnings: string[];
  error?: string | undefined;
}

/**
 * Serial commit, fail-closed on the first failing step. No step is retried:
 * SET_CONFIG and ATTACH_VOLUME have kernel side effects.
 */
export async function commitUnifiedConfig(
  intent: UnifiedEngineIntent,
  ports: OrchestratorPorts,
): Promise<OrchestratorResult> {
  const check = validateAndSanitizeIntent(intent);
  if (!check.valid) {
    return {
      ok: false,
      sanitizedFeatureFlags: 0,
      stepsExecuted: [],
      warnings: check.warnings,
      error: check.error,
    };
  }

  const steps: string[] = [];
  const run = async (label: string, action: () => Promise<CommandResult>): Promise<boolean> => {
    const r = await action();
    if (!r.ok) return false;
    steps.push(label);
    return true;
  };

  if (!(await run('SET_CONFIG', () => ports.setConfig(intent.config)))) {
    return { ok: false, sanitizedFeatureFlags: check.sanitizedFlags, stepsExecuted: steps, warnings: check.warnings, error: 'SET_CONFIG failed' };
  }

  if (intent.bootPolicy) {
    // /rb/bootcfg accepts ONLY AutoAttach / ResumeOnBoot. ResumeLetters is a
    // read-only field; attaching letters is done explicitly below.
    const policy: BootPolicy = {
      AutoAttach: intent.bootPolicy.AutoAttach,
      ResumeOnBoot: intent.bootPolicy.ResumeOnBoot,
    };
    if (!(await run('POST /rb/bootcfg', () => ports.setBootPolicy(policy)))) {
      return { ok: false, sanitizedFeatureFlags: check.sanitizedFlags, stepsExecuted: steps, warnings: check.warnings, error: 'boot policy update failed' };
    }
  }

  for (const letter of check.normalizedLetters) {
    const input: RbVolumeAttach = {
      Letter: letter,
      Attach: 1,
      ReadEnabled: 1,
      WriteEnabled: 1,
      WriteMode: RB_WRITEMODE.GLOBAL,
    };
    if (!(await run(`ATTACH_VOLUME ${letter}:`, () => ports.attachVolume(input)))) {
      return { ok: false, sanitizedFeatureFlags: check.sanitizedFlags, stepsExecuted: steps, warnings: check.warnings, error: `attach ${letter}: failed` };
    }
  }

  for (const step of check.featureSteps) {
    if (!(await run(`/rb/rbfeat ${step.name}=${step.on ? 'on' : 'off'}`, () => ports.setFeature(step.name, step.on)))) {
      return { ok: false, sanitizedFeatureFlags: check.sanitizedFlags, stepsExecuted: steps, warnings: check.warnings, error: `feature ${step.name} failed` };
    }
  }

  return {
    ok: true,
    sanitizedFeatureFlags: check.sanitizedFlags,
    stepsExecuted: steps,
    warnings: check.warnings,
  };
}

// ============================================================================
// [243 APPEND 2026-11-15] Sequential engine settings coordinator.
// Origin: deliverables/243-ios26-liquid-glass-and-fullstack-wiring-2026-11-15.md 4.2
//
// Wired into SettingsPage.onSaved. All 13 exports above are untouched, so the
// 11 existing unit tests keep passing. Step 1 goes through the injected
// transport port ( /rb/rpc method 2 ). Steps 2 and 3 reuse the SAME relative
// fetch calls SettingsPage already uses for /rb/bootcfg and /rb/rbfeat, because
// /rb/rbfeat always answers HTTP 200 with { code, out } - checking res.ok alone
// would report a CLI failure as success.
// ============================================================================

export type SafeFeatureName = 'PREFETCH' | 'HOT' | 'THERMAL' | 'ETW';

export interface BootCfgIntent {
  AutoAttach: number;
  ResumeOnBoot: number;
}

export interface SequentialSavePayload {
  config: RbConfig;
  bootCfg?: BootCfgIntent | undefined;
  features?: Partial<Record<SafeFeatureName | 'WBCOAL', boolean>> | undefined;
}

export class PartialSettingsCommitError extends Error {
  public readonly completedSteps: string[];
  public readonly engineConfigApplied: boolean;

  constructor(message: string, completedSteps: string[]) {
    super(message);
    this.name = 'PartialSettingsCommitError';
    this.completedSteps = completedSteps;
    this.engineConfigApplied = completedSteps.includes('SET_CONFIG(method=2)');
  }
}

/** Red-line gate (R4 / R6) before any byte leaves the control plane. */
export function assertSafeEnginePayload(payload: SequentialSavePayload): void {
  const bs = payload.config.BlockSize;
  if (bs === 131072 || !VALID_BLOCK_SIZES.includes(bs as ValidBlockSize)) {
    throw new Error(
      `红线 R6 拦截：BlockSize=${bs} 非法，仅允许 4096/8192/16384/32768/65536（131072 永久禁用）。`,
    );
  }
  if (payload.features && 'WBCOAL' in payload.features && payload.features.WBCOAL !== undefined) {
    throw new Error('红线 R4 拦截：WBCOAL 特性位已被永久禁开，禁止通过设置界面下发。');
  }
}

/**
 * Serial commit: SET_CONFIG (method 2) -> /rb/bootcfg -> /rb/rbfeat.
 * Fail-closed on the first failure; when the engine config already landed the
 * caller gets PartialSettingsCommitError so the UI can say "written but not
 * fully synced" instead of a blanket "save failed".
 */
export async function commitSequentialEngineSettings(
  setConfigFn: (cfg: RbConfig) => Promise<Result<void>>,
  payload: SequentialSavePayload,
): Promise<{ steps: string[] }> {
  assertSafeEnginePayload(payload);
  const steps: string[] = [];

  // Step 1: full RB_CONFIG write through the real transport (/rb/rpc method 2).
  const cfgRes = await setConfigFn(payload.config);
  if (!cfgRes.ok) {
    throw new PartialSettingsCommitError(
      cfgRes.error.message || 'SET_CONFIG 返回失败',
      steps,
    );
  }
  steps.push('SET_CONFIG(method=2)');

  // Step 2: only when the boot policy query has actually loaded. Passing
  // {0,0} on an undefined query would silently clear the user's AutoAttach.
  if (payload.bootCfg) {
    const bootRes = await fetch('/rb/bootcfg', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        AutoAttach: payload.bootCfg.AutoAttach ? 1 : 0,
        ResumeOnBoot: payload.bootCfg.ResumeOnBoot ? 1 : 0,
      }),
    });
    if (!bootRes.ok) {
      throw new PartialSettingsCommitError(
        `同步开机挂载策略 (/rb/bootcfg) 失败: HTTP ${bootRes.status}`,
        steps,
      );
    }
    steps.push('POST /rb/bootcfg');
  }

  // Step 3: per-name feature bits; /rb/rbfeat answers HTTP 200 with { code, out }.
  if (payload.features) {
    const allowedKeys: SafeFeatureName[] = ['PREFETCH', 'HOT', 'THERMAL', 'ETW'];
    for (const name of allowedKeys) {
      const on = payload.features[name];
      if (typeof on !== 'boolean') continue;
      const featRes = await fetch('/rb/rbfeat', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name, on }),
      });
      const body = (await featRes.json().catch(() => ({}))) as { code?: number; out?: string };
      if (!featRes.ok || body.code !== 0) {
        throw new PartialSettingsCommitError(
          `同步特性位 ${name}=${on ? 1 : 0} (/rb/rbfeat) 失败: HTTP ${featRes.status}, code=${
            body.code ?? 'n/a'
          } ${body.out ?? ''}`.trim(),
          steps,
        );
      }
      steps.push(`POST /rb/rbfeat(${name}=${on ? 1 : 0})`);
    }
  }

  return { steps };
}
