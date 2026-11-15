import createModule from "../build-wasm-auth/p2pdc_browser_secure.mjs";
import { WasmSecureLink } from "../src/browser/wasm_secure_link.mjs";

const fromHex = (value) => Uint8Array.from(value.match(/../g), (byte) => parseInt(byte, 16));
const keys = [
    { sign: "8a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c",
      box: "5dfedd3b6bd47f6fa28ee15d969d5bb0ea53774d488bdaf9df1c6e0124b3ef22" },
    { sign: "8139770ea87d175f56a35466c34c7ecccb8d8a91b4ee37a25df60f5b8fc9b394",
      box: "ac01b2209e86354fb853237b5de0f4fab13c7fcbf433a61c019369617fecf10b" },
];
const now = Math.floor(Date.now() / 1000);
const options = (index) => ({ role: index ? "answerer" : "offerer", timeoutMs: 8000,
    identitySeed: new Uint8Array(32).fill(index + 1), recipientSecret: new Uint8Array(32).fill(index + 3),
    capability: new Uint8Array(32).fill(5), remoteSigningKey: fromHex(keys[1 - index].sign),
    remoteBoxKey: fromHex(keys[1 - index].box), localInstance: new Uint8Array(16).fill(index + 1),
    remoteInstance: new Uint8Array(16).fill(2 - index), attemptId: new Uint8Array(16).fill(3) });
let links = [], resolve, reject, ready = 0, tamperChecked = false;
const received = [false, false];
const complete = new Promise((yes, no) => { resolve = yes; reject = no; });
const payloads = [Uint8Array.from({ length: 7000 }, (_, i) => i % 251),
                  Uint8Array.from({ length: 9000 }, (_, i) => (i * 7) % 256)];
const handlers = (index) => ({
    sendFrame: (frame) => { queueMicrotask(() => { if (!links[1 - index].onFrame(frame)) reject(new Error("frame rejected")); }); return "accepted"; },
    bufferedAmount: () => 0,
    onSignal: (wire) => {
        if (!tamperChecked) {
            const changed = wire.slice(); changed[changed.length - 1] ^= 1;
            if (links[1 - index].receive(changed, now) !== "ignored") throw new Error("tampered signaling accepted");
            tamperChecked = true;
        }
        if (links[1 - index].receive(wire, now) !== "accepted") throw new Error("signaling rejected");
    },
    onDescription: async () => {}, onCandidate: async () => {}, onControl: () => {},
    onReady: () => {
        if (++ready === 2) {
            if (links[0].send(payloads[0]) !== "accepted" || links[1].send(payloads[1]) !== "accepted")
                reject(new Error("application send rejected"));
        }
    },
    onMessage: (value) => {
        const expected = payloads[1 - index];
        if (received[index] || value.length !== expected.length || !value.every((byte, i) => byte === expected[i]))
            reject(new Error("application message mismatch"));
        received[index] = true;
        if (received.every(Boolean)) resolve();
    },
    onWritable: () => {}, onFailure: (reason) => reject(new Error(reason)),
});
try {
    const module = await createModule();
    links = [new WasmSecureLink(module, options(0), handlers(0)), new WasmSecureLink(module, options(1), handlers(1))];
    if (links[0].localDescription(Uint8Array.of(1), now) !== "accepted" ||
        links[1].localDescription(Uint8Array.of(2), now) !== "accepted") throw new Error("local signaling failed");
    await new Promise((done) => setTimeout(done, 100));
    if (!links[0].setTransportReady(true, 65536) || !links[1].setTransportReady(true, 65536))
        throw new Error("transport readiness rejected");
    const timeout = setTimeout(() => reject(new Error("secure bridge timed out")), 8000);
    await complete; clearTimeout(timeout);
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    document.body.dataset.result = "fail"; document.body.textContent = `fail: ${error.message}`;
} finally { for (const link of links) link.close(); }
