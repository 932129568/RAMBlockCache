# 154 — 驱动构建不可复现：逐字节定位、根因与修复方案

- 日期：2026-11-11（本地 11-12 凌晨）
- 结论一句话：**每次 `build\build.ps1 -Target driver` 产物不同的只有 6 处、7~11 个字节**，全部是 PE 元数据（`TimeDateStamp` ×4 副本、派生 `CheckSum`、CodeView `RSDS` 的 `Age`）。根因是 `/Zi`+`/DEBUG` 下 link.exe 写墙钟时间戳，**且 `build.ps1` 只删 `*.obj` 不删 `*.pdb`**，导致 link.exe 复用旧 PDB 并把 `Age` 加一。**已实测**修法：`/Brepro`（cl+link）+ 每次链接前删 PDB → 连续 2~3 次链接产物**字节完全相同**（真实 driver 源码，185344 字节，见 §3.1）。
- 副作用警告：**只加 `/Brepro` 不删 PDB 是无效修复**（实测仍差 65 字节，见 §3.0），这是最容易踩的坑。

**给主线程定 pin 用的那一行（唯一权威）**：

```
src\driver\x64\Release\RAMBlockCache.sys
size   = 185344
sha256 = b64ba1b15b72fec1793c0651096dde44a1ddf3efb8d0ed85b1d8b76c12e8fd04
mtime  = 2026-11-12 01:22:33 (本地)   ← 本文件目录最后一次被写
```

它是**第 2 次（也是最后一次）`build.ps1 -Target driver` 的产物**（本文样本 C，链接完成时刻 01:22:33，PE 时间戳 0x6af4a459）。截至本文定稿（01:32:41）该文件**未被再次重建**，且与 `RB-sign-deploy-150.ps1:27` 的 `-ExpectSha`、`.deploy-pin-state.json:3` 的 `artifactSha256` 三者一致。此后我**没有**再对该路径写入任何字节：所有 `/Brepro` 实验的链接输出都在 `%TEMP%`（见 §3.1）。

---

## §1 实测差异（逐字节证据）

### 1.1 我自己跑出来的三次构建（同一份源码、同一入口、未改任何文件）

工具链：`cl.exe 19.44.35229`（`C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64`），WDK `10.0.26100.6584`，SDK `10.0.26100.0`（来自 `build/sources.manifest.json:4-9`）。

执行方式（**必须用 PS 5.1 且 `-NoProfile`**，见 §5 第 9 条）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build\build.ps1 -Target driver
```

| 样本 | 构建完成时刻（本地） | size | sha256 | PE TimeDateStamp |
|---|---|---|---|---|
| A（主线程回滚后那次构建的产物，构建前先备份） | 01:21 前 | 185344 | `0267aee524e87f267cb66dc4c460f9d93c139d1d19ccbc0cc0c5421b3a76c02b` | 0x6af4a3de = 01:20:30 |
| B（我的第 1 次构建，01:21:57→01:22:08） | 01:22:08 | 185344 | `760376b30cc0233b8f134073abbe1c661ced9bea9e877008d98e98ef0029aed1` | 0x6af4a440 = 01:22:08 |
| C（我的第 2 次构建，01:22:24→01:22:33） | 01:22:33 | 185344 | `b64ba1b15b72fec1793c0651096dde44a1ddf3efb8d0ed85b1d8b76c12e8fd04` | 0x6af4a459 = 01:22:33 |

三个哈希互不相同，**size 完全相同（185344）**，7 个段的 VA/raw/rsz 全部一致（`.text .rdata .data .pdata PAGE INIT .reloc`）→ 布局稳定，变的只是元数据字节。

> 产物副本留在 `C:\Users\陈\AppData\Local\Temp\rbc-repro\{A,B,C}.sys`（bash 的 `/tmp` 即 `%TEMP%`）。
> 第 3 次构建额度**没有用掉**（只用了 2 次 driver 构建）。

### 1.2 逐字节 diff（A vs B、B vs C、A vs C）

分析脚本（原文保存于 `%TEMP%\rbc-repro\diff.py`，核心就是下面这段，可直接复跑）：

```python
import struct, sys
def parse(path):
    d = open(path,'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe+6)[0]
    optsz = struct.unpack_from('<H', d, pe+20)[0]
    so = pe+24+optsz; secs=[]
    for i in range(nsec):
        n = d[so+40*i:so+40*i+8].rstrip(b'\0').decode('latin1')
        vsz,va,rsz,raw = struct.unpack_from('<IIII', d, so+40*i+8)
        secs.append((n,va,vsz,raw,rsz))
    return d, secs
def runs(a,b):
    assert len(a)==len(b)
    diffs=[i for i in range(len(a)) if a[i]!=b[i]]
    g=[]; s=p=diffs[0]
    for i in diffs[1:]:
        if i==p+1: p=i
        else: g.append((s,p)); s=p=i
    g.append((s,p)); return g, len(diffs)
```

实测输出（`A.sys vs B.sys`）：

```
--- A.sys vs B.sys: size=185344 differing bytes=11 (0.0059%)
    offset 0x0000f0..0x0000f1  len=2  A=dea3        B=40a4
    offset 0x000140..0x000141  len=2  A=2731        B=b032
    offset 0x0251ec..0x0251ed  len=2  A=dea3        B=40a4
    offset 0x025208..0x025209  len=2  A=dea3        B=40a4
    offset 0x025224..0x025225  len=2  A=dea3        B=40a4
    offset 0x0252ac..0x0252ac  len=1  A=1a          B=1b
--- B.sys vs C.sys: size=185344 differing bytes=7 (0.0038%)     ← 时间戳高位字节相同，只有低位变
    offset 0x0000f0..0x0000f0  len=1  A=40          B=59
    offset 0x000140..0x000141  len=2  A=b032        B=1533
    offset 0x0251ec..0x0251ec  len=1  A=40          B=59
    offset 0x025208..0x025208  len=1  A=40          B=59
    offset 0x025224..0x025224  len=1  A=40          B=59
    offset 0x0252ac..0x0252ac  len=1  A=1b          B=1c
```

（差值落在 2 字节或 1 字节，纯粹是因为两次构建相隔 98 s / 25 s，`0x6af4a3de → 0x6af4a440 → 0x6af4a459` 只有低字节变化；**字段本身是 4 字节**。）

### 1.3 每处差异的结构归因（这才是"哪几个字节在变"）

`e_lfanew = 0xE8`，COFF 头在 `0xEC`，可选头在 `0x100`，`SizeOfOptionalHeader = 0xF0`，段表在 `0x1F0`，`.rdata` 文件偏移 `0x1D800`（RVA `0x1F000`），debug 目录 RVA `0x269E8`（= file `0x251E8`）。

> **偏移记号对照（避免歧义）**：本表用**文件绝对偏移**。主线程说的 "TimeDateStamp 在 `0x08..0x0B`" 是**相对 COFF 头起点**（`PE\0\0` 之后）的偏移，`0xE8 + 8 = 0x0F0` ——**两者是同一个字段**。同理 `RSDS.GUID` 是相对 RSDS 记录 `+4`、绝对 `0x2529C`。

| # | 文件偏移 | 长度 | 字段（结构） | A | B | C | 含义 |
|---|---|---|---|---|---|---|---|
| 1 | `0x0000F0` | 4 B | `IMAGE_FILE_HEADER.TimeDateStamp` | `de a3 f4 6a` = 0x6af4a3de | `40 a4 f4 6a` = 0x6af4a440 | `59 a4 f4 6a` = 0x6af4a459 | 链接完成时刻（UNIX 秒）。三次分别是 01:20:30 / 01:22:08 / 01:22:33（+08:00） |
| 2 | `0x000140` | 4 B | `IMAGE_OPTIONAL_HEADER64.CheckSum` | 0x00033127 | 0x000332B0 | 0x00033315 | **派生量**：link.exe 对整映像算的校验和，输入里含 #1 与 #3，因此跟着变 |
| 3 | `0x0251EC` | 4 B | `IMAGE_DEBUG_DIRECTORY[0].TimeDateStamp`（CodeView 条目） | 同 #1 | 同 #1 | 同 #1 | 墙钟时间戳的第 1 份副本 |
| 4 | `0x025208` | 4 B | `IMAGE_DEBUG_DIRECTORY[1].TimeDateStamp`（type=12 VC_FEATURE） | 同 #1 | 同 #1 | 同 #1 | 第 2 份副本 |
| 5 | `0x025224` | 4 B | `IMAGE_DEBUG_DIRECTORY[2].TimeDateStamp`（type=13 POGO，340 B） | 同 #1 | 同 #1 | 同 #1 | 第 3 份副本 |
| 6 | `0x0252AC` | 4 B | CodeView `RSDS.Age` | `1a 00 00 00` = 26 | 27 | 28 | **每次链接 +1**，见 §2 第 3 条 |
| — | `0x02529C` | 16 B | CodeView `RSDS.Guid` | `277c61c7ad274a45ad4b7e704be76f44` | **相同** | **相同** | 这三次**没有变**（原因见 §2，别把它当成"GUID 每次都变"） |

`RSDS` 记录定位（实测，`A.sys`）：debug 目录条目 ptr=`0x25298` → `RSDS`(4B) @0x25298、**GUID @0x2529C**、**Age @0x252AC**、`pdb` 路径 @0x252B0 起：

```
C:\Users\<CJK>\Desktop\RAMBlockCache-V10.4.3-build19-源码包\src\driver\x64\Release\RAMBlockCache.pdb
```

→ 注意这是**绝对路径**，也就是说"换一个检出目录构建，哈希也会变"（跨机可复现的第二个障碍，见 §3.2）。

**一句话总结差异面**：`0xF0`(4B) + `0x140`(4B) + 3 × debug 目录时间戳 (12B) + `Age`(4B) 这 6 个字段，共 6 处、每次只有其中 7~11 个字节真正不同；`.text` **0 字节差异**（代码段完全稳定）。

---

## §2 根因

### 1）墙钟时间戳：`/DEBUG` + 未加 `/Brepro`

- `build/sources.manifest.json:96` → link 标志含 `"/DEBUG"`（`/RELEASE` 在 `:97`，`/OPT:REF/ICF` 在 `:98-99`）。
- 没有 `/Brepro`（全文件搜不到）。
- 因此 link.exe 在写盘时刻取当前时间填入 `TimeDateStamp`，并把它**复制到 3 个 debug 目录条目**里（这是 PE 的传统行为：debug 目录的时间戳=映像时间戳），再由 link.exe 算出 `CheckSum`。这解释了 #1/#3/#4/#5 恒等、#2 跟随。

### 2）`CheckSum` 是派生量，不是独立随机源

当 #1/#3 变化时映像内容变了，校验和必然变。修好 #1 后 #2 自动稳定（§3.1 实测：`/Brepro` 产物里 `CheckSum` 是固定值）。

### 3）`RSDS.Age` 每次 +1：`build.ps1` 删了 `*.obj` 但**没删 `*.pdb`**

`build/build.ps1:63-67` 每次构建开头会清空 `x64\Release\obj\*.obj`（防止删源码后残留旧 obj），但 `x64\Release\RAMBlockCache.pdb` 从不清。link.exe 发现同名 PDB 已存在时**复用它**，并把 CodeView 的 `Age`（PDB 的"第几次修订"计数）**加一**，把新 Age 写进映像的 `RSDS`。实测 26 → 27 → 28，与"本次是第几次链接到同一个 PDB 路径"严格对应。

这也解释了主线程观察到的"GUID 不变"：**PDB 被复用时 GUID 保持（GUID 标识 PDB 家族），只有 Age 递增**。而若 PDB 不存在（`-Target clean` 之后、或第一次构建），GUID 会跟时间戳一起变（合成实验证据见 §3.0 表：`n1` vs `n2` 只差时间戳，GUID 就跟着变）。所以"GUID 每次都变"这个说法**在当前构建里不成立**；真正每次都变的是 `Age`+时间戳。若哪天有人 `rm` 掉 PDB 再构建，差异会从 11 字节涨到 ~27 字节 —— 这一点对 driver **未实测**（§5 第 3 条）。

### 4）为什么 `.text` 完全不变

`/Zi` 只把调试信息放进 PDB/obj 的 `.debug$S`，不改代码；`/O2 /Oi /Gy /OPT:REF /OPT:ICF` 在输入相同的确定性下产出相同代码。实测 `.text` 0 字节差异，说明**没有**隐藏的代码级非确定性（没有 `__DATE__/__TIME__`：全仓库 `src/` grep 无命中）。

---

## §3 修复方案

### §3.0 先排除一个"看起来对"的错解（实测）

| 做法 | 实测结果 |
|---|---|
| 只加 `/Brepro`（cl+link 都加），**不删 PDB** | **失败**。同一 obj 连续链接两次：`plain1` vs `plain2` 差 **65 字节**（TimeDateStamp 4B ×4 处 + GUID 16B + Age 1B + type16 REPRO 记录 32B）。原因：PDB 被复用 → Age 变 → `/Brepro` 把 GUID 变成内容哈希 → 内容哈希跟着 Age 变，形成自我污染 |

另外**实测确认 `/Brepro` 被本机工具链真正识别**（不是静默忽略）：

```
cl   /BogusOptionXYZ  -> cl: 命令行 warning D9002 :忽略未知选项"/BogusOptionXYZ"   ← 对照
link /BogusOptionXYZ  -> LINK : warning LNK4044: 无法识别的选项"/BogusOptionXYZ"   ← 对照
cl   /Brepro          -> 无任何警告
link /Brepro          -> 无任何警告
```

且 `/Brepro` 生效的**结构证据**：产物 debug 目录多出第 4 个条目 `type=16`（`IMAGE_DEBUG_TYPE_REPRO`，36 字节），`TimeDateStamp` 变成不像时间的哈希值（如 `0xe304e369`）。这与公开分析（Amossys：`/Brepro` 生成 undocumented 的 `0x10` 型 debug 目录条目 + WinDbg 显示 "Image was built with /Brepro flag"）一致。

---

### 方案 1（推荐，性价比最高）：`/Brepro`（cl+link）+ 每次链接前删 PDB

**改动 A** — `build/sources.manifest.json`，compile 侧（在 `:27` 的 `"/Zi",` 之后插入一行，注意逗号）：

```json
        "flags": [
          "/nologo",
          "/c",
          "/O2",
          "/Oi",
          "/Gy",
          "/Zi",
          "/Brepro",
          "/MT",
          "/utf-8",
          "/W3",
          "/kernel"
        ],
```

**改动 B** — 同一文件，link 侧（在 `:96` 的 `"/DEBUG",` 之后插入）：

```json
        "flags": [
          "/nologo",
          "/DRIVER",
          "/NODEFAULTLIB",
          "/MANIFEST:NO",
          "/SUBSYSTEM:NATIVE",
          "/ENTRY:GsDriverEntry",
          "/MACHINE:X64",
          "/DEBUG",
          "/Brepro",
          "/RELEASE",
          "/OPT:REF",
          "/OPT:ICF"
        ],
```

**改动 C** — `build/build.ps1`，在 `:95`（`$libRel = ...`）之前插入，紧邻已有的 obj 清理逻辑（`:63-67`）：

```powershell
        # Deterministic link: link.exe REUSES an existing PDB and bumps the
        # CodeView RSDS Age, so the image differs on every rebuild even with
        # /Brepro (measured 2026-11-11: Age 26->27->28 on three builds).
        # Start from no PDB so Age is always 1 and the GUID is content-derived.
        Get-ChildItem -LiteralPath $outRel -Filter '*.pdb' -File -ErrorAction SilentlyContinue |
            Remove-Item -Force
```

（`$outRel` 是相对 `$wd` 的 `x64\Release`，`Push-Location $wd` 在 `:61` 已生效，写法与 `:66-67` 一致。）

**预期效果**：**重建两次 sha256 相同**（不只消除时间戳）。实测证据（真实 driver 源码，非玩具工程）：

| 实验 | 做法 | 结果 |
|---|---|---|
| D | 30 个 driver 源文件按 manifest 顺序编译（含 `/Brepro`）→ 链接 3 次，每次先删 PDB | `D1=D2=D3=64d17a5b9ded31f1d1bf881413e5333c86a1cd08735147ebcc3863bdbd034d27`（185344 B） |
| E | 同上，manifest 顺序，PDB 名 `brepro.pdb`，链接 2 次 | `E1=E2=701ba71f5824ea84884c55f5f00be11733004a8bdde7b73fad251aa24a3d833e`，**`E1.sys == E2.sys` 逐字节 True** |
| F | 同上，PDB 名 `f.pdb`，链接 2 次（验证可重复性不是偶然） | `F1=F2=02277f5118fbd968c59b2b71f1fa1db238a44ce2d75814d00321d194a37b9ce1` |

并且 **`/Brepro` 不改代码**：`A.sys`（无 Brepro）vs `E1.sys`（Brepro，manifest 顺序）差异统计 = `.text 0 字节`，只差 `.rdata`(8544 B) 与 `.pdata`(694 B) —— 后者是"debug 目录多一条 28 B 记录 + RSDS 路径串变短"导致的 `.rdata` 内偏移位移，不是代码变化。

**副作用（要认账）**：

1. **PE 时间戳不再是构建时间**，而是哈希（`lmv` 里 WinDbg 会打印 "reproducible build"）。若有人靠映像时间戳判断构建新旧会失效 —— 但本仓库的"产物比源码新"检查用的是**文件 mtime**（`deliverables/RB-sign-deploy-150.ps1:88`），不受影响。
2. **PDB 文件本身仍不可字节复现**（实测 `F1.pdb` ≠ `F2.pdb`，同为 1101824 字节但内容不同）。这不影响功能：`.sys` 里的 `RSDS`(GUID+Age) 与同一次链接写出的 PDB 是配对的；且因为 `.sys` 现在是确定的，GUID/Age 也确定（Age 恒为 1），**同一源码重建出来的 PDB 在符号匹配上是等价的**（是否逐字节相同 = 否；是否可被 WinDbg 正确匹配 = **未实测**，§5 第 4 条）。
3. **PDB 必须随构建一起留存**，且旧 PDB 不再匹配新映像（与现状一致：现在每次构建 GUID/Age 也在动）。
4. **`/Brepro` 与 `/DEBUG` 兼容**：本案实测通过（`/DEBUG /RELEASE /OPT:REF /OPT:ICF /Brepro` 链接 0 错误，debug 目录正常存在 4 条记录）。"官方文档要求编译+链接都加" —— 文档页我抓不到（§5 第 1 条），依据是"本机 cl/link 都接受且行为正确"这次实测。
5. 增量链接不适用于本仓库（`build.ps1` 每次全量重编 30 个 TU），因此 `/Brepro` 与 `/INCREMENTAL` 的冲突风险为 0。

---

### 方案 2：方案 1 + `/PDBALTPATH`（去掉映像里的绝对路径）→ 连"换目录构建"也能复现

**改动** — `build/sources.manifest.json` 的 `link.flags` 里，在 `/Brepro` 之后追加：

```json
          "/Brepro",
          "/PDBALTPATH:%_PDB%",
```

**预期效果**：`.sys` 里的 RSDS 路径从绝对路径变成裸文件名（`RAMBlockCache.pdb`）。**只解决跨机器/跨检出目录的可复现**（当前 RSDS 里嵌着 `C:\Users\<CJK>\Desktop\...`，换路径即换哈希）；单机连续重建的"相同"效果与方案 1 一样。

**实测依据**：合成工程 `/PDBALTPATH:plain.pdb` 两次链接字节相同（`v2a == v2b`）；`/Brepro` + 不同 PDB 名会改 GUID（实测 `E1` vs `F1`：GUID `02ef90b1…` vs `4d56563b…`，且 `0x252CF` 处 `brepro.pdb` → `f.pdb`），说明 RSDS 内容确实进哈希。

**副作用**：`%_PDB%` 是环境/宏展开写法，注意 **bash/MSYS 会改写以 `/` 开头的参数**（`/PDBALTPATH:...` 在 Git-Bash 里会被当成路径重写，MSYS 需要 `MSYS_NO_PATHCONV=1`）。本仓库用 `powershell.exe -File` 调用，不受影响。符号服务器/调试器找 PDB 时依赖 `_NT_SYMBOL_PATH` 而非绝对路径，正常。

---

### 方案 3：去掉 `/DEBUG`（保留 `/Brepro`）——"确定但丢调试信息"

**改动** — `build/sources.manifest.json:96` 删掉 `"/DEBUG",`（或写 `/DEBUG:NONE`），compile 侧 `/Zi` 改 `/Z7` 或保留（保留会白费编译时间）。

**预期效果**：没有 CodeView/RSDS，也没有 PDB，映像只剩时间戳一处非确定性 → `/Brepro` 单独即可让两次构建**字节相同**。

**实测依据**（合成工程）：`v3a == v3b`（`/Brepro` 无 `/DEBUG`，1536 字节，两次哈希 `E9F54D18B52299C7…` 相同）。

**副作用（决定性）**：**驱动没有 PDB 就没有符号**。本项目的取证链路（`KNOWN_ISSUES.md` 里的 bugcheck/dump 分析、`verifier` 试验）依赖 PDB 把 `RAMBlockCache+0x…` 还原成函数名/行号；丢掉 `/DEBUG` 等于把 dump 分析退化为猜地址。**不建议**。附带的体积变化对 driver 未实测。

（还有一个第 4 类做法——链接后用 ducible / `msvcpp-normalize-pe` 之类工具把时间戳/GUID/Age 打补丁成常量。它有效但把"确定性"变成了链接后的后处理，需新引入第三方工具、且会**掩盖**真实变化，故不列入三选一。）

---

### §3.3 三方案对比表

| | 方案 1 `/Brepro`+删 PDB | 方案 2 方案1+`/PDBALTPATH` | 方案 3 去 `/DEBUG` |
|---|---|---|---|
| 改哪里 | manifest `:27`、`:96` 各加 1 行；`build.ps1` `:95` 前加 4 行 | 方案 1 + manifest 再 1 行 | manifest `:96` 删 1 行 |
| 重建两次 sha 相同？ | **是（实测 E/F/D 全等）** | 是（且跨目录也相同） | 是（合成实测） |
| 保留了 PDB/符号？ | 是 | 是 | **否** |
| 代码段有变化？ | 否（`.text` 0 字节差） | 否 | 未实测 |
| 副作用 | 映像时间戳变哈希；PDB 字节仍不可复现 | 同上 + 宏写法注意 MSYS | 失去 dump 符号化能力 |
| 成本 | 低（6 行） | 低（7 行） | 最低（1 行） |
| 建议 | **采纳** | 采纳（若有多机/多检出目录构建） | 不采纳 |

---

## §4 对部署 pin（`-ExpectSha`）机制的建议

现状：`deliverables/RB-sign-deploy-150.ps1:27` 的 `-ExpectSha` 是"人工登记某个构建产物的哈希"，`:77-80` 失配即 `Die`（硬失败，符合 R16：未审查产物不签）；`deliverables/update-deploy-pin.ps1` 是配套的换 pin 工具，且它已经带了"源码指纹"二次门（`:53-57` 收集 `src/driver`+`src/core` 的 `*.c/*.h` 哈希，`:130-151` 改写 pin）。

我的建议（按优先级）：

1. **先修构建（方案 1），`-ExpectSha` 机制原样保留。** 修好之后"重链同一份源码"产出同一哈希，pin 失配重新变成**强信号**（来源变化/工具链变化/被篡改），而不是每天一次的噪声。这是唯一既保住防护力、又止住 churn 的路径 —— 不建议用"放宽/软失败/跳过"来止痛。
2. **pin 的粒度维持"未签名产物"，不要改成 dist 侧已签名哈希。** 签名的输入必须是待审产物，pin 挂在签名之前才有意义（`:72-80` 现在的位置是对的）。已签名哈希已经作为**部署回执**记录在结果文件里（`:205-206` `signed_sha256=` / `built_sha256=`），那是另一个用途，不要混用。
3. **不要用"源码树哈希"替代产物哈希。** 源码相同 ≠ 产物相同（工具链版本、manifest 里的 flags、环境变量都能改产物）。源码指纹只适合当**第二道门**（现状），用来区分"纯重链漂移"和"源码变更"。
4. **补一个现在的真实缺口：源码指纹不覆盖构建配置。** `update-deploy-pin.ps1:53-57` 只收 `src/driver` + `src/core` 的 `*.c/*.h`，**`build/sources.manifest.json`（编译/链接标志、include、工具链根）、`build/build.ps1`、`build/paths.ps1` 都不在指纹里**。后果：有人把 `/O2` 删掉、把 `/kernel` 改了、把 `msvcRoot` 指向别的工具链，`update-deploy-pin.ps1` 会判定"源码没变" → **静默**把 pin 挪到新产物上，pin 的防护力被绕过。建议把这 3 个文件加进 `$srcFiles`（例如在 `:54` 的 `Get-ChildItem -Path` 列表里补 `(Join-Path $Root 'build')`，或显式追加 3 个路径）。
5. **`update-deploy-pin.ps1` 头部注释需要同步更新。** 它现在写着"driver build is NOT bit-reproducible here … PE TimeDateStamp and the CodeView RSDS GUID change on every link"（`:5-7`）。按 §1 实测，**GUID 在当前构建里是不变的，变的是 `Age`**；方案 1 落地后整句都不再成立。建议改成"构建已确定（`/Brepro`+删 PDB，见 154），pin 失配=真实漂移信号"。
6. 操作流程保持"**最后一步登记的才是有效 pin**"：`build` → `update-deploy-pin.ps1`（复核 `srcFingerprint` 与 `AcceptSourceChange` 打印）→ `RB-sign-deploy-150.cmd`，中间不要重建。现在的注释（`:23-24`）就是这个意思，保留即可。

补充事实（写报告时点，01:32:41 复核）：**当前磁盘上的 `src\driver\x64\Release\RAMBlockCache.sys` 就是我第 2 次构建的 C**（sha `b64ba1b1…`，mtime 01:22:33），而 `RB-sign-deploy-150.ps1:27` 的 pin 与 `.deploy-pin-state.json:3` 的 `artifactSha256` 也正是 `b64ba1b1…` → **此刻 pin 是自洽的**。我的 `/Brepro` 实验全部在 `%TEMP%` 里链接（`/OUT:%TEMP%\...`、`/PDB:%TEMP%\...`），**没有覆盖仓库产物**，pin 状态未被我破坏。

### §4.7 兜底：万一 `/Brepro` 在本机不可用（本次**不**适用）

本次实测 `/Brepro` 可用（§3.0：cl/link 均无 `D9002`/`LNK4044`，且产物确定性已验证），所以**不需要**下面这套。但如果将来换工具链后 `/Brepro` 被拒（`LNK1117`/`LNK4044`），推荐这么兜：

| 兜底做法 | 要点 | 防护力评估 |
|---|---|---|
| **pin 改成"源码指纹 + 构建命令行指纹"**，产物哈希降为"记录值"而非门 | 构建命令行指纹 = 对 `build.ps1` 拼出的 `$common + $linkArgs`（含 flags 字面量）+ `sources.manifest.json` 全文做 sha256，写进 `.deploy-pin-state.json`；`-ExpectSha` 失配时若"源码指纹+命令行指纹"均未变 → 判定为纯重链漂移，打印一行告警后继续；任一指纹变化 → 仍然硬失败 | 中。挡住"源码/标志被改"，但挡不住"同源码同标志换了工具链/环境"，也挡不住产物被替换（此时产物哈希只记录不校验）。**这是明确的降级**，不是等价替代 |
| pin 只在发布构建生效（`-Release` 开关） | 日常重链不校验，发布/部署路径必须校验 | 中低。等于把门往后挪，若有人从非发布路径部署就绕过了 |
| 链接后归一化（ducible / `msvcpp-normalize-pe`）把时间戳、debug 目录时间戳、RSDS GUID/Age 打成常量 | 在 `link` 之后、`Write-Sha256` 之前跑一个 Python 脚本；pin 打在归一化后的产物上 | 高（产物真确定），代价：新增第三方逻辑、且归一化会**掩盖**"映像被动过手脚"这类信号，需额外 gate 防双花 |

兜底方案落地时必须同步改一句文档：**pin 的对象从"产物"变成"输入"**（源码+标志+工具链），这也正是风险从"产物正确性"转移到"输入正确性"的地方。

---

## §5 未核实项

1. **`/Brepro` 官方文档措辞**：`learn.microsoft.com/.../reference/brepro` 与 "linker options listed alphabetically" 两个 URL 都 404，未取到原文。"编译与链接都要加"这一说法的依据只有：本机 `cl 19.44.35229` 与 `link.exe` 均接受 `/Brepro`（对照测试无 `D9002`/`LNK4044`），加上第三方资料。**官方原文未核实**。
2. **"`/Brepro` 下 RSDS GUID 是内容哈希"是推断**，不是文档结论。证据链：只改 PDB 文件名 → GUID 变（`E1` vs `F1`）；PDB 复用 → `Age` +1 且 GUID 变（`plain1` vs `plain2`）；输入完全相同 → GUID 相同（`E1 == E2`）。内部算法未核实。
3. **driver 在"删 PDB 但**不**加 `/Brepro`"时的差异字节数**：未在 driver 上实测。合成工程（`n1` vs `n2`，间隔 2 s，各自新建 PDB）显示 GUID 会跟时间戳一起变（差 19 字节，其中 GUID 16 B），据此推测 driver 会从 11 字节涨到约 27 字节。**未核实**。
4. **PDB 字节不可复现是否影响调试匹配**：`F1.pdb` ≠ `F2.pdb` 已实测；"同一次链接产出的 `.sys` 与其 PDB 一定配对" 是按构造推理，**没有用 WinDbg 实际加载 `.reload /f` 验证**。
5. **方案 3 对 driver 的实际体积/变量影响**：只在小合成工程验证（7168 → 1536 字节），driver 未测。
6. **`/PDBPATH:NONE` 报 `LNK1117`** 是主线程的实测结论，我**没有复现**（未核实），因此本文所有方案都不依赖 `PDBPATH`。

---

## §6 落地验证（主线程，2026-11-12 01:40–01:55 实跑）

**采纳方案 1，已改三处，并已实证可复现。**

| # | 文件 | 改动 |
|---|---|---|
| A | `build/sources.manifest.json:27` 后 | compile `flags` 插入 `"/Brepro",` |
| B | `build/sources.manifest.json:96` 后 | link `flags` 插入 `"/Brepro",` |
| C | `build/build.ps1:67` 后 | 6 行：删 `$outRel\*.pdb` 与 `$wd\*.pdb`（原因写在注释里，含 26→27→28 的实测数据） |
| D | `deliverables/update-deploy-pin.ps1` | 按 §4 建议 4 补全源码指纹：追加 `build/sources.manifest.json`、`build/build.ps1`、`build/paths.ps1`（35 → **38** files）；按 §4 建议 5 重写头部注释（不再声称"build is NOT bit-reproducible"） |

**实测结果**

```
build1  sha256 = 048478c23760a41c753f32bb3dca7912872e0f5491c93dea3b2573c0a25c035d  size = 185344
build2  sha256 = 048478c23760a41c753f32bb3dca7912872e0f5491c93dea3b2573c0a25c035d  size = 185344
MATCH_1_2 = True
```

同源码连续两次构建**字节完全相同**，size 与改动前一致（185344，说明 `/Brepro` 未改变代码布局，与 §3 方案 1 的 `.text 0 字节差` 结论一致）。

**指纹缺口的实证**：补全后首次运行 `update-deploy-pin.ps1 -AcceptSourceChange` 打印

```
sources  : 38 files, fp 4f55c927…
[!] SOURCES CHANGED since the last pin registration
    A  build/build.ps1
    A  build/paths.ps1
    A  build/sources.manifest.json
```

这三行正是 §4 建议 4 描述的绕过路径 —— 改动前它们不会出现在差异里，pin 会被静默挪走。现在默认 `exit 4`（fail-closed），必须显式 `-AcceptSourceChange` 才动 pin。

**门禁**：`build.ps1 -Target gates` → `check_build_manifest` ok、`check_ps_syntax files=57 fail=0`、`check_signature` PASS、`ALL GATES PASS`。

**pin 现状**：`RB-sign-deploy-150.ps1:27` = `048478c2…`，与磁盘产物、`.deploy-pin-state.json` 三者自洽。

**仍未做**：`/PDBALTPATH:%_PDB%`（方案 2）。只在多机/多检出目录构建时才需要，当前单机不采纳 —— `.rdata` 里仍嵌着构建机的 CJK 绝对路径（`C:\Users\陈\Desktop\...\x64\Release\RAMBlockCache.pdb`），这意味着**换一台机器构建哈希仍会变**。已登记为已知边界，不影响本机 pin 机制。
7. **跨机器可复现（方案 2）**：`/PDBALTPATH` 只在合成工程验证过两次链接相同；driver 跨目录未实测，且即便做了，工具链版本/`INCLUDE` 顺序/PDB 内部路径仍可能引入差异（`nikhilism` 的实测文章也强调"PE 里的绝对路径 + PDB 内部路径都要处理"）。
8. 我**没有**验证签名后的映像（`signtool` 追加 Authenticode 段）是否仍保持"重建可复现" —— 签名必然改变字节，pin 只能针对未签名产物（§4 第 2 条）。
9. **环境陷阱（与本题无关但会误导复现）**：本会话的 PowerShell 7 profile 里有一个自定义 `Remove-Item` 函数，不接受管道输入，直接跑 `build\build.ps1 -Target driver` 会在 `build.ps1:67` 抛 "输入对象无法绑定到该命令的任何参数"。正确跑法：`powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\build.ps1 -Target driver`。§1.1 的三次构建都是这么跑的。
10. **Gate16 不会被本方案影响**：`src/scripts/check_build_manifest.py` 全文不读取 `compile.flags`/`link.flags`（grep `flags` 零命中），只校验源码清单/`vcxproj` 一致性，所以加 `/Brepro` 不需要改 gate。但 `src/driver/RAMBlockCache.vcxproj:78-85,103-113`（VS 侧第三条构建路径）没有对应改动 —— 若有人用 VS 构建，产物仍不可复现。**该文件是否要同步改，未核实/未做**（超出"只改 build.ps1+manifest"的授权范围）。

---

## 附：复现命令清单

```powershell
# 1) 备份现有产物
$T = "$env:TEMP\rbc-repro"; New-Item -ItemType Directory -Force -Path $T | Out-Null
Copy-Item .\src\driver\x64\Release\RAMBlockCache.sys "$T\A.sys"
Get-FileHash "$T\A.sys" -Algorithm SHA256

# 2) 构建两次（每次构建后另存）
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build\build.ps1 -Target driver
Copy-Item .\src\driver\x64\Release\RAMBlockCache.sys "$T\B.sys"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build\build.ps1 -Target driver
Copy-Item .\src\driver\x64\Release\RAMBlockCache.sys "$T\C.sys"

# 3) 逐字节 diff（脚本见 §1.2）
python "$T\diff.py" "$T\A.sys:$T\B.sys" "$T\B.sys:$T\C.sys"

# 4) /Brepro 正向验证（合成工程，不碰仓库产物）
$msvc='C:\Program1\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64'
$a=@('/nologo','/Brepro','/DEBUG','/ENTRY:mainCRTStartup','/SUBSYSTEM:CONSOLE')
Remove-Item .\x.pdb -Force -ErrorAction SilentlyContinue
& "$msvc\link.exe" @a /OUT:r1.exe /PDB:x.pdb t_plain.obj    # 删 PDB 再链
Remove-Item .\x.pdb -Force -ErrorAction SilentlyContinue
& "$msvc\link.exe" @a /OUT:r2.exe /PDB:x.pdb t_plain.obj
Get-FileHash r1.exe,r2.exe -Algorithm SHA256                 # 期望两个哈希相同
```
