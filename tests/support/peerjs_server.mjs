// A PeerJS server for tests, following peers/peerjs-server: clients register an ID with a key
// and token, the server sets each message's sender, queues messages for an absent ID and, when
// it does not come within expireMs, tells the sender with EXPIRE.
import { startWebSocketServer } from "./websocket_server.mjs";

const forwarded = ["OFFER", "ANSWER", "CANDIDATE", "LEAVE", "EXPIRE"];

export async function startPeerJsServer({ key = "peerjs", expireMs = 300 } = {}) {
    const clients = new Map(); // id -> { connection, token }
    const queued = new Map(); // dst -> [{ message, timer }]
    const peerjs = { clients, messages: 0, expired: 0 };
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
            connection.send(JSON.stringify({ type: "ID-TAKEN", payload: { msg: "ID is taken" } }));
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
            if (!forwarded.includes(message.type)) return;
            ++peerjs.messages;
            deliver(message);
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
