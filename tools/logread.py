#!/usr/bin/env python3
"""logread.py - read a Comms Board .jsonl log file and report what is in it.

The file is preallocated, so only the start holds real data. Valid data ends at the
first line that does not parse, belongs to a different boot (b), or breaks the
sequence (n is not the previous n + 1). Blank padding lines are skipped.

    python3 logread.py B00042_01.jsonl              # summary
    python3 logread.py B00042_01.jsonl --csv out.csv  # also export the CAN frames
"""
import argparse
import csv
import json
import sys

BLOCK = 512


def read_log(path):
    """Yield (offset, record) for every valid line, then return the reason reading stopped."""
    header = None
    boot = None
    expect_n = None     # a later part of the same boot carries on from the previous part's n
    with open(path, "rb") as f:
        block_start = 0
        while True:
            block = f.read(BLOCK)       # every block holds only whole lines
            if not block:
                return "end of file"
            pos = 0
            for raw in block.split(b"\n"):
                line = raw.strip(b" \r\x00")
                line_offset = block_start + pos
                pos += len(raw) + 1
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    return f"line at byte {line_offset} is not valid JSON"
                if not isinstance(rec, dict):
                    return f"line at byte {line_offset} is not a JSON object"
                if header is None:
                    if rec.get("k") != "hdr":
                        return "file does not start with a header line"
                    header, boot = rec, rec.get("b")
                    yield line_offset, rec
                    continue
                if rec.get("b") != boot:
                    return f"line at byte {line_offset} is from another boot (b={rec.get('b')})"
                if expect_n is not None and rec.get("n") != expect_n:
                    return f"sequence gap at byte {line_offset}: expected n={expect_n}, found n={rec.get('n')}"
                if not isinstance(rec.get("n"), int):
                    return f"line at byte {line_offset} has no sequence number"
                expect_n = rec["n"] + 1
                yield line_offset, rec
            block_start += len(block)


def main():
    ap = argparse.ArgumentParser(description="Read a Comms Board .jsonl log file.")
    ap.add_argument("file")
    ap.add_argument("--csv", metavar="OUT", help="export rx/tx frames to a CSV file")
    args = ap.parse_args()

    kinds = {}
    first_n = last_n = None
    dropped = 0
    header = None
    first_t = last_t = None
    end_offset = 0
    frames = []

    gen = read_log(args.file)
    while True:
        try:
            offset, rec = next(gen)
        except StopIteration as stop:
            reason = stop.value
            break
        end_offset = offset
        k = rec.get("k")
        if k == "hdr":
            header = rec
            continue
        kinds[k] = kinds.get(k, 0) + 1
        if first_t is None:
            first_t, first_n = rec["t"], rec["n"]
        last_t, last_n = rec["t"], rec["n"]
        if k == "drop":
            dropped += rec.get("cnt", 0)
        if args.csv and k in ("rx", "tx"):
            d = rec.get("d", "")
            frames.append((rec["t"], k, rec["id"], len(d) // 2, d, rec.get("r", 0)))

    if header is None:
        print(f"{args.file}: no valid header ({reason})")
        return 1

    lines = sum(kinds.values())
    print(f"file      : {args.file}")
    print(f"header    : boot {header.get('b')}, fw {header.get('fw')}, board {header.get('board')}, "
          f"reset {header.get('reset')}, rtc {header.get('rtc')} (valid: {header.get('rtc_ok')}), "
          f"bitrate {header.get('bitrate')}")
    print(f"lines     : {lines}" + (f" (n = {first_n} to {last_n})" if lines else ""))
    for k in sorted(kinds):
        print(f"  {k:<5} {kinds[k]}")
    if lines:
        span = (last_t - first_t) / 1e6
        print(f"time span : {span:.3f} s (t = {first_t} to {last_t} us)")
        frame_count = kinds.get("rx", 0) + kinds.get("tx", 0)
        if span > 0:
            print(f"frame rate: {frame_count / span:.0f} frames/s")
    print(f"dropped   : {dropped} frames")
    print(f"data ends : last valid line starts at byte {end_offset} ({reason})")

    if args.csv:
        with open(args.csv, "w", newline="") as out:
            w = csv.writer(out)
            w.writerow(["t_us", "kind", "id", "dlc", "data", "rtr"])
            w.writerows(frames)
        print(f"csv       : {len(frames)} frames written to {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
