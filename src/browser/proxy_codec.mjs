// OpenDHT proxy records are untrusted. These helpers do not authenticate data.
const byteLength = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), "byteLength").get;

export function sameBytes(first, second) {
    const size = byteLength.call(first);
    if (size !== byteLength.call(second)) return false;
    for (let index = 0; index < size; ++index) if (first[index] !== second[index]) return false;
    return true;
}

export class ProxyError extends Error {
    constructor(code, message) {
        super(message);
        this.name = "ProxyError";
        this.code = code;
        /** @type {number | undefined} */
        this.status = undefined;
    }
}

export function isTransientProxyError(error) {
    return error instanceof ProxyError && (["network_error", "stream_closed", "timeout"].includes(error.code) ||
        (error.code === "http_error" && (error.status === 408 || error.status === 429 || error.status >= 500)));
}

export function keyHex(key) {
    if (!(key instanceof Uint8Array) || key.length !== 20 || !key.some((byte) => byte !== 0)) {
        throw new TypeError("DHT key must be 20 nonzero-in-aggregate bytes");
    }
    return Array.from(key, (byte) => byte.toString(16).padStart(2, "0")).join("");
}

export function encodeBase64(bytes) {
    if (!(bytes instanceof Uint8Array)) throw new TypeError("payload must be Uint8Array");
    let binary = "";
    for (const byte of bytes) binary += String.fromCharCode(byte);
    return btoa(binary);
}

// Oversized payloads are untrusted DHT data, not proxy faults: callers drop
// records that fail with "oversized_record" and keep the operation running.
function decodeBase64(value, maxBytes) {
    if (typeof value !== "string" ||
        !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) {
        throw new ProxyError("invalid_record", "invalid base64 payload");
    }
    if (value.length > 4 * Math.ceil(maxBytes / 3)) throw new ProxyError("oversized_record", "oversized payload");
    const binary = atob(value);
    if (btoa(binary) !== value) throw new ProxyError("invalid_record", "noncanonical base64 payload");
    if (binary.length > maxBytes) throw new ProxyError("oversized_record", "oversized payload");
    return Uint8Array.from(binary, (char) => char.charCodeAt(0));
}

export function decodeRecordOrDrop(line, maxBytes) {
    try { return decodeRecord(line, maxBytes); }
    catch (error) {
        if (error instanceof ProxyError && error.code === "oversized_record") return null;
        throw error;
    }
}

export function decodeRecord(line, maxBytes) {
    let value;
    try { value = JSON.parse(line); }
    catch { throw new ProxyError("invalid_record", "invalid proxy JSON record"); }
    if (!value || typeof value !== "object" || Array.isArray(value) ||
        (value.id !== undefined && (typeof value.id !== "string" || value.id.length > 128)) ||
        (value.expired !== undefined && typeof value.expired !== "boolean")) {
        throw new ProxyError("invalid_record", "invalid proxy record metadata");
    }
    return { bytes: decodeBase64(value.data, maxBytes), id: value.id ?? null,
             expired: value.expired === true };
}
