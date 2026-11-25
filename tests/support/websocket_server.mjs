// A small WebSocket server (RFC 6455) on loopback for tests, using only Node's built-ins.
// Handles unfragmented text and binary messages, ping and close.
import { createHash } from "node:crypto";
import { createServer } from "node:http";
import { createServer as createSecureServer } from "node:https";

const acceptSuffix = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

function frame(opcode, payload) {
    const length = payload.length;
    const header = length < 126 ? Buffer.from([0x80 | opcode, length]) :
        length < 65536 ? Buffer.from([0x80 | opcode, 126, length >> 8, length & 255]) :
        Buffer.concat([Buffer.from([0x80 | opcode, 127]), (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(length)); return b; })()]);
    return Buffer.concat([header, payload]);
}

// Splits complete frames off the front of buffer; returns them and what is left.
function parseFrames(buffer) {
    const frames = [];
    for (;;) {
        if (buffer.length < 2) break;
        const opcode = buffer[0] & 15, masked = (buffer[1] & 128) !== 0;
        let length = buffer[1] & 127, offset = 2;
        if (length === 126) { if (buffer.length < 4) break; length = buffer.readUInt16BE(2); offset = 4; }
        else if (length === 127) { if (buffer.length < 10) break; length = Number(buffer.readBigUInt64BE(2)); offset = 10; }
        const maskOffset = offset; if (masked) offset += 4;
        if (buffer.length < offset + length) break;
        const payload = Buffer.from(buffer.subarray(offset, offset + length));
        if (masked) for (let index = 0; index < length; ++index) payload[index] ^= buffer[maskOffset + (index & 3)];
        frames.push({ opcode, payload });
        buffer = buffer.subarray(offset + length);
    }
    return { frames, rest: buffer };
}

class Connection {
    constructor(socket, request) {
        this.socket = socket; this.request = request; this.url = new URL(request.url, "http://localhost");
        this.onMessage = () => {}; this.onClose = () => {}; this.closed = false;
        let pending = Buffer.alloc(0);
        socket.on("data", (chunk) => {
            const { frames, rest } = parseFrames(Buffer.concat([pending, chunk]));
            pending = rest;
            for (const { opcode, payload } of frames) this.receive(opcode, payload);
        });
        socket.on("close", () => this.ended());
        socket.on("error", () => this.ended());
    }
    receive(opcode, payload) {
        if (opcode === 8) { this.close(); return; }
        if (opcode === 9) { this.write(frame(10, payload)); return; }
        if (opcode === 1) this.onMessage(payload.toString("utf8"));
        else if (opcode === 2) this.onMessage(new Uint8Array(payload));
    }
    write(data) { if (!this.closed) this.socket.write(data); }
    send(message) {
        this.write(typeof message === "string" ? frame(1, Buffer.from(message, "utf8")) : frame(2, Buffer.from(message)));
    }
    close() {
        if (this.closed) return;
        this.write(frame(8, Buffer.alloc(0)));
        this.socket.end();
        this.ended();
    }
    ended() { if (!this.closed) { this.closed = true; this.socket.destroy(); this.onClose(); } }
}

// Starts a server; accept(connection) is called for each new connection. Answers subprotocol
// requests with the first one offered from `protocols`. With `tls` ({ key, cert }), serves wss://.
export async function startWebSocketServer(accept, protocols = [], tls = null) {
    const connections = new Set();
    let closing = false;
    const refuse = (request, response) => { response.writeHead(426); response.end(); };
    const server = tls ? createSecureServer(tls, refuse) : createServer(refuse);
    server.on("upgrade", (request, socket) => {
        if (closing) { socket.destroy(); return; }
        const key = request.headers["sec-websocket-key"];
        const offered = (request.headers["sec-websocket-protocol"] ?? "").split(",").map((item) => item.trim());
        const protocol = offered.find((item) => protocols.includes(item));
        const digest = createHash("sha1").update(key + acceptSuffix).digest("base64");
        socket.write(["HTTP/1.1 101 Switching Protocols", "Upgrade: websocket", "Connection: Upgrade",
                      `Sec-WebSocket-Accept: ${digest}`, ...(protocol ? [`Sec-WebSocket-Protocol: ${protocol}`] : []),
                      "", ""].join("\r\n"));
        const connection = new Connection(socket, request);
        connections.add(connection);
        accept(connection);
        const accepted = connection.onClose;
        connection.onClose = () => { connections.delete(connection); accepted(); };
    });
    await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
    const port = server.address().port;
    return {
        url: (path = "/") => `${tls ? "wss" : "ws"}://127.0.0.1:${port}${path}`,
        connections,
        dropAll() { for (const connection of [...connections]) connection.close(); },
        // Ends every connection, including those that have not finished their upgrade, which
        // server.close() alone would wait for.
        close: () => new Promise((resolve) => {
            closing = true;
            for (const connection of [...connections]) connection.close();
            server.close(() => resolve());
            server.closeAllConnections();
        }),
    };
}
