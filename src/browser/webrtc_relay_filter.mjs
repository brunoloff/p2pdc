// Relay-only mode must not disclose host or server-reflexive addresses to the
// peer, even if the engine ignored iceTransportPolicy. Only relay candidates
// are signaled, with their related address redacted as browsers do, and SDP
// connection lines that do not name a relay address are cleared.
const relayFields = (line) => line.trim().split(/\s+/);

// Accepts "candidate:..." or "a=candidate:..."; returns null for non-relay.
export function relayOnlyCandidateLine(line) {
    const parts = relayFields(line);
    if (parts.length < 8 || parts[6] !== "typ" || parts[7] !== "relay") return null;
    for (let index = 8; index + 1 < parts.length; index += 2) {
        if (parts[index] === "raddr") parts[index + 1] = "0.0.0.0";
        else if (parts[index] === "rport") parts[index + 1] = "0";
    }
    return parts.join(" ");
}

export function relayFilteredCandidate(value, relayOnly) {
    if (!relayOnly || !value || typeof value.candidate !== "string") return value;
    const candidate = relayOnlyCandidateLine(value.candidate);
    return candidate ? { ...value, candidate } : null;
}

export function relayFilteredDescription(value, relayOnly) {
    if (!relayOnly || !value || typeof value.sdp !== "string") return value;
    const lines = value.sdp.split("\r\n"), relays = new Set();
    const kept = lines.flatMap((line) => {
        if (!line.startsWith("a=candidate:")) return [line];
        const relay = relayOnlyCandidateLine(line);
        if (relay) relays.add(relayFields(relay)[4]);
        return relay ? [relay] : [];
    });
    const sdp = kept.map((line) => {
        const parts = line.startsWith("c=IN ") ? relayFields(line) : null;
        return parts && parts.length === 3 && !relays.has(parts[2]) ? "c=IN IP4 0.0.0.0" : line;
    }).join("\r\n");
    return { ...value, sdp };
}
