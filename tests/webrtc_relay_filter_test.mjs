import test from "node:test";
import assert from "node:assert/strict";
import { relayFilteredCandidate, relayFilteredDescription, relayOnlyCandidateLine }
    from "../src/browser/webrtc_relay_filter.mjs";

const host = "candidate:1 1 udp 2122317823 192.168.1.20 50000 typ host generation 0";
const reflexive = "candidate:2 1 udp 1686109951 203.0.113.7 50001 typ srflx raddr 192.168.1.20 rport 50000";
const relay = "candidate:3 1 udp 41885695 198.51.100.9 3478 typ relay raddr 203.0.113.7 rport 50001 generation 0";

test("only relay candidates survive, with their related address redacted", () => {
    assert.equal(relayOnlyCandidateLine(host), null);
    assert.equal(relayOnlyCandidateLine(reflexive), null);
    assert.equal(relayOnlyCandidateLine("candidate:typ 1 udp 1 10.0.0.1 9 typ host"), null);
    assert.equal(relayOnlyCandidateLine("garbage"), null);
    assert.equal(relayOnlyCandidateLine(relay),
                 "candidate:3 1 udp 41885695 198.51.100.9 3478 typ relay raddr 0.0.0.0 rport 0 generation 0");
});

test("candidate objects pass through unless relay is required", () => {
    const value = { candidate: host, sdpMid: "0" };
    assert.equal(relayFilteredCandidate(value, false), value);
    assert.equal(relayFilteredCandidate(value, true), null);
    assert.deepEqual(relayFilteredCandidate({ candidate: relay, sdpMid: "0" }, true).sdpMid, "0");
    assert.equal(relayFilteredCandidate(undefined, true), undefined);
});

test("relay-only descriptions drop non-relay candidates and host connection addresses", () => {
    const sdp = ["v=0", "m=application 50000 UDP/DTLS/SCTP webrtc-datachannel", "c=IN IP4 192.168.1.20",
                 `a=${host}`, `a=${reflexive}`, `a=${relay}`, "a=fingerprint:sha-256 AA", ""].join("\r\n");
    const value = { type: "offer", sdp };
    assert.equal(relayFilteredDescription(value, false), value);
    const filtered = relayFilteredDescription(value, true);
    assert.equal(filtered.type, "offer");
    assert.doesNotMatch(filtered.sdp, /192\.168\.1\.20|203\.0\.113\.7|typ host|typ srflx/);
    assert.match(filtered.sdp, /\r\nc=IN IP4 0\.0\.0\.0\r\n/);
    assert.match(filtered.sdp, /typ relay raddr 0\.0\.0\.0 rport 0/);
    assert.match(filtered.sdp, /a=fingerprint:sha-256 AA\r\n$/);
    const relayed = relayFilteredDescription({ type: "answer", sdp: "c=IN IP4 198.51.100.9\r\na=" + relay }, true);
    assert.match(relayed.sdp, /^c=IN IP4 198\.51\.100\.9\r\n/);
});
