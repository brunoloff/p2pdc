// The unreliable data channel of one browser connection: unordered, never retransmitted.
// Anything that does not fit its limits is dropped; it never fails the connection.
export class BrowserDatagramChannel {
    static label = "p2pdc/datagram/v1";
    static maxBytes = 1024;
    static maxBufferedBytes = 65536;
    constructor(onDatagram) {
        this.onDatagram = onDatagram;
        this.channel = null;
    }
    open(peer) {
        this.attach(peer.createDataChannel(BrowserDatagramChannel.label, { ordered: false, maxRetransmits: 0 }));
    }
    adopt(channel) {
        if (this.channel || channel.ordered || channel.maxRetransmits !== 0 ||
            channel.maxPacketLifeTime !== null) return false;
        this.attach(channel);
        return true;
    }
    attach(channel) {
        this.channel = channel;
        channel.binaryType = "arraybuffer";
        channel.onmessage = (event) => {
            if (event.data instanceof ArrayBuffer && event.data.byteLength <= BrowserDatagramChannel.maxBytes)
                this.onDatagram(new Uint8Array(event.data).slice());
        };
    }
    isOpen() { return this.channel?.readyState === "open"; }
    send(bytes) {
        if (!(bytes instanceof Uint8Array)) throw new TypeError("binary message required");
        if (bytes.length > BrowserDatagramChannel.maxBytes) return "tooLarge";
        if (!this.isOpen()) return "notReady";
        if (this.channel.bufferedAmount > BrowserDatagramChannel.maxBufferedBytes) return "backpressure";
        try { this.channel.send(bytes.slice()); return "accepted"; }
        catch { return "failed"; }
    }
    close() {
        if (!this.channel) return;
        this.channel.onmessage = null;
        this.channel.close();
        this.channel = null;
    }
}
