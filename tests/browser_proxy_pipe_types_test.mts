import { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
import { ProxyMailbox } from "../src/browser/proxy_mailbox.mjs";
import { ProxyOutbox } from "../src/browser/proxy_outbox.mjs";

const proxy = new OpenDhtProxy("https://proxy.example");
const key = new Uint8Array(20);
const mailbox = new ProxyMailbox(proxy, key, { onRecord: (record) => { record.byteLength; },
    onCatchup: () => {}, onError: (error) => { void error; } });
const outbox = new ProxyOutbox(proxy, key, { onPublished: () => {}, onError: (error) => { void error; } });
const pending: Promise<boolean> = mailbox.start();
const result: string = outbox.send(new Uint8Array([1]));
// @ts-expect-error records must be binary
outbox.send("text");
void pending; void result;
