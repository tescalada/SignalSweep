"""Session fixtures for the bench suite.

One Bluetooth link for the whole run. At the start the suite saves every
setting it may touch and mutes the buzzer; at the end, pass or fail, it puts
every saved setting back and checks the board reports them.
"""

import asyncio

import pytest
from bleak.exc import BleakError

from signalsweep_bench import SETTING_KEYS, BleLink, BoardNotFound, not_found_message

SCAN_TIMEOUT_S = 15.0


def pytest_addoption(parser):
    g = parser.getgroup("signalsweep bench")
    g.addoption(
        "--port",
        default=None,
        help="USB serial port of the board, e.g. /dev/cu.usbmodem101. "
        "Tests that need USB are skipped without it. Opening it reboots a C5.",
    )
    g.addoption(
        "--soak-seconds",
        type=float,
        default=60.0,
        help="length of the push soak in seconds (default 60)",
    )
    g.addoption(
        "--name",
        default=None,
        help="only connect to a board whose Bluetooth name starts with this "
        "(default: the first Nordic UART Service advertiser)",
    )


def pytest_configure(config):
    config.addinivalue_line("markers", "reboots: reboots the board; runs after every other test")


def pytest_collection_modifyitems(config, items):
    # Stable sort: everything else keeps its order, the rebooting tests go last.
    items.sort(key=lambda item: item.get_closest_marker("reboots") is not None)


def pick_settings(cfg):
    return {k: cfg[k] for k in SETTING_KEYS if k in cfg}


async def restore_settings(link, saved):
    """Send every saved setting in SETTING_KEYS order; return what differs after.

    Anything that did not land is sent once more (same order) and re-read.
    """
    keys = [k for k in SETTING_KEYS if k in saved]
    for _ in range(2):
        for key in keys:
            await link.send({key: saved[key]}, response=True)
        now = await link.cfg()
        diff = {k: (saved[k], now.get(k)) for k in saved if now.get(k) != saved[k]}
        if not diff:
            return diff
        keys = [k for k in SETTING_KEYS if k in diff]
    return diff


@pytest.fixture(scope="session")
async def _session(request):
    link = BleLink()
    state = {"link": link, "error": None, "reported": False, "saved": None}
    name = request.config.getoption("--name")
    try:
        await link.connect(name=name, timeout=SCAN_TIMEOUT_S)
    except (BoardNotFound, BleakError, asyncio.TimeoutError, OSError) as e:
        await link.disconnect()
        msg = not_found_message(SCAN_TIMEOUT_S, name)
        if not isinstance(e, BoardNotFound):
            msg += f" Connect error: {type(e).__name__}: {e}"
        state["error"] = msg
        yield state
        return

    try:
        start = await link.cfg()
        state["saved"] = pick_settings(start)
        # Quiet for the run: theme and volume changes preview through the
        # buzzer. The buzzer test unmutes it for itself.
        await link.send({"buzzer": False})
        muted = await link.cfg()
        assert muted.get("buzzer") is False, f"buzzer: sent False, CMD:CFG reports {muted.get('buzzer')!r}"
        yield state
    finally:
        diff, restore_error = {}, None
        try:
            if state["saved"] is not None:
                if not link.connected:
                    await link.reconnect()
                diff = await restore_settings(link, state["saved"])
        except Exception as e:  # noqa: BLE001 -- reported below, after disconnect
            restore_error = e
        finally:
            await link.disconnect()
        assert restore_error is None, (
            f"Settings may not be restored: {type(restore_error).__name__}: {restore_error}. "
            f"Wanted: {state['saved']!r}"
        )
        assert not diff, (
            "Board did not return to its starting settings (field: wanted, reported): "
            + ", ".join(f"{k}: {w!r}, {g!r}" for k, (w, g) in diff.items())
        )


@pytest.fixture
async def board(_session):
    """The connected BleLink. Reconnects if a reboot dropped the link."""
    if _session["error"]:
        # One failure naming the causes; every later test that needs the
        # board is skipped rather than repeating it.
        if _session["reported"]:
            pytest.skip("board not found (see the first failure)")
        _session["reported"] = True
        pytest.fail(_session["error"], pytrace=False)
    link = _session["link"]
    if not link.connected:
        await link.reconnect()
    return link


@pytest.fixture
def saved_settings(_session):
    """The settings as they were before the suite touched anything."""
    return dict(_session["saved"] or {})


# Session-scoped so pytest resolves it before the session's Bluetooth
# connection: without --port a USB test skips without touching the board.
@pytest.fixture(scope="session")
def port(request):
    p = request.config.getoption("--port")
    if not p:
        pytest.skip("needs --port (the board's USB serial port)")
    return p


@pytest.fixture
def soak_seconds(request):
    return request.config.getoption("--soak-seconds")
