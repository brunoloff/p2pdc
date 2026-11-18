// Validation of ICE candidates crossing the browser transport. Both functions throw for
// anything malformed or beyond the per-connection limits.
export function checkedLocalCandidate(value, count, bytes) {
    if (!value || typeof value.candidate !== "string" || !value.candidate || value.candidate.length > 4096 ||
        (value.sdpMid != null && (typeof value.sdpMid !== "string" || value.sdpMid.length > 256)) ||
        (value.sdpMLineIndex != null && (!Number.isInteger(value.sdpMLineIndex) ||
         value.sdpMLineIndex < 0 || value.sdpMLineIndex > 65535)) ||
        (value.sdpMid == null && value.sdpMLineIndex == null) ||
        (value.usernameFragment != null && (typeof value.usernameFragment !== "string" ||
         value.usernameFragment.length > 256))) throw new TypeError("invalid local candidate");
    const encoder = new TextEncoder();
    const candidateSize = encoder.encode(value.candidate).length;
    const size = candidateSize + encoder.encode(value.sdpMid ?? "").length +
                 encoder.encode(value.usernameFragment ?? "").length;
    if (candidateSize > 4096 || size > 4608 || count >= 63 || size > 65536 - bytes)
        throw new RangeError("local candidate limits exceeded");
    return { size, copy: { candidate: value.candidate, sdpMid: value.sdpMid ?? null,
                           sdpMLineIndex: value.sdpMLineIndex ?? null,
                           usernameFragment: value.usernameFragment ?? null } };
}
export function checkedRemoteCandidate(candidate, count, bytes) {
    if (candidate.candidate.length > 4096) throw new RangeError("ICE candidate limits exceeded");
    const mid = candidate.sdpMid ?? null, index = candidate.sdpMLineIndex ?? null;
    const username = candidate.usernameFragment ?? null;
    if ((mid !== null && (typeof mid !== "string" || mid.length > 256)) ||
        (username !== null && (typeof username !== "string" || username.length > 256)) ||
        (index !== null && (!Number.isInteger(index) || index < 0 || index > 65535)) ||
        (mid === null && index === null)) throw new TypeError("invalid ICE candidate metadata");
    const encoder = new TextEncoder();
    const candidateSize = encoder.encode(candidate.candidate).length;
    const size = candidateSize + encoder.encode(mid ?? "").length + encoder.encode(username ?? "").length;
    if (!candidateSize || candidateSize > 4096 || size > 4608 || count >= 64 || size > 65536 - bytes)
        throw new RangeError("ICE candidate limits exceeded");
    return { size, candidate: { candidate: candidate.candidate, sdpMid: mid,
                                sdpMLineIndex: index, usernameFragment: username } };
}
