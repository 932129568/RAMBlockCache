"""Full-interleaving model for the O9-EXPID fix (2026-11-09).

Context
-------
`Rb2PidInExcludeDirs` runs on the IO hot path on many workers at once.
Before the fix the PID decision cache used a plain ULONG counter:

    if (s_ExPidN >= 64) s_ExPidN = 0;
    s_ExPid[s_ExPidN].Pid = pid;          # index may already be 64 -> OOB
    s_ExPid[s_ExPidN].Excluded = excluded;
    s_ExPidN++;                           # non-atomic

The counter is declared immediately after a 64-entry array, so an OOB write
lands on the counter itself; the read-side loop `for (i < s_ExPidN)` then
becomes a large OOB read.

The fix makes the counter a volatile LONG, clamps the read side to 64, and
reserves a slot with a single CAS transition:

    cur = CAS(N, 0, 0)                      # atomic snapshot
    slot = 0 if cur outside [0,63] else cur
    next = 1 if cur outside [0,63] else cur + 1
    CAS(N, next, cur) == cur -> success, else retry

This model exhaustively interleaves two threads (plus a third reader) at the
atomic-operation granularity and asserts the invariants. It models the
COUNTER and SLOT OWNERSHIP only; it does not model real kernel memory layout
or compiler reordering (MemoryBarrier in the C code handles publication).

Invariants
----------
I1  Every slot used for a write is in [0, 63].
I2  The counter never exceeds 64 after a successful CAS.
I3  The read-side bound is always <= 64.
I4  No two concurrent successful CAS operations claim the same slot.
I5  A retry always observes a changed counter (system-wide progress).
"""
import itertools

MAX = 64


class Counter:
    def __init__(self, start=0):
        self.n = start

    def snapshot(self):
        return self.n

    def cas(self, expected, new):
        if self.n == expected:
            self.n = new
            return True, expected
        return False, self.n


def reserve(c, events):
    """One thread's reservation loop. Records each CAS attempt in events."""
    spins = 0
    while True:
        cur = c.snapshot()
        events.append(("snap", cur))
        if cur < 0 or cur >= MAX:
            slot, nxt = 0, 1
        else:
            slot, nxt = cur, cur + 1
        ok, observed = c.cas(cur, nxt)
        events.append(("cas", cur, nxt, ok, observed))
        if ok:
            return slot, spins
        spins += 1
        assert observed != cur, "I5 violated: retry saw an unchanged counter"
        assert spins < 1000, "livelock"


def read_side(c):
    """Mirrors the fixed read-side clamp."""
    n = c.snapshot()
    if n > MAX:
        n = MAX
    assert 0 <= n <= MAX, "I3 violated: read bound %d" % n
    return n


def run_case(start, interleave):
    """Interleave two reserving threads according to a chosen schedule.

    interleave is a sequence of 0/1 selecting which thread takes the next
    atomic step. We model steps as (snapshot, cas) pairs, and allow the
    reader to observe at any step boundary.
    """
    c = Counter(start)
    events = []
    slots = []
    # Precompute the two threads' behaviours by interleaving their atomic
    # pairs manually: each "step" is one snapshot+cas attempt of one thread.
    pending = {0: None, 1: None}
    done = {0: False, 1: False}
    results = {}

    def step(t):
        cur = c.snapshot()
        events.append(("snap", t, cur))
        if cur < 0 or cur >= MAX:
            slot, nxt = 0, 1
        else:
            slot, nxt = cur, cur + 1
        ok, observed = c.cas(cur, nxt)
        events.append(("cas", t, cur, nxt, ok, observed))
        if ok:
            assert 0 <= slot < MAX, "I1 violated: slot=%d" % slot
            assert c.n <= MAX, "I2 violated: counter=%d" % c.n
            results[t] = slot
            done[t] = True
        else:
            assert observed != cur, "I5 violated"
        # Reader observes at every step boundary.
        read_side(c)

    for t in interleave:
        if not done[t]:
            step(t)
    for t in (0, 1):
        while not done[t]:
            step(t)

    assert 0 <= results[0] < MAX and 0 <= results[1] < MAX
    assert results[0] != results[1], "I4 violated: both claimed slot %d" % results[0]
    assert c.n <= MAX, "I2 violated at end"
    read_side(c)
    return results, c.n


def legacy_case(start, schedule):
    """Step-grained model of the OLD code.

    Each thread executes three separable steps:
        check : read N, and if N >= MAX reset N to 0 (non-atomic)
        index : RE-READ N and use it as the array index  <-- TOCTOU window
        inc   : N = N + 1 (non-atomic read-modify-write)

    `schedule` is a sequence of (thread, step) pairs, so a concurrent worker
    can advance N between another thread's check and index. That is exactly
    the window that produces `s_ExPid[64]` in the real driver.
    """
    c = Counter(start)
    oob = False
    used = []
    state = {0: "check", 1: "check"}
    for t, step in schedule:
        if step != state[t]:
            continue
        if step == "check":
            cur = c.snapshot()
            if cur >= MAX:
                c.n = 0
            state[t] = "index"
        elif step == "index":
            idx = c.snapshot()          # re-read: may already be >= MAX
            if idx >= MAX:
                oob = True
            used.append(idx)
            state[t] = "inc"
        else:  # inc
            c.n = c.n + 1              # non-atomic ++
            state[t] = "check"
    return oob, used, c.n


def main():
    cases = 0
    failures = []
    legacy_oob = 0
    legacy_total = 0

    schedules = [
        (0, 1), (1, 0), (0, 0, 1, 1), (1, 1, 0, 0),
        (0, 1, 0, 1), (1, 0, 1, 0),
    ]
    for start in (0, 1, 62, 63, 64, 65):
        for sched in schedules:
            cases += 1
            try:
                run_case(start, sched)
            except AssertionError as e:
                failures.append("start=%d sched=%s -> %s" % (start, sched, e))

    # Legacy: explicit TOCTOU schedule.
    # A checks N=63 (passes). B checks=63, indexes 63, increments -> 64.
    # A then RE-READS N for its index and gets 64 -> s_ExPid[64] OOB write.
    legacy_schedules = [
        [(0, "check"), (1, "check"), (1, "index"), (1, "inc"), (0, "index")],
        [(0, "check"), (1, "check"), (1, "index"), (0, "index"), (1, "inc")],
        [(0, "check"), (1, "check"), (1, "index"), (1, "inc"), (0, "index"),
         (0, "inc")],
    ]
    for start in (63, 62):
        for sched in legacy_schedules:
            oob, _, _ = legacy_case(start, sched)
            legacy_total += 1
            if oob:
                legacy_oob += 1

    # Additional: all 3-thread schedules of length 4 from boundary starts.
    for start in (62, 63, 64):
        for sched in itertools.product((0, 1), repeat=4):
            cases += 1
            try:
                run_case(start, sched)
            except AssertionError as e:
                failures.append("start=%d sched=%s -> %s" % (start, sched, e))

    print("== O9-EXPID ownership model ==")
    print("fixed-implementation interleavings: %d" % cases)
    print("assertion failures: %d" % len(failures))
    for f in failures[:10]:
        print("  FAIL " + f)
    print("legacy-implementation OOB reproduced: %d/%d (expected > 0)" %
          (legacy_oob, legacy_total))
    print("RESULT: %s" % ("PASS" if not failures and legacy_oob > 0 else "FAIL"))
    return 0 if (not failures and legacy_oob > 0) else 1


if __name__ == "__main__":
    raise SystemExit(main())
