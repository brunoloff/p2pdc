// Functions run inside tests/peerjs_webrtc_page.html (see chromium_page.mjs), so each is
// self-contained: they use only their arguments and what the page puts on window.p2pdc.

// Two clients of one PeerJS server: one puts, the other watches and looks up, then hears a
// second value. Reports what each step saw.
export async function exchangeInPage(url, serverKey, iceServers, timeoutMs) {
    const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
    for (let i = 0; i < 100 && !window.p2pdc; ++i) await sleep(50);
    const { PeerJsSwitchboard, keyHex, storeLimits } = window.p2pdc;
    const limits = storeLimits({ timeoutMs }), rtc = iceServers ? { iceServers } : {};
    const same = (a, b) => a.length === b.length && a.every((byte, i) => byte === b[i]);
    const key = crypto.getRandomValues(new Uint8Array(20)), first = crypto.getRandomValues(new Uint8Array(300));
    const second = crypto.getRandomValues(new Uint8Array(300));
    const writer = new PeerJsSwitchboard(url, serverKey, limits, rtc), watcher = new PeerJsSwitchboard(url, serverKey, limits, rtc);
    try {
        await writer.put(key, first);
        const heard = [];
        const watch = watcher.listen(key, (record) => heard.push(record.bytes));
        watch.done.catch(() => {}); // a failed watch shows as ready being false
        const ready = await watch.ready;
        const found = await watcher.get(key);
        await writer.put(key, second);
        for (let i = 0; i < 150 && !heard.some((value) => same(value, second)); ++i) await sleep(100);
        watch.close();
        return { ready, found: found.length === 1 && same(found[0].bytes, first), heard: heard.some((value) => same(value, second)),
                 writerHosts: writer.hosts.has(keyHex(key)), watcherConnected: Boolean(watcher.links.get(keyHex(key))?.link) };
    } finally { writer.close(); watcher.close(); }
}

// A host leaves: the watcher's data connection closes, it takes the key over, and hears the
// writer's next value there.
export async function takeoverInPage(url, timeoutMs) {
    const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
    for (let i = 0; i < 100 && !window.p2pdc; ++i) await sleep(50);
    const { PeerJsSwitchboard, keyHex, storeLimits } = window.p2pdc;
    const limits = storeLimits({ timeoutMs }), rtc = { iceServers: [] };
    const [host, watcher, writer] = [0, 1, 2].map(() => new PeerJsSwitchboard(url, "peerjs", limits, rtc));
    const key = crypto.getRandomValues(new Uint8Array(20)), hex = keyHex(key);
    try {
        await host.put(key, Uint8Array.of(1));
        const heard = [];
        const watch = watcher.listen(key, (record) => heard.push(record.bytes[0]));
        watch.done.catch(() => {});
        const ready = await watch.ready;
        await writer.put(key, Uint8Array.of(2));
        host.close();
        for (let i = 0; i < 100 && !watcher.hosts.has(hex); ++i) await sleep(50);
        await writer.put(key, Uint8Array.of(3));
        for (let i = 0; i < 100 && !heard.includes(3); ++i) await sleep(50);
        const values = (await writer.get(key)).map((record) => record.bytes[0]).sort();
        watch.close();
        return { ready, heard, watcherHosts: watcher.hosts.has(hex), values };
    } finally { for (const store of [host, watcher, writer]) store.close(); }
}
