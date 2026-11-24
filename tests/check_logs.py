#!/usr/bin/env python3
"""Check the sample logs written by test_services with Python's json module and logread.py."""
import json
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "tools"))
import logread  # noqa: E402


def read_all(path):
    gen = logread.read_log(path)
    recs = []
    while True:
        try:
            recs.append(next(gen)[1])
        except StopIteration as stop:
            return recs, stop.value


# Every line the formatter wrote must parse on its own, in every block.
with open(os.path.join(here, "out", "clean.jsonl"), "rb") as f:
    data = f.read()
assert len(data) % 512 == 0, "file is not a whole number of 512-byte blocks"
lines = 0
for i in range(0, len(data), 512):
    block = data[i:i + 512]
    assert block.endswith(b"\n"), f"block at {i} does not end with a newline"
    for line in block.split(b"\n"):
        if line.strip():
            json.loads(line)
            lines += 1
assert lines == 5001, lines   # header + 5000 records

recs, reason = read_all(os.path.join(here, "out", "clean.jsonl"))
assert len(recs) == 5001 and reason == "end of file", (len(recs), reason)
assert recs[0]["k"] == "hdr" and recs[0]["b"] == 42
assert [r["n"] for r in recs[1:]] == list(range(5000))
assert sum(r["cnt"] for r in recs if r["k"] == "drop") == 3 * 52   # n = 50, 147, ... 4997

# A power cut: reading stops at the half-written line and ignores the older boot's data.
recs, reason = read_all(os.path.join(here, "out", "torn.jsonl"))
assert len(recs) == 3001, len(recs)
assert "not valid JSON" in reason, reason

out = subprocess.run([sys.executable, os.path.join(here, "..", "tools", "logread.py"),
                      os.path.join(here, "out", "torn.jsonl"), "--csv", os.path.join(here, "out", "torn.csv")],
                     capture_output=True, text=True, check=True).stdout
assert "lines     : 3000 (n = 0 to 2999)" in out, out
with open(os.path.join(here, "out", "torn.csv")) as f:
    rows = f.read().splitlines()
assert rows[0] == "t_us,kind,id,dlc,data,rtr" and len(rows) == 1 + 2304 + 572   # header row + rx + tx
print("log files    ok  (json + logread.py)")

# ---- Files written by the real logger onto the simulated card (test_logger) ----
card = os.path.join(here, "out", "card")


def part(name):
    recs, reason = read_all(os.path.join(card, name))
    assert recs and recs[0]["k"] == "hdr", (name, reason)
    # The unused, preallocated tail is zeros on a blank card (skipped) or old data (stops the read).
    assert reason == "end of file" or "not valid JSON" in reason, (name, reason)
    check_frame_times(name, recs[1:])
    return recs[0], recs[1:]


def check_frame_times(name, recs):
    """Frames are stamped in the order they enter the ring, so their times never go backwards."""
    frames = [r for r in recs if r["k"] in ("rx", "tx")]
    for a, b in zip(frames, frames[1:]):
        assert a["t"] <= b["t"], f"{name}: time jumps back from {a['t']} to {b['t']} at n={b['n']}"


def frame_counters(recs):
    """The test puts a running counter in the first 4 data bytes of every frame it feeds."""
    return [int.from_bytes(bytes.fromhex(r["d"][:8]), "little") for r in recs if r["k"] in ("rx", "tx") and len(r["d"]) >= 8]


# Boot 1: one part, every record there once and in order.
hdr, recs = part("B00001_01.jsonl")
assert hdr["b"] == 1 and hdr["board"] == "NUCLEO" and hdr["reset"] == "POR" and hdr["rtc_ok"] is False and hdr["v"] == 1
assert hdr["loopback"] is True      # the Nucleo build runs CAN in loopback, and the header says so
assert len(recs) == 20001 and recs[0]["k"] == "sys" and recs[0]["ev"] == "start"
assert [r["n"] for r in recs] == list(range(20001))
# dlc cycles 0..8; only frames with 4+ data bytes carry the whole counter
assert frame_counters(recs) == [i for i in range(20000) if i % 9 >= 4]

# Boot 2: boot ID taken from the card.
hdr, recs = part("B00002_01.jsonl")
assert hdr["b"] == 2 and len(recs) == 1001

# Boot 3: rollover. The second part carries on exactly where the first stopped.
hdr1, p1 = part("B00003_01.jsonl")
hdr2, p2 = part("B00003_02.jsonl")
assert hdr1["b"] == hdr2["b"] == 3
assert len(p1) + len(p2) == 260002, (len(p1), len(p2))
assert p2[0]["n"] == p1[-1]["n"] + 1, "sequence must continue across parts"
assert p2[0]["k"] != "sys" or p2[0]["ev"] == "start"
assert frame_counters(p1 + p2) == [i for i in range(260000) if i % 9 >= 4]
size = os.path.getsize(os.path.join(card, "B00003_01.jsonl"))
assert size == 16 * 1024 * 1024, size

# Boot 4: a card fault in the middle. Part 1 ends cleanly; part 2 resumes later with a
# gap no bigger than what was staged in RAM (4 KB of lines) plus the write that failed.
hdr1, p1 = part("B00004_01.jsonl")
hdr2, p2 = part("B00004_02.jsonl")
# This boot started with 5 records too old to timestamp: thrown away, and said so first thing.
assert p1[0]["k"] == "sys" and p1[0]["ev"] == "start", p1[0]
assert p1[1]["k"] == "drop" and p1[1]["cnt"] == 5, p1[1]
assert not any(r["k"] == "time" for r in p1[:3]), "a stale record was written"
assert p1[0]["t"] > 2 ** 32, p1[0]   # two hours in: times past 32 bits print correctly
gap = p2[0]["n"] - p1[-1]["n"] - 1
assert p2[0]["k"] == "sys" and p2[0]["ev"] == "recover", p2[0]
assert 0 < gap <= 2 * (4096 // 40), gap
c1, c2 = frame_counters(p1), frame_counters(p2)
assert c1 == sorted(c1) and c2 == sorted(c2) and c1[-1] < c2[0]
assert c2[-1] == max(i for i in range(60000) if i % 9 >= 4), "the last record fed must be on the card"
print(f"card files   ok  (6 parts; fault lost {gap} lines)")
