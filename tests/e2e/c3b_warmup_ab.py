#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""C3B · A/B 定位 C3 首轮 d_outstanding=+8 的性质

C3 首轮 d_allocs=373 / d_frees=365 -> +8，之后 7 轮全部 0，
idle 60 s 绝对 outstanding 恒 11。两种竞争假设：
  W1 一次性常驻预热（首次触发某路径时懒分配常驻对象，此后不再增长）
  W2 慢泄漏（每轮都净增，只是精度/采样掩盖了）

判别设计（不靠放宽判据，靠可区分的因果量）：
  A 组：在「已预热」态连续 6 轮 D 组压测，逐轮记录绝对 outstanding。
        W1 => 每轮 d_outstanding==0 且绝对 outstanding 恒定
        W2 => 绝对 outstanding 单调上升
  B 组：A 组之后再制造一次「冷路径」（控制面 IOCTL 30 次），
        看是否再次出现 +8；若不再出现，进一步排除「每次冷启动都 +8」。
  阳性对照：控制面 IOCTL 必须产生 allocs>0（器具能看见分配）。
  长窗：30 s idle 后绝对 outstanding 不变。
"""
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, r"C:\Users\Public\o9\stress")
import c3_single_machine_stress as c3

OUT = Path(r"C:\Users\Public\o9\stress\c3b_result.json")


def main():
    r = {}
    prior = json.loads(Path(r"C:\Users\Public\o9\stress\c3_result.json")
                       .read_text(encoding="utf-8"))
    r["c3_prior"] = {"before_pool": prior["before"]["pool"],
                     "after_pool": prior["after"]["pool"],
                     "first_round": prior["D_group"][0],
                     "steady_rounds": [x["d_outstanding"] for x in prior["D_group"][1:]],
                     "idle": prior["idle"]}

    # 重新建文件（C3 已清理）
    c3.make_file(c3.HOT_D)
    c3.make_file(c3.COLD_C)
    try:
        p_start = c3.pool()
        rounds = []
        for i in range(6):
            a = c3.pool()
            c3.run_group("D", 1, 20)
            time.sleep(1.0)
            b = c3.pool()
            rounds.append({"round": i + 1,
                           "abs_outstanding_before": a["outstanding"],
                           "abs_outstanding_after": b["outstanding"],
                           "d_outstanding": b["outstanding"] - a["outstanding"],
                           "d_allocs": b["allocs"] - a["allocs"],
                           "d_frees": b["frees"] - a["frees"],
                           "np_mb": b["np_mb"]})
        r["A_steady_rounds"] = rounds
        r["B_control_after_warm"] = c3.control_ioctls(10)
        p_end = c3.pool()
        time.sleep(30)
        p_idle = c3.pool()
        r["idle_30s"] = {"before": p_end, "after": p_idle,
                         "d_outstanding": p_idle["outstanding"] - p_end["outstanding"]}
    finally:
        for p in (c3.HOT_D, c3.COLD_C):
            try:
                p.unlink()
            except OSError:
                pass

    abs_series = [x["abs_outstanding_after"] for x in rounds]
    delta_series = [x["d_outstanding"] for x in rounds]
    r["checks"] = {
        "A_all_steady_deltas_zero": all(x == 0 for x in delta_series),
        "A_absolute_outstanding_constant": max(abs_series) - min(abs_series) == 0,
        "A_allocs_still_observed": sum(x["d_allocs"] for x in rounds) > 0,
        "B_control_allocs_positive": r["B_control_after_warm"]["d_allocs"] > 0,
        "B_no_second_warm_spike": r["B_control_after_warm"]["d_outstanding"] == 0,
        "idle_constant": r["idle_30s"]["d_outstanding"] == 0,
        "np_mb_constant": len({x["np_mb"] for x in rounds}) == 1,
    }
    r["interpretation"] = (
        "W1 一次性常驻预热成立：+8 只在首次触发数据面路径时出现，"
        "随后 7+6 轮稳态严格配对、绝对 outstanding 恒定、NPagedUsed 恒定、"
        "60s+30s 延迟窗口无变化。W2 慢泄漏被排除。"
        if all(r["checks"].values()) else
        "出现单调增长或第二波尖峰，需按 W2 继续取证")
    r["VERDICT"] = "PASS" if all(r["checks"].values()) else "FAIL"
    OUT.write_text(json.dumps(r, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"A": rounds}, ensure_ascii=False))
    print(json.dumps({"B": r["B_control_after_warm"], "IDLE": r["idle_30s"]},
                     ensure_ascii=False))
    print(json.dumps({"CHECKS": r["checks"]}, ensure_ascii=False))
    print(r["interpretation"])
    print("RESULT: %s" % r["VERDICT"])
    return 0 if r["VERDICT"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
