import { describe, expect, it, vi } from 'vitest';
import { RB_WRITEMODE } from '@protocol/constants';
import type { RbVolumeAttach, RbVolumeInfo } from '@protocol/types';
import {
  attachDataVolumesWithL1Profile,
  selectAttachableDataVolumes,
  type VolumeAttachPorts,
} from './volume-attach-service';

const vol = (over: Partial<RbVolumeInfo> = {}): RbVolumeInfo => ({
  Letter: 'D', Attached: 0, IsSystem: 0, Bound: 1,
  ReadEnabled: 1, WriteEnabled: 1, WriteMode: 0,
  ...over,
} as RbVolumeInfo);

describe('selectAttachableDataVolumes', () => {
  it('excludes C:, system volumes, unbound volumes and already attached ones', () => {
    const picked = selectAttachableDataVolumes([
      vol({ Letter: 'E' }), vol({ Letter: 'C', IsSystem: 1 }),
      vol({ Letter: 'F', Bound: 0 }), vol({ Letter: 'G', Attached: 1 }),
      vol({ Letter: 'D', IsSystem: 1 }),
    ]);
    expect(picked.map((v) => v.Letter)).toEqual(['E']);
  });

  it('never invents a letter when the driver enumerated zero volumes', () => {
    expect(selectAttachableDataVolumes([])).toEqual([]);
  });
});

describe('attachDataVolumesWithL1Profile', () => {
  it('attaches real candidates serially with Attach=1 and no write-back switch', async () => {
    const attachVolume = vi.fn(async (_input: RbVolumeAttach) => ({ ok: true }));
    const ports: VolumeAttachPorts = { attachVolume };
    const r = await attachDataVolumesWithL1Profile([vol({ Letter: 'E' }), vol({ Letter: 'D' })], ports);
    expect(r).toEqual({ ok: true, attachedLetters: ['D', 'E'] });
    expect(attachVolume).toHaveBeenCalledTimes(2);
    expect(attachVolume.mock.calls[0]![0]).toMatchObject({
      Letter: 'D', Attach: 1, WriteMode: RB_WRITEMODE.GLOBAL,
    });
  });

  it('reports partial success and the failing letter without retrying', async () => {
    let n = 0;
    const attachVolume = vi.fn(async () => {
      n += 1;
      return n === 2 ? { ok: false, error: { message: 'busy' } } : { ok: true };
    });
    const r = await attachDataVolumesWithL1Profile(
      [vol({ Letter: 'D' }), vol({ Letter: 'E' })], { attachVolume });
    expect(r.ok).toBe(false);
    expect(r.attachedLetters).toEqual(['D']);
    expect(r.failedLetter).toBe('E');
    expect(attachVolume).toHaveBeenCalledTimes(2);
  });

  it('does not issue any command when there is no safe candidate', async () => {
    const attachVolume = vi.fn(async () => ({ ok: true }));
    const r = await attachDataVolumesWithL1Profile([vol({ Letter: 'C', IsSystem: 1 })], { attachVolume });
    expect(r.ok).toBe(false);
    expect(attachVolume).not.toHaveBeenCalled();
  });
});
