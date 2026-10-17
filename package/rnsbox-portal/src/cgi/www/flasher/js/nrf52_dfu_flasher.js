/**
 * A Web Serial based nRF52 flasher written by liam@liamcottle.com based on dfu_transport.serial.py
 * https://github.com/adafruit/Adafruit_nRF52_nrfutil/blob/master/nordicsemi/dfu/dfu_transport_serial.py
 */
class Nrf52DfuFlasher {

    DFU_TOUCH_BAUD = 1200;
    SERIAL_PORT_OPEN_WAIT_TIME = 0.1;
    TOUCH_RESET_WAIT_TIME = 1.5;

    FLASH_BAUD = 115200;

    HEX_TYPE_APPLICATION = 4;

    DFU_INIT_PACKET = 1;
    DFU_START_PACKET = 3;
    DFU_DATA_PACKET = 4;
    DFU_STOP_DATA_PACKET = 5;

    DATA_INTEGRITY_CHECK_PRESENT = 1;
    RELIABLE_PACKET = 1;
    HCI_PACKET_TYPE = 14;

    FLASH_PAGE_SIZE = 4096;
    FLASH_PAGE_ERASE_TIME = 0.0897;
    FLASH_WORD_WRITE_TIME = 0.000100;
    FLASH_PAGE_WRITE_TIME = (this.FLASH_PAGE_SIZE/4) * this.FLASH_WORD_WRITE_TIME;

    // The DFU packet max size
    DFU_PACKET_MAX_SIZE = 512;

    // How long to wait for the bootloader to ACK a reliable packet before
    // giving up. A real bootloader ACKs almost instantly (it's a link-layer
    // ack, not "erase/write complete") — a device that isn't actually in
    // DFU mode just never responds at all, so this is what turns "silently
    // hangs" into a clear, fast failure.
    ACK_READ_TIMEOUT = 5.0;

    constructor(serialPort) {
        this.serialPort = serialPort;
        this.sequenceNumber = 0;
        this.lastAck = -1;
        this.pendingAckBytes = [];
        this.sd_size = 0;
        this.total_size = 0;
    }

    /**
     * Waits for the provided milliseconds, and then resolves.
     * @param millis
     * @returns {Promise<void>}
     */
    async sleepMillis(millis) {
        await new Promise((resolve) => {
            setTimeout(resolve, millis);
        });
    }

    /**
     * Writes the provided data to the Serial Port, then waits for the
     * bootloader to ACK it. This is the real confirmation that the device is
     * actually in DFU mode and processing what we send — see getAck() below.
     * @param data
     * @returns {Promise<number>} the ACK sequence number
     */
    async sendPacket(data) {
        const writer = this.serialPort.writable.getWriter();
        try {
            await writer.write(new Uint8Array(data));
        } finally {
            writer.releaseLock();
        }
        return await this.getAck();
    }

    /**
     * Wraps the serial reader with a timeout: if the bootloader doesn't ACK
     * within ACK_READ_TIMEOUT, that means nothing is listening on the other
     * end (e.g. the device never actually entered DFU mode) rather than the
     * transfer just being slow.
     * @returns {{read: function(): Promise<*>, releaseLock: function(): void}}
     */
    getSerialReader() {
        const reader = this.serialPort.readable.getReader();
        return {
            read: () => new Promise((resolve, reject) => {
                const timeoutHandle = setTimeout(() => {
                    reader.releaseLock();
                    reject(new Error('Timed out waiting for a response from the DFU bootloader — device may not actually be in DFU mode'));
                }, this.ACK_READ_TIMEOUT * 1000);
                reader.read().then((result) => {
                    clearTimeout(timeoutHandle);
                    resolve(result);
                }).catch((e) => {
                    clearTimeout(timeoutHandle);
                    reject(e);
                });
            }),
            releaseLock: () => reader.releaseLock(),
        };
    }

    /**
     * Reads and validates the ACK for the packet just sent by sendPacket().
     * The Adafruit/Nordic legacy serial DFU bootloader ACKs every reliable
     * packet with a SLIP-framed response carrying a rolling 3-bit sequence
     * number; if it never responds (timeout) or the sequence comes back
     * wrong, we're definitely not talking to a working DFU bootloader —
     * this is real verification, not a guess based on when things happened
     * to go wrong.
     * @returns {Promise<number>} the ACK sequence number
     */
    async getAck() {
        if (!this.serialPort.readable) {
            throw new Error('Serial port not open or not readable.');
        }

        // Start from any bytes left over from the previous call. A single
        // underlying reader.read() isn't guaranteed to return exactly one
        // SLIP frame — the browser/OS can batch more than one USB transfer
        // into one chunk — so if we always started from an empty buffer,
        // any trailing bytes past this frame's closing 0xC0 would be
        // silently discarded, permanently desyncing frame-boundary
        // detection for every ACK after that.
        const buffer = this.pendingAckBytes || [];
        this.pendingAckBytes = [];
        let c0Count = buffer.reduce((n, b) => n + (b === 0xC0 ? 1 : 0), 0);

        const reader = this.getSerialReader();
        try {
            while (c0Count < 2) {
                const { value, done } = await reader.read();
                if (done) {
                    throw new Error('Stream closed before receiving full ACK from DFU bootloader.');
                }
                if (value) {
                    for (const byte of value) {
                        buffer.push(byte);
                        if (byte === 0xC0) c0Count++;
                    }
                }
            }
        } catch(e) {
            this.sequenceNumber = 0;
            this.lastAck = -1;
            this.pendingAckBytes = [];
            throw e;
        } finally {
            reader.releaseLock();
        }

        // extract the SLIP frame between the two 0xC0 delimiters, ignoring
        // any stale bytes that arrived before the opening delimiter
        const firstC0 = buffer.indexOf(0xC0);
        const secondC0 = buffer.indexOf(0xC0, firstC0 + 1);
        if (firstC0 === -1 || secondC0 === -1) {
            throw new Error('Received incomplete ACK from DFU bootloader.');
        }

        // stash anything read past this frame's closing delimiter for the next call
        this.pendingAckBytes = buffer.slice(secondC0 + 1);

        const decoded = this.slipDecodeEscChars(buffer.slice(firstC0 + 1, secondC0));
        if (decoded.length < 2) {
            throw new Error('Received incomplete ACK from DFU bootloader.');
        }

        const ack = (decoded[0] >> 3) & 0x07;
        const expected = (this.lastAck + 1) % 8;
        if (this.lastAck !== -1 && ack !== expected) {
            this.sequenceNumber = 0;
            this.lastAck = -1;
            throw new Error(`Invalid ACK sequence from DFU bootloader (expected ${expected}, got ${ack})`);
        }
        this.lastAck = ack;

        return ack;
    }

    /**
     * Puts an nRF52 board into DFU mode by quickly opening and closing a serial port.
     * @returns {Promise<void>}
     */
    async enterDfuMode() {

        // open port
        await this.serialPort.open({
            baudRate: this.DFU_TOUCH_BAUD,
        });

        // wait SERIAL_PORT_OPEN_WAIT_TIME before closing port
        await this.sleepMillis(this.SERIAL_PORT_OPEN_WAIT_TIME * 1000);

        // Explicitly drop DTR/RTS before closing. The classic Arduino/
        // Adafruit "1200bps touch" is actually triggered by the USB CDC
        // line state's DTR going low while the baud is 1200 (mimicking a
        // terminal hanging up) — not merely by the port object going away.
        // Some OS serial stacks lower DTR implicitly on close(); WebSerial
        // doesn't guarantee that, so send it explicitly to make sure the
        // firmware's touch-detection callback actually sees the signal.
        try {
            await this.serialPort.setSignals({ dataTerminalReady: false, requestToSend: false });
        } catch(e) { /* not fatal — some ports don't support setSignals */ }

        // close port
        await this.serialPort.close();

        // wait TOUCH_RESET_WAIT_TIME for device to enter into DFU mode
        await this.sleepMillis(this.TOUCH_RESET_WAIT_TIME * 1000);

    }

    /**
     * Flashes the provided firmware zip.
     * @param firmwareZipBlob
     * @param progressCallback
     * @returns {Promise<void>}
     */
    async flash(firmwareZipBlob, progressCallback) {

        // read zip file
        const blobReader = new window.zip.BlobReader(firmwareZipBlob);
        const zipReader = new window.zip.ZipReader(blobReader);
        const zipEntries = await zipReader.getEntries();

        // find manifest file
        const manifestFile = zipEntries.find((zipEntry) => zipEntry.filename === "manifest.json");
        if(!manifestFile){
            throw "manifest.json not found in firmware file!";
        }

        // read manifest file as text
        const text = await manifestFile.getData(new window.zip.TextWriter());

        // parse manifest json
        const json = JSON.parse(text);
        const manifest = json.manifest;

        // todo softdevice_bootloader
        // if self.manifest.softdevice_bootloader:
        // self._dfu_send_image(HexType.SD_BL, self.manifest.softdevice_bootloader)

        // todo softdevice
        // if self.manifest.softdevice:
        // self._dfu_send_image(HexType.SOFTDEVICE, self.manifest.softdevice)

        // todo bootloader
        // if self.manifest.bootloader:
        // self._dfu_send_image(HexType.BOOTLOADER, self.manifest.bootloader)

        // flash application image
        if(manifest.application){
            await this.dfuSendImage(this.HEX_TYPE_APPLICATION, zipEntries, manifest.application, progressCallback);
        }

    }

    /**
     * Sends the firmware image to the device in DFU mode.
     * @param programMode
     * @param zipEntries
     * @param firmwareManifest
     * @param progressCallback
     * @returns {Promise<void>}
     */
    async dfuSendImage(programMode, zipEntries, firmwareManifest, progressCallback) {

        // open port
        try {
            await this.serialPort.open({
                baudRate: this.FLASH_BAUD,
            });
        } catch(e) {
            e.dfuPhase = 'open';
            throw e;
        }

        // wait SERIAL_PORT_OPEN_WAIT_TIME
        await this.sleepMillis(this.SERIAL_PORT_OPEN_WAIT_TIME * 1000);

        // file sizes
        var softdeviceSize = 0
        var bootloaderSize = 0
        var applicationSize = 0

        // read bin file (firmware)
        const binFile = zipEntries.find((zipEntry) => zipEntry.filename === firmwareManifest.bin_file);
        const firmware = await binFile.getData(new window.zip.Uint8ArrayWriter());

        // read dat file (init packet)
        const datFile = zipEntries.find((zipEntry) => zipEntry.filename === firmwareManifest.dat_file);
        const init_packet = await datFile.getData(new window.zip.Uint8ArrayWriter());

        // only support flashing application for now
        if(programMode !== this.HEX_TYPE_APPLICATION){
            throw "not implemented";
        }

        // determine application size
        if(programMode === this.HEX_TYPE_APPLICATION){
            applicationSize = firmware.length;
        }

        // Each phase below is tagged with e.dfuPhase on failure, so the UI can
        // tell "never got going" (start) apart from "bootloader accepted the
        // handshake, then rejected the firmware" (init/data). sendPacket()
        // now reads back and validates the bootloader's ACK for every packet,
        // so a 'start'-phase failure almost always means the device wasn't
        // actually in DFU mode at all (no ACK ever came back), while an
        // init/data-phase failure means it was — and rejected something.
        console.log("Sending DFU start packet");
        try {
            await this.sendStartDfu(programMode, softdeviceSize, bootloaderSize, applicationSize);
        } catch(e) {
            e.dfuPhase = e.dfuPhase || 'start';
            throw e;
        }

        console.log("Sending DFU init packet");
        try {
            await this.sendInitPacket(init_packet);
        } catch(e) {
            e.dfuPhase = e.dfuPhase || 'init';
            throw e;
        }

        console.log("Sending firmware");
        try {
            await this.sendFirmware(firmware, progressCallback);
        } catch(e) {
            e.dfuPhase = e.dfuPhase || 'data';
            throw e;
        }

        // todo
        // sleep(self.dfu_transport.get_activate_wait_time())

    }

    /**
     * Calculates CRC16 on the provided binaryData
     * @param {Uint8Array} binaryData - Array with data to run CRC16 calculation on
     * @param {number} crc - CRC value to start calculation with
     * @return {number} - Calculated CRC value of binaryData
     */
    calcCrc16(binaryData, crc = 0xffff) {

        if(!(binaryData instanceof Uint8Array)){
            throw new Error("calcCrc16 requires Uint8Array input");
        }

        for(let b of binaryData){
            crc = (crc >> 8 & 0x00FF) | (crc << 8 & 0xFF00);
            crc ^= b;
            crc ^= (crc & 0x00FF) >> 4;
            crc ^= (crc << 8) << 4;
            crc ^= ((crc & 0x00FF) << 4) << 1;
        }

        return crc & 0xFFFF;

    }

    /**
     * Encode esc characters in a SLIP package.
     * Replace 0xC0 with 0xDBDC and 0xDB with 0xDBDD.
     * @param dataIn
     * @returns {*[]}
     */
    slipEncodeEscChars(dataIn) {

        let result = [];

        for(let i = 0; i < dataIn.length; i++){
            let char = dataIn[i];
            if(char === 0xC0){
                result.push(0xDB);
                result.push(0xDC);
            } else if(char === 0xDB) {
                result.push(0xDB);
                result.push(0xDD);
            } else {
                result.push(char);
            }
        }

        return result;

    }

    /**
     * Decodes SLIP escape sequences (inverse of slipEncodeEscChars):
     * 0xDB 0xDC -> 0xC0, 0xDB 0xDD -> 0xDB.
     * @param dataIn
     * @returns {number[]}
     */
    slipDecodeEscChars(dataIn) {

        let result = [];

        for(let i = 0; i < dataIn.length; i++){
            let char = dataIn[i];
            if(char === 0xDB){
                const next = dataIn[++i];
                if(next === 0xDC){
                    result.push(0xC0);
                } else if(next === 0xDD) {
                    result.push(0xDB);
                } else {
                    throw new Error(`Invalid SLIP escape sequence: DB followed by 0x${(next ?? 0).toString(16)}`);
                }
            } else {
                result.push(char);
            }
        }

        return result;

    }

    /**
     * Creates an HCI packet from the provided frame data.
     * https://github.com/adafruit/Adafruit_nRF52_nrfutil/blob/master/nordicsemi/dfu/dfu_transport_serial.py#L332
     * @param frame
     * @returns {*[]}
     */
    createHciPacketFromFrame(frame) {

        // increase sequence number, but roll over at 8
        this.sequenceNumber = (this.sequenceNumber + 1) % 8;

        // create slip header
        const slipHeaderBytes = this.createSlipHeader(
            this.sequenceNumber,
            this.DATA_INTEGRITY_CHECK_PRESENT,
            this.RELIABLE_PACKET,
            this.HCI_PACKET_TYPE,
            frame.length,
        );

        // create packet data
        let data = [
            ...slipHeaderBytes,
            ...frame,
        ];

        // add crc of data
        const crc = this.calcCrc16(new Uint8Array(data), 0xffff);
        data.push(crc & 0xFF);
        data.push((crc & 0xFF00) >> 8);

        // add escape characters
        return [
            0xc0,
            ...this.slipEncodeEscChars(data),
            0xc0,
        ];

    }

    /**
     * Calculate how long we should wait for erasing data.
     * @returns {number}
     */
    getEraseWaitTime() {
        // always wait at least 0.5 seconds
        return Math.max(0.5, ((this.total_size / this.FLASH_PAGE_SIZE) + 1) * this.FLASH_PAGE_ERASE_TIME);
    }

    /**
     * Constructs the image size packet sent in the DFU Start packet.
     * @param softdeviceSize
     * @param bootloaderSize
     * @param appSize
     * @returns {number[]}
     */
    createImageSizePacket(softdeviceSize = 0, bootloaderSize = 0, appSize = 0) {
        return [
            ...this.int32ToBytes(softdeviceSize),
            ...this.int32ToBytes(bootloaderSize),
            ...this.int32ToBytes(appSize),
        ];
    }

    /**
     * Sends the DFU Start packet to the device.
     * @param mode
     * @param softdevice_size
     * @param bootloader_size
     * @param app_size
     * @returns {Promise<number>} the ACK sequence number
     */
    async sendStartDfu(mode, softdevice_size = 0, bootloader_size = 0, app_size = 0){

        // create frame
        const frame = [
            ...this.int32ToBytes(this.DFU_START_PACKET),
            ...this.int32ToBytes(mode),
            ...this.createImageSizePacket(softdevice_size, bootloader_size, app_size),
        ];

        // send hci packet
        const ack = await this.sendPacket(this.createHciPacketFromFrame(frame));

        // remember file sizes for calculating erase wait time
        this.sd_size = softdevice_size;
        this.total_size = softdevice_size + bootloader_size + app_size;

        // wait for initial erase
        await this.sleepMillis(this.getEraseWaitTime() * 1000);

        return ack;

    }

    /**
     * Sends the DFU Init packet to the device.
     * @param initPacket
     * @returns {Promise<void>}
     */
    async sendInitPacket(initPacket){

        // create frame
        const frame = [
            ...this.int32ToBytes(this.DFU_INIT_PACKET),
            ...initPacket,
            ...this.int16ToBytes(0x0000), // padding required
        ];

        // send hci packet
        await this.sendPacket(this.createHciPacketFromFrame(frame));

    }

    /**
     * Sends the firmware file to the device in multiple chunks.
     * @param firmware
     * @param progressCallback
     * @returns {Promise<void>}
     */
    async sendFirmware(firmware, progressCallback) {

        const packets = [];
        var packetsSent = 0;

        // chunk firmware into separate packets
        for(let i = 0; i < firmware.length; i += this.DFU_PACKET_MAX_SIZE){
            packets.push(this.createHciPacketFromFrame([
                ...this.int32ToBytes(this.DFU_DATA_PACKET),
                ...firmware.slice(i, i + this.DFU_PACKET_MAX_SIZE),
            ]));
        }

        // send initial progress
        if(progressCallback){
            progressCallback(0);
        }

        // send each packet one after the other
        for(var i = 0; i < packets.length; i++){

            // send packet
            await this.sendPacket(packets[i]);

            // wait a bit to allow device to write before sending next packet
            await this.sleepMillis(this.FLASH_PAGE_WRITE_TIME * 1000);

            // update progress
            packetsSent++;
            if(progressCallback){
                const progress = Math.floor((packetsSent / packets.length) * 100);
                progressCallback(progress);
            }

        }

        // finished sending firmware, send DFU Stop Data packet
        await this.sendPacket(this.createHciPacketFromFrame([
            ...this.int32ToBytes(this.DFU_STOP_DATA_PACKET),
        ]));

    }

    /**
     * Creates a SLIP header.
     *
     * For a description of the SLIP header go to:
     * http://developer.nordicsemi.com/nRF51_SDK/doc/7.2.0/s110/html/a00093.html
     *
     * @param {number} seq - Packet sequence number
     * @param {number} dip - Data integrity check
     * @param {number} rp - Reliable packet
     * @param {number} pktType - Payload packet
     * @param {number} pktLen - Packet length
     * @return {Uint8Array} - SLIP header
     */
    createSlipHeader(seq, dip, rp, pktType, pktLen) {
        let ints = [0, 0, 0, 0];
        ints[0] = seq | (((seq + 1) % 8) << 3) | (dip << 6) | (rp << 7);
        ints[1] = pktType | ((pktLen & 0x000F) << 4);
        ints[2] = (pktLen & 0x0FF0) >> 4;
        ints[3] = (~(ints[0] + ints[1] + ints[2]) + 1) & 0xFF;
        return new Uint8Array(ints);
    }

    /**
     * Converts the provided int32 to 4 bytes.
     * @param num
     * @returns {number[]}
     */
    int32ToBytes(num){
        return [
            (num & 0x000000ff),
            (num & 0x0000ff00) >> 8,
            (num & 0x00ff0000) >> 16,
            (num & 0xff000000) >> 24,
        ];
    }

    /**
     * Converts the provided int16 to 2 bytes.
     * @param num
     * @returns {number[]}
     */
    int16ToBytes(num){
        return [
            (num & 0x00FF),
            (num & 0xFF00) >> 8,
        ];
    }

}
