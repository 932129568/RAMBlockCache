# -*- coding: utf-8 -*-
"""把仓库里的「纯源码 + 文档」打成单个 ZIP。

设计取舍（写进回执，便于复查）：
  * 排除构建产物与二进制：node_modules / obj / bin / target / EBWebView 缓存 /
    .pdb .exe .dll .obj .lib 等。这些都能由源码重新生成，不应进源码包。
  * 排除「嵌套的同源压缩包」：deliverables 下 3 个 RecoveryCandidate 备份、1 个 src 快照 zip、
    以及 installer/Assets/payload 下的 controller.zip / webconsole.zip。
    它们是同一份源码的旧快照或编译产物，再打一次只是把 290MB 重复搬一遍，不增加任何信息。
  * 保留 src/ui/web/dist：这是 UI 构建产物，但也是当前 WPF 窗口真正加载的东西，
    留着便于对比「源码构建结果 vs 现场资产」。
  * 文件名用 UTF-8 标志写入，中文路径解压不乱码。
"""
import os
import sys
import zipfile

ROOT = os.path.abspath('.')
OUT = sys.argv[1]

# ---- 目录级排除 ----
EXCL_DIRS = {
    'node_modules', 'obj', 'bin', 'target', '.git', '.mimosa', '.nuget', '.cache',
    'EBWebView', 'Cache', 'Code Cache', 'GPUCache', 'GrShaderCache', 'ShaderCache',
    'BrowserMetrics', 'component_crx_cache', 'Subresource Filter', 'Speech Recognition',
    'archive',          # 历史归档渲染图/草稿，非源码
    '.workbuddy',       # 会话记忆，不是源码
    '.workbuddy-ai', '.zcode',
}
# 顶层 dist/ 是驱动发布物（含已签名 .sys），属构建产物
EXCL_TOP_DIRS = {'dist'}
# 这些相对路径下是测试/构建的中间产物目录（里面有 .c 但属于生成的测试桩）
EXCL_REL_DIRS = {'tests/core/build'}

# ---- 扩展名排除（二进制/构建产物）----
EXCL_EXT = {
    '.pdb', '.exe', '.dll', '.obj', '.lib', '.ilk', '.exp', '.pma', '.pak',
    '.so', '.dylib', '.msi', '.cab', '.mui', '.winmd', '.sys', '.cat', '.node',
    '.zip', '.tar', '.gz', '.7z', '.iso', '.png', '.jpg', '.jpeg', '.gif', '.ico',
    # 编译/测试过程输出，不是源码
    '.log', '.out', '.err',
    # sourcemap：dist 的伴生文件，体积大且非手写源码
    '.map',
}

# ---- 单个文件级排除（嵌套同源快照）----
EXCL_FILES = {
    'deliverables/RAMBlockCache-RecoveryCandidate-2026-10-29.zip',
    'deliverables/RAMBlockCache-RecoveryCandidate-2026-10-26.zip',
    'deliverables/RAMBlockCache-RecoveryCandidate-net10-20261031.tar.gz',
    'deliverables/RAMBlockCache-V10.4.3-build19-src-2026-11-16.zip',
    'src/ui/installer/RAMBlockCache.Setup/Assets/payload/controller.zip',
    'src/ui/installer/RAMBlockCache.Setup/Assets/payload/webconsole.zip',
}

ALLOW_DIST_PREFIX = 'src/ui/web/dist/'

count = 0
total = 0
skipped_big = []

with zipfile.ZipFile(OUT, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
    for dirpath, dirnames, filenames in os.walk(ROOT):
        rel_dir = os.path.relpath(dirpath, ROOT).replace('\\', '/')
        if rel_dir == '.':
            rel_dir = ''

        keep = []
        for d in sorted(dirnames):
            if d in EXCL_DIRS:
                continue
            if rel_dir == '' and d in EXCL_TOP_DIRS:
                continue
            # dist 目录：只放行 src/ui/web/dist
            if d == 'dist' and rel_dir != 'src/ui/web':
                continue
            keep.append(d)
        dirnames[:] = keep

        for fn in sorted(filenames):
            abs_p = os.path.join(dirpath, fn)
            rel_p = (rel_dir + '/' + fn) if rel_dir else fn
            if rel_p in EXCL_FILES:
                continue
            ext = os.path.splitext(fn)[1].lower()
            if ext in EXCL_EXT:
                continue
            try:
                sz = os.path.getsize(abs_p)
            except OSError:
                continue
            if sz > 8_000_000 and not rel_p.startswith(ALLOW_DIST_PREFIX):
                skipped_big.append((sz, rel_p))
                continue
            try:
                with open(abs_p, 'rb') as fh:
                    data = fh.read()
            except OSError:
                continue
            zi = zipfile.ZipInfo(rel_p)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            zf.writestr(zi, data)
            count += 1
            total += sz

print(f'packed_files={count}')
print(f'raw_MB={total/1024/1024:.2f}')
print(f'zip_MB={os.path.getsize(OUT)/1024/1024:.2f}')
if skipped_big:
    print('skipped_big:')
    for s, p in skipped_big:
        print(f'  {s/1024/1024:7.2f} MB  {p}')