const kinds = new Map([["host", "host"], ["srflx", "serverReflexive"],
                       ["prflx", "peerReflexive"], ["relay", "relayed"]]);

// A candidate pair is usable only when exactly one transport identifies it.
// Never guess from a nominated pair without a selected transport reference.
export function selectedPair(report) {
    const ids = new Set();
    for (const value of report.values()) {
        if (value.type === "transport" && typeof value.selectedCandidatePairId === "string")
            ids.add(value.selectedCandidatePairId);
    }
    if (ids.size !== 1) return null;
    const pair = report.get(ids.values().next().value);
    if (!pair || pair.type !== "candidate-pair") return null;
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
