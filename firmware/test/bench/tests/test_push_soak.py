"""Push soak: an idle board pushes once a second over Bluetooth, every push
arrives whole, and none is late by more than a beat."""

import asyncio
import time

import pytest


async def test_push_soak(board, soak_seconds, capsys):
    if soak_seconds < 3:
        pytest.skip(f"--soak-seconds {soak_seconds:g} is too short to measure gaps; use 3 or more")
    torn_before = len(board.torn)
    drops_before = board.disconnects
    board.drain_pushes()

    pushes = []   # (arrival time, size in bytes)
    start = time.monotonic()
    end = start + soak_seconds
    while (left := end - time.monotonic()) > 0:
        try:
            t, size, _ = await board.next_push(timeout=left)
        except asyncio.TimeoutError:
            break
        pushes.append((t, size))

    torn = board.torn[torn_before:]
    # Including soak start to first push and last push to soak end.
    edges = [start] + [t for t, _ in pushes] + [end]
    gaps = [b - a for a, b in zip(edges, edges[1:])]
    sizes = [s for _, s in pushes]
    with capsys.disabled():
        print()
        print(f"  push soak: {len(pushes)} pushes in {soak_seconds:g} s, {len(torn)} torn lines")
        if sizes:
            print(f"  size (B): min {min(sizes)}  mean {sum(sizes) / len(sizes):.0f}  max {max(sizes)}")
        if gaps:
            print(f"  gap (s):  min {min(gaps):.2f}  mean {sum(gaps) / len(gaps):.2f}  max {max(gaps):.2f}")

    assert board.disconnects == drops_before, "the Bluetooth link dropped during the soak"
    assert not torn, f"{len(torn)} torn lines, first: {torn[0][1][:120]!r}"
    assert abs(len(pushes) - soak_seconds) <= 2, (
        f"{len(pushes)} pushes in {soak_seconds:g} s, wanted {soak_seconds:g} +/- 2"
    )
    assert max(gaps) < 2.0, f"max gap between pushes {max(gaps):.2f} s, wanted < 2 s"
