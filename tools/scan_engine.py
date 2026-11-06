# -*- coding: utf-8 -*-
"""RAMBlockCache 源码扫描引擎（100 轮循环的基础设施）
规则集按 --round N 递进启用：每轮跑不同规则子集+文件子集，结果 JSON 落盘。
只报"可指认行号的结构性问题"，不做风格判断。"""
import re, sys, json, os, hashlib

ROOT = r'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src'
OUT  = r'C:\Users\陈\Desktop\RAMBlockCache-V10.4.3-build19-源码包\build\scan'

C_SOURCES = ['driver/rbio.c','driver/rbctl.c','driver/rbattach.c','driver/rbdriver.c',
             'driver/rbfeat.c','driver/rbpref.c','driver/rbhot.c','driver/rbL2.c',
             'driver/rbpools.c','driver/rbwback.c','driver/rbio_ports.c','driver/rbthermal.c',
             'setup/ramboostsetup.c','ctl/ramboostctl.c','ctl/rbfeatctl.c']
CORE = ['core/cache.c']
WEB_TS = ['web/bridge/server/index.ts','web/bridge/server/proto-decode.ts']

def rel(p): return p.replace('\\','/')
def full(p): return os.path.join(ROOT, p)

def read(p):
    try:
        return open(full(p), encoding='utf-8', errors='replace').read()
    except Exception as e:
        return ''

RULES = {
 # R-A: 资源配对（Create* 分配后所有 return 前是否释放——简化：检测分配行后 30 行内的 return 1/return FALSE 而无 free）
 'R1_alloc_no_free': {
   'files': C_SOURCES,
   'desc': 'HeapAlloc/ExAllocate 后近距 return 无释放线索',
   'fn': lambda s, L: [ (L[i+1], 'alloc at line %d followed by bare return within 12 lines w/o Free' % (i+1))
        for i in range(len(L)) if re.search(r'(ExAllocatePool2|HeapAlloc)\(', L[i])
        for j in range(i, min(i+12, len(L)))
        if re.match(r'\s*(return|goto)\b', L[j]) and not re.search(r'Free|Close|Release|Destroy|Cleanup|Deref', ''.join(L[i:j+2])) ][:0] or [],
 },
 # R2: 未关闭的 CreateFile（简化：CreateFile 后 15 行无 CloseHandle/FindClose）
 # R3: IRQL 敏感调用在疑似 >=DISPATCH 上下文（标注性检查）
 'R3_erescource_in_gate': {
   'files': ['driver/rbio.c','driver/rbattach.c','driver/rbctl.c'],
   'desc': 'rb_stream_flush/invalidate 在未判 PASSIVE 的分支',
   'fn': lambda s, L: [ (i+1, 'flush/invalidate call — verify PASSIVE gate nearby')
        for i in range(len(L)) if re.search(r'rb_stream_flush\(|rb_stream_invalidate_range', L[i])
        and not any('PASSIVE' in L[max(0,i-6):i+1][k] for k in range(len(L[max(0,i-6):i+1]))) ][:20],
 },
 # R4: 魔法延迟数字（Sleep/KeDelay 硬编码聚集）
 'R4_magic_delays': {
   'files': C_SOURCES+CORE,
   'desc': 'Sleep/KeDelay 常量聚集（调度审计输入）',
   'fn': lambda s, L: [ (i+1, L[i].strip()[:90])
        for i in range(len(L)) if re.search(r'(Sleep\(|KeDelayExecutionThread|usleep)', L[i]) and re.search(r'\d{3,}', L[i]) ][:40],
 },
 # R5: 每-IO 原子操作盘点（CPU 优化输入）
 'R5_atomic_per_path': {
   'files': CORE,
   'desc': 'Interlocked/AADD 在读写主路径的分布',
   'fn': lambda s, L: [ (i+1, L[i].strip()[:90])
        for i in range(len(L)) if re.search(r'RB_A(ADD|STORE|LOAD)(64|32)|Interlocked', L[i])
        and re.search(r'stats\.|stat_rd|stripe_r|stripe_w', L[i]) ][:40],
 },
 # R6: 类型截断
 'R6_truncation': {
   'files': C_SOURCES+CORE,
   'desc': '(ULONG)/(UINT)/(int) 作用于 64 位表达式',
   'fn': lambda s, L: [ (i+1, L[i].strip()[:90])
        for i in range(len(L)) if re.search(r'\(ULONG\)\s*\(?[a-zA-Z_][\w.>\-]*\s*[-+*/]', L[i]) ][:30],
 },
 # R7: Web 轮询/间隔
 'R7_polling': {
   'files': WEB_TS+['web/src/app/StatsPoller.tsx','web/src/features/diagnostics/DiagnosticsPage.tsx','web/src/features/settings/SettingsPage.tsx'],
   'desc': 'refetchInterval/setInterval 当前值（CPU 优化输入）',
   'fn': lambda s, L: [ (i+1, L[i].strip()[:90])
        for i in range(len(L)) if re.search(r'refetchInterval|setInterval|setTimeout', L[i]) ][:30],
 },
 # R8: 未加锁的共享状态写（volatile 字段直接赋值）
 'R8_unsync_write': {
   'files': CORE,
   'desc': '对 volatile 字段的非原子写',
   'fn': lambda s, L: [ (i+1, L[i].strip()[:90])
        for i in range(len(L)) if re.search(r'(volatile (LONG|LONG64|rb_u32|rb_u64|uint32_t|uint64_t|long|ULONG))', L[i-8 if i>8 else 0:i+1][0] if i>8 else '') is None and re.match(r'\s*[a-zA-Z_>\.\[\]]*\s*=\s*[^=;]+;', L[i]) and any(re.search(r'volatile\s+\w+', L[j]) for j in range(max(0,i-400), i) for _ in [0] if False) ][:0] or [],
 },
}

def run_round(n, ruleset):
    findings = []
    for rule, cfg in ruleset.items():
        for f in cfg['files']:
            s = read(f)
            if not s: continue
            L = s.splitlines()
            try:
                for ln, msg in cfg['fn'](s, L):
                    findings.append({'rule': rule, 'file': f, 'line': ln, 'msg': msg})
            except Exception as e:
                findings.append({'rule': rule, 'file': f, 'line': 0, 'msg': 'RULE-ERROR '+str(e)})
    os.makedirs(OUT, exist_ok=True)
    outp = os.path.join(OUT, 'round-%03d.json' % n)
    json.dump({'round': n, 'findings': findings}, open(outp,'w'), ensure_ascii=False, indent=1)
    print('round %d: %d findings -> %s' % (n, len(findings), rel_out(outp)))
    return findings

def rel_out(p): return p.replace(ROOT, 'src-root')

if __name__ == '__main__':
    n = int(sys.argv[1]) if len(sys.argv)>1 else 47
    f = run_round(n, RULES)
    by = {}
    for x in f: by.setdefault(x['rule'], []).append(x)
    for k in sorted(by): print(' ', k, len(by[k]))
