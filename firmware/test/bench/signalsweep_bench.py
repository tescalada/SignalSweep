"""Links to a live SignalSweep board for the bench suite.

BleLink speaks the app's protocol: the Nordic UART Service, commands written to
RX as '\\n'-terminated JSON (or a raw CMD: line), replies and the 1 Hz push
reassembled on '\\n' from TX notifications.

UsbLink opens the board's USB serial port with DTR/RTS low. On the C5, opening
the port resets the board anyway, so callers treat open() as a reboot and use
wait_for_boot() before talking to it.
"""

import asyncio
import json
import os
import time

import serial
from bleak import BleakClient, BleakScanner

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# Same as the app's BLE_WRITE_CHUNK: one packet at any MTU >= 183. A command
# longer than this goes as several acknowledged writes.
WRITE_CHUNK = 180

# The app re-asks CMD:CFG because a single reply can be dropped (BLE
# notifications are unacknowledged). Seconds to wait after each attempt.
CFG_RETRY_WAITS = (1.0, 2.0, 4.0)

# A reply keeps coming in for this long after the first one: replies to earlier
# setting commands can still be in flight, and notifications arrive in order,
# so the last reply in a quiet window is the newest state.
CFG_SETTLE_S = 0.5

# Every setting the suite may change, in the order they are restored. `led`
# goes before `bri` because on the brightness branch the legacy LED values 2
# and 3 also set `bri`. `buzzer` goes last so the restores before it (theme
# and volume preview through the buzzer) stay quiet.
SETTING_KEYS = ("led", "bri", "theme", "vol", "band", "beep_mask", "scan_all", "attack", "buzzer")

# Fields the 1 Hz push carries only when true; absent means false.
PUSH_ONLY_WHEN_TRUE = ("scan_all", "attack")


class BoardNotFound(RuntimeError):
    pass


def not_found_message(timeout, name=None):
    who = f"named {name!r}* " if name else ""
    return (
        f"No board {who}advertising the Nordic UART Service within {timeout:g} s. "
        "Likely causes: the phone app is still connected (the board takes one "
        "Bluetooth central at a time; close the app or disconnect it), the board is "
        "in receive-only mode (not advertising), it is out of range or unpowered, "
        "or macOS has not given this terminal Bluetooth permission "
        "(System Settings > Privacy & Security > Bluetooth)."
    )


def is_cfg(obj):
    return isinstance(obj, dict) and obj.get("cfg") is True


def is_push(obj):
    return isinstance(obj, dict) and "status" in obj and "mode" in obj


def push_value(push, key):
    """A setting as the push reports it. Only PUSH_ONLY_WHEN_TRUE keys may be
    absent (meaning false); any other absent key gives None."""
    if key in PUSH_ONLY_WHEN_TRUE:
        return bool(push.get(key, False))
    return push.get(key)


def next_value(key, cur):
    """A valid value for `key` that differs from `cur`."""
    if key in ("buzzer", "scan_all", "attack"):
        return not cur
    if key == "beep_mask":
        return cur ^ 0x01
    # led 0..3 is the wire range on every firmware (the brightness branch
    # reports its Bar as 2 or 3); theme 0..4; vol, bri, band 0..2.
    span = {"led": 4, "theme": 5, "vol": 3, "bri": 3, "band": 3}[key]
    return (cur + 1) % span


class BleLink:
    def __init__(self):
        self.device = None
        self.client = None
        self.name = None
        self.cfg_q = asyncio.Queue()
        self.push_q = asyncio.Queue()
        # Lines that did not parse as JSON: (monotonic time, text).
        self.torn = []
        self.disconnects = 0
        self._buf = bytearray()
        self._synced = False

    @property
    def connected(self):
        return self.client is not None and self.client.is_connected

    async def find(self, name=None, timeout=15.0):
        def match(dev, adv):
            if NUS_SERVICE not in [u.lower() for u in adv.service_uuids]:
                return False
            if name:
                return (adv.local_name or dev.name or "").startswith(name)
            return True

        dev = await BleakScanner.find_device_by_filter(match, timeout=timeout)
        if dev is None:
            raise BoardNotFound(not_found_message(timeout, name))
        return dev

    async def connect(self, name=None, timeout=15.0):
        self.name = name
        self.device = await self.find(name, timeout)
        await self._open()

    async def reconnect(self, timeout=30.0):
        """Scan again and connect, e.g. after a reboot dropped the link."""
        if self.connected:
            return
        deadline = time.monotonic() + timeout
        last = None
        while (left := deadline - time.monotonic()) > 0:
            try:
                # Bounded as a whole: the scan and BleakClient.connect() each
                # have their own timeouts, which could overrun the deadline.
                await asyncio.wait_for(self._find_and_open(min(15.0, left)), left)
                return
            except Exception as e:  # noqa: BLE001 -- retried until the deadline
                last = e
                await self.disconnect()
                await asyncio.sleep(min(1.0, max(0.0, deadline - time.monotonic())))
        raise BoardNotFound(f"Could not reconnect within {timeout:g} s: {last}")

    async def _find_and_open(self, scan_timeout):
        self.device = await self.find(self.name, scan_timeout)
        await self._open()

    async def _open(self):
        self._buf.clear()
        self._synced = False
        self.client = BleakClient(self.device, disconnected_callback=self._on_disconnect)
        try:
            await self.client.connect()
            await self.client.start_notify(NUS_TX, self._on_notify)
        except BaseException:
            # A connected client that never subscribed holds the board's one
            # central slot, so it stops advertising and no retry can find it.
            await self.disconnect()
            raise

    async def disconnect(self):
        if self.client is not None and self.client.is_connected:
            try:
                await self.client.disconnect()
            except Exception:  # noqa: BLE001 -- best effort; the link is going anyway
                pass

    def _on_disconnect(self, _client):
        self.disconnects += 1

    def _on_notify(self, _sender, data):
        self._buf += data
        while True:
            nl = self._buf.find(b"\n")
            if nl < 0:
                break
            raw = bytes(self._buf[:nl])
            del self._buf[: nl + 1]
            # The first line after subscribing may be the tail of a payload
            # that started before the subscription: kept if it is whole JSON,
            # otherwise discarded without counting as torn.
            first = not self._synced
            self._synced = True
            self._dispatch(raw, time.monotonic(), count_torn=not first)

    def _dispatch(self, raw, t, count_torn=True):
        text = raw.decode("utf-8", errors="replace").strip()
        if not text:
            return
        try:
            obj = json.loads(text)
        except ValueError:
            if count_torn:
                self.torn.append((t, text))
            return
        if is_cfg(obj):
            self.cfg_q.put_nowait(obj)
        elif is_push(obj):
            self.push_q.put_nowait((t, len(raw), obj))

    async def send(self, cmd, response=None):
        """Send a dict as JSON, or a str as-is, '\\n'-terminated like the app."""
        line = cmd if isinstance(cmd, str) else json.dumps(cmd, separators=(",", ":"))
        data = (line + "\n").encode()
        multi = len(data) > WRITE_CHUNK
        if response is None:
            response = multi
        for i in range(0, len(data), WRITE_CHUNK):
            await self.client.write_gatt_char(NUS_RX, data[i : i + WRITE_CHUNK], response=response)

    async def next_cfg(self, timeout=5.0):
        return await asyncio.wait_for(self.cfg_q.get(), timeout)

    async def next_push(self, timeout=3.0):
        """The next push as (arrival time, size in bytes, parsed object)."""
        return await asyncio.wait_for(self.push_q.get(), timeout)

    async def fresh_push(self, timeout=3.0):
        """A push built after now: the one after the next.

        The next push may have been serialized on the watchers task before a
        setting landed on the NimBLE host task, so it is skipped.
        """
        self.drain_pushes()
        await self.next_push(timeout)
        _, _, obj = await self.next_push(timeout)
        return obj

    def drain_pushes(self):
        while not self.push_q.empty():
            self.push_q.get_nowait()

    async def cfg(self):
        """CMD:CFG with the app's retry; returns the newest reply."""
        for wait in CFG_RETRY_WAITS:
            while not self.cfg_q.empty():
                self.cfg_q.get_nowait()
            await self.send("CMD:CFG")
            try:
                got = await self.next_cfg(wait)
            except asyncio.TimeoutError:
                continue
            while True:
                try:
                    got = await self.next_cfg(CFG_SETTLE_S)
                except asyncio.TimeoutError:
                    return got
        raise AssertionError(f"No CMD:CFG reply over Bluetooth after {len(CFG_RETRY_WAITS)} attempts")

    async def set(self, key, value):
        """Send one setting and return the CMD:CFG that follows it."""
        await self.send({key: value})
        return await self.cfg()


class UsbLink:
    def __init__(self, port, baud=115200):
        self.port = port
        self.baud = baud
        self.ser = None
        self._buf = bytearray()

    def open(self):
        # DTR/RTS low BEFORE open, as firmware/tools/read-log.py does. The C5
        # still resets when the port opens; callers treat this as a reboot.
        ser = serial.Serial()
        ser.port, ser.baudrate, ser.timeout = self.port, self.baud, 0.2
        ser.dtr = False
        ser.rts = False
        ser.open()
        self.ser = ser
        self._buf.clear()

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            finally:
                self.ser = None

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *exc):
        self.close()

    def send(self, cmd):
        line = cmd if isinstance(cmd, str) else json.dumps(cmd, separators=(",", ":"))
        self.ser.write((line + "\n").encode())
        self.ser.flush()

    def read_line(self, timeout=1.0):
        """One line of text, or None on timeout."""
        deadline = time.monotonic() + timeout
        while True:
            nl = self._buf.find(b"\n")
            if nl >= 0:
                raw = bytes(self._buf[:nl])
                del self._buf[: nl + 1]
                return raw.decode("utf-8", errors="replace").strip()
            if time.monotonic() >= deadline:
                return None
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                self._buf += chunk

    def cfg(self, timeout=3.0):
        """Send CMD:CFG and return the newest reply within the timeout, or None."""
        self.send("CMD:CFG")
        deadline = time.monotonic() + timeout
        got = None
        settle_until = None
        while time.monotonic() < deadline:
            if settle_until is not None and time.monotonic() >= settle_until:
                break
            line = self.read_line(0.2)
            if not line or not line.startswith("{"):
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if is_cfg(obj):
                got = obj
                settle_until = time.monotonic() + CFG_SETTLE_S
        return got

    def wait_for_boot(self, timeout=30.0, max_uptime=None, max_reopens=3):
        """Ask CMD:CFG until the rebooted board answers; returns the reply.

        With max_uptime, a reply whose uptime is at or above it is from the
        instance before the reset and is ignored. If nothing qualifying comes
        before the deadline, returns the last reply seen (None if none).

        A reset can re-enumerate the USB device and invalidate the open handle,
        so a serial error closes the port, waits until its path exists again
        plus a second, and reopens it (DTR/RTS low again). On the C5 every
        reopen resets the board again, so reopens are capped.
        """
        deadline = time.monotonic() + timeout
        last_err = None
        last_cfg = None
        reopens = 0
        while time.monotonic() < deadline:
            try:
                if self.ser is None:
                    if reopens >= max_reopens:
                        break
                    while not os.path.exists(self.port) and time.monotonic() < deadline:
                        time.sleep(0.2)
                    time.sleep(1.0)
                    reopens += 1
                    self.open()
                got = self.cfg(timeout=1.5)
                if got is None:
                    continue
                last_cfg = got
                if max_uptime is None or got.get("uptime", max_uptime) < max_uptime:
                    return got
            except (serial.SerialException, OSError) as e:
                last_err = e
                self.close()
        if last_cfg is not None:
            return last_cfg
        raise AssertionError(
            f"No CMD:CFG reply over USB on {self.port} within {timeout:g} s"
            + (f" ({reopens} reopens, last serial error: {last_err})" if last_err else "")
        )
