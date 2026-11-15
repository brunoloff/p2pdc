const { spawn } = require("child_process");
const fs = require("fs");
const path = require("path");
const { pathToFileURL } = require("url");
const profile = fs.mkdtempSync("/tmp/p2pdc-chrome-profile-");
const browser = spawn(process.env.P2PDC_BROWSER_BINARY || "chromium", [
  "--headless",
  "--no-sandbox",
  "--disable-gpu",
  "--disable-dev-shm-usage",
  "--disable-background-networking",
  "--remote-debugging-port=0",
  `--user-data-dir=${profile}`,
  "about:blank",
], { stdio: "ignore", env: { ...process.env, XDG_CONFIG_HOME: profile } });
/** @type {Error | undefined} */
let launchError;
browser.on("error", (error) => {
  launchError = error;
});

/** @param {number} ms */
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function debuggingPort() {
  for (let i = 0; i < 100; i++) {
    if (launchError) throw launchError;
    try {
      return fs.readFileSync(path.join(profile, "DevToolsActivePort"), "utf8")
        .split("\n")[0];
    } catch (error) {
      if (
        typeof error !== "object" || error === null || !("code" in error) ||
        error.code !== "ENOENT"
      ) throw error;
      await delay(100);
    }
  }
  throw Error("browser did not start");
}

/** @param {WebSocket} ws */
async function readResult(ws) {
  await new Promise((resolve, reject) => {
    ws.onopen = resolve;
    ws.onerror = reject;
  });
  return await new Promise((resolve, reject) => {
    ws.onmessage = (event) => {
      const data = JSON.parse(event.data);
      if (data.id === 1) {
        data.result?.exceptionDetails
          ? reject(Error(JSON.stringify(data.result.exceptionDetails)))
          : resolve(data.result?.result?.value);
      }
    };
    ws.send(JSON.stringify({
      id: 1,
      method: "Runtime.evaluate",
      params: {
        expression:
          `new Promise((resolve,reject)=>{let id=setInterval(()=>{const s=document.body?.textContent;if(s && s!=='pending' && !s.includes('const a =')){clearInterval(id);resolve(s)}},50);setTimeout(()=>{clearInterval(id);reject(Error('page timeout'))},15000)})`,
        awaitPromise: true,
        returnByValue: true,
      },
    }));
  });
}

async function main() {
  try {
    const port = await debuggingPort();
    const page = pathToFileURL(path.join(__dirname, "index.html")).href;
    const response = await fetch(
      `http://127.0.0.1:${port}/json/new?${encodeURIComponent(page)}`,
      { method: "PUT" },
    );
    if (!response.ok) {
      throw Error(`browser debugging request failed: ${response.status}`);
    }
    const tab = await response.json();
    const ws = new WebSocket(tab.webSocketDebuggerUrl);
    const result = await readResult(ws);
    console.log(result);
    ws.close();
    if (
      typeof result !== "string" || !result.startsWith("echo=true ") ||
      result.includes("=unknown")
    ) {
      process.exitCode = 1;
    }
  } catch (error) {
    console.error(error);
    process.exitCode = 1;
  } finally {
    browser.kill("SIGTERM");
  }
}

main();
