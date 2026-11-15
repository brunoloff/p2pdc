const turn = new URLSearchParams(location.search).get("turn");
const signalEnd = new URLSearchParams(location.search).get("end") === "1";
const servers = [{ urls: "turn:127.0.0.1:34789?transport=tcp", username: "p2pdc",
    credential: "temporary-test-password" }];
const offererConfig = turn === "both" ? { iceServers: servers, iceTransportPolicy: "relay" } :
    turn === "auto" ? { iceServers: servers } : {};
const offerer = new RTCPeerConnection(offererConfig);
const answerer = new RTCPeerConnection(turn ? { iceServers: servers, iceTransportPolicy: "relay" } : {});
const peers = [offerer, answerer], pending = [[], []], candidates = [[], []], ends = [0, 0], appliedEnds = [0, 0];
let resolve, reject, gotPing = false;
const complete = new Promise((yes, no) => { resolve = yes; reject = no; });
const safely = (promise) => { void promise.catch(reject); };
function finish() { if (gotPing && (!signalEnd || appliedEnds.every((count) => count === 1))) resolve(); }

async function apply(index, candidate) {
    await peers[index].addIceCandidate(candidate);
    if (candidate === null) { ++appliedEnds[index]; finish(); }
}
async function receive(index, candidate) {
    if (!peers[index].remoteDescription) pending[index].push(candidate);
    else await apply(index, candidate);
}
async function flush(index) {
    for (const candidate of pending[index].splice(0)) await apply(index, candidate);
}
for (let index = 0; index < 2; ++index) {
    peers[index].onicecandidate = ({ candidate }) => {
        if (!candidate) {
            if (signalEnd) { ++ends[index]; safely(receive(1 - index, null)); }
            return;
        }
        candidates[index].push(candidate.candidate.match(/\btyp (host|srflx|prflx|relay)\b/)?.[1] ?? "unknown");
        safely(receive(1 - index, candidate));
    };
}
answerer.ondatachannel = ({ channel }) => { channel.onmessage = ({ data }) => { if (data === "ping") { gotPing = true; finish(); } }; };
const channel = offerer.createDataChannel("test");
channel.onopen = () => channel.send("ping");
let timeout;
try {
    await offerer.setLocalDescription(await offerer.createOffer());
    await answerer.setRemoteDescription(offerer.localDescription); await flush(1);
    await answerer.setLocalDescription(await answerer.createAnswer());
    await offerer.setRemoteDescription(answerer.localDescription); await flush(0);
    await Promise.race([complete, new Promise((_resolve, no) => { timeout = setTimeout(() => no(new Error("timeout")), 8000); })]);
    document.body.dataset.result = "pass"; document.body.textContent = "pass";
} catch (error) {
    const states = peers.map((peer) => ({ connection: peer.connectionState, ice: peer.iceConnectionState }));
    document.body.dataset.result = "fail";
    document.body.textContent = `fail: ${error.message}; ${JSON.stringify({ candidates, states, ends, appliedEnds })}`;
} finally {
    clearTimeout(timeout); channel.close(); for (const peer of peers) peer.close();
}
