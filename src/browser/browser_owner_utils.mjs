import { settings } from "./webrtc_settings.mjs";

const byteLength = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "byteLength").get;
const fields = ["signingKey", "recipientKey", "instance", "inbox"];
const noop = () => {};
const validationHandlers = { onDescription: noop, onCandidate: noop, onState: noop, onBinary: noop,
    onRoute: noop, onError: noop, onWritable: noop };

export const sameBytes = (a, b) => a.length === b.length && a.every((value, i) => value === b[i]);
export const sameEndpoint = (a, b) => a && b && fields.every((field) => sameBytes(a[field], b[field]));
export const unknownRoute = () => ({ current: "unknown", turnHistory: "unknown", selectedPair: null });

export function copyBytes(value, size) {
    if (!(value instanceof Uint8Array) || byteLength.call(value) !== size) throw new TypeError("invalid binary input");
    const result = new Uint8Array(size); Uint8Array.prototype.set.call(result, value); return result;
}
export function copyIdentity(identity) {
    const result = {};
    try {
        for (const name of ["identitySeed", "recipientSecret", "capability"]) result[name] = copyBytes(identity?.[name], 32);
        return result;
    } catch (error) { wipeIdentity(result); throw error; }
}
export function wipeIdentity(identity) {
    if (identity) for (const value of Object.values(identity)) value.fill(0);
}
export function copyTransport(value, role) {
    if (!value || !["automatic", "requireLocalRelay"].includes(value.relayPolicy)) throw new TypeError("invalid relay policy");
    const servers = value.iceServers ?? [];
    if (!Array.isArray(servers)) throw new TypeError("invalid ICE servers");
    const result = { ...value, role, iceServers: servers.map((server) => ({ ...server,
        urls: Array.isArray(server?.urls) ? [...server.urls] : server?.urls })) };
    settings(result, validationHandlers);
    for (const name of ["maxMessageBytes", "maxBufferBytes", "maxInboundBytes"]) {
        if (result[name] !== undefined && result[name] < 1024) throw new RangeError(`${name} is too small for secure frames`);
    }
    if (result.peerConnectionFactory !== undefined && typeof result.peerConnectionFactory !== "function")
        throw new TypeError("invalid peer connection factory");
    return result;
}
export function matchingIdentity(lease, watch) {
    const endpoint = lease.endpoint(), watched = watch.discovery.self();
    const localKey = lease.discovery.key(), watchKey = watch.discovery.key();
    return Boolean(lease.windowed === watch.windowed && endpoint && watched && localKey && watchKey && sameBytes(localKey, watchKey) &&
        sameBytes(endpoint.signingKey, watched.signingKey) && sameBytes(endpoint.recipientKey, watched.recipientKey));
}
