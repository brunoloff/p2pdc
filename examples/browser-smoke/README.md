# Browser WebRTC smoke test

Run `node examples/browser-smoke/run.cjs` from any directory. It requires a
recent Node with `fetch` and `WebSocket`, plus Chromium on `PATH`; set
`P2PDC_BROWSER_BINARY` to select another Chromium executable. The driver starts
its own headless browser with a fresh `/tmp` profile and stops that browser on
completion. It uses `--no-sandbox`, which is needed in the current test container.

The page connects two browser peer connections using in-process SDP signaling,
echoes binary data, and reports the selected ICE candidate types when available.
A failed echo, unavailable candidate types, or timeout exits nonzero. This checks a local direct browser pair;
it does not test the library, Wasm, TURN, discovery, cross-network connectivity,
browser-to-native interoperability, or browser engines other than Chromium.
