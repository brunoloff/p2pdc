import { BrowserConnection, type BrowserConnectionHandlers, type BrowserConnectionOptions }
    from "../src/browser/browser_connection.mjs";
import type { SecureModule } from "../src/browser/wasm_secure_link.mjs";

declare const module: SecureModule;
declare const options: BrowserConnectionOptions;
const handlers: BrowserConnectionHandlers = {
    onSignal: () => {}, onState: () => {}, onRoute: () => {}, onMessage: () => {},
    onWritable: () => {}, onError: () => {},
};
const connection = new BrowserConnection(module, options, handlers);
const maximum: number | null = connection.maxMessageSize();
const result: "accepted" | "notReady" | "tooLarge" | "backpressure" | "failed" | "closed" =
    connection.send(new Uint8Array([1]));
// @ts-expect-error The connection only sends bytes.
connection.send("message");
// @ts-expect-error Signaling records are bytes.
connection.receive("record");
void result;
void maximum;
