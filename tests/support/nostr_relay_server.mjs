// A minimal Nostr relay (NIP-01, NIP-40 expiration) for tests. It checks event ids and
// signatures, so tests prove that the backend's events are well formed.
import { createHash } from "node:crypto";
import { verify } from "../../src/browser/schnorr.mjs";
import { startWebSocketServer } from "./websocket_server.mjs";

const hexBytes = (text) => Uint8Array.from(text.match(/../g) ?? [], (pair) => parseInt(pair, 16));
const tag = (event, name) => event.tags.find((entry) => entry[0] === name)?.[1];
const expired = (event) => Number(tag(event, "expiration") ?? Infinity) <= Date.now() / 1000;

function matches(filter, event) {
    if (filter.kinds && !filter.kinds.includes(event.kind)) return false;
    if (filter["#x"] && !filter["#x"].includes(tag(event, "x"))) return false;
    return !(filter.since !== undefined && event.created_at < filter.since);
}

async function valid(event) {
    const serial = JSON.stringify([0, event.pubkey, event.created_at, event.kind, event.tags, event.content]);
    const id = createHash("sha256").update(serial).digest("hex");
    return id === event.id && await verify(hexBytes(event.id), hexBytes(event.pubkey), hexBytes(event.sig));
}

export async function startNostrRelay() {
    const events = [], subscriptions = new Set();
    const relay = { events, refuseNext: null, closeNextRequest: null, invalid: 0 };
    async function publish(connection, event) {
        if (relay.refuseNext) { connection.send(JSON.stringify(["OK", event.id, false, relay.refuseNext])); relay.refuseNext = null; return; }
        if (!(await valid(event))) { ++relay.invalid; connection.send(JSON.stringify(["OK", event.id, false, "invalid: bad id or signature"])); return; }
        events.push(event);
        connection.send(JSON.stringify(["OK", event.id, true, ""]));
        for (const subscription of subscriptions)
            if (matches(subscription.filter, event)) subscription.connection.send(JSON.stringify(["EVENT", subscription.id, event]));
    }
    function request(connection, id, filter) {
        if (relay.closeNextRequest) { connection.send(JSON.stringify(["CLOSED", id, relay.closeNextRequest])); relay.closeNextRequest = null; return; }
        const found = events.filter((event) => !expired(event) && matches(filter, event)).slice(-(filter.limit ?? 500));
        for (const event of found) connection.send(JSON.stringify(["EVENT", id, event]));
        connection.send(JSON.stringify(["EOSE", id]));
        subscriptions.add({ connection, id, filter });
    }
    const server = await startWebSocketServer((connection) => {
        connection.onMessage = (text) => {
            const [type, first, second] = JSON.parse(text);
            if (type === "EVENT") void publish(connection, first);
            else if (type === "REQ") request(connection, first, second);
            else if (type === "CLOSE") for (const s of subscriptions) if (s.connection === connection && s.id === first) subscriptions.delete(s);
        };
        connection.onClose = () => { for (const s of subscriptions) if (s.connection === connection) subscriptions.delete(s); };
    });
    return Object.assign(relay, { url: server.url("/"), server, subscriptions, close: () => server.close() });
}
