const kinds = new Map([["host", "host"], ["srflx", "serverReflexive"],
                       ["prflx", "peerReflexive"], ["relay", "relayed"]]);

const roundTripPollMs = 2000;

// A candidate pair is usable only when exactly one transport identifies it.
// Never guess from a nominated pair without a selected transport reference.
function selectedPairStats(report) {
    const ids = new Set();
    for (const value of report.values()) {
        if (value.type === "transport" && typeof value.selectedCandidatePairId === "string")
            ids.add(value.selectedCandidatePairId);
    }
    if (ids.size !== 1) return null;
    const pair = report.get(ids.values().next().value);
    return pair && pair.type === "candidate-pair" ? pair : null;
}

export function selectedPair(report) {
    const pair = selectedPairStats(report);
    if (!pair) return null;
    const local = report.get(pair.localCandidateId), remote = report.get(pair.remoteCandidateId);
    if (!local || !remote || local.type !== "local-candidate" || remote.type !== "remote-candidate") return null;
    return { local: kinds.get(local.candidateType) ?? "unknown",
             remote: kinds.get(remote.candidateType) ?? "unknown" };
}

export function routeKind(pair) {
    if (!pair) return "unknown";
    if (pair.local === "relayed" || pair.remote === "relayed") return "turnRelayed";
    if (["unknown", "peerReflexive"].includes(pair.local) ||
        ["unknown", "peerReflexive"].includes(pair.remote)) return "unknown";
    return "direct";
}

// The selected pair's latest round-trip time (STUN consent checks) in milliseconds, or null.
export function selectedRoundTripMs(report) {
    let seconds;
    try { seconds = selectedPairStats(report)?.currentRoundTripTime; } catch { return null; }
    return typeof seconds === "number" && Number.isFinite(seconds) && seconds >= 0 ? seconds * 1000 : null;
}

// Runs poll again a couple of seconds after each schedule(), so a connection's round-trip
// time stays fresh without polling stats more often.
export class RoundTripPoller {
    constructor(poll) { this.poll = poll; this.timer = null; }
    schedule() {
        if (this.timer === null) this.timer = setTimeout(() => { this.timer = null; this.poll(); }, roundTripPollMs);
    }
    stop() { clearTimeout(this.timer); this.timer = null; }
}
