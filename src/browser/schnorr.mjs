// BIP340 Schnorr signatures over secp256k1, as Nostr relays require of events.
// Only throwaway keys sign here: p2pdc's own signatures inside each value are the ones peers
// check, so this needs correct signatures, not resistance to timing attacks.
const P = 2n ** 256n - 2n ** 32n - 977n;
const N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141n;
const G = [0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798n,
           0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8n, 1n];

const mod = (a, m = P) => ((a % m) + m) % m;
function power(base, exponent, m = P) {
    let result = 1n;
    for (base = mod(base, m); exponent > 0n; exponent >>= 1n, base = base * base % m)
        if (exponent & 1n) result = result * base % m;
    return result;
}
const invert = (a) => power(a, P - 2n);

// Points in Jacobian coordinates [X, Y, Z]; Z = 0 is the point at infinity.
function double([x, y, z]) {
    if (z === 0n || y === 0n) return [0n, 1n, 0n];
    const s = 4n * x * y * y % P, m = 3n * x * x % P;
    const nx = mod(m * m - 2n * s), ny = mod(m * (s - nx) - 8n * y ** 4n);
    return [nx, ny, 2n * y * z % P];
}
function add(a, b) {
    if (a[2] === 0n) return b;
    if (b[2] === 0n) return a;
    const z1z1 = a[2] * a[2] % P, z2z2 = b[2] * b[2] % P;
    const u1 = a[0] * z2z2 % P, u2 = b[0] * z1z1 % P;
    const s1 = a[1] * b[2] * z2z2 % P, s2 = b[1] * a[2] * z1z1 % P;
    if (u1 === u2) return s1 === s2 ? double(a) : [0n, 1n, 0n];
    const h = mod(u2 - u1), r = mod(s2 - s1), hh = h * h % P, hhh = h * hh % P;
    const x = mod(r * r - hhh - 2n * u1 * hh);
    return [x, mod(r * (u1 * hh - x) - s1 * hhh), a[2] * b[2] * h % P];
}
function multiply(k, point = G) {
    let result = [0n, 1n, 0n];
    for (let bit = BigInt(k.toString(2).length) - 1n; bit >= 0n; --bit) {
        result = double(result);
        if ((k >> bit) & 1n) result = add(result, point);
    }
    return result;
}
function affine([x, y, z]) {
    const zi = invert(z), zi2 = zi * zi % P;
    return [x * zi2 % P, y * zi2 * zi % P];
}

const toBytes = (value) => Uint8Array.from(value.toString(16).padStart(64, "0").match(/../g), (h) => parseInt(h, 16));
const toBig = (bytes) => BigInt(`0x${Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("") || "0"}`);
const concat = (...parts) => {
    const out = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
    let offset = 0;
    for (const part of parts) { out.set(part, offset); offset += part.length; }
    return out;
};
async function sha256(bytes) { return new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)); }
async function taggedHash(tag, ...parts) {
    const tagHash = await sha256(new TextEncoder().encode(tag));
    return sha256(concat(tagHash, tagHash, ...parts));
}

// The point with x and an even y (BIP340 lift_x), or null when x is not on the curve.
function liftX(x) {
    if (x >= P) return null;
    const c = mod(x ** 3n + 7n), y = power(c, (P + 1n) / 4n);
    if (y * y % P !== c) return null;
    return [x, y % 2n === 0n ? y : P - y, 1n];
}

function secretScalar(secretKey) {
    const d = toBig(secretKey);
    if (secretKey.length !== 32 || d === 0n || d >= N) throw new TypeError("invalid secp256k1 secret key");
    return d;
}

// The 32-byte x-only public key of a 32-byte secret key.
export function publicKey(secretKey) { return toBytes(affine(multiply(secretScalar(secretKey)))[0]); }

// Signs a message with a secret key and 32 bytes of auxiliary randomness.
export async function sign(message, secretKey, auxRand = crypto.getRandomValues(new Uint8Array(32))) {
    const d0 = secretScalar(secretKey);
    const [px, py] = affine(multiply(d0));
    const d = py % 2n === 0n ? d0 : N - d0, pBytes = toBytes(px);
    const t = toBytes(d ^ toBig(await taggedHash("BIP0340/aux", auxRand)));
    const k0 = toBig(await taggedHash("BIP0340/nonce", t, pBytes, message)) % N;
    if (k0 === 0n) throw new Error("BIP340 nonce is zero");
    const [rx, ry] = affine(multiply(k0));
    const k = ry % 2n === 0n ? k0 : N - k0, rBytes = toBytes(rx);
    const e = toBig(await taggedHash("BIP0340/challenge", rBytes, pBytes, message)) % N;
    return concat(rBytes, toBytes((k + e * d) % N));
}

// Whether sig is a valid signature of message by the x-only public key.
export async function verify(message, key, sig) {
    if (key.length !== 32 || sig.length !== 64) return false;
    const point = liftX(toBig(key)), r = toBig(sig.subarray(0, 32)), s = toBig(sig.subarray(32));
    if (!point || r >= P || s >= N) return false;
    const e = toBig(await taggedHash("BIP0340/challenge", sig.subarray(0, 32), key, message)) % N;
    const R = add(multiply(s), multiply(N - e, point));
    if (R[2] === 0n) return false;
    const [x, y] = affine(R);
    return y % 2n === 0n && x === r;
}
