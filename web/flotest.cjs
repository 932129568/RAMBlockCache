const { execSync } = require('child_process');
const B = 'http://127.0.0.1:5199/rpc';
const rpc = async (m) => {
  const r = await fetch(B, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ method: m }) });
  return r.json();
};
(async () => {
  const s0 = (await rpc(1)).data;
  console.log(`前: ReadReq=${s0.ReadReq} ReadBytes=${(s0.ReadBytes / 1048576).toFixed(1)}MB HitBytes=${(s0.ReadHitBytes / 1048576).toFixed(1)}MB`);
  const exe = 'C:/Users/陈/Desktop/RAMBlockCache-V10.4.3-build19-源码包/src/ctl/ramboostctl.exe';
  const file = 'F:/Delta Force/Delta Force/DeltaForce/Content/Paks/pak-0-0-pakchunk20-WindowsClient.pak';
  execSync(`"${exe}" bench "${file}"`, { stdio: 'ignore' });
  const s1 = (await rpc(1)).data;
  const dReq = s1.ReadReq - s0.ReadReq;
  const dBytes = (s1.ReadBytes - s0.ReadBytes) / 1048576;
  const dHit = (s1.ReadHitBytes - s0.ReadHitBytes) / 1048576;
  console.log(`后: ReadReq=${s1.ReadReq} ReadBytes=${(s1.ReadBytes / 1048576).toFixed(1)}MB`);
  console.log(`增量: ReadReq+${dReq} · ReadBytes+${dBytes.toFixed(1)}MB · HitBytes+${dHit.toFixed(1)}MB`);
  console.log(dReq > 0 ? '★ 数据面接通！' : '✗ 数据面仍旁路');
  process.exit(0);
})().catch((e) => { console.log('ERR', e.message); process.exit(2); });
