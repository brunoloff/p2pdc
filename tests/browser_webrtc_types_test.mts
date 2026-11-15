import { BrowserWebRtcTransport, type BrowserTransportHandlers, type BrowserRoute }
    from "../src/browser/webrtc_transport.mjs";

const handlers: BrowserTransportHandlers = {
    onDescription: () => {}, onCandidate: () => {}, onState: () => {}, onBinary: () => {},
    onRoute: () => {}, onError: () => {}, onWritable: () => {},
};
const transport = new BrowserWebRtcTransport({ role: "offerer", relayPolicy: "automatic" }, handlers);
const route: Promise<BrowserRoute> = transport.pollRoute();
const count: number | null = transport.maxMessageSize();
// @ts-expect-error Binary sends require a byte array.
transport.send("text");
// @ts-expect-error Unknown relay policies are invalid.
new BrowserWebRtcTransport({ role: "offerer", relayPolicy: "direct" }, handlers);
void [route, count];
