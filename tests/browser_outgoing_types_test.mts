import { BrowserOutgoing, type BrowserOutgoingHandlers, type BrowserOutgoingOptions }
    from "../src/browser/browser_outgoing.mjs";
import type { DiscoveryModule, DiscoveryOptions, DiscoveredPeer } from "../src/browser/wasm_discovery.mjs";
import type { SecureModule } from "../src/browser/wasm_secure_link.mjs";
import type { BrowserWatch } from "../src/browser/browser_watch.mjs";
import type { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
declare const module: DiscoveryModule & SecureModule, proxy: OpenDhtProxy, identity: DiscoveryOptions;
declare const watch: BrowserWatch, peer: DiscoveredPeer, options: BrowserOutgoingOptions;
const smallOptions: BrowserOutgoingOptions = { transport: { relayPolicy: "automatic" }, maxMessageBytes: 8192 };
// @ts-expect-error Application message limit is numeric.
const invalidOptions: BrowserOutgoingOptions = { transport: { relayPolicy: "automatic" }, maxMessageBytes: "8192" };
const handlers: BrowserOutgoingHandlers = { onState: () => {}, onRoute: () => {}, onMessage: () => {},
    onWritable: () => {}, onStatus: () => {}, onError: () => {} };
const owner = new BrowserOutgoing(module, proxy, identity, watch, peer, options, handlers);
const started: Promise<boolean> = owner.start();
// @ts-expect-error Messages must be binary.
owner.send("text");
void started;
void smallOptions; void invalidOptions;
