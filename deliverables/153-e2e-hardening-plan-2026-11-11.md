# 153 · e2e 探针 fail-open 加固施工方案（2026-11-11）

**上游**：`tools/audit_probe_failopen.py`（43 文件 / 61 条：FO1=37 FO2=0 FO3=0 FO4=1 FO5=23，exit 1）
**输入报告**：`deliverables/152-probe-failopen-audit-2026-11-11.md`
**本文件性质**：施工方案，**不含任何源码改动**。所有 `文件:行号` 均为本轮 `Read`/`grep` 实测，
抽查方式见每章末"验收"。

**本轮实跑（只读，`-B` 不产生 `.pyc`）**

```
python -B tools/audit_probe_failopen.py --ignore-cleanup
  → 发现 36 条；硬失败规则 FO1/FO2/FO5 命中 35 条（FO1=12 FO5=23）；EXIT 1
```

---

## 0. 本轮新核实的事实（推翻了 152 报告的一条推断）

152 报告 §7.1 把"`status != 0` 时 `data` 是否存在"列为**未核实**，并据此把 19 处 `rpc()`
的严重度写成"多半 KeyError 崩溃"。本轮读了服务端实现，**该不确定性已消除**：

| 事实 | 证据 |
|---|---|
| `/rb/rpc` 成功：`{status:0, data:<decode>}` | `src/ui/web/bridge/server/index.ts:510-513` |
| `/rb/rpc` 失败（IOCTL 抛异常）：**HTTP 200 信封 + `status:-1` + 只有 `message`，没有 `data`** | `index.ts:514-520`（:515 注释原文"以 200 信封回传（status=-1）"） |
| 桥接未就绪 / 非法 JSON / 缺 method / 跨源：`status:-1`，HTTP 503/400/403 | `index.ts:468-475`、`:485`、`:492`、`:496`、`:532` |
| 因此 `rpc(n)["data"]` 在 status 失败时**确定 KeyError**；`rpc(n).get("data")` 得到 **None** | 由上一行直接推出 |
| `/rb/health` 响应**没有 `status` 字段**，只有 `ok/native/deviceOpen/pluginError/deviceError/pid/node` | `index.ts:246-255`（handler）、`:356`（路由） |

**推论（直接决定 A 章的写法）**：

1. `if st != 0: raise` 这个校验**对所有 method 都成立**——不存在"某个 method 成功时不带 status"的情况。
   即模板可以全量套用，不需要按 method 白名单（残余不确定性见 §D.9）。
2. 4xx/503 场景下 `urllib.request.urlopen` 会先抛 `HTTPError`，**status 校验根本走不到**；
   所以校验只覆盖"200 信封 + status=-1"，这正是 152 报告里"静默形态"的唯一来源。**闭环。**
3. 152 §4.4 #7 说 d3 "判据可照常 PASS（静默）"**不准确**：d3:223/:250 用 `.get("data")`，
   失败得到 `None`，而 `:263` 是 `after["config"].get("WriteBack")` —— **`None.get` 会 AttributeError**。
   d3 目前是"意外 fail-closed"，但代价是 traceback + 不落 `d3_result.json`（`:268` 走不到）。
   §A 里据此改写成"显式抛 + 全字段留证"。

---

## A. `rpc()` 状态校验统一化（FO5，21 处真缺陷）

### A.1 权威模板判定：`d4` 的 `rpc()` **确实**校验了 status

`tests/e2e/d4_verify_cache_takeover.py:78-84`：

```python
def rpc(method):
    d = http_json(BASE + "/rb/rpc", {"method": int(method)})
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%d status=%s message=%s"
                           % (method, st, d.get("message")))
    return d.get("data") or {}
```

配套的容错版 `tests/e2e/d4_verify_cache_takeover.py:87-92`：

```python
def try_rpc(method):
    """(data, err). 桥接/设备抖动不应让脚本崩在 traceback 上。"""
    try:
        return rpc(method), None
    except Exception as e:
        return {}, "%s: %s" % (type(e).__name__, e)
```

**结论**：不需要新发明基准，d4 就是可用的双向模板（strict + tolerant），且它已在
`[150/N-150-6]` 之后真实跑通过。全仓 21 处只需把这两段搬过去。`grep -rn "status" tests/e2e tools`
显示**除 d4 外没有任何探针做过 status 比较**，与 152 报告的"19 个文件各抄一份"一致。

注意 `st != 0` 对缺字段（`st is None`）也判失败——**这就是 d4 的语义**（缺 status = 失败）。
保留该语义，不要改成 `if st and st != 0`，否则把 fail-closed 改成 fail-open。

### A.2 可直接复制的模板（三种形态）

各文件 helper 签名不一（无 params / 带 params / 带 timeout / 带 base），**只替换 helper 的"返回"部分，
保留原有的 body 组装、签名与 timeout**。按"返回什么"分三种形态：

**T1-strict（返回整个 body；21 处里 17 处属此类）**

```python
# 原：    return json.loads(r.read().decode())
# 改成：
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read().decode())
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%s status=%s message=%s"
                           % (method, st, d.get("message")))
    return d
```

**T1b-strict（helper 自身既发 HTTP 又就地取 `["data"]`，共 3 处：c3 `:60`、t4 `:52`、
probe_aligned_write `:67`）**

```python
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read().decode())
    st = d.get("status")
    if st != 0:
        raise RuntimeError("method=%s status=%s message=%s"
                           % (method, st, d.get("message")))
    if "data" not in d:
        raise RuntimeError("method=%s status=0 but no data field" % (method,))
    return d["data"]
```

（加 `"data" not in d` 一行是刻意的：`status:0` 但无 data 是**服务端契约破损**，
不能静默退化成 `{}` 让下游 `if not data:` 判通过。）

**T2-tolerant（仅用于 finally 里的收尾与轮询；直接抄 d4:87-92，改签名即可）**

```python
def try_rpc(method, params=None, timeout=20):
    """(data, err)。收尾/轮询专用：桥接抖动不该让脚本崩在 traceback 上。"""
    try:
        return rpc(method, params, timeout=timeout), None
    except Exception as e:
        return {}, "%s: %s" % (type(e).__name__, e)
```

**T3-degraded（仅 1 处：`tools/probe_write_readback_integrity.py` 的 `stats_sample`）**

```python
def stats_sample(base):
    """尽力取驱动计数。失败返回 None 并把原因写到 stderr（不抛：主判据是内容一致性）。"""
    try:
        import urllib.request
        req = urllib.request.Request(base + "/rb/rpc", data=b'{"method":1}',
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=3) as r:
            d = json.loads(r.read().decode("utf-8"))
    except Exception as e:
        print("WARN counters: transport %s: %s" % (type(e).__name__, e), file=sys.stderr)
        return None
    if d.get("status") != 0:
        print("WARN counters: rpc status=%s message=%s"
              % (d.get("status"), d.get("message")), file=sys.stderr)
        return None
    data = d.get("data")
    return data if isinstance(data, dict) and data else None
```

（`sys` 已在该文件 `:37` import。返回 None 后现有 `:165 if before and after:` 与
`:174 stats_ok=False` 逻辑不变，只是**原因可见**——这正是 152 §9.2 那半个修复缺失的另一半。）

### A.3 逐处清单（21 处真缺陷 + 2 处误报）

"调用点"列的是**该文件内全部**依赖此 helper 的行；"风险"列只写**收紧后会改变行为**的点。

| # | 文件 | helper 定义 | 调用点（行） | 现状（status!=0 时） | 改成 | 风险 |
|---|---|---|---|---|---|---|
| A1 | `tests/e2e/a2_concurrency_sweep.py` | `rpc` `:37-46` | `:50`,`:54`(cfg/st)；`:85`,`:99`,`:100`,`:112`,`:116`,`:132`,`:133-136`,**`:147`**,`:148-150` | `["data"]` → KeyError | T1 | **`:147` 在 `finally` 内**：收紧后 finally 抛异常会掩盖 try 段的原始异常，且 `res["recovery"]` 永不赋值（见 §B-3） |
| A2 | `tests/e2e/a2_repro_ab.py` | `rpc` `:33-42` | `:46`,`:50`；`:78`,`:81`,`:89`,`:90-91`,`:103`,`:104`,`:106`,`:107`,`:108`,**`:120`**,`:121-124` | 同上 | T1 | **`:120` 在 `finally` 内**，同 A1 |
| A3 | `tests/e2e/a2_reverify_after_reboot.py` | `rpc` `:46-55` | `:59`,`:63`；`:120`,`:121`,`:134`,`:135-136`,`:153`,`:154`,`:156`,**`:171`**,`:172-174` | 同上 | T1 | **`:171-174` 在 `finally` 内**；而 `:182-199` 的 `checks`/`VERDICT` 在 finally **之后**，finally 一抛就没有 VERDICT、没有 `RESULT:` 行 |
| A4 | `tests/e2e/a2_writeback_stress.py` | `rpc` `:102-110` | `config` `:114`/`stats` `:118`/`volumes` `:123`；`:232`,`:233`,`:234`,`:259`,`:316`,`:327`,`:331`,`:342`,`:344`,`:350`,**`:380`,`:384`,`:386`**,`:403`,`:412` | 同上 | T1 | `:380-386` 已被 `:391 except Exception` 兜住（现状"打印后 return 0"）；`:403 final = stats()` **裸露在 finally 之后**，会 traceback |
| A5 | `tests/e2e/b_reverify_after_reboot.py` | `rpc` `:62-71` | `:75`,`:79`；`:100`,`:103`,`:150`,`:151`,`:174`,`:175`,`:176`,`:183`,`:184`,**`:206`,`:207`,`:208`,`:209`** | 同上 | T1 | **`:206-209` 在 `finally` 内**，且 `:238 E_config_restored` 依赖 `r["recovery"]`（finally 里赋值）→ finally 抛 = 判据整段丢失 |
| A6 | `tests/e2e/c3_single_machine_stress.py` | `rpc` `:53-60`（内联 `["data"]`） | `:64`(stats)；`:120`,`:124`；`:142`(control_ioctls，**忽略返回值**)；`:156`,`:197` | KeyError（`:142` 完全静默） | T1b | `:142` 是"器具阳性对照"，`for m in (1,4,7)` 硬编码合法 method；收紧后该方法失败会让整个 C3（8 轮 + 60s idle）中途死掉 |
| A7 | `tests/e2e/d3_verify_after_reboot.py` | `rpc` `:104-110`（返回整个 body） | `:145`(one_getlog)、`:223`、`:250` | `.get("data")` → `None` → `:263` AttributeError | T1 **+ one_getlog 改造**（见下） | **B 段语义退化**：`one_getlog` 今天记录的是**真实 status**；若只把 `rpc` 改成抛，`:150 except` 会把它变成 `status:-999`，`B_all_getlog_status0` 仍 FAIL（正确）但**丢掉了真实 NTSTATUS**，排障价值下降 |
| A8 | `tests/e2e/e1_abatch_discriminative.py` | `rpc` `:122-130` | `:134`,`:138`；`:165`,`:174`(phase 内)、`:192`,`:193`,`:217`,`:218`,`:246`,`:247`,`:260`,`:261`,`:268`,`:277`,`:279`,`:297`,`:299`,`:322`,`:323`,`:330`,`:331`,`:332`,`:354`,**`:373`,`:375`,`:377`,`:391`,`:395`** | KeyError | T1 | `:165`/`:174` 在 `phase()` 的 try **之外**（`:167 try` 只包 `fn()`），所以 stats 失败会直接从 phase 抛出；`:391`/`:395` 在 finally |
| A9 | `tests/e2e/e2_concurrent_eviction.py` | `rpc` `:113-121` | `:125`,`:129`；`:179`,`:185`,`:195`,`:208`,`:231`,`:243`,`:253`,`:255`,`:257`,`:267`,**`:277`,`:279`,`:288`,`:291`** | KeyError | T1 | `:277-293` 全在 finally；`:288 st_fin=stats()`、`:291 config()` 裸露在 finally 的 try 之外（只被 `:276 try` 覆盖到 `:282`）→ 见 §B-3 |
| A10 | `tests/e2e/e4_i47_repro.py` | `rpc` `:33-40` | `:43`,`:46`；`:52`,`:53`,`:55`,`:67`,`:73`,`:74`,`:84`,`:86`,`:119`,`:120`,`:131`,`:133`,**`:138`,`:140`,`:143`,`:145`,`:146`**，`:159` | KeyError | T1 | `:140` 的 detach 已有 `except Exception: pass`（`:141`，尽力而为，**不改**）；`:138-146` 都在 `:137 try` 内，被 `:151 except` 兜住 → 收紧后只会"丢失 `RESTORED` 详情"，不会新增崩溃。`:159 st_end = stats()` 在 finally **之后**，是真正裸露的一处（对应 B-6） |
| A11 | `tests/e2e/e5_flush_probe.py` | `rpc` `:15-21` | `:23`,`:24`；`:39`,`:42`,`:45`,`:47`,`:53`,`:56`,`:59`,**`:70`,`:71`**,**`:77`** | KeyError | T1 | **`:77 s3 = st()` 在 finally 内且无任何保护**（`:69 try` 只包 `:70-71`）→ 收紧后必然 traceback，与 §B-1 的 `return 0` 相互作用 |
| A12 | `tests/e2e/t1_trigger_evidence.py` | `rpc` `:55-61` | `:65`(snap)、`:119` | KeyError | T1 | 无 finally、无清理，**最干净的一处** |
| A13 | `tests/e2e/t2_admit_discrimination.py` | `rpc` `:62-68` | `:72`(stats)、`:137`,`:138` | KeyError | T1 | 无 |
| A14 | `tests/e2e/t4_exclude_three_way.py` | `rpc` `:46-52`（内联 `["data"]`） | `:56`(stats)、`:77`,`:82` | KeyError | T1b | 无 |
| A15 | `tests/e2e/t6_ioctl_fuzz.py` | `rpc` `:25-32` | `:34`(config)；`:70`,`:95`,`:106`,`:112`,`:123`,`:133`（均已有 except）；`:140`,`:141`,`:144`（已 except）；`:149`,`:150`,**`:169`** | 部分被 `except Exception` 兜住，`["data"]` 处 KeyError | T1 | **唯一可能"新增红"的点**：R1(`:70`)/R2(`:95`) 失败时今天走"`rpc(2,c)` 静默成功 → `config()` 读到旧值 → 记为一次正常钳位"；收紧后进 `bad` → `:166` verdict 变 1。见 §A.5 |
| A16 | `tools/probe_aligned_write.py` | `stats` `:63-67` | `:92`,`:94` | KeyError → **exit 1** | T1b | 该文件文档写"退出码 0=未复现 / 2=复现"（`:17`），崩溃给 1 属第三态；建议把 `main()` 的 RPC 失败显式 `return 2` 或文档补 1（见 §D.6） |
| A17 | `tools/probe_disk_truth.py` | `rpc` `:48-52` | `set_read` `:80`；`invalidate` `:85`；调用 `:116`、`:122`（finally）、`:125` | **返回值被整体丢弃**，status 失败等于"开关生效" | T1 **+ 调用点必须校验** | **本批最严重**：`:116` 失败会让 `:118` 的"旁路读"实际走缓存，`bypass_identical` 判据失真 → 归因 A/B 结论**直接错**，且 exit code 仍是 0/2 |
| A18 | `tools/probe_stale_attribution.py` | `rpc` `:49-53` | `set_attach` `:82`；调用 `:116`、`:122`（finally） | 同上 | T1 **+ 调用点必须校验** | 同上；额外：`:122` 是 `finally` 里的**恢复 attach**，收紧后若恢复调用自身失败会掩盖 `:116` 的异常 |
| A19 | `tools/probe_through_staleness.py` | `rpc` `:57-65` | `stats` `:69` → `:128`,`:136`,`:155`；`:148` | KeyError | T1 | `:148`(FLUSH) 已被 `:147 try` 兜住，保持 |
| A20 | `tools/probe_unbuffered_read.py` | `rpc` `:45-53` | `stats` `:57` → `:115`,`:118` | KeyError | T1 | 无 |
| A21 | `tools/probe_write_readback_integrity.py` | `stats_sample` `:115-124` | `:153`,`:157` | `.get("data") or {}` → `{}` → `:165 if before and after:` 假 → **整段计数增量静默跳过** | **T3-degraded（不抛）** | 若改成抛，主判据 `identical`（`:162`）都跑不到——**必须保持不抛** |

**误报 2 处（不属 A 章范围，§C 里用抑制机制处理，不要改代码）**

| 位置 | 为什么是误报 | 证据 |
|---|---|---|
| `tests/e2e/d3_verify_after_reboot.py:159` `wait_bridge()` | 打的是 `/rb/health`，该端点**没有 `status` 字段**；"不通"由 `:165 return False` 表达 | `index.ts:246-255`、`:356` |
| `tests/e2e/d4_verify_cache_takeover.py:70` `http_json()` | 纯传输层；唯一调用者 `rpc()` `:79` 在 `:80-83` 校验并 raise | 代码即证据 |

### A.4 每个调用点失败后的期望行为

| 期望 | 文件 | 理由 |
|---|---|---|
| **抛异常中止（strict）** | A1 A2 A3 A4 A5 A6 A8 A9 A10 A11 A12 A13 A14 A15 A16 A19 A20 | 判据输入（stats/config/volumes）拿不到时，任何"继续"都只能产出无证据的结论 |
| **抛异常中止，但 finally 走 T2 尽力而为** | A1 `:147`、A2 `:120`、A3 `:171`、A4 `:380-386`、A5 `:206-209`、A9 `:277-291`、A10 `:138-146`、A11 `:70-71` | 恢复段失败必须**可观测**，但不能替代原始异常（§B 给出具体写法） |
| **记 degraded 继续** | A21 `stats_sample`；A7 `one_getlog` | A21：主判据是内容一致性，计数只是旁证。<br>A7：B 段判据本身就是"status==0"，**必须把真实 status 记下来**，不能变成 -999 |
| **调用点分档（helper strict，调用点自决）** | A17 `set_read`/`invalidate`、A18 `set_attach` | 正向开关（关读/关 attach）失败 → **必须中止**（否则归因错）；<br>恢复开关（finally 里的开回）失败 → 尽力而为 + 响亮告警 + 非 0 退出 |
| **保持现状（不改）** | A10 `:140` detach 的 `except Exception: pass`、A15 R4/R5 的 except | 语义就是"看它是否优雅拒绝"，异常本身就是记录项 |

**A7 的 `one_getlog` 具体改法**（`d3_verify_after_reboot.py:143-151`）：不要让它依赖会抛的 `rpc()`
而丢掉 status，改成显式读 status：

```python
def one_getlog(_):
    try:
        r = rpc(9)                      # 失败会抛；需要 status 的场合单独走下面的分支
        d = r.get("data") or {}
        return {"status": r.get("status"), "count": d.get("Count"),
                "cursor": d.get("Cursor"),
                "items": len(d.get("Items") or [])}
    except RuntimeError as e:
        return {"status": -999, "error": str(e)}     # 消息里已含真实 status
    except Exception as e:
        return {"status": -999, "error": str(e)}
```

关键点：`rpc()` 的 `RuntimeError` 消息里带 `status=%s`，所以**真实 status 不丢**，
只是从结构化字段变成文本。若要求结构化，则让 `rpc` 抛一个带 `.status` 属性的异常类
（见 §D.2：本轮不建议一次性引入自定义异常类，21 个文件各加一个 class 收益不抵成本）。

### A.5 "靠空值分支容错"的点（收紧会改变控制流）——必须逐条确认

全仓 `grep "or {}\|if not data\|if before and"` 命中只有 3 处（不含 t7b 的无关命中）：

| 位置 | 现状 | 收紧后 | 处理 |
|---|---|---|---|
| `tools/probe_write_readback_integrity.py:122` + `:165 if before and after:` | `{}`/`None` 使分支为假 → 计数增量**静默跳过** | 用 T3 后仍是 `None`，但 stderr 有原因，`stats_ok=False`（`:174`）已存在 | **不需要改控制流**，T3 即可 |
| `tests/e2e/d3_verify_after_reboot.py:146 d = r.get("data") or {}` | 空 dict 兜底，`status` 另行记录（`:147`） | 若 `rpc` 抛则走 except | 见 A7 改法 |
| `tests/e2e/d4_verify_cache_takeover.py:84 return d.get("data") or {}` | 已有 status 校验在前（`:80-83`），`or {}` 只是防御 | 不变 | 不动 |

**除这 3 处外没有别的地方**靠"失败返回空/None"实现容错。按收紧后的净收益分两类：

| 类别 | 条数 | 是哪几处 | 收紧后得到什么 |
|---|---|---|---|
| **崩溃式 fail-closed**（本来就非 0 退出） | 18（17 处 `KeyError` + d3 的 `AttributeError`） | A1-A6、A8-A16、A19、A20，以及 A7 | 把 `KeyError`/`AttributeError` 换成带 `method`+`status` 的可诊断异常；**判据结论不变**（都是非 0），只是不再需要看 traceback 猜是哪个 method |
| **真静默假绿**（失败仍可判 PASS / 结论失真） | 3 | A17、A18（返回值被丢弃）、A21（空值跳过） | 这是本批**唯一**会改变判据结论的三处，也是优先级最高的三处 |

（A7 归第一类：d3 现在是"意外 fail-closed"，见 §0 推论 3。）

### A.6 A 章验收

1. `python -B tools/audit_probe_failopen.py --ignore-cleanup` 的 `FO5` 从 23 → 2（只剩 d3:159、d4:70 两个误报）。
2. `python -B -m py_compile` 全部改过的文件（21 个）。
3. **不跑真机**的可做验证：把每个 helper 的 body 抽出来做 3 条单元断言（可用 `unittest.mock.patch("urllib.request.urlopen")`）：
   `status:0,data:{...}` → 返回数据；`status:-1,message:"x"`（无 data）→ 抛且消息含 `status=-1`；
   `status:0` 无 data → 抛（仅 T1b）。
4. **必须跑真机的验证**（下一批执行）：`python -B tests/e2e/d3_verify_after_reboot.py` 应仍 **PASS**
   （152 交接记录里 d3 是"开机 10 检查"的主判据），且新增场景"桥接活着但 IOCTL 失败"应给出
   `method=4 status=-1` 而不是 `KeyError`。d4 应仍 PASS（它本来就有校验，用于证明模板未破坏已有行为）。
5. `tests/e2e/t6_ioctl_fuzz.py` **先单独跑一次**并保存输出，确认 R1/R2 没有大面积落进 `bad`
   （见 A15 风险）——这一步是"会不会误伤"的唯一实证。

---

## B. 恢复段失败必须影响退出码（6 处 + 同类 1 处）

### B.1 先说 `return` 在 `try` 里的语义陷阱（这决定了改法不能是"加个标志位"）

Python 语义：`try` 内的 `return X` **先求值 X**，然后执行 `finally`，最后返回那个**已经算好的** X。
所以：

```python
def f():
    rc = 0
    try:
        return 0            # 值 0 在这一刻已定
    finally:
        rc = 2              # 改了也没用：f() 仍返回 0
```

**结论**：凡 `return` 出现在 `try` 内（`e5:67`、`t6:166`、`e2:193`、`e4:51/77/90`），
"在 finally 里设标志位"是**无效修改**。必须把 `return` 移到 `try/finally` **之后**，
finally 只负责改 `rc`。这就是本批最容易写错的地方，也是 152 §8.2 那句
"注意 e5 :67 的 `return 0` 在 try 里"的真正含义。

### B.2 逐处清单

| # | 文件 | `finally` | 现状（恢复失败时） | `main()` 返回语句 | 是否受 B.1 陷阱影响 |
|---|---|---|---|---|---|
| B-1 | `tests/e2e/e5_flush_probe.py` | `:68`（块 `:68-78`） | `:69 try` 包 `:70-71` 两个恢复 RPC，`:72 except` → 只 `print("restore err:", ...)`；随后 `:77 s3 = st()` **在 try 之外、无保护** | `:67 return 0`（**在 try 内**） | **是** |
| B-2 | `tests/e2e/t6_ioctl_fuzz.py` | `:167`（块 `:167-171`） | `:169 rpc(2, cfg0)` + `:170 except Exception: pass`，**完全静默** | `:166 return 0 if (not bad and restored) else 1`（**在 try 内**） | **是** |
| B-3 | `tests/e2e/a2_concurrency_sweep.py` | `:146`（块 `:146-150`） | `:147 rpc(2, c0)` **无 try/except**，`:148-150` 直接用 `cfg()`/`st()` —— 失败即 finally 抛，掩盖原异常且 `res["recovery"]` 缺失 | `:170 return 0`（**在 try 外**，恒 0） | 否（return 已在外面） |
| B-4 | `tests/e2e/e1_abatch_discriminative.py` | `:371`（块 `:371-398`） | `:372-385` try/except 打印 `RECOVERY_FATAL`；`:391 st_fin = stats()`、`:395 config()` 在 `:372 try` 之外 | `:402 return 0`（在 try 外，恒 0） | 否 |
| B-5 | `tests/e2e/e2_concurrent_eviction.py` | `:275`（块 `:275-293`） | `:276-282` try/except 打印 `RECOVERY_FATAL`；`:288 st_fin = stats()`、`:291 config()` 在 `:276 try` 之外 | `:297 return 0`（在 try 外，恒 0）；另有 `:193 return 3`（ABORT，**在 try 内**） | 否（`:297`）；**是**（`:193` 的 3 会被 finally 掩盖恢复失败，仍非 0，可接受） |
| B-6 | `tests/e2e/e4_i47_repro.py` | `:136`（块 `:136-158`） | `:137-150` try/except 打印 `RESTORE_FATAL`；`:140` detach 单独 `except: pass`（`:141`）；`:156-158` 删除文件 | `:166 return 0`（恒 0）；`:51/:77/:90 return 2`（**在 try 内**） | 否（`:166`）；**是**（`:51/77/90`，但 2 已是非 0） |
| B-7 | `tests/e2e/a2_writeback_stress.py`（不在指定 6 处内，同型，152 §4.2 #26 已登记） | `:377`（块 `:377-394`） | `:379-390` try（含 `:384 rpc(2,cfg0)`、`:386 config()`），`:391 except` 打印 `RECOVERY_FATAL` | `:418 return 0`（在 try 外，恒 0） | 否 |

> 口径说明：任务书点名的 6 处里含 `a2_concurrency_sweep.py`，152 报告 §4.2 的 6 条里含
> `a2_writeback_stress.py:391`。两者**形态相同**（恢复失败只打印/静默，退出码不变），
> 本方案把它们都覆盖为 7 处，不改动任何一处的其他地方。

### B.3 统一改法与逐文件片段

**通用骨架（B-3 ~ B-7 适用：`return` 本来就在 try 外）**

```python
def main():
    ...
    rc = 0
    try:
        ...
    finally:
        try:
            rpc(2, cfg0)
            # 其余恢复/收尾 RPC
        except Exception as e:
            rc = 2                      # 恢复失败 -> 非 0
            print(json.dumps({"RECOVERY_FAILED": repr(e)[:200],
                              "ACTION": "manually POST method=2 with <backup path>"},
                             ensure_ascii=False), flush=True)
        # 文件清理保持原样；清理失败不算恢复失败（属 §C 的 cleanup 类）
    return rc
```

**B-1 `e5_flush_probe.py`（受陷阱影响，必须两步）**

```python
def main():
    print("=== E5 FLUSH PROBE ===", flush=True)
    c0 = cfg()
    e0 = errcount()
    rc = 0                                  # 新增
    try:
        ...
        # :67 原来是 `return 0`  —— 删掉这行，改为落 rc（或直接留空继续）
        rc = 0
    finally:
        try:
            rpc(2, c0)
            rpc(8, {"Letter": "D", "Attach": 0, "ReadEnabled": 1,
                    "WriteEnabled": 1, "WriteMode": 2})
        except Exception as e:
            rc = 2                          # 恢复失败必须体现在退出码
            print("restore FAILED (WriteBack may still be 1):", repr(e)[:160], flush=True)
        try:
            if os.path.exists(F): os.remove(F)
        except OSError: pass
        try:                                # :77 的 st() 现在无保护，必须自己兜
            s3 = st()
            print(json.dumps({"final": {k: s3.get(k) for k in
                  ("WriteBackMode", "DirtyBytes", "LostDirtyBytes")}}), flush=True)
        except Exception as e:
            rc = 2
            print("final stats unavailable:", repr(e)[:160], flush=True)
    return rc
```

要点：`rc` 必须在 `try` 之前初始化；`:67` 的 `return` 必须**删除**；`finally` 里
**任何第二个可能抛的调用都要单独兜**（`:77 st()`），否则它会把 `rc=2` 连同 traceback 一起盖掉。

**B-2 `t6_ioctl_fuzz.py`（受陷阱影响）**

```python
    rc = 0                                   # main() 开头附近
    try:
        ...
        rc = 0 if (not bad and restored) else 1     # 原 :166 的 return 改成赋值
    finally:
        try:
            rpc(2, cfg0)
        except Exception as e:
            rc = 2                            # 恢复失败优先级最高
            print("RESTORE_FAILED config restore: %r" % (e,), flush=True)
    return rc
```

要点：原 `:166` 的 `return 0 if ... else 1` 是 `try` 内 return，**必须**变成赋值。
`restored` 变量本身是 `:151` 用 `:150 config()` 算出来的——一旦 `config()` 抛就根本到不了 `:151`，
所以 `rc=2` 的两条路径（`:150` 抛 / `:169` 抛）都要能落地：建议在 `:150` 外层再兜一层，
把"读不到配置"也映射成 `rc=2`。

**B-3 `a2_concurrency_sweep.py`（无陷阱，但 finally 有裸 RPC）**

```python
    rc = 0
    try:
        ...
    finally:
        try:
            rpc(2, c0)
            res["recovery"] = {"WriteBack": cfg()["WriteBack"],
                               "DirtyBytes": st()["DirtyBytes"],
                               "LostDirtyBytes": st()["LostDirtyBytes"]}
        except Exception as e:
            rc = 2
            res.setdefault("recovery_error", repr(e)[:200])
            print(json.dumps({"RECOVERY_FAILED": repr(e)[:200]}, ensure_ascii=False),
                  flush=True)
    ...                                      # :152-169 保持原样
    return rc                                # 原 :170 return 0
```

要点：`:148-150` 现在直接把 `cfg()`/`st()` 的结果塞进 `res["recovery"]`，**没有兜底**；
加 try 之后 `res["recovery"]` 可能缺失 → 下游只读 `res["recovery"]` 的地方（本文件内没有，
只 `print(json.dumps(res))` 到 :167 和写 `:168` 的 json）不会 KeyError，但要保证 JSON 里
有 `recovery_error` 字段，别让"缺字段"看起来像"恢复成功"。

**B-4 / B-5 / B-6 / B-7 的差异点（同样的骨架，四处不同处）**

| 文件 | 现在被兜住的 | 仍然裸露的 | 必须一并兜 |
|---|---|---|---|
| B-4 `e1` | `:373 config()`、`:375 rpc`、`:377 config()`（在 `:372 try` 内） | `:391 st_fin = stats()`、`:395 config()` | 这两个，用 `rc=2` + 打印 |
| B-5 `e2` | `:277 config()`、`:279 rpc`（在 `:276 try` 内） | `:288 st_fin = stats()`、`:291 config()` | 同上 |
| B-6 `e4` | `:138 rpc`、`:143 config()`、`:145 rpc(7)`、`:146 stats()`（在 `:137 try` 内） | `:159 st_end = stats()`（在 finally **之后**） | `:159`；同时 `:140` 的 detach `except: pass` 建议改成"记一条 `DETACH_BEST_EFFORT` 日志但**不**抬高 rc"（detach 失败不等于配置污染） |
| B-7 `a2_writeback` | `:380 config()`、`:384 rpc`、`:386 config()`（在 `:379 try` 内） | `:403 final = stats()`、`:412 config()` | 这两个；后者在 `print(json.dumps({"FINAL":...}))` 里 |

### B.4 风险：改成非 0 后，**本来能过**的场景会不会变红

这是本批最实际的问题，逐条给判断依据（不给"应该没问题"）：

| 处 | 会不会新红 | 判断依据 / 必须先做的验证 |
|---|---|---|
| B-1 e5 | **可能** | `:71` 的 detach 参数是 `{"Letter":"D","Attach":0,...}`。若 D 当前未 attach，driver 可能返回 `status!=0`（"卷未 attach"），旧代码静默、新代码 → 2。**验证方法**：`:70-73` 是 `try: rpc(2,c0); rpc(8,detach) except: print("restore err:")`，只要真机跑一次并 grep 该字符串就能判定。但仓库内 `deliverables/`、`docs/` grep 不到任何 e5 运行输出（仅命中 152 报告在引用代码），**没有历史样本可查** → 必须实跑一次。 |
| B-2 t6 | **可能** | `:169` 的 `except: pass` 至今未见触发记录；但 t6 只做控制面（不 attach/不写盘），恢复只是一个 SET_CONFIG 全量原值写，正常应 status=0。**风险低于 e5**。 |
| B-3 a2_concurrency | **低** | `:147` 的 `rpc(2, c0)` 与 `:132` 是同一份全量原值，`:132` 若成功 `:147` 也应成功。但 `:147` 在 finally —— 若 try 段因别的原因炸了，恢复 RPC 会在"设备可能已异常"的状态下再发一次，失败概率高于正常路径。**这是收紧后最容易被误判为"新缺陷"的一处**。 |
| B-4 e1 / B-5 e2 / B-7 a2_writeback | **低** | 恢复逻辑是"读当前值→不一致才写回"（`e1:374`、`e2:278`、`a2w:381-383`），已经写对配置时**不发写请求**，`status` 风险最小。真正新增的只有 `:391/:288/:403` 的 `stats()` 裸露路径——它只在"桥接掉了"时失败，那种场景本来就该红。 |
| B-6 e4 | **低-中** | e4 是**故意制造崩溃面**的 I47 复现（`:9-15`），设计上就允许 `BRIDGE-LOST`（`:14`）。若某次跑完确实把桥接搞挂了，`RESTORE_FATAL` + 新 `rc=2` 会同时出现——**这正是想要的信号**，但要更新 e4 的读法：`rc=2` 现在有两种含义（"oracle 缺失"=2 与"恢复失败"=2）。建议恢复失败用 **3**（`:51/:77/:90` 已用 2 表示前置缺失，`:193` 在 e2 用 3 表示 ABORT，e4 内 3 未占用）。 |

**降低风险的施工顺序（强烈建议照此执行）**：

1. **阶段 0（不改退出码）**：只把"被吞掉的恢复异常"变成**响亮日志**——e5 已有（`:72`），
   给 B-2/B-3 补同样的一行 `print`。跑一轮真机，看日志里到底有没有。
2. **阶段 1**：有日志证据后再把 `rc=2` 接上。这样"会不会变红"从推测变成**看过日志的事实**。
3. e4 的恢复失败码用 3，不要复用 2。

### B.5 B 章验收

1. 静态：`python -B -m py_compile` 7 个文件。
2. **反向用例**（不跑真机也能做）：把每个文件的恢复 RPC 用 monkeypatch 强制抛异常，
   断言 `main()` 返回非 0，且 stdout 仍含原有 `RECOVERY_FATAL`/`restore` 字样。
   这直接证明"改完没把退出码漏掉"，比真机跑更可靠。
3. 顺序陷阱的专项断言：`e5` 与 `t6` 各写一条"恢复成功"用例，断言返回**仍为 0/1**（不能恒 2），
   证明 `return` 已移出 `try` 且 `rc` 初值正确。
4. 真机（下一批）：`d3` 应仍 PASS；`e5`/`t6`/`e2` 在恢复成功的机器上退出码不变。

---

## C. 审计工具进 CI 的前置条件与分阶段方案

### C.1 现有门禁的实际组织方式（已读源码确认）

| 项 | 事实 | 证据 |
|---|---|---|
| 门禁脚本位置 | `src/scripts/check_*.py` 各一个独立脚本，**无共享库**，各自 `print` + `sys.exit(0/1)` | `check_repo.py:238-245`、`check_build_manifest.py:258-268` |
| 本地调用点 | `build/build.ps1` 的 `Invoke-Gates`，`$py` 数组**内联枚举** | `build/build.ps1:333-349`（数组）、`:350-358`（循环：非 0 即 `Fail`） |
| 目标名 | `-Target gates` → `Invoke-Gates` | `build/build.ps1:23`（ValidateSet）、`:401` |
| CI 调用点 | `.github/workflows/ci.yml` 每个门禁一个 `name: GateN xxx` step | `ci.yml:64-172`（Gate1..Gate16） |
| **门禁清单不在 manifest 里** | `sources.manifest.json` 顶层键只有 `manifestVersion/comment/toolchain/targets/tests/forbiddenSourceGlobs`；**没有任何 gate 列表** | `python -c` 读 keys 实测 |
| **新门禁必须自证注册** | 多个门禁自己断言"我在 ci.yml 里被调用 + 我的位置在谁之后" | `check_blocksize_contract.py:150-158`（`:157` 断言 `Gate9 < Gate10 < Web typecheck`）、`check_poolcopy_contract.py:135`、`check_stats_contract.py:156`、`check_buffer_write_contract.py:138`、`check_installer_contract.py:176-180`、`check_config_decode_contract.py:151` |
| 根目录白名单已含 `tools` | 新增 `tools/*.py` 不会踩 Gate15 | `check_layout.py:46` |
| `build.ps1` 编码契约 | **纯 ASCII、无 BOM**（实测 0 个 >0x7F 字节、无 BOM）；Gate5 规则是"非 ASCII 且无 BOM → FAIL" | `build/build.ps1` 字节实测；`check_ps_syntax.ps1:92-106` |

**因此"改哪个清单文件"答案是：`build/build.ps1` 的 `$py` 数组 + `.github/workflows/ci.yml`，
不是 `build/sources.manifest.json`。** 后者只管编译源清单（`check_build_manifest.py:1-20` 六条规则全是
driver/core/vcxproj/sln/hosts），把审计挂进去是错的位置。

### C.2 开成阻断门禁前必须先消掉的条目

当前 `--ignore-cleanup` 形态：**36 条，硬失败 35 条**（FO1=12 + FO5=23）。它们是：

| 类别 | 条数 | 位置 | 处置 |
|---|---|---|---|
| FO5 真缺陷 | 21 | §A.3 表 A1–A21 | **改代码**（A 章），FO5 直接归零 |
| FO5 误报 | 2 | `d3:159`、`d4:70` | **不能靠改代码消**（`/rb/health` 没有 status 字段；`http_json` 是传输层）→ 需工具侧抑制 |
| FO1 真缺陷 | 6 | `a2_writeback_stress:391`、`e1:382`、`e2:281`、`e4:151`、`e5:72`、`t6:170` | **改代码**（B 章）。改完后异常分支里有 `rc = 2` 赋值 → 该条 FO1 会**自动消失**（机制见下），不需要抑制条目 |
| FO1 误报 | 6 | `d3:77`、`d3:84`、`d3:91`、`d3:162`、`e4:141`、`o9_probe_diag:26` | 工具侧抑制（均有下游 fail-closed：`d3:93-96 missing`、`d3:165 return False`、`e4` 的 detach 尽力而为、`o9_probe_diag` 本身就是诊断聚合器） |

**FO1 会不会被 B 章的改动自动消掉：会，而且这带来一个新盲区（已读码确认）**

FO1 的触发条件是 `all(is_silent_stmt(s) for s in handler.body)` 且分支文本不含
`FAIL|fail|exit(1)|sys.exit`（`tools/audit_probe_failopen.py:251-255`、`:73`），
而 `is_silent_stmt` 只认 `pass/continue/break/裸字符串/print|log|debug|info|warning|warn|write|echo`
（`:117-128`）——**`rc = 2` 是 `ast.Assign`，不属"沉默语句"**。

结论：
1. B 章给 except 分支加 `rc = 2` 后，那 6 条真缺陷 FO1 **会自动归零**，不需要抑制条目。
2. **新盲区**：正因为"只要分支里有任何非沉默语句就放过"，这条规则**无法区分**
   "真的把恢复失败接到退出码" 与 "写了 `rc = 2` 但 `return` 仍在 `try` 内（§B.1 陷阱）"。
   后者 FO1 消失、行为完全没变——**审计工具看不见 return-in-try 陷阱**。
   因此 §B.5-3 那条"恢复成功用例断言返回仍是 0/1"的反向测试是**必需项，不能被审计绿灯替代**。
3. 同理，抑制条目只需覆盖 C.2 表里的 **2 条 FO5 误报 + 6 条 FO1 误报**，共 8 条。

### C.3 三阶段建议

**阶段 1：只报不拦（1 天，可立刻做）**

```powershell
# build/build.ps1 —— 新增 target 'audit'，不改 Invoke-Gates
# 注意：本文件必须保持纯 ASCII、无 BOM（见 C.1）
function Invoke-Audit {
    Step 'audit: probe fail-open (report only)'
    Push-Location $RepoRoot
    try {
        $log = Join-Path $env:TEMP 'rbc-audit-failopen.log'
        & python -B tools/audit_probe_failopen.py --ignore-cleanup > $log 2>&1
        $rc = $LASTEXITCODE
        Get-Content -LiteralPath $log | Select-Object -Last 20
        Write-Host "  audit exit=$rc (report only, never blocks)"
    }
    finally { Pop-Location }
}
```

CI 侧（阶段 1）：

```yaml
      - name: Audit probe fail-open (report only)
        continue-on-error: true
        run: python -B tools/audit_probe_failopen.py --ignore-cleanup
```

配套动作（阶段 1 必做，否则阶段 2 无路可走）：
1. 给 `tools/audit_probe_failopen.py` 加抑制机制。最小实现：`tools/audit_probe_failopen.allow.json`，
   形如 `{"file": "tests/e2e/d3_verify_after_reboot.py", "rule": "FO5", "line_hint": "wait_bridge", "why": "/rb/health has no status field (index.ts:246-255)"}`，
   命中时**在输出里保留为 `[SUPPRESSED]` 行并计入摘要**（不许凭空消失——否则工具自己 fail-open，
   与其设计目标相反）。
2. `--strict-allow` 开关：allow 条目里必须带 `why` 且非空，否则 exit 2。

**阶段 2：拦 FO2 + FO5（A 章落地后即可，不必等 B 章；FO1 仍只报）**

- 门禁形态：`python -B tools/audit_probe_failopen.py --ignore-cleanup --rules FO2,FO5`
  （需给工具加 `--rules`；allow 里剩余 FO1 条目标注"待 B 章修"）。
- 此时 FO5 应为 0 命中（21 修 + 2 抑制），FO1 剩 12 条只报不拦（含 6 误报 + 6 待修）。
- 若不想给工具加 `--rules`：阶段 2 就等价于"阶段 1 + 人工看摘要"，**不建议**——没有牙的门禁会退化成状态灯。

**阶段 3：全拦（B 章落地后）**

- 门禁形态：`--ignore-cleanup`（FO1 也拦）。
- 前置条件只有一条：C.2 的 6 条真缺陷全部改成"except 分支里有 `rc = 2`"（`rc = 2` 一加，
  FO1 按 `is_silent_stmt` 就会停止命中）。但**必须同时用 §B.5-2/§B.5-3 的反向测试兜住**——
  审计绿灯不等于退出码真的接上了（见上文"新盲区"）。

**每一阶段的"有牙"验收**（照抄现有门禁的纪律，见 `check_blocksize_contract.py:150-158`）：

1. 故意在 `tests/e2e/` 造一个 `status` 不校验的 helper → 门禁必须红。
2. 把抑制条目删掉一条 → 门禁必须红（证明 allow 不是永久静音）。
3. `python -B tools/audit_probe_failopen.py --self-test` 必须 0（现状已 0；若改工具后变 3，说明规则被改坏）。

### C.4 具体改动清单（阶段 2/3 落地时）

| 文件 | 改什么 | 约束 |
|---|---|---|
| `src/scripts/check_probe_failopen.py`（**新建**） | 薄封装：先 `--self-test`（非 0 直接 FAIL，防"规则失效还绿灯"），再 `--ignore-cleanup`；并断言 **自己在 `ci.yml` 里被注册**、位置在 Gate16 与 `Web typecheck + build` 之间（抄 `check_blocksize_contract.py:150-158` 的写法） | 与其它门禁同构：无共享库、exit 0/1、末尾打 `result: PASS/FAIL` |
| `build/build.ps1` | `:23` ValidateSet 加 `'audit'`（或阶段 2 起并入 `'gates'`）；`:11-12` 文档行同步；`:333-349` `$py` 数组加一行 `@('src/scripts/check_probe_failopen.py', @('--ignore-cleanup'))`；`:393-411` switch 加分支 | **纯 ASCII、无 BOM**；不得内联 `.c` 字面量（`check_build_manifest.py:132-167` 会扫 `build.ps1`） |
| `.github/workflows/ci.yml` | 在 Gate16（`:171-172`）之后、`Web typecheck + build`（`:177`）之前插入 `- name: Gate17 check_probe_failopen` | 位置断言写进新门禁自身；`env: PYTHONIOENCODING/PYTHONUTF8` 已在 `:29-31` 设好，无需重复 |
| `tools/audit_probe_failopen.allow.json`（新建） | 抑制条目（2 条 FO5 + 6 条 FO1），每条带 `why` | 禁止通配整文件；`why` 必填 |

**不需要改**：`build/sources.manifest.json`（无 gate 列表）、`src/scripts/check_layout.py`
（`tools` 已在根白名单 `:46`）、`check_repo.py`（新脚本不含版本号/`RB_WIRED_*`/`g_dbg_*`）。

### C.5 C 章验收

1. `pwsh -NoProfile -File build/build.ps1 -Target gates` 全绿（阶段 2 后含新门禁）。
2. `powershell -NoProfile -ExecutionPolicy Bypass -File src/scripts/check_ps_syntax.ps1` 仍 PASS
   （证明 `build.ps1` 的 ASCII/BOM 契约没破；这也是 `build.ps1:359-365` 的调用方式）。
3. `python src/scripts/check_build_manifest.py` 仍 PASS（证明 `build.ps1` 没引入 `.c` 字面量）。
4. 变异：删掉 `ci.yml` 里新 step → 新门禁自身必须 FAIL（自证注册）。
5. 变异：把某个 `rpc()` 的 `if st != 0` 删掉 → `--ignore-cleanup` 必须 EXIT 1。

---

## D. 未核实项（不要当成"应该没问题"）

1. **FO1 规则的结论是读码得出的，没有实测验证**。C.2 已把 `:117-128`（`is_silent_stmt`）与
   `:251-255`（`all(...)` 才触发）读了出来，据此判定"加 `rc = 2` 会让 FO1 自动消失"。
   但本轮**没有真的改一个文件再跑一次审计**（任务书禁止改源码），所以这是静态推断。
   动手前请先做单文件最小实验（改 `t6_ioctl_fuzz.py:170-171` 一处，跑审计看 FO1 是否从 12 变 11）。
   这是阶段 2 能否一次通过的最大不确定项。
2. **没有引入自定义异常类的成本评估**。A 章用 `RuntimeError`（与 d4 一致）。若要求调用点区分
   "RPC 失败"与"其它异常"（A7 的 `one_getlog` 会需要），21 个文件各加一个 `class` 的收益/成本未评估。
   本轮结论只到"先统一用 RuntimeError，消息里带 status"。
3. **真机行为一律未验证**。本轮是纯静态：没有跑任何探针（依任务书），所以
   "某处收紧后是否真的不误伤"全部只有推断。B.4 已逐条给出验证方法，但未执行。
4. **没有历史运行输出可查**。`grep -rl "restore err|RECOVERY_FATAL|RESTORE_FATAL" deliverables docs`
   只命中 `deliverables/152-*.md`（该报告在引用代码本身），**没有任何 e5/t6/e1/e2/e4 的归档 stdout**。
   因此 B.4 表里"e5 是否一直在恢复失败"无法从仓库证据回答。
5. **driver 侧对"非法/幂等控制请求"的 status 语义未知**。A15（t6 R1/R2）、B-1（e5 的 detach D）、
   A17/A18（`set_read`/`set_attach` 重复调用）是否返回 `status:0` 取决于 `rbctl.c` / `rbio.c` 的
   返回约定，本轮**没有读驱动源码**。这正是"会不会误伤"的根因，也是 §A.6-5 要求先单独跑一次 t6 的原因。
6. **`tools/probe_aligned_write.py` 的退出码契约冲突未决**。文档写 0/2（`:17`），
   收紧后 RPC 失败会以 exit 1 退出（未捕获异常）。是补一个 `except` 映射成 2、还是把文档改成 0/1/2，
   属设计决策，本方案不擅自定。
7. **`probe_disk_truth` / `probe_stale_attribution` 的"恢复失败"退出码未定义**。它们的文档写
   "0=未复现 / 2=复现"（`probe_disk_truth.py:23`、`probe_stale_attribution.py:17-20` 附近），
   而"开关恢复失败"是第三种状态。本方案建议用 3，但**没有确认这两个工具的调用方**（谁在读它们的
   退出码：人工？某脚本？）——`grep` 未见自动化调用方，仍未穷尽（可能在 `archive/` 或外部批处理里）。
8. **`c3_single_machine_stress.py:142` 的 `rpc(m)` 忽略返回值是否为有意设计未确认**。它是"器具阳性对照"
   （`:19-20` 注释），返回值确实无用；但收紧后该方法失败会中断整个约 10 分钟的运行，是否可接受
   需作者判断。
9. **152 报告 §4.4 #7 对 d3 的"静默"定性已被本轮推翻**（见 §0 推论 3），但我**没有回读
   `index.ts` 的 `encodeParams`/`DECODERS`**，因此"method 4 失败时确实不带 data"是从
   `:514-520` 的 catch 分支统一推断的，未按 method 逐个验证——不过 `:519` 是**唯一**的失败出口，
   该推断的置信度高。
