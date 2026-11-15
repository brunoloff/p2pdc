import { OpenDhtProxy, ProxyError, type ProxyRecord } from "../src/browser/opendht_proxy.mjs";

const proxy = new OpenDhtProxy("https://example.com", { maxValues: 4 });
const key = new Uint8Array(20);
const values: Promise<ProxyRecord[]> = proxy.get(key);
const sent: Promise<void> = proxy.put(key, new Uint8Array());
const listener = proxy.listen(key, (record) => { record.bytes.byteLength; });
const done: Promise<void> = listener.done;
const error: string = new ProxyError("http_error", "failed").code;
// @ts-expect-error DHT keys are byte arrays.
proxy.get("room");
// @ts-expect-error Payloads are byte arrays.
proxy.put(key, "text");
// @ts-expect-error Listener callbacks receive a record, not a string.
proxy.listen(key, (record: string) => record);
void [values, sent, done, error];
