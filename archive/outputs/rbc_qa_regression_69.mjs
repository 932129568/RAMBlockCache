// QA independent regression teeth -- task #5 (Yan / software-qa-engineer)
// Evaluates the REAL repository sources (no copy of the logic, no engineer's script).
// Run: node27 C:\Users\Public\rbc_qa_regression_69.mjs
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { createRequire, stripTypeScriptTypes } from 'node:module';
import vm from 'node:vm';
import path from 'node:path';

const ROOT = 'C:\\Users\\陈\\Desktop\\RAMBlockCache-V10.4.3-build19-源码包';
const p = (rel) => path.join(ROOT, rel);
const require = createRequire(pathToFileURL(p('web/package.json')).href);
const { z } = require('zod');

let pass = 0, fail = 0;
function rec(id, tag, ok, obs) {
  if (ok) pass++; else fail++;
  console.log(`${id} | ${ok ? 'PASS' : 'FAIL'} | ${tag} | ${obs}`);
}
function guard(id, tag, fn) {
  try { const [ok, obs] = fn(); rec(id, tag, ok, obs); }
  catch (e) { rec(id, tag, false, 'THREW: ' + (e && e.message ? e.message : String(e))); }
}

console.log('== QA independent regression teeth (real repo sources) ==');
console.log('node = ' + process.version + ' | zod = ' + (z.looseObject ? 'v4-api' : 'other'));

// ---- A. decodeConfig: direct native TS import of the real file ----
const { decodeConfig } = await import(pathToFileURL(p('web/bridge/server/proto-decode.ts')).href);
console.log('source A = web/bridge/server/proto-decode.ts (import, type-stripping)');

// ---- B. encodeParams: slice the real definition block out of index.ts, strip TS, eval ----
const idxSrc = readFileSync(p('web/bridge/server/index.ts'), 'utf8');
const bStart = idxSrc.indexOf('const volumeSetSchema = z.object({');
const bEnd = idxSrc.indexOf('const app = new Hono()');
if (bStart < 0 || bEnd < 0 || bEnd <= bStart) throw new Error('cannot slice index.ts definition block');
const idxJs = stripTypeScriptTypes(idxSrc.slice(bStart, bEnd), { mode: 'strip' });
const ctx1 = vm.createContext({ z, Buffer, console, BigInt, Number, Math, Array, Object, String });
vm.runInContext(idxJs + '\n;globalThis.__encodeParams = encodeParams;', ctx1);
const encodeParams = ctx1.__encodeParams;
console.log('source B = web/bridge/server/index.ts definition block (stripTypeScriptTypes + vm)');

// ---- C. front-end configSchema: slice the real declaration, strip TS, eval ----
const htSrc = readFileSync(p('web/src/bridge/http-transport.ts'), 'utf8');
const cStart = htSrc.indexOf('const configSchema = z.looseObject({');
const cEnd = htSrc.indexOf('\n});', cStart) + 4;
if (cStart < 0 || cEnd <= cStart) throw new Error('cannot slice http-transport.ts configSchema');
const htJs = stripTypeScriptTypes(htSrc.slice(cStart, cEnd), { mode: 'strip' });
const ctx2 = vm.createContext({ z, console, Number, Object });
vm.runInContext(htJs + '\n;globalThis.__configSchema = configSchema;', ctx2);
const configSchema = ctx2.__configSchema;
console.log('source C = web/src/bridge/http-transport.ts configSchema (stripTypeScriptTypes + vm)');
console.log('');

const U32_HEAD = [
  'Enabled', 'ReadCacheEnabled', 'WriteCacheEnabled', 'WriteBack',
  'CacheSizeMB', 'FlushIntervalMs', 'DirtyHighPct', 'DirtyLowPct',
  'SystemVolumeCached', 'IdleReclaimEnabled', 'IdleReclaimSec', 'ParallelFlush',
  'CachePagingIo', 'WriteRelease', 'Frozen', 'IdleFlush',
];
const U32_TAIL = ['ReadaheadEnabled', 'ReadaheadWindowKB', 'AutoMemory', 'MemReserveMB', 'AutoUsePct', 'BlockSize', 'TinyLFU'];

const full = {
  Enabled: 1, ReadCacheEnabled: 1, WriteCacheEnabled: 1, WriteBack: 0,
  CacheSizeMB: 0, FlushIntervalMs: 60000, DirtyHighPct: 60, DirtyLowPct: 40,
  SystemVolumeCached: 0, IdleReclaimEnabled: 1, IdleReclaimSec: 1800,
  ParallelFlush: 1, CachePagingIo: 0, WriteRelease: 0, Frozen: 0, IdleFlush: 0,
  VolumeFilter: ['', '', '', '', '', '', '', ''],
  ReadaheadEnabled: 1, ReadaheadWindowKB: 2048, AutoMemory: 1,
  MemReserveMB: 0, AutoUsePct: 80, BlockSize: 4096, TinyLFU: 1,
};

function mkBuf(n, tiny) {
  const b = Buffer.alloc(n);
  for (let i = 0; i < U32_HEAD.length; i++) b.writeUInt32LE(full[U32_HEAD[i]], i * 4);
  const tailOff = [128, 132, 136, 140, 144, 148, 152];
  for (let i = 0; i < U32_TAIL.length; i++) {
    const off = tailOff[i];
    const v = U32_TAIL[i] === 'TinyLFU' ? tiny : full[U32_TAIL[i]];
    if (off + 4 <= n) b.writeUInt32LE(v, off);
  }
  return b;
}

// ---------- read side ----------
guard('R01', '156B new driver, TinyLFU=1 -> 24 fields / TinyLFU=1', () => {
  const d = decodeConfig(mkBuf(156, 1));
  const n = Object.keys(d).length;
  return [n === 24 && d.TinyLFU === 1, `fields=${n} TinyLFU=${d.TinyLFU} BlockSize=${d.BlockSize} Enabled=${d.Enabled}`];
});
guard('R02', '152B OLD driver reply -> no RangeError, TinyLFU defaults 0', () => {
  const d = decodeConfig(mkBuf(152, 0));
  const n = Object.keys(d).length;
  return [n === 24 && d.TinyLFU === 0, `fields=${n} TinyLFU=${d.TinyLFU} (len=152, 0=OFF fallback)`];
});
guard('R03', '152B old reply: other fields still strictly decoded at fixed offsets', () => {
  const b = mkBuf(152, 0); b.writeUInt32LE(7, 0); b.writeUInt32LE(8192, 148);
  const d = decodeConfig(b);
  return [d.Enabled === 7 && d.BlockSize === 8192 && d.ParallelFlush === full.ParallelFlush,
    `Enabled=${d.Enabled} BlockSize=${d.BlockSize} ParallelFlush=${d.ParallelFlush}`];
});
guard('R04', 'boundary 151B -> RangeError (pre-existing strictness, NOT a regression)', () => {
  let threw = false, msg = '';
  try { decodeConfig(mkBuf(151, 0)); } catch (e) { threw = true; msg = e.constructor.name + ': ' + e.message; }
  return [threw, `threw=${threw} ${msg} -- header field BlockSize@148 still needs 152B, unchanged behaviour`];
});
guard('R05', 'boundary 155B -> no throw, TinyLFU=0', () => {
  const d = decodeConfig(mkBuf(155, 0));
  return [d.TinyLFU === 0 && Object.keys(d).length === 24, `fields=${Object.keys(d).length} TinyLFU=${d.TinyLFU} len=155`];
});
guard('R06', 'boundary 160B -> TinyLFU read at 152, value 0xFFFFFFFF preserved', () => {
  const d = decodeConfig(mkBuf(160, 0xFFFFFFFF));
  return [d.TinyLFU === 0xFFFFFFFF, `TinyLFU=${d.TinyLFU} len=160 (codec fidelity; driver rbctl.c:145 clamps >1 to 0)`];
});
guard('R07', 'truncated 100B -> RangeError (only TinyLFU relaxed, others NOT swallowed)', () => {
  let threw = false, msg = '';
  try { decodeConfig(Buffer.alloc(100)); } catch (e) { threw = true; msg = e.constructor.name; }
  return [threw, `threw=${threw} ${msg} -- proves no blanket try/catch was added`];
});

// ---------- write side ----------
guard('R08', 'encodeParams(2) buffer length == 156', () => {
  const b = encodeParams(2, full);
  return [b.length === 156, `length=${b.length}`];
});
guard('R09', 'write TinyLFU=1 -> readUInt32LE(152)==1, BlockSize@148 untouched', () => {
  const b = encodeParams(2, { ...full, TinyLFU: 1 });
  return [b.readUInt32LE(152) === 1 && b.readUInt32LE(148) === 4096,
    `@152=${b.readUInt32LE(152)} @148=${b.readUInt32LE(148)} length=${b.length}`];
});
guard('R10', 'write TinyLFU=0 -> readUInt32LE(152)==0', () => {
  const b = encodeParams(2, { ...full, TinyLFU: 0 });
  return [b.readUInt32LE(152) === 0, `@152=${b.readUInt32LE(152)}`];
});
guard('R11', 'TinyLFU omitted -> u32(undefined)=0 (default OFF, no silent field loss)', () => {
  const c = { ...full }; delete c.TinyLFU;
  const b = encodeParams(2, c);
  return [b.readUInt32LE(152) === 0 && b.length === 156, `@152=${b.readUInt32LE(152)} length=${b.length}`];
});
guard('R12', 'write TinyLFU=0xFFFFFFFF -> codec preserves u32 max', () => {
  const b = encodeParams(2, { ...full, TinyLFU: 0xFFFFFFFF });
  return [b.readUInt32LE(152) === 0xFFFFFFFF, `@152=${b.readUInt32LE(152)}`];
});

// ---------- round trip ----------
guard('R13', 'encode -> decode round trip: all 23 u32 + VolumeFilter + TinyLFU identical', () => {
  const b = encodeParams(2, { ...full, TinyLFU: 1 });
  const d = decodeConfig(b.subarray(0));
  const diffs = [];
  for (const k of [...U32_HEAD, ...U32_TAIL]) if (d[k] !== full[k]) diffs.push(`${k}:${full[k]}->${d[k]}`);
  if (JSON.stringify(d.VolumeFilter) !== JSON.stringify(full.VolumeFilter)) diffs.push('VolumeFilter');
  return [diffs.length === 0, `diffs=${diffs.length ? diffs.join(',') : 'none'} fields=${Object.keys(d).length} TinyLFU=${d.TinyLFU}`];
});

// ---------- front-end zod schema ----------
const validFront = { Enabled: 1, WriteBack: 0, CacheSizeMB: 0, BlockSize: 4096 };
guard('R14', 'front configSchema: TinyLFU missing -> default(0) (new FE + old bridge mix)', () => {
  const r = configSchema.parse({ ...validFront });
  return [r.TinyLFU === 0, `TinyLFU=${r.TinyLFU} (default applied)`];
});
guard('R15', 'front configSchema: TinyLFU=1 preserved', () => {
  const r = configSchema.parse({ ...validFront, TinyLFU: 1 });
  return [r.TinyLFU === 1, `TinyLFU=${r.TinyLFU}`];
});
guard('R16', 'front configSchema: TinyLFU="on" REJECTED (still z.number)', () => {
  let rej = false; try { configSchema.parse({ ...validFront, TinyLFU: 'on' }); } catch { rej = true; }
  return [rej, `rejected=${rej}`];
});
guard('R17', 'front configSchema: TinyLFU=null REJECTED', () => {
  let rej = false; try { configSchema.parse({ ...validFront, TinyLFU: null }); } catch { rej = true; }
  return [rej, `rejected=${rej}`];
});
guard('R18', 'front configSchema: missing Enabled still REJECTED (other 4 fields stay strict)', () => {
  let rej = false; try { configSchema.parse({ WriteBack: 0, CacheSizeMB: 0, BlockSize: 4096, TinyLFU: 1 }); } catch { rej = true; }
  return [rej, `rejected=${rej}`];
});
guard('R19', 'front configSchema: WriteBack="x" still REJECTED', () => {
  let rej = false; try { configSchema.parse({ ...validFront, WriteBack: 'x' }); } catch { rej = true; }
  return [rej, `rejected=${rej}`];
});
guard('R20', 'default(0) applies ONLY to TinyLFU: missing BlockSize still REJECTED', () => {
  let rej = false; try { configSchema.parse({ Enabled: 1, WriteBack: 0, CacheSizeMB: 0 }); } catch { rej = true; }
  return [rej, `rejected=${rej} (no blanket default on other fields)`];
});

// ---------- old-driver -> new-bridge -> new-FE combined ----------
guard('R21', 'COMBINED: old 152B driver reply -> new decodeConfig -> new front configSchema', () => {
  const d = decodeConfig(mkBuf(152, 0));
  const r = configSchema.parse(d);
  return [r.TinyLFU === 0 && d.TinyLFU === 0, `decode.TinyLFU=${d.TinyLFU} front.TinyLFU=${r.TinyLFU} (no DECODE error)`];
});

// ---------- observation (not a gate): server-side index.ts configSchema is loose by pre-existing design ----------
guard('R22', 'OBSERVATION: server index.ts configSchema accepts string TinyLFU -> u32()=0 (pre-existing, NOT this patch)', () => {
  const b = encodeParams(2, { ...full, TinyLFU: 'on' });
  return [b.readUInt32LE(152) === 0, `@152=${b.readUInt32LE(152)} -- z.record(z.union([number,string,array])) is pre-existing; front schema R16 is the strict gate`];
});

console.log('');
console.log(`== RESULT: PASS=${pass} FAIL=${fail} ==`);
if (fail > 0) process.exit(1);
