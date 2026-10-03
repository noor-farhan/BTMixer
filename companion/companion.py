"""
BTMixer companion (Windows).

    pip install bleak pycaw comtypes psutil
    python companion.py

Finds the ESP32 over BLE (no pairing needed), syncs master + per-app volumes
to it, and applies volume/mute changes coming back from the knob.
Use pythonw.exe (or a startup shortcut) to run it without a console window.
"""
import sys
sys.coinit_flags = 0  # COINIT_MULTITHREADED: must be set BEFORE comtypes/pycaw/bleak import

import asyncio
from ctypes import POINTER, cast

from bleak import BleakClient, BleakScanner
from comtypes import CLSCTX_ALL
from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume

SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # we write here  (PC -> device)
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # we listen here (device -> PC)

POLL_SECONDS = 1.0
MAX_ENTRIES = 12      # must match MAX_SESSIONS in the firmware
NAME_LEN = 12


class Entry:
    def __init__(self, name, handles, master=False):
        self.name, self.handles, self.master = name[:NAME_LEN], handles, master

    def get(self):
        h = self.handles[0]
        if self.master:
            vol = h.GetMasterVolumeLevelScalar()
        else:
            vol = h.GetMasterVolume()
        return round(vol * 100), bool(h.GetMute())

    def set_vol(self, pct):
        for h in self.handles:
            if self.master:
                h.SetMasterVolumeLevelScalar(pct / 100, None)
            else:
                h.SetMasterVolume(pct / 100, None)

    def set_mute(self, mute):
        for h in self.handles:
            h.SetMute(int(mute), None)


def get_master():
    dev = AudioUtilities.GetSpeakers()
    try:                                  # newer pycaw
        return dev.EndpointVolume
    except AttributeError:                # older pycaw
        iface = dev.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
        return cast(iface, POINTER(IAudioEndpointVolume))


def snapshot():
    """Master first, then one entry per process name (multi-process apps merged)."""
    entries = [Entry("Master", [get_master()], master=True)]
    groups = {}
    for s in AudioUtilities.GetAllSessions():
        try:
            if s.Process is None:
                continue
            name = s.Process.name()
            if name.lower().endswith(".exe"):
                name = name[:-4]
            groups.setdefault(name, []).append(s.SimpleAudioVolume)
        except Exception:
            continue
    for name in sorted(groups, key=str.lower):
        entries.append(Entry(name, groups[name]))
    return entries[:MAX_ENTRIES]


class Mixer:
    def __init__(self):
        self.entries = []
        self.sent = []          # last (name, vol, mute) per index sent to the device
        self.client = None

    async def push(self, force=False):
        entries = snapshot()
        state = []
        for e in entries:
            try:
                vol, mute = e.get()
            except Exception:
                vol, mute = 0, False
            state.append((e.name, vol, mute))
        self.entries = entries

        names_changed = [s[0] for s in state] != [s[0] for s in self.sent]
        for i, st in enumerate(state):
            if force or names_changed or st != self.sent[i]:
                msg = f"S|{i}|{len(state)}|{st[0]}|{st[1]}|{int(st[2])}"
                await self.client.write_gatt_char(RX, msg.encode(), response=True)
        self.sent = state

    def on_notify(self, _, data):
        try:
            p = data.decode(errors="ignore").strip().split("|")
            idx = int(p[1])
            e = self.entries[idx]
            name, vol, mute = self.sent[idx]
            if p[0] == "V":
                vol = max(0, min(100, int(p[2])))
                e.set_vol(vol)
            elif p[0] == "M":
                mute = not mute
                e.set_mute(mute)
            self.sent[idx] = (name, vol, mute)   # so the poll doesn't echo it back
        except Exception as ex:
            print("bad command:", data, ex)


async def main():
    mixer = Mixer()
    while True:
        print("Scanning for BTMixer...")
        dev = await BleakScanner.find_device_by_filter(
            lambda d, ad: SERVICE in [u.lower() for u in (ad.service_uuids or [])],
            timeout=10,
        )
        if dev is None:
            continue
        gone = asyncio.Event()
        try:
            async with BleakClient(dev, disconnected_callback=lambda c: gone.set()) as client:
                print("Connected:", dev.address)
                mixer.client, mixer.sent = client, []
                await client.start_notify(TX, mixer.on_notify)
                await mixer.push(force=True)
                while not gone.is_set():
                    try:
                        await asyncio.wait_for(gone.wait(), POLL_SECONDS)
                    except asyncio.TimeoutError:
                        await mixer.push()
        except Exception as ex:
            print("Connection error:", ex)
        print("Disconnected, retrying...")
        await asyncio.sleep(2)


if __name__ == "__main__":
    asyncio.run(main())
