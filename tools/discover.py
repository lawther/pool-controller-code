#!/usr/bin/env python3
"""Unattended pool controller register discovery.

Runs three phases in a continuous loop, saving state after every step:

  Phase 1 — SCAN    : queries every register/slot (256 × 16, ~17 min)
  Phase 2 — RETEST  : direct-queries any single-observation suspects
  Phase 3 — LISTEN  : passive listen for LISTEN_DURATION minutes,
                       then loops back to Phase 1

Tracks value changes across rounds — any register whose value shifts is
flagged prominently at the start of the next round and in every report.

Usage:
  python3 tools/discover.py                   # auto-discover host
  python3 tools/discover.py 192.168.1.213
  python3 tools/discover.py --fresh           # start over

State is saved to discover_YYYYMMDD_HHMMSS.json.  Resumable at any point.

The output file is compatible with scan_registers.py:
  python3 tools/scan_registers.py discover_*.json
"""

import glob
import json
import math
import os
import socket
import statistics
import sys
import time
from datetime import datetime
from typing import Any

DEFAULT_HOST = "192.168.1.213"
DEFAULT_PORT = 7373
REGS = range(0x100)
SLOTS = range(0x10)
RESPONSE_WAIT = 0.25  # seconds per query
RETEST_PASSES = 3
RETEST_WAIT = 0.5
LISTEN_DURATION = 10 * 60  # seconds of passive listen per cycle
LISTEN_REPORT = 60  # seconds between listen status prints
SAVE_INTERVAL = 60  # seconds between saves during listen

# ---------------------------------------------------------------------------
# Protocol helpers
# ---------------------------------------------------------------------------

CHAN_TYPES = {
    0x00: "None",
    0x01: "Filter",
    0x02: "Cleaning",
    0x03: "Waterfall",
    0x04: "Blower",
    0x09: "Blower",
    0x0A: "Heater",
    0x12: "Light",
}
ZONE_NAMES = {
    0x00: "Pool",
    0x01: "Spa",
    0x02: "Pool & Spa",
    0x03: "Waterfall 1",
    0x04: "Waterfall 2",
    0x05: "Waterfall 3",
}
LIGHT_COLORS = {
    0x00: "White",
    0x01: "Magenta",
    0x02: "Red",
    0x03: "Violet",
    0x04: "Purple",
    0x05: "Blue",
    0x06: "Cyan",
    0x07: "Green",
    0x08: "Teal",
}


def make_read_request(reg: int, slot: int) -> str:
    hdr = [0x02, 0x00, 0xF0, 0xFF, 0xFF, 0x80, 0x00, 0x39, 0x0E]
    hdr_cs = sum(hdr) & 0xFF
    msg = hdr + [hdr_cs, reg, slot, (reg + slot) & 0xFF, 0x03]
    return " ".join(f"{b:02X}" for b in msg)


def parse_hex_line(line: str) -> list[int]:
    parts = line.strip().split()
    if not parts:
        return []
    try:
        vals = [int(p, 16) for p in parts]
        return vals if all(0 <= v <= 255 for v in vals) else []
    except ValueError:
        return []


def is_register_response(msg: list[int]) -> bool:
    return (
        len(msg) >= 14
        and msg[0] == 0x02
        and msg[1] == 0x00
        and msg[2] == 0x50
        and msg[3] == 0xFF
        and msg[4] == 0xFF
        and msg[7] == 0x38
    )


def extract_responses(raw: bytes) -> list[tuple[int, int, list[int]]]:
    found = []
    for line in raw.decode(errors="replace").split("\n"):
        msg = parse_hex_line(line.strip())
        if is_register_response(msg):
            found.append((msg[10], msg[11], msg[12:-2]))
    return found


def _connect(host: str, port: int) -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((host, port))
    s.settimeout(RESPONSE_WAIT)
    time.sleep(0.3)
    try:
        while s.recv(4096):
            pass
    except (TimeoutError, OSError):
        pass
    return s


def _reconnect(host: str, port: int, label: str = "") -> socket.socket:
    """Keep retrying until a connection succeeds; returns the new socket."""
    delay = 5
    while True:
        try:
            s = _connect(host, port)
            print(f"  [reconnected{' at ' + label if label else ''}]")
            return s
        except Exception as e:
            print(f"  [connect failed: {e} — retrying in {delay}s]")
            time.sleep(delay)


def _recv(s: socket.socket) -> bytes:
    raw = b""
    deadline = time.time() + RESPONSE_WAIT
    while time.time() < deadline:
        try:
            chunk = s.recv(4096)
            if chunk:
                raw += chunk
        except TimeoutError:
            break
    return raw


def decode_payload(payload: list[int] | bytes) -> str:
    if not payload:
        return ""
    if all(v == 0 for v in payload):
        return "[zeros]"
    try:
        s = bytes(payload).split(b"\x00")[0].decode("ascii")
        if s.isprintable() and s:
            return f'"{s}"'
    except Exception:
        pass
    return " ".join(f"{v:02X}" for v in payload)


def _fmt_iv(seconds: float) -> str:
    return f"{seconds:.1f}s" if seconds < 60 else f"{seconds / 60:.1f}m"


# ---------------------------------------------------------------------------
# Value history — tracks every distinct value seen for each register/slot
# ---------------------------------------------------------------------------


def _record_value(
    state: dict[str, Any], key: str, payload: list[int] | bytes, source: str
) -> bool:
    """Record an observation.  Returns True only if the value DIFFERS from the
    last known value for this key — i.e. a genuine change, not a first sighting.
    """
    history = state.setdefault("value_history", {}).setdefault(key, [])
    pay_list = list(payload)
    rnd = state.get("round", 1)

    if not history:
        history.append(
            {"when": time.time(), "round": rnd, "source": source, "value": pay_list},
        )
        return False  # first observation — not a change

    if history[-1]["value"] == pay_list:
        return False  # same as last known value

    history.append(
        {"when": time.time(), "round": rnd, "source": source, "value": pay_list},
    )
    return True  # genuine value change


def _print_changes(state: dict[str, Any], since_round: int | None = None) -> None:
    """Print all keys whose value has changed, optionally filtered by round."""
    history = state.get("value_history", {})
    changed = {k: v for k, v in history.items() if len(v) > 1}
    if since_round is not None:
        changed = {
            k: v
            for k, v in changed.items()
            if any(e["round"] >= since_round for e in v[1:])
        }
    if not changed:
        return
    print("  *** VALUE CHANGES ***")
    for key in sorted(changed):
        entries = history[key]
        vals = [
            f"[{decode_payload(e['value'])}] r{e['round']}@{e['source']}"
            for e in entries
        ]
        print(f"  {key}: {' → '.join(vals)}")
    print()


# ---------------------------------------------------------------------------
# State persistence
# ---------------------------------------------------------------------------


def _state_path() -> str | None:
    files = sorted(glob.glob("discover_*.json"), key=os.path.getmtime, reverse=True)
    return files[0] if files else None


def _load_state(path: str) -> dict[str, Any]:
    with open(path) as f:
        return json.load(f)


def _save_state(state: dict[str, Any], path: str) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f, indent=2)
    os.replace(tmp, path)


def _new_state(host: str, port: int) -> dict[str, Any]:
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    return {
        "host": host,
        "port": port,
        "started": ts,
        "round": 1,
        "phase": "scanning",
        # scan — compatible with scan_registers.py
        "scanned_at": None,
        "summary": {},
        "all_hits": [],
        "completed_regs": [],
        # retest
        "retest_at": None,
        # listen
        "listen_cycle_started": None,
        "listen_obs": {},
        # cross-round value tracking
        "value_history": {},
    }


def _hit_key(reg: int, slot: int) -> str:
    return f"0x{reg:02X}/0x{slot:02X}"


# ---------------------------------------------------------------------------
# Phase 1 — Active scan
# ---------------------------------------------------------------------------


def run_scan(state: dict[str, Any], host: str, port: int, out_path: str) -> None:
    rnd = state.get("round", 1)
    raw_completed = state.get("completed_regs", [])
    completed = set(int(r, 16) if isinstance(r, str) else r for r in raw_completed)
    remaining = [r for r in REGS if r not in completed]
    total_q = len(list(REGS)) * len(list(SLOTS))
    done_q = len(completed) * len(list(SLOTS))

    print(f"\n{'=' * 60}")
    print(f"ROUND {rnd}  PHASE 1: ACTIVE SCAN")
    print(f"{'=' * 60}")
    if completed:
        print(f"Resuming — {len(completed)}/256 registers already scanned.")
    else:
        print(
            f"Querying {len(list(REGS))} regs × {len(list(SLOTS))} slots "
            f"(~{total_q * RESPONSE_WAIT / 60:.0f} min)",
        )

    _print_changes(state, since_round=rnd)

    scan_start = time.time()
    queries_at_start = len(completed) * len(list(SLOTS))
    s = _reconnect(host, port)
    print("Connected.\n")

    for reg in remaining:
        for slot in SLOTS:
            cmd = make_read_request(reg, slot)
            label = f"0x{reg:02X}/0x{slot:02X}"
            while True:
                try:
                    s.sendall((cmd + "\n").encode())
                    raw = _recv(s)
                    break
                except OSError:
                    print(f"\n  [reconnecting at {label}]")
                    try:
                        s.close()
                    except Exception:
                        pass
                    s = _reconnect(host, port, label)
            for r_reg, r_slot, payload in extract_responses(raw):
                key = _hit_key(r_reg, r_slot)
                qkey = _hit_key(reg, slot)
                pay_hex = [f"0x{b:02X}" for b in payload]
                state["all_hits"].append(
                    {
                        "queried": qkey,
                        "response": key,
                        "payload": pay_hex,
                        "round": rnd,
                    },
                )
                # Always keep summary as the latest direct hit, or first bycatch
                if key == qkey or key not in state["summary"]:
                    state["summary"][key] = pay_hex

                changed = _record_value(state, key, payload, "scan")
                marker = "" if key == qkey else f"  ← queried {qkey}"
                change = "  *** CHANGED ***" if changed and rnd > 1 else ""
                print(f"  {key}  [{decode_payload(payload)}]{marker}{change}")

        completed.add(reg)
        state["completed_regs"] = [f"0x{r:02X}" for r in sorted(completed)]
        done_q = len(completed) * len(list(SLOTS))
        done_session = done_q - queries_at_start
        pct = done_q / total_q * 100
        elapsed = time.time() - scan_start
        if done_session > 0:
            eta = elapsed / done_session * (total_q - done_q)
            print(
                f"  [{done_q}/{total_q} {pct:.0f}%]  ETA {eta / 60:.1f} min", flush=True,
            )
        _save_state(state, out_path)

    s.close()
    state["scanned_at"] = datetime.now().strftime("%Y%m%d_%H%M%S")
    state["phase"] = "retesting"
    _save_state(state, out_path)

    unique = len(state["summary"])
    print(f"\nScan complete — {unique} unique register/slot pairs.")


# ---------------------------------------------------------------------------
# Phase 2 — Retest suspects
# ---------------------------------------------------------------------------


def _find_suspects(hits: list[dict[str, Any]]) -> list[dict[str, Any]]:
    from collections import defaultdict

    direct = defaultdict(list)
    bycatch = defaultdict(list)
    for h in hits:
        if h["queried"] == h["response"]:
            direct[h["response"]].append(h["payload"])
        else:
            bycatch[h["response"]].append(h["queried"])

    suspects = []
    for key in sorted(set(list(direct) + list(bycatch))):
        if direct.get(key):
            continue
        entries = bycatch.get(key, [])
        if not entries:
            continue
        payload_ints = None
        for h in hits:
            if h["response"] == key:
                pi = [int(b, 16) for b in h["payload"]]
                if any(v != 0 for v in pi):
                    payload_ints = pi
                    break
        if payload_ints is None:
            continue

        reg, slot = int(key.split("/")[0], 16), int(key.split("/")[1], 16)
        n = len(entries)
        all_adj = all(
            q.split("/")[0] == key.split("/")[0]
            and int(q.split("/")[1], 16) > int(key.split("/")[1], 16)
            for q in entries
        )
        priority = (
            "ADJACENT"
            if all_adj
            else ("HIGH" if n == 1 else ("MEDIUM" if n <= 3 else "LOW"))
        )
        suspects.append(
            {
                "key": key,
                "reg": reg,
                "slot": slot,
                "priority": priority,
                "obs": n,
                "payload": payload_ints,
            },
        )
    return suspects


def run_retest(state: dict[str, Any], host: str, port: int, out_path: str) -> None:
    rnd = state.get("round", 1)
    print(f"\n{'=' * 60}")
    print(f"ROUND {rnd}  PHASE 2: RETEST SUSPECTS")
    print(f"{'=' * 60}\n")

    # Only retest suspects from this round's scan (avoid re-retesting confirmed ones)
    this_round_hits = [h for h in state["all_hits"] if h.get("round", 1) == rnd]
    suspects = _find_suspects(this_round_hits)
    targets = [s for s in suspects if s["priority"] in ("HIGH", "MEDIUM", "ADJACENT")]

    if not targets:
        print("No suspects to retest.")
        state["retest_at"] = datetime.now().strftime("%Y%m%d_%H%M%S")
        state["phase"] = "listening"
        _save_state(state, out_path)
        return

    print(f"{len(suspects)} suspects  ({len(targets)} to retest)\n")

    sock = _reconnect(host, port)
    sock.settimeout(RETEST_WAIT)
    print("Connected.\n")

    confirmed = 0
    for s in targets:
        reg, slot = s["reg"], s["slot"]
        key = s["key"]
        orig_str = decode_payload(s["payload"])
        direct_seen = []

        for _ in range(RETEST_PASSES):
            cmd = make_read_request(reg, slot)
            sock.sendall((cmd + "\n").encode())
            raw = b""
            deadline = time.time() + RETEST_WAIT
            while time.time() < deadline:
                try:
                    raw += sock.recv(4096)
                except TimeoutError:
                    break
            for r_reg, r_slot, pay in extract_responses(raw):
                if r_reg == reg and r_slot == slot:
                    direct_seen.append(pay)

        if direct_seen:
            confirmed += 1
            pay_hex = [f"0x{b:02X}" for b in direct_seen[0]]
            state["all_hits"].append(
                {
                    "queried": key,
                    "response": key,
                    "payload": pay_hex,
                    "round": rnd,
                },
            )
            state["summary"][key] = pay_hex
            changed = _record_value(state, key, direct_seen[0], "retest")
            match = (
                "✓"
                if direct_seen[0] == s["payload"]
                else f"⚠ now {decode_payload(direct_seen[0])}"
            )
            change = "  *** CHANGED ***" if changed and rnd > 1 else ""
            print(f"  {key}  [{orig_str}]  →  ✅ ×{len(direct_seen)}  {match}{change}")
        else:
            print(f"  {key}  [{orig_str}]  →  ❌ no direct response")

    sock.close()
    state["retest_at"] = datetime.now().strftime("%Y%m%d_%H%M%S")
    state["phase"] = "listening"
    _save_state(state, out_path)
    print(f"\n{confirmed}/{len(targets)} confirmed.")


# ---------------------------------------------------------------------------
# Phase 3 — Passive listen (time-bounded; returns when cycle complete)
# ---------------------------------------------------------------------------


def _conviction(n: int, cv: float) -> float:
    if n < 2:
        return 0.0
    return (1.0 - math.exp(-n / 10.0)) * math.exp(-3.0 * cv)


def _listen_stats(obs: dict[str, Any]) -> tuple[int, float | None, float, float]:
    ts = obs["timestamps"]
    n = len(ts)
    ivs = [ts[i + 1] - ts[i] for i in range(len(ts) - 1)]
    if ivs:
        mean_iv = statistics.mean(ivs)
        cv = (statistics.stdev(ivs) / mean_iv) if len(ivs) > 1 and mean_iv > 0 else 0.0
    else:
        mean_iv, cv = None, float("inf")
    return n, mean_iv, cv, _conviction(n, cv)


def _print_listen_report(
    state: dict[str, Any], cycle_start: float, cycle_end: float
) -> None:
    obs_map = state.get("listen_obs", {})
    now = time.time()
    elapsed = now - cycle_start
    remain = max(0, cycle_end - now)
    total = sum(len(v["timestamps"]) for v in obs_map.values())
    unique = len(obs_map)
    rnd = state.get("round", 1)

    print(f"\n{'=' * 60}")
    print(f"ROUND {rnd}  PHASE 3: LISTEN  {datetime.now().strftime('%H:%M:%S')}")
    print(
        f"{total} obs  ·  {unique} unique regs  "
        f"·  {elapsed / 60:.1f}m elapsed  ·  {remain / 60:.1f}m until next scan",
    )
    print(f"{'=' * 60}")

    # Value changes this session
    _print_changes(state, since_round=rnd)

    if not obs_map:
        print("  Nothing seen yet.\n")
        return

    entries = []
    for key, obs in obs_map.items():
        n, mean_iv, cv, conv = _listen_stats(obs)
        entries.append((key, obs, n, mean_iv, cv, conv))
    entries.sort(key=lambda x: (-x[5], -x[2]))

    multi = [e for e in entries if e[2] > 1]
    singles = [e[0] for e in entries if e[2] == 1]

    if multi:
        print(
            f"\n{'Register':<14} {'N':>5}  {'Period':>8}  {'CV':>6}  {'Conv%':>6}  Value",
        )
        for key, obs, n, mean_iv, cv, conv in multi:
            period = _fmt_iv(mean_iv) if mean_iv else "?"
            cv_str = f"{cv:.2f}" if cv != float("inf") else "—"
            pay = decode_payload(obs["payloads"][-1])
            hist = state.get("value_history", {}).get(key, [])
            flag = "  ***" if len(hist) > 1 else ""
            print(
                f"{key:<14} {n:>5}  {period:>8}  {cv_str:>6}  {conv * 100:>5.0f}%  {pay}{flag}",
            )

    if singles:
        print(f"\n  Seen once: {', '.join(sorted(singles))}")

    # Causal analysis
    all_hits = state.get("all_hits", [])
    if all_hits and elapsed > 30:
        scan_bycatch = {
            h["response"] for h in all_hits if h["queried"] != h["response"]
        }
        not_seen = scan_bycatch - set(obs_map.keys())
        if not_seen:
            c30 = 1.0 - math.exp(-elapsed / 30)
            c300 = 1.0 - math.exp(-elapsed / 300)
            print(
                f"\n  Scan bycatch silent here: {len(not_seen)} regs  "
                f"(conf NOT periodic: ≤30s={c30 * 100:.0f}%  ≤5min={c300 * 100:.0f}%)",
            )
            if c300 > 0.90:
                print("  → Almost certainly query-triggered, not periodic broadcasts.")
    print()


def run_listen(state: dict[str, Any], host: str, port: int, out_path: str) -> None:
    rnd = state.get("round", 1)
    print(f"\n{'=' * 60}")
    print(f"ROUND {rnd}  PHASE 3: PASSIVE LISTEN  ({LISTEN_DURATION // 60} min)")
    print(f"{'=' * 60}")
    print("Listening without queries; will loop back to scan when done.")
    print("Ctrl-C saves and exits cleanly.\n")

    if "listen_obs" not in state:
        state["listen_obs"] = {}
    if state.get("listen_cycle_started") is None:
        state["listen_cycle_started"] = time.time()
        _save_state(state, out_path)

    cycle_start = state["listen_cycle_started"]
    cycle_end = cycle_start + LISTEN_DURATION
    remaining = cycle_end - time.time()
    if remaining < 0:
        print("Listen cycle already complete; proceeding to next scan.")
        return

    if time.time() - cycle_start > 10:
        print(
            f"Resuming — {(time.time() - cycle_start) / 60:.1f}m of {LISTEN_DURATION // 60}m elapsed.",
        )

    last_report = 0.0
    last_save = 0.0

    while time.time() < cycle_end:
        try:
            print(f"Connecting to {host}:{port} …", flush=True)
            sock = socket.create_connection((host, port), timeout=10)
            sock.settimeout(2.0)
            buf = ""
            print("Connected.\n")

            while time.time() < cycle_end:
                now = time.time()
                if now - last_report >= LISTEN_REPORT:
                    _print_listen_report(state, cycle_start, cycle_end)
                    last_report = now
                if now - last_save >= SAVE_INTERVAL:
                    _save_state(state, out_path)
                    last_save = now

                try:
                    chunk = sock.recv(4096).decode("ascii", errors="replace")
                    if not chunk:
                        raise ConnectionResetError("server closed connection")
                    buf += chunk
                except TimeoutError:
                    continue

                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    msg = parse_hex_line(line.strip())
                    if not is_register_response(msg):
                        continue
                    r_reg, r_slot, payload = msg[10], msg[11], msg[12:-2]
                    key = _hit_key(r_reg, r_slot)
                    if key not in state["listen_obs"]:
                        state["listen_obs"][key] = {"timestamps": [], "payloads": []}
                    state["listen_obs"][key]["timestamps"].append(time.time())
                    state["listen_obs"][key]["payloads"].append(payload)
                    changed = _record_value(state, key, payload, f"listen-r{rnd}")
                    if changed and rnd > 1:
                        print(f"  *** {key} changed → [{decode_payload(payload)}]")

            sock.close()

        except KeyboardInterrupt:
            print("\nInterrupted — saving …")
            _save_state(state, out_path)
            _print_listen_report(state, cycle_start, cycle_end)
            print(f"Session saved to {out_path}")
            print("Run again to resume.")
            sys.exit(0)

        except Exception as e:
            print(f"Connection error: {e}  — retrying in 5s …")
            time.sleep(5)

    _save_state(state, out_path)
    _print_listen_report(state, cycle_start, cycle_end)
    print("Listen cycle complete.  Looping back to scan …\n")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main() -> None:
    args = sys.argv[1:]
    host = None
    port = DEFAULT_PORT
    fresh = "--fresh" in args
    args = [a for a in args if a != "--fresh"]

    for a in args:
        if not a.startswith("-"):
            if host is None:
                host = a
            else:
                port = int(a)

    existing = None if fresh else _state_path()

    if existing:
        state = _load_state(existing)
        out_path = existing
        rnd = state.get("round", 1)
        phase = state.get("phase", "scanning")
        print(f"Resuming {existing}  (round {rnd}, phase={phase})")
        if host is None:
            host = state.get("host", DEFAULT_HOST)
        port = state.get("port", port)
    else:
        if host is None:
            scans = sorted(glob.glob("register_scan_*.json"), key=os.path.getmtime)
            if scans:
                try:
                    with open(scans[-1]) as f:
                        d = json.load(f)
                    host = d.get("host", DEFAULT_HOST)
                    port = int(d.get("port", port))
                    print(f"Using host {host}:{port} from {scans[-1]}")
                except Exception:
                    host = DEFAULT_HOST
            else:
                host = DEFAULT_HOST
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        out_path = f"discover_{ts}.json"
        state = _new_state(host, port)
        _save_state(state, out_path)
        print(f"New session → {out_path}")

    print(f"Host: {host}:{port}")
    print(
        f"Cycle: ~{len(list(REGS)) * len(list(SLOTS)) * RESPONSE_WAIT / 60:.0f}m scan  "
        f"+ retest  +  {LISTEN_DURATION // 60}m listen  =  "
        f"~{(len(list(REGS)) * len(list(SLOTS)) * RESPONSE_WAIT + LISTEN_DURATION) / 60:.0f}m per round\n",
    )

    while True:
        phase = state.get("phase", "scanning")

        if phase == "scanning":
            run_scan(state, host, port, out_path)

        if state.get("phase") == "retesting":
            run_retest(state, host, port, out_path)

        if state.get("phase") == "listening":
            run_listen(state, host, port, out_path)

        # Listen cycle complete — advance to next round
        rnd = state.get("round", 1) + 1
        print(f"\n{'=' * 60}")
        print(f"Starting round {rnd}")
        print(f"{'=' * 60}\n")
        state["round"] = rnd
        state["phase"] = "scanning"
        state["completed_regs"] = []
        state["listen_cycle_started"] = None
        # listen_obs accumulates across rounds — timestamps are absolute so
        # interval stats improve with every cycle. Do NOT reset it here.
        # all_hits, summary, value_history also accumulate.
        _save_state(state, out_path)


if __name__ == "__main__":
    main()
