"""Legacy LED values: {"led":0..3} reads back as sent; on the brightness
branch the old Dim (2) also sets bri Low and the old Full (3) sets bri High."""

import pytest


async def restore_led(board, before):
    await board.send({"led": before["led"]})
    if "bri" in before:
        await board.send({"bri": before["bri"]})


async def test_legacy_led_values(board):
    before = await board.cfg()
    try:
        for v in range(4):
            cfg = await board.set("led", v)
            assert cfg.get("led") == v, f"led: sent {v!r}, CMD:CFG reports {cfg.get('led')!r}"
    finally:
        await restore_led(board, before)


async def test_legacy_led_sets_brightness(board):
    before = await board.cfg()
    if "bri" not in before:
        pytest.skip("this board's CMD:CFG has no 'bri' (not the brightness branch)")
    try:
        # From Med, so each legacy value has to move bri to pass.
        for led, want_bri in ((2, 0), (3, 2)):
            await board.set("bri", 1)
            cfg = await board.set("led", led)
            assert cfg.get("led") == led, f"led: sent {led!r}, CMD:CFG reports {cfg.get('led')!r}"
            assert cfg.get("bri") == want_bri, (
                f"bri: after led {led}, wanted {want_bri!r}, CMD:CFG reports {cfg.get('bri')!r}"
            )
    finally:
        await restore_led(board, before)
