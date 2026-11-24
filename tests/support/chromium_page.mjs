// Runs functions in a page of headless Chromium, for tests of browser APIs that Node lacks
// (WebRTC). The page is served from this repository on loopback and driven through the Chrome
// DevTools Protocol, with Node's own WebSocket. Set P2PDC_TEST_CHROMIUM to the browser.
import { spawn } from "node:child_process";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join, normalize, sep } from "node:path";
import { fileURLToPath } from "node:url";

export const chromiumPath = process.env.P2PDC_TEST_CHROMIUM || null;
const root = fileURLToPath(new URL("../..", import.meta.url));
const types = { ".mjs": "text/javascript", ".js": "text/javascript", ".html": "text/html" };

// Serves src/browser and tests, nothing else.
async function serveRepository() {
    const server = createServer(async (request, response) => {
        const path = normalize(join(root, decodeURIComponent(new URL(request.url, "http://x").pathname)));
        const allowed = [join(root, "src", "browser") + sep, join(root, "tests") + sep].some((dir) => path.startsWith(dir));
        const type = types[path.slice(path.lastIndexOf("."))];
        try {
            if (!allowed || !type) throw new Error("not served");
            const body = await readFile(path);
            response.writeHead(200, { "content-type": type }).end(body);
        } catch { response.writeHead(404).end(); }
    });
    await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
    return server;
}

function launch(profile) {
    const args = ["-n", "15", chromiumPath, "--headless=new", `--user-data-dir=${profile}`, "--remote-debugging-port=0",
                  "--no-first-run", "--no-default-browser-check", "--disable-features=WebRtcHideLocalIpsWithMdns", "about:blank"];
    const child = spawn("nice", args, { stdio: ["ignore", "ignore", "pipe"] });
    const endpoint = new Promise((resolve, reject) => {
        let text = "";
        child.stderr.on("data", (data) => {
            text += data;
            const match = /DevTools listening on (ws:\/\/\S+)/.exec(text);
            if (match) resolve(match[1]);
        });
        child.on("exit", (code) => reject(new Error(`Chromium exited (${code}): ${text.slice(-500)}`)));
    });
    return { child, endpoint };
}

// A DevTools connection: call(method, params, sessionId) resolves with the result.
async function devtools(url) {
    const socket = new WebSocket(url), pending = new Map();
    let next = 0;
    await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = () => reject(new Error("DevTools connection failed")); });
    socket.onmessage = (event) => {
        const message = JSON.parse(event.data), entry = pending.get(message.id);
        if (!entry) return;
        pending.delete(message.id);
        if (message.error) entry.reject(new Error(`DevTools: ${message.error.message}`)); else entry.resolve(message.result);
    };
    const call = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
        const id = ++next;
        pending.set(id, { resolve, reject });
        socket.send(JSON.stringify({ id, method, params, sessionId }));
    });
    return { call, close: () => socket.close() };
}

// Waits until the page at url has loaded; evaluating while it navigates fails.
async function loaded(tools, sessionId, url) {
    const check = { expression: "location.href + ' ' + document.readyState", returnByValue: true };
    for (let i = 0; i < 200; ++i) {
        const state = await tools.call("Runtime.evaluate", check, sessionId).catch(() => null);
        if (state?.result.value === `${url} complete`) return;
        await new Promise((resolve) => setTimeout(resolve, 50));
    }
    throw new Error(`Chromium did not load ${url}`);
}

// Opens path (relative to the repository) in Chromium; evaluate(fn, ...args) runs fn there.
export async function openChromiumPage(path) {
    if (!chromiumPath) throw new Error("set P2PDC_TEST_CHROMIUM to a Chromium executable");
    const profile = await mkdtemp(join(tmpdir(), "p2pdc-chromium-")), server = await serveRepository();
    const browser = launch(profile);
    const cleanup = async () => {
        browser.child.kill();
        await new Promise((resolve) => (browser.child.exitCode === null ? browser.child.once("exit", resolve) : resolve()));
        server.close();
        // Chromium's helper processes may still write the profile for a moment.
        await rm(profile, { recursive: true, force: true, maxRetries: 20, retryDelay: 100 });
    };
    try {
        const tools = await devtools(await browser.endpoint);
        const url = `http://127.0.0.1:${server.address().port}/${path}`;
        const { targetId } = await tools.call("Target.createTarget", { url });
        const { sessionId } = await tools.call("Target.attachToTarget", { targetId, flatten: true });
        await loaded(tools, sessionId, url);
        const evaluate = async (fn, ...args) => {
            const { result, exceptionDetails } = await tools.call("Runtime.evaluate", {
                expression: `(${fn})(...${JSON.stringify(args)})`, awaitPromise: true, returnByValue: true }, sessionId);
            if (exceptionDetails) throw new Error(exceptionDetails.exception?.description ?? exceptionDetails.text);
            return result.value;
        };
        return { evaluate, close: async () => { tools.close(); await cleanup(); } };
    } catch (error) { await cleanup(); throw error; }
}
