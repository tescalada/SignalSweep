"""Setting round trip: each setting sent over Bluetooth comes back in CMD:CFG
and, where the push carries the field, in the next push."""

import pytest

from signalsweep_bench import PUSH_ONLY_WHEN_TRUE, next_value, push_value


@pytest.mark.parametrize(
    "key", ["buzzer", "led", "theme", "vol", "beep_mask", "scan_all", "attack", "bri", "band"]
)
async def test_round_trip(board, key):
    before = await board.cfg()
    if key not in before:
        pytest.skip(f"this board's CMD:CFG has no {key!r}")
    cur = before[key]
    want = next_value(key, cur)
    try:
        cfg = await board.set(key, want)
        assert cfg.get(key) == want, f"{key}: sent {want!r}, CMD:CFG reports {cfg.get(key)!r}"

        # Every push carries these fields (bri and band whenever CMD:CFG
        # does); only scan_all and attack are absent when false.
        push = await board.fresh_push()
        if key not in PUSH_ONLY_WHEN_TRUE:
            assert key in push, f"{key}: missing from the push"
        got = push_value(push, key)
        assert got == want, f"{key}: sent {want!r}, the push reports {got!r}"
    finally:
        # Put it back, and for the buzzer that means muted again. Not on a
        # dropped link: that error would replace the real failure.
        if board.connected:
            await board.send({key: cur})
            # The led command on the brightness branch also moves bri.
            if key == "led" and "bri" in before:
                await board.send({"bri": before["bri"]})
    after = await board.cfg()
    assert after.get(key) == cur, f"{key}: restored {cur!r}, CMD:CFG reports {after.get(key)!r}"
