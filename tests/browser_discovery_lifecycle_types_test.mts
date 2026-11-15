import { BrowserLease, type LeaseHandlers } from "../src/browser/browser_lease.mjs";
import { BrowserWatch, type WatchHandlers } from "../src/browser/browser_watch.mjs";
import type { OpenDhtProxy } from "../src/browser/opendht_proxy.mjs";
import type { DiscoveryModule, DiscoveryOptions } from "../src/browser/wasm_discovery.mjs";
declare const module: DiscoveryModule;
declare const proxy: OpenDhtProxy;
declare const identity: DiscoveryOptions;
declare const watchHandlers: WatchHandlers;
declare const leaseHandlers: LeaseHandlers;
const watch = new BrowserWatch(module, proxy, identity, watchHandlers);
const lease = new BrowserLease(module, proxy, identity, leaseHandlers);
const endpoint = lease.endpoint();
if (endpoint) { const inbox: Uint8Array = endpoint.inbox; void inbox; }
const started: Promise<boolean> = watch.start();
const recovering: boolean = watch.recovering();
// @ts-expect-error peer handles are numeric
watch.current("peer");
void started;
void recovering;
