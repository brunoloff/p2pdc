import { BrowserNode, type BrowserNodeHandlers } from "../src/browser/browser_node.mjs";
import type { DiscoveryModule, DiscoveryOptions, DiscoveredPeer } from "../src/browser/wasm_discovery.mjs";
import type { SecureModule } from "../src/browser/wasm_secure_link.mjs";
import type { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
import type { BrowserOutgoingHandlers } from "../src/browser/browser_outgoing.mjs";
declare const module: DiscoveryModule & SecureModule, proxy: OpenDhtProxy, identity: DiscoveryOptions;
declare const peer: DiscoveredPeer, handlers: BrowserOutgoingHandlers;
const events: BrowserNodeHandlers = { onPeer: () => {}, onCatchup: () => {}, onError: () => {},
    onStatus: (status) => { const valid: "ready" | "reconnecting" = status; void valid; } };
const node = new BrowserNode(module, proxy, identity, events, { watch: { recovery: { maxRetries: 4, retryDelayMs: 1000 } } });
const connection = node.connect(peer, { transport: { relayPolicy: "automatic" } }, handlers);
const maximum: number | null = connection.maxMessageSize();
void maximum;
// @ts-expect-error The node starts owned connections.
connection.start();
// @ts-expect-error Messages must be binary.
connection.send("text");
