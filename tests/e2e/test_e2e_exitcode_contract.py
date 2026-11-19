#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""e2e 恢复段退出码契约测试（反向用例；纯本地，不连设备）

对应 153 号方案 §B：「恢复段失败必须影响退出码」。

覆盖什么
  * 恢复 RPC 抛异常 -> main() 返回非 0，且 stdout 有 RECOVERY_FAILED / RECOVERY_FATAL /
    RESTORE_FATAL 关键字，且结果 JSON 里有 recovery_error 字段（不靠"字段缺失"暗示失败）。
  * 恢复动作成功但随后读最终 stats/config 抛异常 -> 同样返回非 0（证明"恢复 RPC"与
    "读最终态"是两个独立的兜底层，前一个成功不会掩盖后一个失败）。
  * 恢复全程正常 -> main() 仍返回改动前的 0（防"恒返回非 0"的假绿）。

不覆盖什么
  * 各阶段判据本身（RMW/驱逐/oracle 的语义）——那些要真机，本文件一律不跑。

隔离手段（保证零副作用）
  * mod.rpc        所有探针唯一的 HTTP 出口；换成进程内假实现后不可能出网。
  * mod.time       sleep 空转；time() 换成快进时钟（e4 有 60s 实时循环）。
  * mod.subprocess run() 返回假的 CompletedProcess，不启动 oracle exe。
  * mod.open       模块全局遮蔽内建 open：备份/结果 JSON 落进内存 StringIO，不写盘。
  * mod.os         os.remove 空转，os.path.isfile / exists 由用例指定。
  * mod.phase      e1 / a2_writeback 的 phase() 换成记账桩，其内部 lambda 一律不执行。
  * mod.run / mod.oracle / mod.make_main / mod.volumes
                   跳过真实子进程、真实文件 IO。
  * mod.LOGF / mod.OUT / mod.BACKUP / mod.WORK
                   假 Path 对象，不落到 C:\\Users\\Public\\o9 等真实路径。

注意：因为 phase() 被替换，本文件验证的是"恢复段与退出码的接线"，不是相位逻辑。
"""
import contextlib
import hashlib
import importlib
import io
import json
import os
import sys
import types
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

_MISSING = object()

# 21 个统计字段（与 e1/e2/a2w 的 KEYS 一致），另加 e4 需要的 SafeMode/WriteBackMode 已在内。
KEYS = ["ReadReq", "ReadHit", "ReadBytes", "WriteReq", "WriteBytes",
        "WriteCached", "WbFlush", "WbBytes", "EvictBytes", "CurBytes",
        "DirtyBytes", "ChunkCount", "LostDirtyBytes", "RmwReads",
        "PressureLevel", "PressureShrinkBytes", "PeakBytes",
        "SafeMode", "WriteBackMode", "EngineBlockSize", "PanicActive"]

STATS = {k: 0 for k in KEYS}
STATS.update({"CurBytes": 1024 * 1024, "PeakBytes": 1024 * 1024,
              "SafeMode": 0, "WriteBackMode": 0, "EngineBlockSize": 4096,
              "EffectiveCacheMB": 128, "DynamicLimitMB": 128})

CFG = {"WriteBack": 0, "AutoMemory": 1, "CacheSizeMB": 128, "BlockSize": 4096,
       "TinyLFU": 1, "EngineBlockSize": 4096, "Flags": 0}

VOLS = [{"Letter": "D", "Attached": 1, "ReadEnabled": 1, "WriteEnabled": 1,
         "WriteMode": 2},
        {"Letter": "F", "Attached": 1, "ReadEnabled": 1, "WriteEnabled": 1,
         "WriteMode": 2}]

DUMMY_INFO = {"WriteBackMode": 1, "delta_this_write": 1,
              "rmw_before_this_write": 0, "rmw_after_this_write": 1,
              "cached_content_ok": True, "surface_content_ok": True}
DUMMY_REC = {"info": dict(DUMMY_INFO), "error": None,
             "stats_before": dict(STATS), "stats_after": dict(STATS),
             "d_stats": {k: 0 for k in KEYS}, "elapsed_s": 0.0}


class FakeBridge(object):
    """127.0.0.1:5199 的进程内替身；hook 用来按用例注入失败。"""

    def __init__(self, cfg=None):
        self.cfg = dict(cfg or CFG)
        self.calls = []
        self.hook = None
        self.evict = 0
        self.skip_apply = False

    def __call__(self, method, params=None, timeout=20):
        self.calls.append((method, params))
        if self.hook is not None:
            self.hook(self, method, params)
        return self._respond(method, params)

    def _respond(self, method, params):
        if method == 1:
            self.evict += 4096
            st = dict(STATS)
            st["EvictBytes"] = self.evict
            st["WriteBackMode"] = 1 if self.cfg.get("WriteBack") else 0
            return {"status": 0, "data": st}
        if method == 4:
            return {"status": 0, "data": dict(self.cfg)}
        if method == 2:
            if params and not self.skip_apply:
                self.cfg.update(params)
            self.skip_apply = False
            return {"status": 0, "data": {}}
        if method == 7:
            return {"status": 0, "data": {"Items": [dict(v) for v in VOLS]}}
        return {"status": 0, "data": {}}


class FakeTime(object):
    """sleep 空转；time() 快进，用来让 e4 的 60s 循环只跑一两轮。"""

    def __init__(self, step=30.0):
        self._t = 0.0
        self._step = step

    def time(self):
        self._t += self._step
        return self._t

    def sleep(self, _seconds):
        return None

    def strftime(self, _fmt):
        return "2026-11-12T00:00:00"


class FakeSubprocess(object):
    class _Completed(object):
        returncode = 0
        stdout = b"RESULT: PASS\nSHA256_EXPECTED=1 MISMATCH_COUNT=0\n"
        stderr = b""

    @staticmethod
    def run(_cmd, **_kw):
        return FakeSubprocess._Completed()


class FakeOS(object):
    """去掉删除动作；isfile/exists 由用例决定。"""

    def __init__(self, isfile=False, exists=False):
        self.path = types.SimpleNamespace(
            isfile=lambda _p: isfile,
            exists=lambda _p: exists,
            join=os.path.join)

    def remove(self, _path):
        return None

    def unlink(self, _path):
        return None

    def makedirs(self, *_a, **_kw):
        return None

    def rmdir(self, *_a, **_kw):
        return None


class FakePath(object):
    """接受 Path 的用法但不落盘：write_text 收进内存，exists 恒 False。"""

    def __init__(self):
        self.written = []

    def __truediv__(self, _other):
        return self

    def write_text(self, data, encoding=None, errors=None):
        self.written.append(data)
        return len(data)

    def exists(self):
        return False

    def __str__(self):
        return "<fakepath>"


def fake_open(*_a, **_kw):
    return io.StringIO()


def fake_phase(bridge, on_first=None):
    """phase() 桩：不执行传入的 lambda，只在第一次调用时模拟"已被改了配置"。"""
    state = {"n": 0}

    def _phase(name, fn, settle=0.8):
        state["n"] += 1
        if on_first is not None and state["n"] == 1:
            on_first(bridge)
        return dict(DUMMY_REC, name=name)

    return _phase


def fake_run(_path, threads, _seed, verify_only=False):
    """a2_concurrency_sweep.run() 桩：不启动 oracle exe。"""
    return {"exit": 0, "ok_threads": threads, "threads": threads,
            "fail_messages": [], "err_codes": [], "sha_ok": True,
            "stdout_tail": ""}


def fake_oracle(_path, _size, _seed, _threads, pattern="lcg", verify_only=False):
    return {"result": "PASS", "verify_only": verify_only, "pattern": pattern}


class _Resp(object):
    """urlopen() 的返回体替身（支持 with 上下文）。"""

    def __init__(self, payload):
        self._payload = payload

    def read(self):
        return json.dumps(self._payload).encode("utf-8")

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        return False


class FakeTransport(object):
    """HTTP 层的假实现：让被测文件里**真实的** rpc() 跑起来。

    与 FakeBridge 的区别很关键：FakeBridge 是 patch `mod.rpc`，会整体绕过新加的
    status 校验；FakeTransport 只替换 urlopen，所以 `d.get("status") != 0 -> raise`
    这段代码是**真的被执行**的。153 §A 的验收判据必须用这个。
    """

    def __init__(self, cfg=None, fail=None, boom=None):
        self.cfg = dict(cfg or CFG)
        self.evict = 0
        self.calls = []
        self.fail = fail        # fail(method, params, self) -> bool（True = 回 status:-1）
        self.boom = boom        # boom(method, params, self) -> bool（True = 抛传输异常）

    def __call__(self, req, timeout=None, **_kw):
        body = json.loads((req.data or b"{}").decode("utf-8"))
        method = int(body.get("method"))
        params = body.get("params")
        self.calls.append((method, params))
        if self.boom is not None and self.boom(method, params, self):
            raise OSError("injected: bridge went away (transport)")
        if self.fail is not None and self.fail(method, params, self):
            return _Resp({"status": -1, "message": "injected: IOCTL failed"})
        return _Resp(self._respond(method, params))

    def _respond(self, method, params):
        if method == 1:
            self.evict += 4096
            st = dict(STATS)
            st["EvictBytes"] = self.evict
            st["WriteBackMode"] = 1 if self.cfg.get("WriteBack") else 0
            return {"status": 0, "data": st}
        if method == 4:
            return {"status": 0, "data": dict(self.cfg)}
        if method == 2:
            if params:
                self.cfg.update(params)
            return {"status": 0, "data": {}}
        if method == 7:
            return {"status": 0, "data": {"Items": [dict(v) for v in VOLS]}}
        return {"status": 0, "data": {}}


class _ContractBase(unittest.TestCase):
    MODULE = None
    PATCH_RPC = True     # False => 让模块里**真实的** rpc() 跑，用 HTTP 层假实现驱动

    def setUp(self):
        self.mod = importlib.import_module(self.MODULE)
        self._saved = {}
        self._saved_urlopen = None
        self.out = io.StringIO()
        self.bridge = FakeBridge()
        if self.PATCH_RPC:
            self.patch("rpc", self.bridge)
        self.patch("time", FakeTime())
        self.patch("subprocess", FakeSubprocess())
        self.patch("open", fake_open)

    def tearDown(self):
        if self._saved_urlopen is not None:
            import urllib.request
            urllib.request.urlopen = self._saved_urlopen
            self._saved_urlopen = None
        for name, value in self._saved.items():
            if value is _MISSING:
                delattr(self.mod, name)
            else:
                setattr(self.mod, name, value)

    # ---- 工具 ----
    def patch(self, name, value):
        if name not in self._saved:
            self._saved[name] = getattr(self.mod, name, _MISSING)
        setattr(self.mod, name, value)

    def patch_urlopen(self, transport):
        """换掉 urllib.request.urlopen，让真实 rpc() 面对假 HTTP（不走网络）。"""
        import urllib.request
        self._saved_urlopen = urllib.request.urlopen
        urllib.request.urlopen = transport

    def run_main(self):
        with contextlib.redirect_stdout(self.out):
            rc = self.mod.main()
        return rc, self.out.getvalue()



class A2ConcurrencySweepContract(_ContractBase):
    """B-3 a2_concurrency_sweep.py：finally 里两个独立兜底层。"""

    MODULE = "a2_concurrency_sweep"

    def setUp(self):
        super(A2ConcurrencySweepContract, self).setUp()
        self.patch("os", FakeOS(isfile=False))
        self.patch("run", fake_run)
        self.patch("LOGF", FakePath())
        self.patch("OUT", FakePath())

    def test_restore_rpc_failure_is_nonzero(self):
        # 体内有 2 次 method=2（:99 开启回写、:132 关回写），finally 的第 3 次是恢复。
        state = {"n": 0}

        def hook(bridge, method, params):
            if method == 2:
                state["n"] += 1
                if state["n"] == 3:
                    raise RuntimeError("injected: bridge died in recovery")

        self.bridge.hook = hook
        rc, out = self.run_main()
        self.assertEqual(rc, 2)
        self.assertIn("RECOVERY_FAILED", out)
        self.assertIn('"STAGE": "restore_rpc"', out)
        self.assertIn("recovery_error", out)

    def test_recovery_readback_failure_is_nonzero(self):
        # 恢复 RPC 成功，但读回 cfg()/统计失败：第二个独立 try 必须自己兜住。
        state = {"restore_calls": 0, "restored": False}

        def hook(bridge, method, params):
            if method == 2:
                state["restore_calls"] += 1
                if state["restore_calls"] == 3:
                    state["restored"] = True
            elif method == 4 and state["restored"]:
                raise RuntimeError("injected: cfg() unavailable after restore")

        self.bridge.hook = hook
        rc, out = self.run_main()
        self.assertEqual(rc, 2)
        self.assertIn("RECOVERY_FAILED", out)
        self.assertIn('"STAGE": "recovery_readback"', out)

    def test_healthy_recovery_returns_zero(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertNotIn("RECOVERY_FAILED", out)
        self.assertNotIn("recovery_error", out)
        self.assertIn("=== SWEEP SUMMARY ===", out)


class E1Contract(_ContractBase):
    """B-4 e1_abatch_discriminative.py。"""

    MODULE = "e1_abatch_discriminative"

    def setUp(self):
        super(E1Contract, self).setUp()
        self.patch("os", FakeOS(isfile=False))

        def drift(bridge):
            # 模拟体内某相位改过配置（真实相位被桩掉，这里显式制造"需要恢复"）
            bridge.cfg["WriteBack"] = 1
            bridge.cfg["CacheSizeMB"] = 8

        self.patch("phase", fake_phase(self.bridge, on_first=drift))

    def test_restore_failure_is_nonzero(self):
        def hook(bridge, method, params):
            if method == 2:
                raise RuntimeError("injected: SET_CONFIG(original) failed")

        self.bridge.hook = hook
        rc, out = self.run_main()
        self.assertEqual(rc, 2)
        self.assertIn("RECOVERY_FATAL", out)
        self.assertIn('"STAGE": "restore"', out)

    def test_final_read_failure_is_nonzero(self):
        # 恢复写成功，但 finally 里读最终 stats/config 失败 -> 仍然非 0。
        state = {"recovered": False}

        def hook(bridge, method, params):
            if method == 2:
                state["recovered"] = True
            elif method == 1 and state["recovered"]:
                raise RuntimeError("injected: stats() unavailable after restore")

        self.bridge.hook = hook
        rc, out = self.run_main()
        self.assertEqual(rc, 2)
        self.assertIn("RECOVERY_FATAL", out)
        self.assertIn('"STAGE": "final_read"', out)

    def test_healthy_recovery_returns_zero(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertNotIn("RECOVERY_FATAL", out)
        self.assertIn("=== E-BATCH DONE ===", out)


class E2Contract(_ContractBase):
    """B-5 e2_concurrent_eviction.py。"""

    MODULE = "e2_concurrent_eviction"

    def setUp(self):
        super(E2Contract, self).setUp()
        self.patch("os", FakeOS(isfile=True))
        self.patch("oracle", fake_oracle)
        self.patch("make_main", lambda _p, _mib: None)

    def test_restore_failure_is_nonzero(self):
        # 第 4 次 method=2 是 P3 的"写回原值"。让它静默不生效（模拟恢复写失败），
        # 于是 finally 会再次尝试恢复，而这次注入异常。
        state = {"wb_cfg0": 0}

        def hook(bridge, method, params):
            if (method == 2 and params is not None
                    and params.get("WriteBack") == 0
                    and bridge.cfg.get("WriteBack") == 1):
                state["wb_cfg0"] += 1
                if state["wb_cfg0"] == 1:
                    bridge.skip_apply = True
                else:
                    raise RuntimeError("injected: SET_CONFIG(original) failed in recovery")

        self.bridge.hook = hook
        rc, out = self.run_main()
        self.assertEqual(rc, 2)
        self.assertIn("RECOVERY_FATAL", out)
        self.assertIn('"STAGE": "restore"', out)

    def test_healthy_recovery_returns_zero(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertNotIn("RECOVERY_FATAL", out)
        self.assertIn("=== E2 DONE ===", out)


class E4Contract(_ContractBase):
    """B-6 e4_i47_repro.py：恢复失败用 3（2 已被"oracle/前置缺失"占用）。"""

    MODULE = "e4_i47_repro"

    def setUp(self):
        super(E4Contract, self).setUp()
        self.patch("os", FakeOS(isfile=True, exists=False))

    def test_restore_failure_returns_three(self):
        # 体内唯一一次 method=2 是 :84 打开回写（此时原值是 WriteBack=0）；
        # finally 的恢复（WriteBack 已为 1）被注入失败。
        def hook(bridge, method, params):
            if method == 2 and bridge.cfg.get("WriteBack") == 1:
                raise RuntimeError("injected: bridge down during recovery")

        self.bridge.hook = hook
        rc, out = self.run_main()
        self.assertEqual(rc, 3)
        self.assertIn("RESTORE_FATAL", out)
        self.assertIn('"STAGE": "restore"', out)

    def test_final_stats_failure_returns_three(self):
        # finally 之后的 st_end = stats()（原 :159）也必须被兜住，不能裸抛。
        state = {"n": 0}

        def hook(bridge, method, params):
            if method == 1:
                state["n"] += 1
                if state["n"] == 7:      # 前 6 次：st0/st/触发后/循环/settle/恢复后读回
                    raise RuntimeError("injected: final stats() unavailable")

        self.bridge.hook = hook
        rc, out = self.run_main()
        self.assertEqual(rc, 3)
        self.assertIn("RESTORE_FATAL", out)
        self.assertIn('"STAGE": "final_stats"', out)
        self.assertNotIn("E4_VERDICT", out)     # 读不到最终态就不许编造判据

    def test_healthy_recovery_returns_zero(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertNotIn("RESTORE_FATAL", out)
        self.assertIn("E4_VERDICT", out)
        self.assertIn('"SURVIVED": true', out)


class A2WritebackStressContract(_ContractBase):
    """B-7 a2_writeback_stress.py。"""

    MODULE = "a2_writeback_stress"

    def setUp(self):
        super(A2WritebackStressContract, self).setUp()
        self.patch("os", FakeOS(isfile=True))
        self.patch("BACKUP", FakePath())
        self.patch("WORK", FakePath())

        def drift(bridge):
            bridge.cfg["WriteBack"] = 1

        self.patch("phase", fake_phase(self.bridge, on_first=drift))

    def test_restore_failure_is_nonzero(self):
        def hook(bridge, method, params):
            if method == 2:
                raise RuntimeError("injected: SET_CONFIG(original) failed")

        self.bridge.hook = hook
        rc, out = self.run_main()
        self.assertEqual(rc, 2)
        self.assertIn("RECOVERY_FATAL", out)
        self.assertIn('"STAGE": "restore"', out)

    def test_final_stats_failure_is_nonzero(self):
        # finally 之后的 final = stats()（原 :403）也要独立兜住。
        state = {"recovered": False}

        def hook(bridge, method, params):
            if method == 2:
                state["recovered"] = True
            elif method == 1 and state["recovered"]:
                raise RuntimeError("injected: final stats() unavailable")

        self.bridge.hook = hook
        rc, out = self.run_main()
        self.assertEqual(rc, 2)
        self.assertIn("RECOVERY_FATAL", out)
        self.assertIn('"STAGE": "final_stats"', out)
        self.assertIn("=== A2 SUMMARY ===", out)

    def test_healthy_recovery_returns_zero(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertNotIn("RECOVERY_FATAL", out)
        self.assertIn("=== A2 SUMMARY ===", out)


TOOLS_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "tools"))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

# (模块, 调用方式, 形态)。形态：T1=返回整个 body；T1b=helper 就地取 data；
# T3=degraded（不抛，返回 None 并把原因写 stderr）。
STATUS_TARGETS = [
    ("a2_concurrency_sweep", lambda f: f(1), "T1"),
    ("a2_repro_ab", lambda f: f(1), "T1"),
    ("a2_reverify_after_reboot", lambda f: f(1), "T1"),
    ("a2_writeback_stress", lambda f: f(1), "T1"),
    ("b_reverify_after_reboot", lambda f: f(1), "T1"),
    ("c3_single_machine_stress", lambda f: f(1), "T1b"),
    ("d3_verify_after_reboot", lambda f: f(1), "T1"),
    ("e1_abatch_discriminative", lambda f: f(1), "T1"),
    ("e2_concurrent_eviction", lambda f: f(1), "T1"),
    ("e4_i47_repro", lambda f: f(1), "T1"),
    ("e5_flush_probe", lambda f: f(1), "T1"),
    ("t1_trigger_evidence", lambda f: f(1), "T1"),
    ("t2_admit_discrimination", lambda f: f(1), "T1"),
    ("t4_exclude_three_way", lambda f: f(1), "T1b"),
    ("t6_ioctl_fuzz", lambda f: f(1), "T1"),
    ("probe_aligned_write", lambda f: f("http://127.0.0.1:5199"), "T1b"),
    ("probe_disk_truth", lambda f: f({"method": 1}), "T1"),
    ("probe_stale_attribution", lambda f: f("http://127.0.0.1:5199", {"method": 1}), "T1"),
    ("probe_through_staleness", lambda f: f("http://127.0.0.1:5199", 1), "T1"),
    ("probe_unbuffered_read", lambda f: f("http://127.0.0.1:5199", 1), "T1"),
    ("probe_write_readback_integrity", lambda f: f("http://127.0.0.1:5199"), "T3"),
]

HELPER_NAME = {"probe_aligned_write": "stats",
               "probe_write_readback_integrity": "stats_sample"}


class StatusEnforcement(unittest.TestCase):
    """153 §A 的 21 个 helper：status 必须真的被比较（§A.6 验收第 3 条）。"""

    def _helper(self, name, mod, _kind):
        return getattr(mod, HELPER_NAME.get(name, "rpc"))

    def test_status_ok_returns_data(self):
        for entry in STATUS_TARGETS:
            name, call, kind = entry
            with self.subTest(target=name):
                mod = importlib.import_module(name)
                saved = urllib.request.urlopen
                urllib.request.urlopen = FakeTransport()
                try:
                    res = call(self._helper(name, mod, kind))
                finally:
                    urllib.request.urlopen = saved
                if kind == "T1":
                    self.assertEqual(res.get("status"), 0)
                    self.assertIn("data", res)
                else:
                    self.assertIsInstance(res, dict)
                    self.assertTrue(res)

    def test_status_minus_one_raises(self):
        for entry in STATUS_TARGETS:
            name, call, kind = entry
            with self.subTest(target=name):
                mod = importlib.import_module(name)
                saved = urllib.request.urlopen
                urllib.request.urlopen = _StatusMinusOne()
                try:
                    if kind == "T3":
                        with contextlib.redirect_stderr(io.StringIO()) as err:
                            res = call(self._helper(name, mod, kind))
                        self.assertIsNone(res)
                        self.assertIn("WARN counters: rpc status=-1", err.getvalue())
                    else:
                        with self.assertRaises(RuntimeError) as ctx:
                            call(self._helper(name, mod, kind))
                        self.assertIn("status=-1", str(ctx.exception))
                        self.assertIn("injected: IOCTL failed", str(ctx.exception))
                finally:
                    urllib.request.urlopen = saved

    def test_status0_without_data_raises_only_for_t1b(self):
        for entry in STATUS_TARGETS:
            name, call, kind = entry
            if kind != "T1b":
                continue
            with self.subTest(target=name):
                mod = importlib.import_module(name)
                saved = urllib.request.urlopen
                urllib.request.urlopen = _NoData()
                try:
                    with self.assertRaises(RuntimeError) as ctx:
                        call(self._helper(name, mod, kind))
                    self.assertIn("no data field", str(ctx.exception))
                finally:
                    urllib.request.urlopen = saved


class _StatusMinusOne(object):
    def __call__(self, req, timeout=None, **_kw):
        return _Resp({"status": -1, "message": "injected: IOCTL failed"})


class _NoData(object):
    """status:0 但没有 data 字段 —— 服务端契约破损，T1b 必须拒绝。"""

    def __call__(self, req, timeout=None, **_kw):
        return _Resp({"status": 0})


# ---------------------------------------------------------------------------
# 端到端：status=-1（HTTP 200，不抛传输异常）也必须变成非 0 退出码
# ---------------------------------------------------------------------------
class A2ConcurrencySweepStatusContract(_ContractBase):
    MODULE = "a2_concurrency_sweep"
    PATCH_RPC = False       # 真实 rpc() + 假 HTTP

    def setUp(self):
        super(A2ConcurrencySweepStatusContract, self).setUp()
        self.patch("os", FakeOS(isfile=False))
        self.patch("run", fake_run)
        self.patch("LOGF", FakePath())
        self.patch("OUT", FakePath())
        state = {"m2": 0}

        def fail(method, params, _t):
            if method == 2:
                state["m2"] += 1
                return state["m2"] == 3      # finally 的恢复写被驱动拒绝
            return False

        self.patch_urlopen(FakeTransport(fail=fail))

    def test_status_minus_one_on_restore_returns_two(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 2)
        self.assertIn("RECOVERY_FAILED", out)
        self.assertIn("status=-1", out)


class E1StatusContract(_ContractBase):
    MODULE = "e1_abatch_discriminative"
    PATCH_RPC = False

    def setUp(self):
        super(E1StatusContract, self).setUp()
        self.patch("os", FakeOS(isfile=False))
        # phase 桩只记账 + 制造"体内改过配置"，绝不执行其 lambda
        transport = FakeTransport(fail=lambda m, p, t: m == 2)
        self.patch_urlopen(transport)
        self.patch("phase", fake_phase(
            transport, on_first=lambda tr: tr.cfg.__setitem__("WriteBack", 1)))

    def test_status_minus_one_on_restore_returns_two(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 2)
        self.assertIn("RECOVERY_FATAL", out)
        self.assertIn("status=-1", out)


class E2StatusContract(_ContractBase):
    MODULE = "e2_concurrent_eviction"
    PATCH_RPC = False

    def setUp(self):
        super(E2StatusContract, self).setUp()
        self.patch("os", FakeOS(isfile=True))
        self.patch("oracle", fake_oracle)
        self.patch("make_main", lambda _p, _mib: None)
        state = {"m4": 0}

        def fail(method, params, _t):
            if method == 4:
                state["m4"] += 1
                return state["m4"] == 2      # finally 里 config() 被驱动拒绝
            return False

        self.patch_urlopen(FakeTransport(fail=fail))

    def test_status_minus_one_in_finally_returns_two(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 2)
        self.assertIn("RECOVERY_FATAL", out)
        self.assertIn("status=-1", out)


class E4StatusContract(_ContractBase):
    MODULE = "e4_i47_repro"
    PATCH_RPC = False

    def setUp(self):
        super(E4StatusContract, self).setUp()
        self.patch("os", FakeOS(isfile=True, exists=False))
        self.patch_urlopen(FakeTransport(
            fail=lambda m, p, t: m == 2 and t.cfg.get("WriteBack") == 1))

    def test_status_minus_one_on_restore_returns_three(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 3)
        self.assertIn("RESTORE_FATAL", out)
        self.assertIn("status=-1", out)


class A2WritebackStatusContract(_ContractBase):
    MODULE = "a2_writeback_stress"
    PATCH_RPC = False

    def setUp(self):
        super(A2WritebackStatusContract, self).setUp()
        self.patch("os", FakeOS(isfile=True))
        self.patch("BACKUP", FakePath())
        self.patch("WORK", FakePath())
        transport = FakeTransport(fail=lambda m, p, t: m == 2)
        self.patch_urlopen(transport)

        def drift(tr):
            tr.cfg["WriteBack"] = 1

        self.mod.phase = fake_phase(transport, on_first=drift)

    def test_status_minus_one_on_restore_returns_two(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 2)
        self.assertIn("RECOVERY_FATAL", out)
        self.assertIn("status=-1", out)


# ---------------------------------------------------------------------------
# 157：a2_repro_ab / a2_reverify / b_reverify 的退出码契约
# ---------------------------------------------------------------------------
FAKE_SYS = b"RAMBlockCache.sys FAKE IMAGE 2026-11-12"
FAKE_SHA = hashlib.sha256(FAKE_SYS).hexdigest()
DEPLOY_TXT = "SIGNED sha=%s\nRESULT: OK\n" % FAKE_SHA


def fail_nth_method(n, method):
    """第 n 次调用指定 method 时返回 True（用于只打中 finally 的恢复调用）。"""
    state = {"n": 0}

    def pred(m, _p, _t):
        if m != method:
            return False
        state["n"] += 1
        return state["n"] == n

    return pred


class FakeSysFile(object):
    """DEPLOY_LOG / SYS32 的替身（Path 的两个方法 + exists）。"""

    def __init__(self, text="", data=b""):
        self._text = text
        self._data = data

    def exists(self):
        return True

    def read_bytes(self):
        return self._data

    def read_text(self, encoding=None, errors=None):
        return self._text


def fake_oracle_pass(_path, _threads, _seed, verify_only=False):
    return {"exit": 0, "sha_ok": True, "ok_threads": 0, "threads": _threads,
            "seed": _seed, "fail_msgs": [], "verify_only": verify_only}


def fake_read_group(exe, label, settle=0.8):
    """Q 组命中（阳性对照），P/E 组被排除 —— 只用于把 main() 推到结尾。"""
    if label.startswith("Q"):
        return {"label": label, "exe": exe, "exit": 0, "d_ReadReq": 8,
                "d_ReadHit": 8, "d_ReadBytes": 4096, "excluded": False}
    return {"label": label, "exe": exe, "exit": 0, "d_ReadReq": 0,
            "d_ReadHit": 0, "d_ReadBytes": 0, "excluded": True}


class A2ReproAbExitCodeContract(_ContractBase):
    """157-1：原本恒 return 0（恢复失败也 0）→ 恢复失败 3。"""

    MODULE = "a2_repro_ab"
    PATCH_RPC = False

    def _install(self, **kw):
        self.patch("os", FakeOS())
        self.patch("OUT", FakePath())
        self.patch("run_oracle", lambda path, threads, tag: {
            "tag": tag, "exit": 0, "ok_thread_count": threads,
            "write_thread_lines": [], "fail_lines": [], "verify_lines": [],
            "full_log": "<mem>"})
        self.patch_urlopen(FakeTransport(**kw))

    def test_transport_error_in_recovery_returns_three(self):
        # 体内 3 次 method=2（:103 cwb / :117 csmall / :122 cwb），第 4 次是 finally 的恢复
        self._install(boom=fail_nth_method(4, 2))
        rc, out = self.run_main()
        self.assertEqual(rc, 3)
        self.assertIn("RECOVERY_FAILED", out)

    def test_status_minus_one_in_recovery_returns_three(self):
        self._install(fail=fail_nth_method(4, 2))
        rc, out = self.run_main()
        self.assertEqual(rc, 3)
        self.assertIn("RECOVERY_FAILED", out)
        self.assertIn("status=-1", out)

    def test_healthy_recovery_returns_zero(self):
        self._install()
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertNotIn("RECOVERY_FAILED", out)


class A2ReverifyExitCodeContract(_ContractBase):
    """157-2：原本恢复失败只让 VERDICT=FAIL→1；现在用 3 区分。"""

    MODULE = "a2_reverify_after_reboot"
    PATCH_RPC = False

    def _install(self, **kw):
        self.patch("DEPLOY_LOG", FakeSysFile(text=DEPLOY_TXT))
        self.patch("SYS32", FakeSysFile(data=FAKE_SYS))
        self.patch("LOGF", FakePath())
        self.patch("OUT", FakePath())
        self.patch("os", FakeOS(isfile=False))
        self.patch("oracle", fake_oracle_pass)
        self.patch_urlopen(FakeTransport(**kw))

    def test_transport_error_in_recovery_returns_three(self):
        # 体内 2 次 method=2（:148 cwb / :167 c0），第 3 次是 finally 的恢复
        self._install(boom=fail_nth_method(3, 2))
        rc, out = self.run_main()
        self.assertEqual(rc, 3)
        self.assertIn("RECOVERY_FAILED", out)

    def test_status_minus_one_in_recovery_returns_three(self):
        self._install(fail=fail_nth_method(3, 2))
        rc, out = self.run_main()
        self.assertEqual(rc, 3)
        self.assertIn("RECOVERY_FAILED", out)
        self.assertIn("status=-1", out)

    def test_healthy_recovery_returns_zero(self):
        self._install()
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertNotIn("RECOVERY_FAILED", out)
        self.assertIn("RESULT: PASS", out)

    def test_verdict_fail_without_recovery_problem_returns_one(self):
        """判据失败仍走原来的 1，不能与"恢复失败 3"混为一谈。"""
        self._install()
        self.patch("os", FakeOS(isfile=True))       # 让 final_disk_oracle 非空
        self.patch("oracle", lambda *a, **k: {"exit": 0, "sha_ok": False})
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertNotIn("RECOVERY_FAILED", out)
        self.assertIn("RESULT: FAIL", out)


class BReverifyExitCodeContract(_ContractBase):
    """157-3：同 a2_reverify。"""

    MODULE = "b_reverify_after_reboot"
    PATCH_RPC = False

    def _install(self, **kw):
        self.patch("DEPLOY_LOG", FakeSysFile(text=DEPLOY_TXT))
        self.patch("SYS32", FakeSysFile(data=FAKE_SYS))
        self.patch("OUT", FakePath())
        self.patch("os", FakeOS(isfile=False))
        self.patch("shutil", types.SimpleNamespace(copy2=lambda *a, **k: None))
        self.patch("oracle", fake_oracle_pass)
        self.patch("read_group", fake_read_group)
        self.patch("pool", lambda: {"allocs": 0, "frees": 0, "outstanding": 0})
        self.patch_urlopen(FakeTransport(**kw))

    def test_transport_error_in_recovery_returns_three(self):
        # 体内 2 次 method=2（:188 cwb / :197 c0），第 3 次是 finally 的恢复
        self._install(boom=fail_nth_method(3, 2))
        rc, out = self.run_main()
        self.assertEqual(rc, 3)
        self.assertIn("RECOVERY_FAILED", out)

    def test_status_minus_one_in_recovery_returns_three(self):
        self._install(fail=fail_nth_method(3, 2))
        rc, out = self.run_main()
        self.assertEqual(rc, 3)
        self.assertIn("RECOVERY_FAILED", out)
        self.assertIn("status=-1", out)

    def test_healthy_recovery_returns_zero(self):
        self._install()
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertNotIn("RECOVERY_FAILED", out)
        self.assertIn("RESULT: PASS", out)

    def test_verdict_fail_without_recovery_problem_returns_one(self):
        """判据失败仍走原来的 1（Q 组无命中 → B_positive_control_Q_hits False）。"""
        self._install()
        self.patch("read_group", lambda exe, label, settle=0.8: {
            "label": label, "exe": exe, "exit": 0, "d_ReadReq": 0,
            "d_ReadHit": 0, "d_ReadBytes": 0, "excluded": True})
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertNotIn("RECOVERY_FAILED", out)
        self.assertIn("RESULT: FAIL", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)
