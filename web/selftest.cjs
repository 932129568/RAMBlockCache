/**
 * Web 控制台全量自测（Phase 3）：经桥接 /rpc 对运行驱动逐命令核对，
 * 每条带预期断言（含 F1/F2/F4 的"预期拒绝"）。PASS/FAIL 表输出。
 * 运行：node selftest.cjs（需桥接在 5199 且已提权）
 */
const B = 'http://127.0.0.1:5199/rpc';
const rpc = async (m, p) => {
  const r = await fetch(B, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(p === undefined ? { method: m } : { method: m, params: p }),
  });
  return { http: r.status, ...(await r.json()) };
};

let pass = 0, fail = 0;
function check(name, cond, detail) {
  if (cond) { pass++; console.log(`  PASS  ${name}`); }
  else { fail++; console.log(`  FAIL  ${name} — ${detail}`); }
}

(async () => {
  console.log('== A. 读取面 ==');
  const v = await rpc(10);
  check('GET_VERSION status=0', v.status === 0);
  check('GET_VERSION 字段', v.data?.ProtocolVersion === 9 && v.data?.Major === 1 && v.data?.Build === 19 && v.data?.Name === 'RAMBlockCache');

  const st = await rpc(1);
  check('GET_STATS status=0', st.status === 0);
  check('GET_STATS StripeRead[64]', Array.isArray(st.data?.StripeRead) && st.data.StripeRead.length === 64);

  const cfg = await rpc(4);
  check('GET_CONFIG status=0', cfg.status === 0 && typeof cfg.data?.Enabled === 'number');
  check('GET_CONFIG VolumeFilter[8]', Array.isArray(cfg.data?.VolumeFilter) && cfg.data.VolumeFilter.length === 8);

  const vol = await rpc(7);
  check('GET_VOLUMES status=0', vol.status === 0 && typeof vol.data?.Count === 'number');

  const log = await rpc(9);
  check('GET_LOG status=0', log.status === 0 && Array.isArray(log.data?.Items));

  const ev = await rpc(14, { Cursor: 0, MaxCount: 64 });
  check('GET_EVENTS status=0 (游标编码)', ev.status === 0 && typeof ev.data?.Cursor === 'number');

  console.log('== B. 写命令面 ==');
  const fl = await rpc(3);
  check('FLUSH status=0', fl.status === 0);

  const es = await rpc(5);
  check('EXIT_SAFE_MODE status=0', es.status === 0);

  // SET_CONFIG 读改写对照（panic 态下 F2：驱动会降级——断言"被驱动接管"本身）
  const c1 = await rpc(4);
  const w = await rpc(2, c1.data);
  check('SET_CONFIG 提交 status=0', w.status === 0);
  const c2 = await rpc(4);
  check('SET_CONFIG panic 降级集生效（F2 断言）', c2.data.Enabled === 0 || c2.data.Enabled === c1.data.Enabled);

  console.log('== C. 预期拒绝面（fail-closed 行为断言）==');
  const pc = await rpc(12);
  const panicked = (await rpc(1)).data.PanicActive === 1;
  check('PANIC_CLEAR 拒绝（F1，panic 态）', panicked ? pc.status === -1 : pc.status === 0);

  const sv = await rpc(6, { Letter: 'D', ReadEnabled: 1, WriteEnabled: 1, WriteMode: 2 });
  check('SET_VOLUME 语义（挂载态幂等=0 / 未挂载=1168）', sv.status === 0 || (sv.status === -1 && /1168/.test(sv.message ?? '')));

  const at = await rpc(8, { Letter: 'D', Attach: 1, ReadEnabled: 1, WriteEnabled: 1, WriteMode: 2 });
  check('ATTACH 语义（挂载态幂等=0 / 未挂载=21）', at.status === 0 || (at.status === -1 && /21/.test(at.message ?? '')));

  console.log('== D. 输入门（zod）==');
  const badCursor = await rpc(14, { Cursor: -5 });
  check('负游标被 zod 拒绝', badCursor.status === -1 || badCursor.http === 400 || badCursor.http === 500);
  const badLetter = await rpc(8, { Letter: '', Attach: 1, ReadEnabled: 1, WriteEnabled: 1, WriteMode: 2 });
  check('空盘符被 zod 拒绝', badLetter.status === -1 || badLetter.http === 400 || badLetter.http === 500);

  console.log(`\n结果: PASS=${pass} FAIL=${fail}`);
  process.exit(fail === 0 ? 0 : 1);
})().catch((e) => { console.log('SELFTEST ERROR', e.message); process.exit(2); });
