"""Burst: 50 setting commands written back to back over Bluetooth. Short
commands run on the NimBLE host task and each answers with a CMD:CFG reply, so
this loads the host stack with writes and notifications at once."""

import asyncio

BURST = 50


async def test_burst(board):
    before = await board.cfg()
    for field in ("uptime", "reset", "vol"):
        assert field in before, f"CMD:CFG has no {field!r}; the burst checks need it"
    drops_before = board.disconnects
    start_vol = before["vol"]
    values = [(start_vol + 1 + i) % 3 for i in range(BURST)]
    try:
        for v in values:
            await board.send({"vol": v}, response=False)
        # Let the replies drain, and let uptime (whole seconds) move on.
        await asyncio.sleep(1.5)
        assert board.connected and board.disconnects == drops_before, (
            "the Bluetooth link dropped during the burst"
        )

        after = await board.cfg()
        assert after.get("vol") == values[-1], (
            f"vol: last sent {values[-1]!r}, CMD:CFG reports {after.get('vol')!r}"
        )
        assert after.get("reset") == before.get("reset"), (
            f"reset: was {before.get('reset')!r}, now {after.get('reset')!r} (the board rebooted)"
        )
        assert after.get("uptime", -1) > before.get("uptime", 0), (
            f"uptime: was {before.get('uptime')!r} s, now {after.get('uptime')!r} s (the board rebooted)"
        )
    finally:
        if board.connected:
            await board.send({"vol": start_vol})
