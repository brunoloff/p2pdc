// Opt-in test instrumentation: never log candidate strings or addresses.
(() => {
    const Native = globalThis.RTCPeerConnection;
    const started = performance.now();
    function trace(value) {
        console.info(`p2pdc-ice:${value}:ms=${Math.round(performance.now() - started)}`);
    }
    function category(candidate) {
        const found = /\btyp\s+(host|srflx|prflx|relay)\b/i.exec(candidate?.candidate || "");
        return found ? found[1].toLowerCase() : "unknown";
    }
    globalThis.RTCPeerConnection = class extends Native {
        constructor(...args) {
            super(...args);
            this.addEventListener("icecandidate", (event) => {
                trace(`local:${event.candidate ? category(event.candidate) : "end"}`);
            });
            this.addEventListener("iceconnectionstatechange", () => trace(`state:${this.iceConnectionState}`));
        }
        addIceCandidate(candidate) {
            trace(`remote:${candidate ? category(candidate) : "end"}`);
            return super.addIceCandidate(candidate);
        }
        async setRemoteDescription(description) {
            const sdp = typeof description?.sdp === "string" ? description.sdp : "";
            trace(`sdp-trickle:${/^a=ice-options:.*\btrickle\b/m.test(sdp)}`);
            trace(`sdp-end:${/^a=end-of-candidates\s*$/m.test(sdp)}`);
            for (const line of sdp.split(/\r?\n/)) {
                if (line.startsWith("a=candidate:")) trace(`sdp-candidate:${category({ candidate: line })}`);
            }
            await super.setRemoteDescription(description);
            trace(`can-trickle:${this.canTrickleIceCandidates}`);
        }
    };
})();
