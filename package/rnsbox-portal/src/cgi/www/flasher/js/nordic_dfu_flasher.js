/**
 * Nordic Legacy DFU flasher for nRF52 devices in MBR fallback mode (PID 0x8029).
 *
 * The Nordic MBR uses the same SLIP/HCI framing as the Adafruit DFU but is
 * strictly bidirectional for control packets: it sends a reliable HCI response
 * after the DFU_START_PACKET and DFU_INIT_PACKET and expects the host to ACK
 * those responses before it continues processing.  Without ACKs the MBR stalls.
 *
 * Control packets (start, init, stop): sent as reliable HCI, full ACK exchange.
 * Data packets: sent as non-reliable HCI (rp=0), no per-packet ACK needed.
 */
class NordicDfuFlasher {

    FLASH_BAUD = 115200;

    // DFU op codes
    DFU_START_PACKET     = 3;
    DFU_INIT_PACKET      = 1;
    DFU_DATA_PACKET      = 4;
    DFU_STOP_DATA_PACKET = 5;
    DFU_RESPONSE_PACKET  = 0x11;

    // DFU image types
    HEX_TYPE_SD_BL       = 3;
    HEX_TYPE_APPLICATION = 4;

    // DFU status codes
    DFU_UPDATE_COMPLETED = 1;

    // HCI / SLIP constants (shared with Nrf52DfuFlasher)
    DATA_INTEGRITY_CHECK_PRESENT = 1;
    HCI_PACKET_TYPE              = 14;

    FLASH_PAGE_SIZE       = 4096;
    FLASH_PAGE_ERASE_TIME = 0.0897;
    FLASH_WORD_WRITE_TIME = 0.000100;
    FLASH_PAGE_WRITE_TIME = (this.FLASH_PAGE_SIZE / 4) * this.FLASH_WORD_WRITE_TIME;

    DFU_PACKET_MAX_SIZE = 512;

    constructor(serialPort) {
        this.port           = serialPort;
        this.sequenceNumber = 0;
        this.total_size     = 0;

        // Persistent SLIP receive buffer (survives across _readPacket calls so
        // bytes from a multi-packet reader.read() chunk are never lost).
        this._rxBuf    = [];   // raw undecoded bytes
        this._rxRaw    = [];   // bytes of the packet currently being assembled
        this._rxInPkt  = false;
    }

    // ── Helpers ───────────────────────────────────────────────────────────

    async _sleep(ms) {
        await new Promise(r => setTimeout(r, ms));
    }

    _int32(n) {
        return [(n & 0xFF), (n >> 8 & 0xFF), (n >> 16 & 0xFF), (n >> 24 & 0xFF)];
    }

    _int16(n) {
        return [(n & 0xFF), (n >> 8 & 0xFF)];
    }

    // ── CRC-16 ────────────────────────────────────────────────────────────

    _crc16(data, crc = 0xFFFF) {
        for (const b of data) {
            crc = ((crc >> 8) & 0x00FF) | ((crc << 8) & 0xFF00);
            crc ^= b;
            crc ^= (crc & 0x00FF) >> 4;
            crc ^= ((crc << 8) << 4) & 0xFFFF;
            crc ^= (((crc & 0x00FF) << 4) << 1) & 0xFFFF;
            crc &= 0xFFFF;
        }
        return crc;
    }

    // ── SLIP encode/decode ────────────────────────────────────────────────

    _slipEncode(data) {
        const out = [];
        for (const b of data) {
            if (b === 0xC0)      { out.push(0xDB, 0xDC); }
            else if (b === 0xDB) { out.push(0xDB, 0xDD); }
            else                 { out.push(b); }
        }
        return out;
    }

    _slipDecode(raw) {
        const out = [];
        for (let i = 0; i < raw.length; i++) {
            if (raw[i] === 0xDB) {
                i++;
                out.push(raw[i] === 0xDC ? 0xC0 : 0xDB);
            } else {
                out.push(raw[i]);
            }
        }
        return new Uint8Array(out);
    }

    // ── HCI packet construction ───────────────────────────────────────────

    /** Reliable HCI packet (device must ACK it). Used for control packets. */
    _makeHciPacket(frame) {
        this.sequenceNumber = (this.sequenceNumber + 1) % 8;
        const seq = this.sequenceNumber;
        const len = frame.length;
        const h = [0, 0, 0, 0];
        h[0] = seq | (((seq + 1) % 8) << 3) | (this.DATA_INTEGRITY_CHECK_PRESENT << 6) | (1 << 7);
        h[1] = this.HCI_PACKET_TYPE | ((len & 0x0F) << 4);
        h[2] = (len & 0xFF0) >> 4;
        h[3] = (~(h[0] + h[1] + h[2]) + 1) & 0xFF;
        const data = [...h, ...frame];
        const crc = this._crc16(new Uint8Array(data));
        data.push(crc & 0xFF, (crc >> 8) & 0xFF);
        return new Uint8Array([0xC0, ...this._slipEncode(data), 0xC0]);
    }

    /** Non-reliable HCI packet (device does NOT need to ACK). Used for data. */
    _makeDataHciPacket(frame) {
        const len = frame.length;
        const h = [0, 0, 0, 0];
        h[0] = 0 | (0 << 3) | (this.DATA_INTEGRITY_CHECK_PRESENT << 6) | (0 << 7);
        h[1] = this.HCI_PACKET_TYPE | ((len & 0x0F) << 4);
        h[2] = (len & 0xFF0) >> 4;
        h[3] = (~(h[0] + h[1] + h[2]) + 1) & 0xFF;
        const data = [...h, ...frame];
        const crc = this._crc16(new Uint8Array(data));
        data.push(crc & 0xFF, (crc >> 8) & 0xFF);
        return new Uint8Array([0xC0, ...this._slipEncode(data), 0xC0]);
    }

    /** Non-reliable ACK packet acknowledging device's reliable packet with seq=forSeq. */
    _makeAckPacket(forSeq) {
        const ackSeq = (forSeq + 1) % 8;
        const h = [0, 0, 0, 0];
        h[0] = 0 | (ackSeq << 3) | (this.DATA_INTEGRITY_CHECK_PRESENT << 6) | (0 << 7);
        h[1] = this.HCI_PACKET_TYPE;  // payload length = 0
        h[2] = 0;
        h[3] = (~(h[0] + h[1] + h[2]) + 1) & 0xFF;
        const raw = [...h];
        const crc = this._crc16(new Uint8Array(raw));
        raw.push(crc & 0xFF, (crc >> 8) & 0xFF);
        return new Uint8Array([0xC0, ...this._slipEncode(raw), 0xC0]);
    }

    // ── Serial write ──────────────────────────────────────────────────────

    async _write(encoded) {
        if (!this.port.writable) await this._reconnect();
        const writer = this.port.writable.getWriter();
        try {
            await writer.write(encoded);
            writer.releaseLock();
        } catch(e) {
            try { writer.releaseLock(); } catch(ex) {}
            await this._reconnect();
            const w = this.port.writable.getWriter();
            try { await w.write(encoded); } finally { w.releaseLock(); }
        }
    }

    // ── SLIP receive with persistent byte buffer ──────────────────────────

    /**
     * Drain this._rxBuf looking for a complete SLIP packet.
     * Maintains framing state in this._rxInPkt / this._rxRaw.
     * Returns a decoded Uint8Array when a complete packet is found, else null.
     */
    _drainBuf() {
        while (this._rxBuf.length > 0) {
            const b = this._rxBuf.shift();
            if (b === 0xC0) {
                if (this._rxInPkt && this._rxRaw.length > 0) {
                    const pkt = this._slipDecode(this._rxRaw);
                    this._rxRaw    = [];
                    this._rxInPkt  = true;  // ready for next packet
                    return pkt;
                }
                this._rxInPkt = true;
                this._rxRaw   = [];
            } else if (this._rxInPkt) {
                this._rxRaw.push(b);
            }
        }
        return null;
    }

    /**
     * Read one SLIP packet with a timeout.
     * All bytes received are pushed to the persistent _rxBuf so that bytes from
     * a multi-packet reader.read() chunk are never discarded.
     */
    async _readPacket(timeoutMs = 5000) {
        // Return anything already assembled in the buffer first.
        const pending = this._drainBuf();
        if (pending) return pending;

        const deadline = Date.now() + timeoutMs;
        while (Date.now() < deadline) {
            const remaining = deadline - Date.now();
            if (remaining <= 0) break;

            const reader = this.port.readable.getReader();
            let chunk;
            try {
                chunk = await Promise.race([
                    reader.read(),
                    new Promise((_, rej) => setTimeout(() => rej(null), remaining)),
                ]).catch(() => ({ value: null, done: true }));
            } finally {
                reader.releaseLock();
            }

            if (!chunk || chunk.done || !chunk.value) break;

            // Push ALL bytes from this chunk into the buffer (none are lost).
            console.log(`Nordic DFU: raw rx ${chunk.value.length} bytes: ${Array.from(chunk.value).map(b=>b.toString(16).padStart(2,'0')).join(' ')}`);
            for (const b of chunk.value) this._rxBuf.push(b);

            const pkt = this._drainBuf();
            if (pkt) return pkt;
        }
        return null;
    }

    // ── Control-packet ACK exchange ───────────────────────────────────────

    /**
     * Send a reliable HCI packet and do the full HCI handshake:
     *   1. Send our reliable packet.
     *   2. Read packets until we see the device's reliable DFU response.
     *      (Non-reliable device ACKs are silently consumed.)
     *   3. Send a non-reliable ACK for the device's reliable response.
     *   4. Validate the DFU status code; throw on error.
     *
     * maxWaitMs should cover any erase time (e.g. 12 s for DFU_START_PACKET).
     */
    async _sendControlPacket(encoded, maxWaitMs) {
        await this._write(encoded);

        const deadline = Date.now() + maxWaitMs;
        while (Date.now() < deadline) {
            const remaining = deadline - Date.now();
            const pkt = await this._readPacket(Math.min(1000, remaining));

            if (!pkt) {
                // Timeout on this sub-read; reconnect if port dropped then keep waiting.
                if (!this.port.writable) {
                    try { await this._reconnect(); } catch(e) { break; }
                }
                continue;
            }

            if (pkt.length < 6) continue;  // malformed, skip

            const reliable = (pkt[0] >> 7) & 1;
            const devSeq   = pkt[0] & 0x07;
            const h1 = pkt[1], h2 = pkt[2];
            const payloadLen = ((h1 >> 4) & 0x0F) | (h2 << 4);

            console.log(`Nordic DFU recv: reliable=${reliable} seq=${devSeq} payloadLen=${payloadLen} raw=${Array.from(pkt.slice(0,6)).map(b=>b.toString(16).padStart(2,'0')).join(' ')}`);

            if (!reliable) continue;  // device ACK for our packet — nothing to do

            // Device sent a reliable response — ACK it first.
            await this._write(this._makeAckPacket(devSeq));

            if (pkt.length < 4 + payloadLen + 2) continue;
            const payload = pkt.slice(4, 4 + payloadLen);

            console.log(`Nordic DFU payload: ${Array.from(payload).map(b=>b.toString(16).padStart(2,'0')).join(' ')}`);

            // Look for DFU response in payload (either HCI-framed payload or raw).
            let dfuPayload = null;
            if (payload.length >= 3 && payload[0] === this.DFU_RESPONSE_PACKET) {
                dfuPayload = payload;
            }
            if (!dfuPayload) continue;  // not a DFU response, keep reading

            const reqOp = dfuPayload[1];
            const status = dfuPayload[2];
            console.log(`Nordic DFU response: reqOp=0x${reqOp.toString(16)} status=${status}`);

            if (status !== this.DFU_UPDATE_COMPLETED) {
                const CODES = {
                    2: 'INVALID_STATE',
                    3: 'NOT_SUPPORTED',
                    4: 'DATA_SIZE_EXCEEDS_LIMIT',
                    5: 'CRC_ERROR',
                    6: 'OPERATION_FAILED',
                };
                const name = CODES[status] || `0x${status.toString(16)}`;
                throw new Error(`DFU error after op 0x${reqOp.toString(16)}: ${name}`);
            }

            return;  // ✓ success
        }

        // No DFU response within the timeout — continue optimistically.
        console.warn(`Nordic DFU: no response within ${maxWaitMs} ms, continuing`);
    }

    // ── USB reconnect ─────────────────────────────────────────────────────

    async _reconnect() {
        try { await this.port.close(); } catch(e) {}
        for (let i = 0; i < 8; i++) {
            await this._sleep(500);
            try { await this.port.open({ baudRate: this.FLASH_BAUD }); return; } catch(e) {}
        }
        throw new Error('Device did not reconnect after USB drop');
    }

    // ── DFU phases ────────────────────────────────────────────────────────

    async _sendStartDfu(mode, sdSize, blSize, appSize) {
        const frame = [
            ...this._int32(this.DFU_START_PACKET),
            ...this._int32(mode),
            ...this._int32(sdSize),
            ...this._int32(blSize),
            ...this._int32(appSize),
        ];
        this.total_size = sdSize + blSize + appSize;

        // The erase time is baked into the response wait: the MBR erases flash
        // before sending its DFU response, so we wait up to eraseTime + margin.
        const eraseMs = Math.max(5000,
            ((this.total_size / this.FLASH_PAGE_SIZE) + 2) * this.FLASH_PAGE_ERASE_TIME * 1000 + 3000);

        console.log(`Nordic DFU: sending DFU_START mode=${mode} sdSize=${sdSize} blSize=${blSize} appSize=${appSize} eraseWait=${eraseMs}ms`);
        await this._sendControlPacket(this._makeHciPacket(frame), eraseMs);
    }

    async _sendInitPacket(initPacket) {
        const frame = [
            ...this._int32(this.DFU_INIT_PACKET),
            ...initPacket,
            ...this._int16(0x0000),  // required padding
        ];
        console.log(`Nordic DFU: sending DFU_INIT (${initPacket.length} bytes)`);
        await this._sendControlPacket(this._makeHciPacket(frame), 5000);
    }

    async _sendFirmware(firmware, onProgress) {
        // Build reliable data packets (RP=1, incrementing seq) matching nrf52_dfu_flasher.js.
        const packets = [];
        for (let i = 0; i < firmware.length; i += this.DFU_PACKET_MAX_SIZE) {
            packets.push(this._makeHciPacket([
                ...this._int32(this.DFU_DATA_PACKET),
                ...firmware.slice(i, i + this.DFU_PACKET_MAX_SIZE),
            ]));
        }

        if (onProgress) onProgress(0);
        console.log(`Nordic DFU: sending ${packets.length} data packets (reliable)`);

        for (let i = 0; i < packets.length; i++) {
            await this._write(packets[i]);
            await this._sleep(this.FLASH_PAGE_WRITE_TIME * 1000);
            if (onProgress) onProgress(Math.floor(((i + 1) / packets.length) * 100));
        }

        // Stop packet is a reliable control packet; device validates CRC then reboots.
        // Device may disconnect before sending its final response, so errors are swallowed.
        console.log('Nordic DFU: sending DFU_STOP');
        await this._sendControlPacket(
            this._makeHciPacket([...this._int32(this.DFU_STOP_DATA_PACKET)]),
            10000,
        ).catch(e => console.warn('Nordic DFU: stop response error (may be normal reboot):', e));
    }

    // ── Public entry point ────────────────────────────────────────────────

    /**
     * Flashes a Nordic DFU zip package to the device.
     * Supports softdevice_bootloader and application package types.
     *
     * @param {Blob}     zipBlob          – Nordic DFU zip with manifest.json
     * @param {Function} progressCallback – called with 0–100
     */
    async flash(zipBlob, progressCallback) {
        const blobReader = new window.zip.BlobReader(zipBlob);
        const zipReader  = new window.zip.ZipReader(blobReader);
        const entries    = await zipReader.getEntries();

        const mfEntry = entries.find(e => e.filename === 'manifest.json');
        if (!mfEntry) throw new Error('manifest.json not found in DFU zip');

        const manifest = JSON.parse(await mfEntry.getData(new window.zip.TextWriter())).manifest;

        let pkg, mode, sdSize = 0, blSize = 0, appSize = 0;

        if (manifest.softdevice_bootloader) {
            pkg    = manifest.softdevice_bootloader;
            mode   = this.HEX_TYPE_SD_BL;
            sdSize = pkg.sd_size || 0;
            blSize = pkg.bl_size || 0;
        } else if (manifest.bootloader) {
            pkg    = manifest.bootloader;
            mode   = 2;
        } else if (manifest.application) {
            pkg    = manifest.application;
            mode   = this.HEX_TYPE_APPLICATION;
        } else {
            throw new Error('No flashable image found in manifest');
        }

        const binEntry = entries.find(e => e.filename === pkg.bin_file);
        const datEntry = entries.find(e => e.filename === pkg.dat_file);
        if (!binEntry) throw new Error(`bin file not found: ${pkg.bin_file}`);
        if (!datEntry) throw new Error(`dat file not found: ${pkg.dat_file}`);

        const firmware   = await binEntry.getData(new window.zip.Uint8ArrayWriter());
        const initPacket = await datEntry.getData(new window.zip.Uint8ArrayWriter());

        if (mode === this.HEX_TYPE_APPLICATION) appSize = firmware.length;
        else if (mode === 2)                     blSize  = firmware.length;

        // Open port with retries in case the device is still enumerating.
        for (let attempt = 0; attempt < 8; attempt++) {
            try { await this.port.open({ baudRate: this.FLASH_BAUD }); break; }
            catch(e) { if (attempt === 7) throw e; await this._sleep(500); }
        }
        await this._sleep(100);

        try {
            await this._sendStartDfu(mode, sdSize, blSize, appSize);
            await this._sendInitPacket(initPacket);
            await this._sendFirmware(firmware, progressCallback);
        } finally {
            try { await this.port.close(); } catch(e) {}
        }
    }
}
