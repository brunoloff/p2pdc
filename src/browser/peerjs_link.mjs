import { ProxyError } from "./proxy_codec.mjs";

// Every browser takes data-channel messages of 64 KiB; replies to lookups are longer, so each
// JSON message travels as fragments of at most fragmentChars, each marked "+" when more of the
// message follows and "=" on its last. The channel is ordered and reliable.
const fragmentChars = 16384, maxLinkMessageChars = 2 * 1024 * 1024, maxBufferedBytes = 8 * 1024 * 1024;

// A PeerJS data connection, once its channel is open: JSON messages both ways, until either
// side closes it or WebRTC gives up on the path. Owners set onmessage and onclose.
export class DataLink {
    constructor(peer, connection, channel) {
        this.peer = peer; this.connection = connection; this.channel = channel;
        this.onmessage = () => {}; this.onclose = () => {}; this.released = () => {};
        this.closed = false; this.partial = "";
        channel.onmessage = (event) => this.receive(event.data);
        channel.onclose = () => this.close();
        channel.onerror = () => this.close();
        connection.onconnectionstatechange = () => {
            if (["failed", "closed"].includes(connection.connectionState)) this.close();
        };
    }
    send(message) {
        if (this.closed || this.channel.readyState !== "open") throw new ProxyError("network_error", "data connection closed");
        const text = JSON.stringify(message);
        if (text.length > maxLinkMessageChars) throw new TypeError("message too long for a data connection");
        if (this.channel.bufferedAmount + text.length > maxBufferedBytes) {
            this.close();
            throw new ProxyError("network_error", "data connection is not draining");
        }
        for (let start = 0; start < text.length || start === 0; start += fragmentChars) {
            const more = start + fragmentChars < text.length;
            this.channel.send((more ? "+" : "=") + text.slice(start, start + fragmentChars));
        }
    }
    receive(data) {
        if (this.closed || typeof data !== "string" || !["+", "="].includes(data[0])) { this.close(); return; }
        this.partial += data.slice(1);
        if (this.partial.length > maxLinkMessageChars) { this.close(); return; }
        if (data[0] === "+") return;
        const text = this.partial;
        this.partial = "";
        let message;
        try { message = JSON.parse(text); } catch { this.close(); return; }
        if (message && typeof message === "object" && !Array.isArray(message)) this.onmessage(message);
    }
    close() {
        if (this.closed) return;
        this.closed = true;
        try { this.channel.close(); } catch { /* already closing */ }
        try { this.connection.close(); } catch { /* already closed */ }
        this.released();
        this.onclose();
    }
}
