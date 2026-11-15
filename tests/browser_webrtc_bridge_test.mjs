import test from "node:test";
import assert from "node:assert/strict";
import { createWebRtcBridge } from "../src/browser/webrtc_bridge.mjs";

test("bridge preserves resource and ordinary transport error categories", () => {
    const events = [];
    let handlers;
    const bridge = createWebRtcBridge(
        (id, kind, value) => { events.push([id, kind, value]); return true; },
        (_options, callbacks) => { handlers = callbacks; return { close() {} }; },
    );
    const id = bridge.begin("offerer", "automatic", {});
    bridge.open(id);
    handlers.onError("receive limit exceeded", "resource");
    handlers.onError("channel closed", "transport");
    handlers.onError("unknown failure");
    assert.deepEqual(events, [[id, 5, 1], [id, 5, 0], [id, 5, 0]]);
    bridge.close(id);
});
