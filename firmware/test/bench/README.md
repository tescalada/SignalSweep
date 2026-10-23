# Bench harness

A pytest suite run from a Mac against a live board on USB and in Bluetooth
range. It checks only what the board reports about itself: `CMD:CFG` replies
and the 1 Hz push.

It needs hardware, so it is not part of CI.

## Setup

1. Install [uv](https://docs.astral.sh/uv/). The suite is its own uv project;
   `uv run` creates `.venv` here and installs bleak, pyserial, pytest and
   pytest-asyncio from the `dev` dependency group on first use.
2. Give your terminal app Bluetooth permission: System Settings > Privacy &
   Security > Bluetooth. Without it macOS kills the Python process the moment
   bleak touches CoreBluetooth (`Fatal Python error: Aborted`, no other
   message).
3. Disconnect the phone app, or close it. The board takes one Bluetooth
   central at a time, so the suite cannot connect while the phone is.
4. Make sure the board is not in receive-only mode (it does not advertise).

## Run

```bash
uv run --directory firmware/test/bench pytest --port=/dev/cu.usbmodem101
```

Write `--port=PATH`, not `--port PATH`: with a space, pytest takes the path as
a test location before it has loaded the `--port` option, sets its root
directory to `/dev`, never finds this project's settings, and rejects `--port`.

Options:

- `--port` — the board's USB serial port. Without it the USB test is skipped,
  not failed. **Opening the port reboots a C5**; the suite treats it as a
  reboot and opens it only in the persistence test, which runs last.
- `--soak-seconds` — length of the push soak (default 60).
- `--name` — connect only to a board whose Bluetooth name starts with this.
  Default: the first device advertising the Nordic UART Service.

Add `-s` to see the soak statistics as they print, `-rA` for a per-test summary.

`uv run --directory firmware/test/bench pytest --collect-only` lists the tests
without a board.

## What the run does to the board

At the start it reads `CMD:CFG` and saves every setting it may touch (`led`,
`bri`, `theme`, `vol`, `band`, `beep_mask`, `scan_all`, `attack`, `buzzer`),
then mutes the buzzer so theme and volume previews stay quiet. At the end,
pass or fail, it reconnects if a reboot dropped the link, sends every saved
setting back (`led` before `bri`, `buzzer` last), sends anything that did
not land once more, and fails the run if `CMD:CFG` then reports anything
different, or if reconnecting or restoring fails (settings may then not be
restored).

It never sends the identity (`ble_name`, `rand_mac`), `rx_only`, signatures, a
factory reset, `hunt`, `ring`, or the SD and alert-log readers.

## Tests

| Module | Checks | Cannot tell you |
|---|---|---|
| `test_round_trip.py` | Each of `buzzer`, `led`, `theme`, `vol`, `beep_mask`, `scan_all`, `attack`, `bri`, `band` sent over Bluetooth comes back in `CMD:CFG`, and in the push where the push carries it (`scan_all` and `attack` ride the push only when true). Each test puts its field back. `bri` and `band` skip on a board whose `CMD:CFG` lacks them. The buzzer test is the one place the buzzer is unmuted. | — |
| `test_legacy_led.py` | `{"led":0..3}` reads back as sent. On the brightness branch, `led 2` gives `bri 0` and `led 3` gives `bri 2`; that half skips without `bri`. | — |
| `test_push_soak.py` | Idle for `--soak-seconds`: push count within 2 of the duration, longest gap under 2 s, no torn lines (a line that does not parse as JSON after reassembly on `\n`, which is what a notification lost mid-line leaves; a lost notification holding only whole lines leaves no torn line and only lowers the push count, inside the +/- 2). Gaps include soak start to first push and last push to soak end. Prints count, sizes and gaps. Skips below 3 s. | Push behavior with many devices nearby; it soaks whatever the room holds. |
| `test_burst.py` | 50 `vol` commands written back to back without response. The link stays up, `CMD:CFG` afterwards has the last value, `reset` is unchanged and `uptime` went up (no reboot). | Which of the 50 config replies were dropped; only the final state is checked. |
| `test_persistence.py` | Changes every setting over Bluetooth except the buzzer (which stays muted) and fails naming any that did not change, opens USB (reboots the C5), then `CMD:CFG` over USB has the values and the buzzer still muted, with `uptime` < 30 s (replies from before the reset are ignored). Restores them over USB. Needs `--port`. | Persistence across a real power cut, as opposed to a reset. |
| `test_board_not_found.py` | A scan that finds no matching advertiser ends in one error naming the likely causes. Scans for a name no board has, so it needs the Mac's Bluetooth but not the board. | — |

If the board is not found within 15 s, or connecting to it fails (Bluetooth
off, connect timeout, subscribing to TX fails), the first test that needs it
fails with that message, plus the connect error if there was one, and the rest
are skipped.

## Transports

- Bluetooth: Nordic UART Service. Commands are `\n`-terminated JSON (or a raw
  `CMD:` line) written to RX `6e400002-b5a3-f393-e0a9-e50e24dcca9e`, in
  180-byte writes like the app. Replies and pushes are reassembled on `\n`
  from TX `6e400003-b5a3-f393-e0a9-e50e24dcca9e` notifications. `CMD:CFG` is
  re-asked up to three times, as the app does, because a single reply can be
  dropped.
- USB: the port is opened with DTR and RTS set low before open, and kept open
  for the rest of the test. If the reset re-enumerates the port, the harness
  closes it, waits until the port path exists again plus 1 s, and reopens it,
  at most 3 times (each reopen resets a C5 again), until `CMD:CFG` answers
  (30 s limit).
