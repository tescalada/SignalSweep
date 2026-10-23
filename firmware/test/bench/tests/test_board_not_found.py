"""Board not found: a scan that finds no matching Nordic UART Service
advertiser ends in one error naming the likely causes. Needs the Mac's
Bluetooth, not the board: it scans for a name no board has."""

import pytest

from signalsweep_bench import BleLink, BoardNotFound


async def test_board_not_found_names_causes():
    link = BleLink()
    with pytest.raises(BoardNotFound) as exc:
        await link.connect(name="no-such-board-", timeout=3.0)
    msg = str(exc.value)
    for cause in ("phone app is still connected", "receive-only", "out of range", "Bluetooth permission"):
        assert cause in msg, f"not-found message does not mention {cause!r}: {msg}"
    assert not link.connected
