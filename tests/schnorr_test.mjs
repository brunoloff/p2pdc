import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { publicKey, sign, verify } from "../src/browser/schnorr.mjs";

const hex = (text) => Uint8Array.from(text.match(/../g) ?? [], (pair) => parseInt(pair, 16));
const toHex = (bytes) => Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("").toUpperCase();
const vectors = readFileSync(new URL("data/bip340-test-vectors.csv", import.meta.url), "utf8")
    .split("\n").filter((line) => line && !line.startsWith("#") && !line.startsWith("index"))
    .map((line) => {
        const [index, secret, key, aux, message, signature, result] = line.split(",");
        return { index, secret, key, aux, message, signature, valid: result === "TRUE" };
    });

test("signs as the BIP340 test vectors do", async () => {
    const signing = vectors.filter((vector) => vector.secret);
    assert.ok(signing.length >= 4);
    for (const vector of signing) {
        assert.equal(toHex(publicKey(hex(vector.secret))), vector.key, `vector ${vector.index}`);
        assert.equal(toHex(await sign(hex(vector.message), hex(vector.secret), hex(vector.aux))), vector.signature,
                     `vector ${vector.index}`);
    }
});

test("verifies as the BIP340 test vectors say", async () => {
    for (const vector of vectors)
        assert.equal(await verify(hex(vector.message), hex(vector.key), hex(vector.signature)), vector.valid,
                     `vector ${vector.index}`);
});

test("a fresh key signs what it verifies, and rejects invalid secret keys", async () => {
    const secret = crypto.getRandomValues(new Uint8Array(32)), message = crypto.getRandomValues(new Uint8Array(32));
    const signature = await sign(message, secret);
    assert.equal(await verify(message, publicKey(secret), signature), true);
    message[0] ^= 1;
    assert.equal(await verify(message, publicKey(secret), signature), false);
    assert.throws(() => publicKey(new Uint8Array(32)), TypeError);
    assert.throws(() => publicKey(new Uint8Array(32).fill(255)), TypeError);
});
