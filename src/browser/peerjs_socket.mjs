import { ProxyError } from "./proxy_codec.mjs";
import { randomId } from "./store_limits.mjs";
import { SocketChannel } from "./websocket_channel.mjs";

const heartbeatMs = 25000; // the server drops clients silent for 90 s

// One ID registered with a PeerJS server (peers/peerjs-server): messages to that ID arrive
// here, with the sender's ID that the server sets. The server forwards OFFER messages, keeps
// those for an absent ID five seconds, then tells the sender with EXPIRE.
export class PeerJsSocket {
    constructor(url, key, id, handlers, timeoutMs) {
        const address = new URL(url);
        address.search = new URLSearchParams({ key, id, token: randomId() }).toString();
        this.id = id; this.handlers = handlers; this.heartbeat = null; this.registration = null;
        this.channel = new SocketChannel(address.href, { message: (data) => this.receive(data), close: (error) => this.lost(error) },
                                         { timeoutMs });
        this.timeoutMs = timeoutMs;
    }
    // "open" once the ID is ours, "taken" when another client holds it.
    register() {
        if (this.registration) return this.registration.promise;
        let resolve, reject;
        const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
        this.registration = { promise, resolve, reject };
        const timer = setTimeout(() => this.fail(new ProxyError("timeout", "PeerJS server did not register the ID")), this.timeoutMs);
        promise.finally(() => clearTimeout(timer)).catch(() => {});
        this.channel.ready().catch((error) => this.fail(error));
        return promise;
    }
    fail(error) {
        const registration = this.registration;
        this.registration = { promise: Promise.reject(error), resolve() {}, reject() {} };
        this.registration.promise.catch(() => {});
        registration?.reject(error);
    }
    receive(data) {
        let message;
        try { message = JSON.parse(typeof data === "string" ? data : new TextDecoder().decode(data)); } catch { return; }
        if (!message || typeof message !== "object") return;
        if (message.type === "OPEN") {
            this.heartbeat = setInterval(() => this.post({ type: "HEARTBEAT" }), heartbeatMs);
            this.registration?.resolve("open");
        } else if (message.type === "ID-TAKEN") {
            this.registration?.resolve("taken");
            this.close();
        } else if (message.type === "ERROR") {
            const text = String(message.payload?.msg ?? "");
            const error = new ProxyError(/limit/i.test(text) ? "http_error" : "rejected", `PeerJS server: ${text}`);
            if (error.code === "http_error") error.status = 503;
            this.fail(error);
        } else if (message.type === "OFFER" && typeof message.src === "string") this.handlers.message(message.src, message.payload);
        else if ((message.type === "EXPIRE" || message.type === "LEAVE") && typeof message.src === "string")
            this.handlers.gone(message.src);
    }
    post(message) { try { this.channel.send(JSON.stringify(message)); } catch { /* reported by close */ } }
    send(dst, payload) { this.channel.send(JSON.stringify({ type: "OFFER", dst, payload })); }
    // The next register() connects again, under the same ID and token.
    lost(error) {
        clearInterval(this.heartbeat); this.heartbeat = null;
        const registration = this.registration;
        this.registration = null;
        registration?.reject(error);
        this.handlers.close(error);
    }
    close() {
        clearInterval(this.heartbeat); this.heartbeat = null;
        this.channel.close();
    }
}
