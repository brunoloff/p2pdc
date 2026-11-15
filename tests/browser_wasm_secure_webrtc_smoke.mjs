import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { BrowserConnection } from "../src/browser/browser_connection.mjs";

const fromHex = (value) => Uint8Array.from(value.match(/../g), (byte) => parseInt(byte, 16));
const keys = [
    { sign: "8a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c",
      box: "5dfedd3b6bd47f6fa28ee15d969d5bb0ea53774d488bdaf9df1c6e0124b3ef22" },
    { sign: "8139770ea87d175f56a35466c34c7ecccb8d8a91b4ee37a25df60f5b8fc9b394",
      box: "ac01b2209e86354fb853237b5de0f4fab13c7fcbf433a61c019369617fecf10b" },
];
const options = (index) => {
    const role = index ? "answerer" : "offerer";
    return { secure: { role, timeoutMs: 10000, identitySeed: new Uint8Array(32).fill(index + 1),
        recipientSecret: new Uint8Array(32).fill(index + 3), capability: new Uint8Array(32).fill(5),
        remoteSigningKey: fromHex(keys[1 - index].sign), remoteBoxKey: fromHex(keys[1 - index].box),
        localInstance: new Uint8Array(16).fill(index + 1), remoteInstance: new Uint8Array(16).fill(2 - index),
        attemptId: new Uint8Array(16).fill(3) }, transport: { role, relayPolicy: "automatic" } };
};
const payloads = [Uint8Array.from({ length: 70000 }, (_, i) => i % 251),
                  Uint8Array.from({ length: 90000 }, (_, i) => (i * 7) % 256)];
let connections = [], resolve, reject, ready = 0;
const received = [false, false];
const complete = new Promise((yes, no) => { resolve = yes; reject = no; });
const handlers = (index) => ({
    onSignal: (wire) => {
        if (connections[1 - index].receive(wire) !== "accepted") throw new Error("signal rejected");
    },
    onState: (state) => {
        if (state === "ready" && ++ready === 2 &&
            (connections[0].send(payloads[0]) !== "accepted" || connections[1].send(payloads[1]) !== "accepted"))
            reject(new Error("application send failed"));
    },
    onMessage: (value) => {
        const expected = payloads[1 - index];
        if (received[index] || value.length !== expected.length || !value.every((byte, i) => byte === expected[i]))
            reject(new Error("application message mismatch"));
        received[index] = true;
        if (received.every(Boolean)) resolve();
    },
    onRoute: () => {}, onWritable: () => {}, onError: (reason) => reject(new Error(reason)),
});
try {
    const module = await createModule();
    connections = [0, 1].map((index) => new BrowserConnection(module, options(index), handlers(index)));
    const timeout = setTimeout(() => reject(new Error("authenticated WebRTC timed out")), 10000);
    await connections[1].start(); await connections[0].start(); await complete; clearTimeout(timeout);
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { for (const connection of connections) connection.close(); }
