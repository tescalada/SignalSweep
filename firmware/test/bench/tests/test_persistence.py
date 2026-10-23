"""Persistence: settings sent over Bluetooth survive a reboot. Opening the USB
port reboots the C5, and the board's CMD:CFG over USB after boot must carry
the values."""

import asyncio

import pytest

from signalsweep_bench import SETTING_KEYS, UsbLink, next_value

# Changed to next_value() of the board's current ones. The buzzer stays muted:
# the suite keeps it off, and a mute that persists is part of what is checked.
CHANGES = ("led", "bri", "theme", "vol", "band", "beep_mask", "scan_all", "attack")


@pytest.mark.reboots
async def test_settings_survive_reboot(port, board):
    before = await board.cfg()
    keys = [k for k in CHANGES if k in before]
    # led before bri (SETTING_KEYS order): on the brightness branch a legacy
    # led value moves bri.
    for key in SETTING_KEYS:
        if key in keys:
            await board.send({key: next_value(key, before[key])})
    set_cfg = await board.cfg()
    want = {k: set_cfg[k] for k in SETTING_KEYS if k in set_cfg}
    unchanged = [k for k in keys if want.get(k) == before[k]]
    assert not unchanged, "Did not change before the reboot: " + ", ".join(
        f"{k} (still {before[k]!r})" for k in unchanged
    )

    usb = UsbLink(port)
    try:
        usb.open()   # reboots the C5
        boot = await asyncio.to_thread(usb.wait_for_boot, max_uptime=30)
        assert boot.get("uptime", 99) < 30, (
            f"uptime {boot.get('uptime')!r} s over USB: the board did not reboot when the port opened"
        )
        wrong = {k: (v, boot.get(k)) for k, v in want.items() if boot.get(k) != v}
        assert not wrong, "Not kept across the reboot (field: set, after boot): " + ", ".join(
            f"{k}: {w!r}, {g!r}" for k, (w, g) in wrong.items()
        )

        # Put back what this test changed, over the link that is up.
        for key in SETTING_KEYS:
            if key in keys:
                usb.send({key: before[key]})
        back = await asyncio.to_thread(usb.cfg)
        assert back is not None, "no CMD:CFG reply over USB after restoring"
        not_back = {k: (before[k], back.get(k)) for k in keys if back.get(k) != before[k]}
        assert not not_back, "Not restored over USB (field: wanted, reported): " + ", ".join(
            f"{k}: {w!r}, {g!r}" for k, (w, g) in not_back.items()
        )
    finally:
        usb.close()
