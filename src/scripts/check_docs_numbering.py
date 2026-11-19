#!/usr/bin/env python3
"""Gate22 - deliverable numbering contract.

Why this exists (report 222, round R23 "docs vs code consistency"):
  `AGENTS.md` is the entry point for every new session and it *instructs* the
  reader which number to use next ("current max = N, next = N+1"). That claim
  is hand-maintained, so it silently rots. On 2026-11-13 it still said 143/144
  while `deliverables/` was already at 221 -- and `144-gate-audit-2026-11-11.md`
  already existed. Following the protocol therefore meant **overwriting a real
  deliverable**, which is silent data loss with no error anywhere.

  A doc that actively tells the next person to do the wrong thing is worse than
  a missing doc, so the claim is now machine-checked.

Checks (pure static, no writes, no network):
  1. `deliverables/` numbers form no duplicates across extensions
     (`123-a.md` + `123-b.md` is fine -- same number, different slug).
  2. Every "next number" declaration in `AGENTS.md` equals max+1.
  3. Same for `deliverables/HANDOVER-CURRENT.md` *if* it carries such a claim
     (it is the live handover; it may legitimately be silent, but if it names a
     number it must name the right one).
  4. The declared max in AGENTS.md (if present) equals the real max.
  5. README release-baseline SHA256 hashes (sys sidecar + computed Setup.exe)
     must agree with the real distribution; an empty match set is a FAIL.

Exit 0 = PASS, 1 = FAIL.
"""
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DELIV = ROOT / "deliverables"
AGENTS = ROOT / "AGENTS.md"
HANDOVER = DELIV / "HANDOVER-CURRENT.md"

fails = []

# `NNN-slug.md` / `NNN-handover-YYYY-MM-DD.md`; ignore bare `123.md` and `09-xxx`.
NUM_RE = re.compile(r"^(\d{3})-[^/]+$")

# Declarations seen in the docs, in the forms actually used:
#   AGENTS.md : "（当前 143，下一个 144）" ... "当前最大 = **143**，下一个 **144**。"
#   HANDOVER  : "**下一个可用编号 = 219**"
NEXT_PATTERNS = [
    re.compile(r"下一个\s*\*{0,2}\s*(\d{3})"),
    re.compile(r"下一个可用编号\s*=\s*\*{0,2}\s*(\d{3})"),
]
MAX_PATTERNS = [
    re.compile(r"当前最大\s*=\s*\*{0,2}\s*(\d{3})"),
    re.compile(r"当前\s+(\d{3})\s*，\s*下一个"),
]


def collect_numbers():
    """Return dict: number -> sorted list of filenames that use it."""
    used = {}
    if not DELIV.is_dir():
        fails.append(f"deliverables/ not found at {DELIV}")
        return used
    for p in sorted(DELIV.iterdir()):
        if not p.is_file():
            continue
        m = NUM_RE.match(p.name)
        if not m:
            continue
        used.setdefault(int(m.group(1)), []).append(p.name)
    return used


def scan(path, label):
    """Return (declared_next:set[int], declared_max:set[int])."""
    if not path.is_file():
        return set(), set()
    text = path.read_text(encoding="utf-8", errors="replace")
    nxt = set()
    for rx in NEXT_PATTERNS:
        nxt.update(int(x) for x in rx.findall(text))
    mx = set()
    for rx in MAX_PATTERNS:
        mx.update(int(x) for x in rx.findall(text))
    return nxt, mx


def check_readme_release_hashes(repo_root: Path) -> list[str]:
    """Check README's first-30-line release baseline against real dist hashes.

    F-01: the anchor is EXACTLY 'SHA256: <64-hex>'. A bare 64-hex value
    without the declared label used to pass silently (QA finding B.1/S3a),
    so the format check is now fail-closed.
    BUG-NEW-02: never truncate to 8 chars; compare declared prefix and optional
    abbreviated suffix, and treat an empty match set as FAIL (not a vacuous PASS).
    Both dist/RAMBlockCache.sys (via its anchor) and the real Setup.exe are checked.
    """
    errors: list[str] = []
    sha_file = repo_root / "dist" / "RAMBlockCache.sys.sha256"
    setup_file = repo_root / "dist" / "RAMBlockCache-Setup.exe"
    readme_file = repo_root / "README.md"

    if not sha_file.is_file() or not readme_file.is_file():
        return ["[FAIL] missing dist/RAMBlockCache.sys.sha256 or README.md"]

    sha_raw = sha_file.read_text(encoding="utf-8", errors="replace").strip()
    # [QA finding #1 closure 2026-11-13] Require the 'SHA256: ' label. The old
    # `re.search(r"\b([0-9a-fA-F]{64})\b", ...)` accepted a bare hash, which
    # meant a malformed anchor file could never be detected. Empty/malformed
    # match sets must FAIL (project iron rule), never pass vacuously.
    m_anchor = re.fullmatch(r"SHA256:\s*([0-9a-fA-F]{64})", sha_raw)
    if not m_anchor:
        return [f"[FAIL] {sha_file} must contain exactly 'SHA256: <64-hex>'"]
    anchor_hex = m_anchor.group(1).lower()

    readme_lines = readme_file.read_text(encoding="utf-8", errors="replace").splitlines()
    # BUG-NEW-02 fix: limit extraction to the release baseline zone (first 30
    # lines) so historical prose cannot interfere.
    baseline_zone = "\n".join(readme_lines[:30])

    sys_matches = re.findall(
        r"dist/RAMBlockCache\.sys[^\r\n]*?`?([0-9a-fA-F]{8,64})(?:…|\.\.\.)?([0-9a-fA-F]{0,16})`?",
        baseline_zone,
    )
    # Iron rule: an empty match set must never be read as PASS.
    if not sys_matches:
        return ["[FAIL] README.md baseline zone has 0 matches for dist/RAMBlockCache.sys hash"]

    for prefix_hex, suffix_hex in sys_matches:
        p_low = prefix_hex.lower()
        s_low = suffix_hex.lower()
        if not anchor_hex.startswith(p_low) or (s_low and not anchor_hex.endswith(s_low)):
            errors.append(
                f"[FAIL] README.md dist/RAMBlockCache.sys hash '{prefix_hex}...{suffix_hex}' "
                f"does not match anchor '{anchor_hex}'"
            )

    if setup_file.is_file():
        setup_hex = hashlib.sha256(setup_file.read_bytes()).hexdigest().lower()
        setup_matches = re.findall(
            r"dist/RAMBlockCache-Setup\.exe[^\r\n]*?`?([0-9a-fA-F]{8,64})(?:…|\.\.\.)?([0-9a-fA-F]{0,16})`?",
            baseline_zone,
        )
        if not setup_matches:
            errors.append("[FAIL] README.md baseline zone has 0 matches for dist/RAMBlockCache-Setup.exe hash")
        for prefix_hex, suffix_hex in setup_matches:
            p_low = prefix_hex.lower()
            s_low = suffix_hex.lower()
            if not setup_hex.startswith(p_low) or (s_low and not setup_hex.endswith(s_low)):
                errors.append(
                    f"[FAIL] README.md dist/RAMBlockCache-Setup.exe hash '{prefix_hex}...{suffix_hex}' "
                    f"does not match actual '{setup_hex}'"
                )

    return errors


def main():
    used = collect_numbers()
    if not used:
        print("[FAIL] no numbered deliverables found")
        return 1
    real_max = max(used)
    expected = real_max + 1

    # 1. duplicate numbers with the same slug prefix are a real hazard:
    #    `222-audit.md` written twice under different names splits one report.
    #    We only fail when two files share number AND slug stem.
    for num, names in sorted(used.items()):
        stems = {}
        for n in names:
            stem = n[len(f"{num:03d}-"):].rsplit(".", 1)[0]
            stems.setdefault(stem, []).append(n)
        for stem, group in stems.items():
            if len(group) > 1:
                fails.append(f"duplicate deliverable number+slug {num:03d}-{stem}: {group}")

    # 2/3/4. "next number" claims must match reality.
    #
    # Fail-closed discipline (the trap this project has hit before): an empty
    # match set must never be read as "PASS". If AGENTS.md is renamed, emptied,
    # or the wording drifts away from the recognised patterns, the old version
    # of this check would have reported success while checking nothing at all.
    # So: AGENTS.md must exist AND must still contain a next-number claim.
    nxt, mx = scan(AGENTS, "AGENTS.md")
    if not AGENTS.is_file():
        fails.append("AGENTS.md missing -- the protocol entry point must exist")
    elif not nxt:
        fails.append(
            "AGENTS.md no longer declares a next deliverable number "
            "(expected a sentence containing '下一个 NNN'); refusing to pass vacuously"
        )
    for n in sorted(nxt):
        if n != expected:
            fails.append(
                f"AGENTS.md: says next number is {n:03d}, but deliverables/ max is "
                f"{real_max:03d} => next must be {expected:03d}"
            )
    for m in sorted(mx):
        if m != real_max:
            fails.append(f"AGENTS.md: says current max is {m:03d}, but real max is {real_max:03d}")

    # HANDOVER-CURRENT.md is rewritten from scratch on every context compaction.
    # Its template does not mandate a numbering line, so a missing claim is only
    # a warning -- but a *wrong* claim is a hard failure.
    hnxt, hmx = scan(HANDOVER, "HANDOVER-CURRENT.md")
    if not HANDOVER.is_file():
        print("[WARN] HANDOVER-CURRENT.md missing (live handover not present)")
    for n in sorted(hnxt):
        if n != expected:
            fails.append(
                f"HANDOVER-CURRENT.md: says next number is {n:03d}, but deliverables/ max is "
                f"{real_max:03d} => next must be {expected:03d}"
            )
    for m in sorted(hmx):
        if m != real_max:
            fails.append(
                f"HANDOVER-CURRENT.md: says current max is {m:03d}, but real max is {real_max:03d}"
            )

    # 5. README release baseline hashes (ISS-05 / BUG-NEW-02). The function
    #    returns [] only when both hashes were actually matched and verified;
    #    an empty match set inside it is a hard FAIL, so extending `fails` here
    #    cannot silently pass.
    fails.extend(check_readme_release_hashes(ROOT))

    print(f"deliverables numbered files: {sum(len(v) for v in used.values())} "
          f"unique numbers: {len(used)} max={real_max:03d} next={expected:03d}")
    if fails:
        print(f"[FAIL] {len(fails)} issue(s):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("[OK] numbering contract holds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
