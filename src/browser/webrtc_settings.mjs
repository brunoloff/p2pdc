export function settings(options, handlers) {
    if (!options || !handlers || !["offerer", "answerer"].includes(options.role) ||
        !["automatic", "requireLocalRelay"].includes(options.relayPolicy)) throw new TypeError("invalid WebRTC settings");
    for (const name of ["onDescription", "onCandidate", "onState", "onBinary", "onRoute", "onError", "onWritable"]) {
        if (typeof handlers[name] !== "function") throw new TypeError(`missing ${name} callback`);
    }
    const servers = options.iceServers ?? [];
    if (!Array.isArray(servers) || servers.length > 16) throw new TypeError("invalid ICE server list");
    for (const server of servers) {
        if (!server || typeof server !== "object") throw new TypeError("invalid ICE server");
        const urls = Array.isArray(server.urls) ? server.urls : [server.urls];
        if (!urls.length || urls.length > 8 || urls.some((url) => typeof url !== "string" ||
            url.length > 2048 || !/^(?:stun|stuns|turn|turns):/i.test(url)))
            throw new TypeError("invalid ICE server URL");
        for (const field of [server.username, server.credential]) {
            if (field !== undefined && (typeof field !== "string" || field.length > 1024))
                throw new TypeError("invalid ICE server credential");
        }
    }
    if (options.relayPolicy === "requireLocalRelay" && !servers.some((server) => {
        const urls = Array.isArray(server.urls) ? server.urls : [server.urls];
        return urls.some((url) => typeof url === "string" && /^turns?:/i.test(url));
    })) throw new TypeError("relay policy requires a TURN server");
    const maxMessageBytes = options.maxMessageBytes ?? 262145;
    const maxBufferBytes = options.maxBufferBytes ?? 2097152;
    const maxInboundBytes = options.maxInboundBytes ?? 1048576;
    const maxInboundMessages = options.maxInboundMessages ?? 128;
    for (const value of [maxMessageBytes, maxBufferBytes, maxInboundBytes, maxInboundMessages]) {
        if (!Number.isSafeInteger(value) || value <= 0) throw new TypeError("invalid WebRTC limits");
    }
    if (maxMessageBytes > 1048576 || maxBufferBytes > 16777216 || maxInboundBytes > 16777216 ||
        maxInboundMessages > 1024 || maxMessageBytes > maxBufferBytes || maxMessageBytes > maxInboundBytes)
        throw new TypeError("WebRTC limits exceed supported bounds");
    return { servers, maxMessageBytes, maxBufferBytes, maxInboundBytes, maxInboundMessages };
}
