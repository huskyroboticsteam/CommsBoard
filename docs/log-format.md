# Log file format

The Comms Board writes its logs as **JSON Lines**: plain text, one complete JSON object
per line. Any line can be read on its own with `json.loads()`.

`tools/logread.py` reads these files. This page is the reference for anyone writing
another reader.

## Files

- One file per boot and part: `/LOG/B00042_01.jsonl` is boot 42, part 1.
- The **boot ID** goes up by one on every boot. It is 1 + the larger of the value kept in
  an RTC backup register and the highest boot ID already on the card.
- When a part fills up, logging carries on in the next part (`_02`, `_03`, ... up to
  `_99`). The sequence number `n` carries on too.
- Each part is **preallocated** at its full size when it is opened (1 GB on the real
  board, 16 MB on the Nucleo). So the file's size says nothing about how much data it
  holds. Everything after the valid data is unused space: zeros on a new card, or
  leftovers from an older file.
- Old files are never deleted automatically.

## Blocks

The file is written in 512-byte blocks, and **a line never straddles two blocks**. If
the next line does not fit in the current block, the rest of the block is filled with
spaces and ends with a newline. Readers skip blank lines.

This is what makes a power cut safe: it can only damage the block being written.

## Where the valid data ends

Read line by line and stop at the first line that:

- does not parse as JSON, or
- has a different boot ID `b` from the header, or
- has a sequence number `n` that is not the previous `n` + 1.

Everything before that point is valid.

## The header line

The first block holds one header line, padded to 512 bytes:

```json
{"k":"hdr","v":1,"fw":"a1b2c3d","board":"NUCLEO","b":42,"reset":"POR","rtc":1762909680,"rtc_ok":true,"bitrate":1000000}
```

| Key | Meaning |
|---|---|
| `v` | Format version (1) |
| `fw` | Firmware git hash; ends in `-dirty` if built with uncommitted changes |
| `board` | `NUCLEO` or `COMMS_V1` |
| `b` | Boot ID |
| `reset` | Why the MCU last reset: `POR`, `PIN`, `IWDG` (watchdog), `SW`, `WWDG`, `LPWR`, `OBL` |
| `rtc` | RTC time when this boot started logging, in Unix seconds (0 if not valid) |
| `rtc_ok` | Whether the RTC time is valid |
| `bitrate` | CAN bit rate |
| `loopback` | Present and `true` only when CAN ran in internal loopback. Every frame the board sends then also appears as a received frame. |

## Record lines

Every other line starts with the same four keys:

| Key | Meaning |
|---|---|
| `b` | Boot ID |
| `n` | Sequence number: 0, 1, 2, ... for the whole boot |
| `t` | Microseconds since boot (64-bit, never wraps) |
| `k` | Kind of record |

```json
{"b":42,"n":0,"t":5123,"k":"time","rtc":1762909680}
{"b":42,"n":1,"t":5217,"k":"rx","id":"0x408","d":"02610000000001"}
{"b":42,"n":2,"t":5340,"k":"tx","id":"0x10F","d":"01"}
```

| `k` | Written when | Extra keys |
|---|---|---|
| `rx` | A frame is received | `id`: 11-bit CAN ID as hex. `d`: the data bytes as hex; its length gives the DLC. `r`: 1 if it is a remote frame (then `d` is empty). |
| `tx` | A frame this board sent was acknowledged on the bus | Same as `rx` |
| `time` | Once a second | `rtc`: Unix seconds, or 0 if the RTC is not set |
| `err` | The CAN error state changes | `tec`, `rec`: error counters. `lec`: last error code. `st`: `ok`, `warn`, `passive` or `busoff`. |
| `drop` | Space returns after an overflow | `cnt`: records lost. A lower bound: when the CAN hardware's own FIFO overflows, it does not say how many frames went. |
| `sys` | A logger or system event | `ev`: see below |
| `tset` | The RTC is set | `old`, `new`: Unix seconds |

`sys` events: `start` (logging began on a new part), `recover` (logging resumed after a
card fault), `busoff_restart` (the CAN controller was restarted after bus-off). Reserved
for later: `card_in`, `card_out`, `powerfail`, `fault`.

## Things a reader should know

- `t` of frames never goes backwards, because frames are stamped in the order they
  arrive. A `sys` line is stamped when the logger writes it, so it can carry a later
  time than the frame lines right after it.
- All frames collected by one receive interrupt share one timestamp.
- After a card fault, the lines that were waiting in RAM are lost. The next part starts
  with a `recover` line, and `n` shows the gap.
