export type BrowserTransportState = "connecting" | "ready" | "suspended" | "failed" | "closed";
export type BrowserSendResult = "accepted" | "notReady" | "tooLarge" | "backpressure" | "failed" | "closed";
export type BrowserCandidateKind = "host" | "serverReflexive" | "peerReflexive" | "relayed" | "unknown";
export interface BrowserRoute {
    current: "unknown" | "direct" | "turnRelayed";
    turnHistory: "unknown" | "used";
    selectedPair: { local: BrowserCandidateKind; remote: BrowserCandidateKind } | null;
}
export interface BrowserDescription {
    type: "offer" | "answer";
    sdp: string;
}
export interface BrowserTransportOptions {
    role: "offerer" | "answerer";
    relayPolicy: "automatic" | "requireLocalRelay";
    iceServers?: RTCIceServer[];
    maxMessageBytes?: number;
    maxBufferBytes?: number;
    maxInboundBytes?: number;
    maxInboundMessages?: number;
    peerConnectionFactory?: (configuration: RTCConfiguration) => RTCPeerConnection;
}
/** Handlers must be synchronous; asynchronous signaling should handle its own errors. */
export interface BrowserTransportHandlers {
    onDescription(description: BrowserDescription): void;
    onCandidate(candidate: RTCIceCandidateInit): void;
    onState(state: BrowserTransportState): void;
    onBinary(bytes: Uint8Array): void;
    onRoute(route: BrowserRoute): void;
    onError(reason: string, category: "transport" | "resource"): void;
    onWritable(): void;
}
/** Raw WebRTC transport; callers must authenticate signaling and peer identity.
 * Poll route periodically: sampled statistics cannot prove TURN was never used. */
export class BrowserWebRtcTransport {
    constructor(options: BrowserTransportOptions, handlers: BrowserTransportHandlers);
    readonly status: BrowserTransportState;
    readonly route: BrowserRoute;
    startOffer(): Promise<void>;
    setRemoteDescription(description: BrowserDescription): Promise<void>;
    addRemoteCandidate(candidate: RTCIceCandidateInit): Promise<void>;
    send(bytes: Uint8Array): BrowserSendResult;
    bufferedAmount(): number | null;
    /** Zero or positive infinity means unlimited; null means the limit is unavailable. */
    maxMessageSize(): number | null;
    pollRoute(): Promise<BrowserRoute>;
    close(): void;
}
