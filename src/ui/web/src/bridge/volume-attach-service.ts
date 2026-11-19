/**
 * Volume hot-attach service (240 module 3, rewritten against the real API).
 *
 * The original sample invented drive letters ['D','E','F','G','H'] and wrote
 * ResumeLetters through /rb/bootcfg. Neither is real: /rb/bootcfg accepts only
 * AutoAttach / ResumeOnBoot, and the only truthful source of drive letters is
 * GET_VOLUMES (transport.getVolumes(), /rb/rpc method 7).
 *
 * This module never guesses a letter and never attaches a system/boot volume.
 */
import { RB_WRITEMODE } from '@protocol/constants';
import type { RbVolumeAttach, RbVolumeInfo } from '@protocol/types';
import type { CommandResult } from './config-orchestrator';

export interface VolumeAttachPorts {
  attachVolume(input: RbVolumeAttach): Promise<CommandResult>;
}

export interface VolumeAttachResult {
  ok: boolean;
  attachedLetters: string[];
  failedLetter?: string | undefined;
  error?: string | undefined;
}

/** Real, safe candidates: enumerated, bound, not system/boot, not yet attached. */
export function selectAttachableDataVolumes(
  volumes: readonly RbVolumeInfo[],
): RbVolumeInfo[] {
  return volumes
    .filter((v) => v.Letter.toUpperCase() !== 'C')
    .filter((v) => v.IsSystem !== 1)
    .filter((v) => v.Bound !== 0)
    .filter((v) => v.Attached !== 1)
    .sort((a, b) => a.Letter.localeCompare(b.Letter));
}

/**
 * Serial attach, fail-fast. ATTACH_VOLUME has kernel side effects and the
 * transport deliberately has retry=0, so a partial success is reported rather
 * than silently retried.
 */
export async function attachDataVolumesWithL1Profile(
  volumes: readonly RbVolumeInfo[],
  ports: VolumeAttachPorts,
): Promise<VolumeAttachResult> {
  const candidates = selectAttachableDataVolumes(volumes);
  if (candidates.length === 0) {
    return {
      ok: false,
      attachedLetters: [],
      error: 'No attachable non-system data volume was enumerated by the driver.',
    };
  }

  const attachedLetters: string[] = [];
  for (const v of candidates) {
    const input: RbVolumeAttach = {
      Letter: v.Letter,
      Attach: 1,
      ReadEnabled: 1,
      WriteEnabled: 1,
      // Keep the per-volume write mode unchanged (GLOBAL follows the engine
      // default); switching to write-back is a separate, user-confirmed action
      // and is blocked by R9 while antivirus/Verifier is present.
      WriteMode: RB_WRITEMODE.GLOBAL,
    };
    const r = await ports.attachVolume(input);
    if (!r.ok) {
      return {
        ok: false,
        attachedLetters,
        failedLetter: v.Letter,
        error: r.error?.message ?? `attach ${v.Letter}: failed`,
      };
    }
    attachedLetters.push(v.Letter);
  }
  return { ok: true, attachedLetters };
}
