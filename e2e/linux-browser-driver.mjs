import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";

async function until(predicate, description) {
  const deadline = performance.now() + 15000;
  while (!(await predicate())) {
    if (performance.now() >= deadline) throw new Error(`timed out: ${description}`);
    await delay(150);
  }
}

export async function browserDriver() {
  if (process.env.MREC_BROWSER === "firefox") {
    const base = "http://127.0.0.1:4445";
    async function request(path, body) {
      const response = await fetch(base + path, {
        method: body === undefined ? "GET" : "POST",
        headers: { "Content-Type": "application/json" },
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: AbortSignal.timeout(35000),
      });
      const { value } = await response.json();
      assert(response.ok, JSON.stringify(value));
      return value;
    }
    await until(async () => {
      try {
        return (await request("/status")).ready;
      } catch {
        return false;
      }
    }, "geckodriver startup");
    const session = await request("/session", {
      capabilities: {
        alwaysMatch: {
          browserName: "firefox",
          acceptInsecureCerts: true,
          "moz:firefoxOptions": {
            binary: process.env.FIREFOX,
            prefs: {
              "accessibility.force_disabled": -1,
              "network.dns.localDomains": "meet.google.com,unrelated.example",
              "network.proxy.type": 0,
              "media.navigator.permission.disabled": true,
              "media.autoplay.default": 0,
              "media.autoplay.block-webaudio": false,
            },
          },
        },
      },
    });
    const path = `/session/${session.sessionId}`;
    const navigate = (url) => request(`${path}/url`, { url });
    await navigate("https://meet.google.com:9443/abc-defg-hij");
    return {
      pid: session.capabilities["moz:processID"],
      async evaluate(expression) {
        const result = await request(`${path}/execute/async`, {
          script: `const done = arguments[arguments.length - 1];
            Promise.resolve().then(() => (${expression})).then(
              value => done({value}), error => done({error: String(error)}));`,
          args: [],
        });
        assert(!result.error, result.error);
        await delay(400);
        return result.value;
      },
      navigate,
      async close() {
        const response = await fetch(base + path, { method: "DELETE" });
        assert(response.ok, "Firefox session cleanup failed");
      },
    };
  }

  let page;
  await until(async () => {
    try {
      const pages = await (await fetch("http://127.0.0.1:9223/json/list")).json();
      page = pages.find((item) => item.url.includes("meet.google.com:9443"));
      return page;
    } catch {
      return false;
    }
  }, "Chrome startup");
  const socket = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((resolve) => socket.addEventListener("open", resolve, { once: true }));
  let nextId = 1;
  async function command(method, params) {
    const id = nextId++;
    let handler;
    const response = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        socket.removeEventListener("message", handler);
        reject(new Error(`timed out: ${method}`));
      }, 15000);
      handler = (event) => {
        const message = JSON.parse(event.data);
        if (message.id !== id) return;
        socket.removeEventListener("message", handler);
        clearTimeout(timeout);
        resolve(message);
      };
      socket.addEventListener("message", handler);
    });
    socket.send(JSON.stringify({ id, method, params }));
    const result = await response;
    assert(!result.error, JSON.stringify(result));
    return result.result;
  }
  return {
    pid: Number(process.env.TEST_BROWSER_PID),
    async evaluate(expression) {
      const result = await command("Runtime.evaluate", {
        expression,
        awaitPromise: true,
        returnByValue: true,
      });
      assert(!result.exceptionDetails, JSON.stringify(result));
      await delay(400);
      return result.result.value;
    },
    navigate: (url) => command("Page.navigate", { url }),
    close: () => socket.close(),
  };
}
