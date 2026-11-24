// Runs the test servers for native tests (tests/backend_servers.hpp): prints one JSON line
// with their URLs, then answers one line per command read from stdin, and exits when stdin
// closes. Commands: "nostr-refuse <message>", "nostr-close-next <message>",
// "mqtt-refuse-publish <reason>", "mqtt-refuse-subscribe <reason>", "drop-all",
// "peerjs-drop <id>", "stats",
// and, through the browser's own clients (src/browser/), "js-put <kind> <key> <value>",
// "js-get <kind> <key>", "js-listen <kind> <key>" and "js-heard <kind> <key>", where kind is
// nostr or mqtt and keys and values are hex. With P2PDC_TEST_CHROMIUM set, "browser-open"
// starts the browser's PeerJS client in headless Chromium on the PeerJS server, and
// "browser-put <key> <value>", "browser-get <key>", "browser-listen <key>" and
// "browser-heard <key>" use it.
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createInterface } from "node:readline";
import { MqttBroker } from "../../src/browser/mqtt_broker.mjs";
import { NostrRelay } from "../../src/browser/nostr_relay.mjs";
import { storeLimits } from "../../src/browser/store_limits.mjs";
import { startMqttBroker } from "./mqtt_broker_server.mjs";
import { startNostrRelay } from "./nostr_relay_server.mjs";
import { startPeerJsServer } from "./peerjs_server.mjs";
import { startWebSocketServer } from "./websocket_server.mjs";

// A wss:// echo server whose certificate no one trusts, for checking that clients refuse it.
async function startUntrustedServer() {
    const folder = mkdtempSync(join(tmpdir(), "p2pdc-tls-"));
    const key = join(folder, "key.pem"), cert = join(folder, "cert.pem");
    execFileSync("openssl", ["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                             "-nodes", "-subj", "/CN=127.0.0.1", "-days", "1", "-keyout", key, "-out", cert],
                 { stdio: "ignore" });
    const tls = { key: readFileSync(key), cert: readFileSync(cert) };
    rmSync(folder, { recursive: true });
    return startWebSocketServer((connection) => { connection.onMessage = (text) => connection.send(text); }, [], tls);
}

const relay = await startNostrRelay();
const broker = await startMqttBroker();
const untrusted = await startUntrustedServer();
const peerjs = await startPeerJsServer({ expireMs: 1000 });
const silentPeerjs = await startPeerJsServer({ expireMs: 1000, silentTaken: true });
const write = (value) => process.stdout.write(`${JSON.stringify(value)}\n`);
write({ nostr: relay.url, mqtt: broker.url, untrusted: untrusted.url("/"), peerjs: peerjs.url,
        silentPeerjs: silentPeerjs.url });

const limits = storeLimits({ timeoutMs: 5000 });
const clients = { nostr: new NostrRelay(relay.url, limits), mqtt: new MqttBroker(broker.url, limits) };
const heard = new Map(); // "kind key" -> values heard by js-listen, in hex
const fromHex = (text) => Uint8Array.from(text.match(/../g) ?? [], (pair) => parseInt(pair, 16));
const toHex = (bytes) => Buffer.from(bytes).toString("hex");
const words = (rest) => { const [kind, key, value] = rest.split(" "); return { client: clients[kind], kind, key, value }; };
const failed = (error) => ({ error: String(error?.message ?? error) });

// The browser's PeerJS client, in a Chromium page (tests/peerjs_webrtc_page.html).
let page = null;
const inPage = {
    open: async function (url) {
        for (let i = 0; i < 100 && !window.p2pdc; ++i) await new Promise((resolve) => setTimeout(resolve, 50));
        const { PeerJsSwitchboard, storeLimits } = window.p2pdc;
        window.board = new PeerJsSwitchboard(url, "peerjs", storeLimits({ timeoutMs: 10000 }), { iceServers: [] });
        window.heard = {};
        window.bytes = (hex) => Uint8Array.from(hex.match(/../g) ?? [], (pair) => parseInt(pair, 16));
        window.hex = (bytes) => Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
        return { ok: true };
    },
    put: async function (key, value) {
        try { await window.board.put(window.bytes(key), window.bytes(value)); }
        catch (error) { return { error: String(error.message) }; }
        return { ok: true, hosts: window.board.hosts.has(key) };
    },
    get: async function (key) {
        try { return { values: (await window.board.get(window.bytes(key))).map((record) => window.hex(record.bytes)).sort() }; }
        catch (error) { return { error: String(error.message) }; }
    },
    listen: async function (key) {
        const values = window.heard[key] = [];
        const watch = window.board.listen(window.bytes(key), (record) => values.push(window.hex(record.bytes)));
        watch.done.catch(() => {});
        return { ready: await watch.ready };
    },
    heard: (key) => ({ values: [...(window.heard[key] ?? [])].sort() }),
};
const browser = async (name, ...args) => {
    if (!page) return { error: "run browser-open first" };
    return page.evaluate(inPage[name], ...args);
};

const commands = {
    "browser-open": async () => {
        const { openChromiumPage } = await import("./chromium_page.mjs");
        page = await openChromiumPage("tests/peerjs_webrtc_page.html");
        return page.evaluate(inPage.open, peerjs.url);
    },
    "browser-put": (rest) => browser("put", ...rest.split(" ")),
    "browser-get": (rest) => browser("get", rest),
    "browser-listen": (rest) => browser("listen", rest),
    "browser-heard": (rest) => browser("heard", rest),
    "js-put": ({ client, key, value }) => client.put(fromHex(key), fromHex(value)).then(() => ({ ok: true }), failed),
    "js-get": ({ client, key }) => client.get(fromHex(key)).then((records) => ({ values: records.map((record) => toHex(record.bytes)).sort() }), failed),
    "js-listen": ({ client, kind, key }) => {
        const values = []; heard.set(`${kind} ${key}`, values);
        return client.listen(fromHex(key), (record) => values.push(toHex(record.bytes))).ready.then((ready) => ({ ready }));
    },
    "js-heard": ({ kind, key }) => ({ values: [...(heard.get(`${kind} ${key}`) ?? [])].sort() }),
    "nostr-refuse": (rest) => { relay.refuseNext = rest; },
    "nostr-close-next": (rest) => { relay.closeNextRequest = rest; },
    "mqtt-refuse-publish": (rest) => { broker.refuseNextPublish = Number(rest); },
    "mqtt-refuse-subscribe": (rest) => { broker.refuseNextSubscribe = Number(rest); },
    "drop-all": () => { relay.server.dropAll(); broker.server.dropAll(); },
    "peerjs-drop": (rest) => { peerjs.drop(rest); },
    stats: () => ({ nostrEvents: relay.events.length, nostrInvalid: relay.invalid,
                    nostrSubscriptions: relay.subscriptions.size, mqttConnects: broker.connects,
                    mqttRetained: broker.retained.size, peerjsMessages: peerjs.messages,
                    peerjsDropped: peerjs.dropped, peerjsClients: [...peerjs.clients.keys()] }),
};
const lines = createInterface({ input: process.stdin });
lines.on("line", (line) => {
    const space = line.indexOf(" ");
    const name = space < 0 ? line : line.slice(0, space), rest = space < 0 ? "" : line.slice(space + 1);
    const command = commands[name];
    if (!command) { write({ error: `unknown command ${name}` }); return; }
    const answer = name.startsWith("js-") ? command(words(rest)) : command(rest);
    Promise.resolve(answer).then((value) => write(value ?? { ok: true }), (error) => write(failed(error)));
});
lines.on("close", async () => {
    clients.nostr.close(); clients.mqtt.close();
    if (page) { await page.evaluate(() => window.board.close()).catch(() => {}); await page.close(); }
    await Promise.all([relay.close(), broker.close(), untrusted.close(), peerjs.close(), silentPeerjs.close()]);
    process.exit(0);
});
