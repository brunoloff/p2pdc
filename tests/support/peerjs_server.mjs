// A PeerJS server for tests, following peers/peerjs-server: clients register an ID with a key
// and token, the server sets each message's sender, queues messages for an absent ID and, when
// it does not come within expireMs, tells the sender with EXPIRE. Like the public
// 0.peerjs.com it relays only messages shaped as PeerJS WebRTC signalling (strict) and drops
// the rest, counting them in dropped.
import { startWebSocketServer } from "./websocket_server.mjs";

const forwarded = ["OFFER", "ANSWER", "CANDIDATE", "LEAVE", "EXPIRE"];
const description = (sdp, type) => sdp?.type === type && typeof sdp.sdp === "string";
function genuine(message) {
    const payload = message.payload;
    if (!payload || !["data", "media"].includes(payload.type) || typeof payload.connectionId !== "string") return false;
    if (message.type === "OFFER") return description(payload.sdp, "offer");
    if (message.type === "ANSWER") return description(payload.sdp, "answer");
    if (message.type === "CANDIDATE") return typeof payload.candidate?.candidate === "string";
    return true;
}

// delayAnswersMs holds ANSWERs back, so that candidates overtake them. silentTaken closes a
// connection for a taken ID without sending ID-TAKEN first, as 0.peerjs.com does at times.
export async function startPeerJsServer({ key = "peerjs", expireMs = 300, strict = true, delayAnswersMs = 0,
                                          silentTaken = false } = {}) {
    const clients = new Map(); // id -> { connection, token }
    const queued = new Map(); // dst -> [{ message, timer }]
    const peerjs = { clients, messages: 0, expired: 0, dropped: 0, types: new Map() };
    function deliver(message) {
        const client = clients.get(message.dst);
        if (client) { client.connection.send(JSON.stringify(message)); return; }
        if (["LEAVE", "EXPIRE"].includes(message.type) || !message.dst) return;
        const entry = { message, timer: setTimeout(() => {
            const list = queued.get(message.dst) ?? [];
            list.splice(list.indexOf(entry), 1);
            ++peerjs.expired;
            clients.get(message.src)?.connection.send(JSON.stringify({ type: "EXPIRE", src: message.dst, dst: message.src }));
        }, expireMs) };
        queued.set(message.dst, [...(queued.get(message.dst) ?? []), entry]);
    }
    function accept(connection) {
        const { key: offered, id, token } = Object.fromEntries(connection.url.searchParams);
        if (!id || !token || offered !== key) {
            connection.send(JSON.stringify({ type: "ERROR", payload: { msg: "Invalid key provided" } }));
            connection.close(); return;
        }
        const existing = clients.get(id);
        if (existing && existing.token !== token) {
            if (!silentTaken) connection.send(JSON.stringify({ type: "ID-TAKEN", payload: { msg: "ID is taken" } }));
            connection.close(); return;
        }
        const client = { connection, token };
        clients.set(id, client);
        connection.send(JSON.stringify({ type: "OPEN" }));
        for (const entry of queued.get(id) ?? []) { clearTimeout(entry.timer); connection.send(JSON.stringify(entry.message)); }
        queued.delete(id);
        connection.onMessage = (text) => {
            const message = JSON.parse(text);
            message.src = id;
            if (message.type === "HEARTBEAT") return;
            if (!forwarded.includes(message.type) || (strict && !genuine(message))) { ++peerjs.dropped; return; }
            ++peerjs.messages;
            peerjs.types.set(message.type, (peerjs.types.get(message.type) ?? 0) + 1);
            if (message.type === "ANSWER" && delayAnswersMs) setTimeout(() => deliver(message), delayAnswersMs);
            else deliver(message);
        };
        connection.onClose = () => { if (clients.get(id) === client) clients.delete(id); };
    }
    const server = await startWebSocketServer(accept);
    return Object.assign(peerjs, {
        url: server.url("/peerjs"), server,
        drop: (id) => clients.get(id)?.connection.close(),
        close: () => { for (const list of queued.values()) for (const entry of list) clearTimeout(entry.timer); return server.close(); },
    });
}
