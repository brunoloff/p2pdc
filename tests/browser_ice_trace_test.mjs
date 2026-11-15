import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import vm from "node:vm";

test("ICE trace emits categories and states without candidate addresses", async () => {
    const lines = [];
    class Peer {
        constructor() { this.listeners = {}; this.iceConnectionState = "new"; }
        addEventListener(name, callback) { this.listeners[name] = callback; }
        addIceCandidate(value) { return value; }
        async setRemoteDescription() { this.canTrickleIceCandidates = true; }
    }
    const context = { RTCPeerConnection: Peer, performance: { now: () => 42 },
                      console: { info: (line) => lines.push(line) } };
    vm.runInNewContext(await readFile(new URL("./browser_ice_trace.js", import.meta.url), "utf8"), context);
    const peer = new context.RTCPeerConnection();
    peer.listeners.icecandidate({ candidate: { candidate: "candidate:1 1 udp 1 192.0.2.1 5 typ relay" } });
    peer.addIceCandidate({ candidate: "bad 198.51.100.1" });
    peer.listeners.icecandidate({ candidate: null });
    peer.iceConnectionState = "failed"; peer.listeners.iceconnectionstatechange();
    await peer.setRemoteDescription({ sdp: "a=ice-options:trickle\na=end-of-candidates\na=candidate:1 1 udp 1 203.0.113.1 5 typ host\nsecret-value" });
    assert.deepEqual(lines, ["local:relay", "remote:unknown", "local:end", "state:failed", "sdp-trickle:true",
                            "sdp-end:true", "sdp-candidate:host", "can-trickle:true"].map((line) => `p2pdc-ice:${line}:ms=0`));
    assert.ok(lines.every((line) => !/192\.0\.2\.1|198\.51\.100\.1|203\.0\.113\.1|secret-value/.test(line)));
});

test("ICE trace records elapsed time and absent SDP options", async () => {
    const lines = [];
    let time = 10;
    class Peer {
        addEventListener() {}
        async setRemoteDescription() { this.canTrickleIceCandidates = false; }
    }
    const context = { RTCPeerConnection: Peer, performance: { now: () => time },
                      console: { info: (line) => lines.push(line) } };
    vm.runInNewContext(await readFile(new URL("./browser_ice_trace.js", import.meta.url), "utf8"), context);
    const peer = new context.RTCPeerConnection();
    time = 27;
    await peer.setRemoteDescription({ sdp: "v=0\r\n" });
    assert.deepEqual(lines, ["p2pdc-ice:sdp-trickle:false:ms=17", "p2pdc-ice:sdp-end:false:ms=17",
                            "p2pdc-ice:can-trickle:false:ms=17"]);
});
