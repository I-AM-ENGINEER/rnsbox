"""
RNode-over-BLE emulation for RNSBox.

The box exposes a Nordic UART Service (NUS) GATT endpoint that speaks the
RNode serial protocol.  A phone running Sideband (or anything else built on
RNS's RNodeInterface) can pair with the box and select it as an "RNode over
BLE"; from then on Reticulum packets from the phone are injected into this
node's transport, and packets routed to the phone are written back as
CMD_DATA frames.

This is deliberately a *bridge*, not a radio: there is no LoRa hardware
behind it.  The radio parameters the client sets are acknowledged and
reported back unchanged (RNS validates that the device echoed them), while
the actual transport is the box's own WiFi/ethernet/BLE mesh links.
"""

from __future__ import annotations

import os
import struct
import time

import RNS

try:
    from RNS.Interfaces.Interface import Interface
except ImportError:  # very old RNS
    Interface = RNS.Interface


class RNodeBLEEmulator(Interface):
    """An RNode-compatible transport endpoint on top of the BLE GATT server."""

    IN = True
    OUT = True
    FWD = False
    RPT = False

    # RNode serial framing
    FEND = 0xC0
    FESC = 0xDB
    TFEND = 0xDC
    TFESC = 0xDD

    CMD_DATA = 0x00
    CMD_FREQUENCY = 0x01
    CMD_BANDWIDTH = 0x02
    CMD_TXPOWER = 0x03
    CMD_SF = 0x04
    CMD_CR = 0x05
    CMD_RADIO_STATE = 0x06
    CMD_RADIO_LOCK = 0x07
    CMD_DETECT = 0x08
    CMD_IMPLICIT = 0x09
    CMD_LEAVE = 0x0A
    CMD_ST_ALOCK = 0x0B
    CMD_LT_ALOCK = 0x0C
    CMD_PROMISC = 0x0E
    CMD_READY = 0x0F
    CMD_STAT_RX = 0x21
    CMD_STAT_TX = 0x22
    CMD_STAT_RSSI = 0x23
    CMD_STAT_SNR = 0x24
    CMD_STAT_CHTM = 0x25
    CMD_STAT_PHYPRM = 0x26
    CMD_STAT_BAT = 0x27
    CMD_STAT_CSMA = 0x28
    CMD_STAT_TEMP = 0x29
    CMD_BLINK = 0x30
    CMD_RANDOM = 0x40
    CMD_FB_EXT = 0x41
    CMD_FB_READ = 0x42
    CMD_BOARD = 0x47
    CMD_PLATFORM = 0x48
    CMD_MCU = 0x49
    CMD_FW_VERSION = 0x50
    CMD_ROM_READ = 0x51
    CMD_FW_HASH = 0x58
    CMD_DISP_READ = 0x66

    DETECT_REQ = 0x73
    DETECT_RESP = 0x46

    # Values reported to the client.  ESP32 is the platform whose RNode
    # implementation supports BLE, so clients enable display/framebuffer
    # logic only when they see it; GENERIC_ESP32 keeps rnodeconf from
    # assuming a specific board's EEPROM layout.
    PLATFORM_ESP32 = 0x80
    MCU_ESP32 = 0x81
    BOARD_GENERIC_ESP32 = 0x35
    FW_MAJOR = 1
    FW_MINOR = 86

    # Client-side RSSI is `byte - RSSI_OFFSET`; send a healthy mid value.
    RSSI_BYTE = 0xA0

    HW_MTU = 500
    BITRATE = 2400

    def __init__(self, parent):
        super().__init__()
        self.parent_interface = parent
        self.owner = parent.owner
        self.name = "RNode BLE"
        self.online = False
        self.mode = Interface.MODE_FULL
        self.bitrate = self.BITRATE
        self.HW_MTU = 500
        self.FIXED_MTU = True
        self.mtu = self.HW_MTU
        # The RNode bridge is appended to Transport.interfaces; rnstatus's
        # ifstats RPC reads the announce-rate policy fields from every
        # interface, so they must exist even though this endpoint never
        # announces on its own.
        self.announce_rate_target = None
        self.announce_rate_grace = None
        self.announce_rate_penalty = None
        self.announce_rate = 0
        self.announce_cap = None
        self.ifac_size = getattr(parent, "ifac_size", None)
        if getattr(parent, "announce_cap", None) is not None:
            self.announce_cap = parent.announce_cap

        # Radio state echoed back to the client.  RNS sends the values it
        # wants and then verifies the device reports the same ones.
        self.frequency = 869525000
        self.bandwidth = 125000
        self.txpower = 17
        self.sf = 8
        self.cr = 5
        self.radio_state = 0
        self.implicit = 0
        self.promisc = 0
        self.st_alock = b"\x00\x00"
        self.lt_alock = b"\x00\x00"

        self.rxb = 0
        self.txb = 0
        self.stat_rx = 0
        self.stat_tx = 0

        # Per-central HDLC decoder state.
        self._rxbuf = {}
        self._escape = {}
        self._centrals = set()
        self._lock = __import__("threading").RLock()

        RNS.log(f"{self} created (RNode serial protocol over NUS)", RNS.LOG_DEBUG)

    # ------------------------------------------------------------------ RX

    def central_connected(self, address):
        with self._lock:
            self._centrals.add(address)
            self._rxbuf.setdefault(address, bytearray())
            self._escape.setdefault(address, False)
        RNS.log(f"{self} central {address} connected", RNS.LOG_DEBUG)


    def central_disconnected(self, address):
        with self._lock:
            self._centrals.discard(address)
            self._rxbuf.pop(address, None)
            self._escape.pop(address, None)
        RNS.log(f"{self} central {address} disconnected", RNS.LOG_DEBUG)

    def feed(self, data: bytes, address: str):
        """Feed raw NUS RX bytes from a central into the HDLC decoder."""
        with self._lock:
            if address not in self._rxbuf:
                self.central_connected(address)
            buf = self._rxbuf[address]
            esc = self._escape[address]
            frames = []
            for b in data:
                if b == self.FEND:
                    if buf:
                        frames.append(bytes(buf))
                        buf.clear()
                    esc = False
                elif b == self.FESC:
                    esc = True
                else:
                    if esc:
                        if b == self.TFEND:
                            b = self.FEND
                        elif b == self.TFESC:
                            b = self.FESC
                        esc = False
                    buf.append(b)
            self._escape[address] = esc
        for frame in frames:
            try:
                self._process_frame(frame, address)
            except Exception as e:
                RNS.log(f"{self} frame error: {type(e).__name__}: {e}", RNS.LOG_ERROR)

    def _process_frame(self, frame: bytes, address: str):
        if not frame:
            return
        cmd, payload = frame[0], frame[1:]

        if cmd == self.CMD_DATA:
            if not payload:
                return
            self.stat_rx += 1
            self.rxb += len(payload)
            RNS.log(f"{self} RX: {len(payload)} B from {address}", RNS.LOG_DEBUG)
            if self.online and self.owner:
                self.owner.inbound(payload, self)
            return

        if cmd == self.CMD_DETECT:
            if payload and payload[0] == self.DETECT_REQ:
                self._send(self.CMD_DETECT, bytes([self.DETECT_RESP]), address)
        elif cmd == self.CMD_FW_VERSION:
            self._send(self.CMD_FW_VERSION,
                       bytes([self.FW_MAJOR, self.FW_MINOR]), address)
        elif cmd == self.CMD_PLATFORM:
            self._send(self.CMD_PLATFORM, bytes([self.PLATFORM_ESP32]), address)
        elif cmd == self.CMD_MCU:
            self._send(self.CMD_MCU, bytes([self.MCU_ESP32]), address)
        elif cmd == self.CMD_BOARD:
            self._send(self.CMD_BOARD, bytes([self.BOARD_GENERIC_ESP32]), address)
        elif cmd == self.CMD_FREQUENCY and len(payload) >= 4:
            self.frequency = struct.unpack("!I", payload[:4])[0]
            self._send(cmd, payload[:4], address)
        elif cmd == self.CMD_BANDWIDTH and len(payload) >= 4:
            self.bandwidth = struct.unpack("!I", payload[:4])[0]
            self._send(cmd, payload[:4], address)
        elif cmd == self.CMD_TXPOWER and payload:
            self.txpower = payload[0]
            self._send(cmd, payload[:1], address)
        elif cmd == self.CMD_SF and payload:
            self.sf = payload[0]
            self._send(cmd, payload[:1], address)
        elif cmd == self.CMD_CR and payload:
            self.cr = payload[0]
            self._send(cmd, payload[:1], address)
        elif cmd == self.CMD_RADIO_STATE and payload:
            self.radio_state = 1 if payload[0] else 0
            self.online = bool(self.radio_state)
            self._send(cmd, bytes([self.radio_state]), address)
            RNS.log(f"{self} radio state {'ON' if self.radio_state else 'OFF'}", RNS.LOG_DEBUG)
        elif cmd == self.CMD_RADIO_LOCK:
            self._send(cmd, b"\x00", address)
        elif cmd == self.CMD_IMPLICIT and payload:
            self.implicit = payload[0]
            self._send(cmd, payload[:1], address)
        elif cmd == self.CMD_ST_ALOCK:
            if payload:
                self.st_alock = payload[:2]
            self._send(cmd, self.st_alock, address)
        elif cmd == self.CMD_LT_ALOCK:
            if payload:
                self.lt_alock = payload[:2]
            self._send(cmd, self.lt_alock, address)
        elif cmd == self.CMD_PROMISC:
            if payload:
                self.promisc = payload[0]
            self._send(cmd, bytes([self.promisc]), address)
        elif cmd == self.CMD_READY:
            self._send(self.CMD_READY, b"", address)
        elif cmd == self.CMD_STAT_RX:
            self._send(cmd, struct.pack("!I", self.stat_rx), address)
        elif cmd == self.CMD_STAT_TX:
            self._send(cmd, struct.pack("!I", self.stat_tx), address)
        elif cmd == self.CMD_STAT_RSSI:
            self._send(cmd, bytes([self.RSSI_BYTE]), address)
        elif cmd == self.CMD_STAT_SNR:
            self._send(cmd, b"\x00", address)
        elif cmd == self.CMD_STAT_BAT:
            self._send(cmd, struct.pack("!H", 0), address)
        elif cmd == self.CMD_STAT_TEMP:
            self._send(cmd, struct.pack("!b", 35), address)
        elif cmd == self.CMD_STAT_CHTM:
            self._send(cmd, struct.pack("!I", 0), address)
        elif cmd == self.CMD_STAT_PHYPRM:
            # symbol time / preamble / bitrate — not used for validation
            self._send(cmd, b"", address)
        elif cmd == self.CMD_STAT_CSMA:
            self._send(cmd, b"", address)
        elif cmd == self.CMD_RANDOM:
            self._send(cmd, os.urandom(4), address)
        elif cmd == self.CMD_FW_HASH:
            self._send(cmd, b"\x00" * 32, address)
        elif cmd == self.CMD_BLINK:
            pass
        elif cmd == self.CMD_LEAVE:
            RNS.log(f"{self} client {address} sent LEAVE", RNS.LOG_DEBUG)
        elif cmd == self.CMD_FB_EXT:
            # No framebuffer: report "external framebuffer disabled".
            self._send(cmd, b"\x00", address)
        elif cmd == self.CMD_DISP_READ:
            self._send(cmd, b"", address)
        else:
            RNS.log(f"{self} unknown RNode command 0x{cmd:02x}", RNS.LOG_DEBUG)
            self._send(0xFE, bytes([cmd]), address)

    # ------------------------------------------------------------------ TX

    def process_outgoing(self, data):
        if not self.online:
            return
        if not self._centrals:
            return
        payload = bytes(data)
        self.stat_tx += 1
        self.txb += len(payload)
        RNS.log(f"{self} TX: {len(payload)} B to {len(self._centrals)} central(s)",
                RNS.LOG_DEBUG)
        self._send(self.CMD_DATA, payload)

    def _send(self, cmd, payload=b"", address=None):
        body = bytes([cmd])
        for b in payload:
            if b == self.FEND:
                body += bytes([self.FESC, self.TFEND])
            elif b == self.FESC:
                body += bytes([self.FESC, self.TFESC])
            else:
                body += bytes([b])
        frame = bytes([self.FEND]) + body + bytes([self.FEND])
        driver = getattr(self.parent_interface, "driver", None)
        if driver is not None and hasattr(driver, "rnode_send"):
            driver.rnode_send(frame, address)

    def should_ingress_limit(self):
        return False

    def detach(self):
        self.online = False
        try:
            if self in RNS.Transport.interfaces:
                RNS.Transport.interfaces.remove(self)
        except Exception:
            pass

    def __str__(self):
        return f"RNodeBLEInterface[{self.name}]"
