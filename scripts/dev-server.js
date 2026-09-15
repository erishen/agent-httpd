#!/usr/bin/env node
/*
 * Development server with true HMR for the AgentHTTPD React project.
 *
 * Architecture (dev):
 *   - ONE C process (bin/agent-httpd) is the only public HTTP server, on
 *     PORT. It serves static files, CGI, /health, the native chat route and
 *     error pages itself (the same code path as production). Everything it
 *     cannot render — Vite client module transforms under /@ and /src/,
 *     the dev SSR pages under /react/ (except the native chat route) — is
 *     proxied to this script over an internal Vite server bound to
 *     127.0.0.1:PORT+2 (never exposed). HMR WebSocket upgrades are
 *     tunnelled by the C process to the same internal port.
 *   - This script only runs Vite (+ the watcher / tailwind rebuild /
 *     react-refresh preamble) on that internal port. It no longer owns an
 *     HTTP listener of its own, and no express routing for the browser.
 *
 * The C binary is spawned here with -v <internal-port>; credentials from
 * .env are handed to it explicitly. Same behavior as production for the
 * fast path; dev-only module transforms and SSR come from Vite.
 */

const PORT = Number(process.env.PORT || 3100);
const VITE_PORT = PORT + 2; // internal Vite service, 127.0.0.1 only

const path = require("path");
const fs = require("fs");
const os = require("os");
const http = require("http");
const { createRequire } = require("module");
const { spawn, spawnSync } = require("child_process");
const SRC_DIR = path.resolve(__dirname, "../cgi-bin/react-ssr");
const ROOT_DIR = path.resolve(__dirname, "..");
const HTTPD_BIN = path.join(ROOT_DIR, "bin/agent-httpd");

// This script lives in scripts/, but every dependency (express, vite,
// @vitejs/plugin-react) is installed in cgi-bin/react-ssr/node_modules.
const localRequire = createRequire(path.join(SRC_DIR, "package.json"));

const express = localRequire("express");
const { createServer: createViteServer } = localRequire("vite");

const SSR_RE = /render\.tsx|App\.tsx|server\/.+\.tsx?$|types\.tsx?$|pages\/.+\.tsx$/;
const SSR_WATCH = ["server", "App.tsx", "types.ts", "tailwind.css", "styles/main.css", "pages"].map((f) =>
  path.join(SRC_DIR, f)
);
const TAILWIND_BIN = path.join(SRC_DIR, "node_modules/.bin/tailwindcss");

// render.tsx inlines compiled Tailwind as text; serve it as a string so the
// dev <head> carries the same <style> block the esbuild bundles embed.
const tailwindCssPath = path.join(SRC_DIR, "tailwind.css");
const cssAsStringPlugin = {
  name: "tailwind-css-as-string",
  enforce: "pre",
  load(id) {
    try {
      if (path.resolve(id) === tailwindCssPath) {
        return "export default " + JSON.stringify(fs.readFileSync(tailwindCssPath, "utf8"));
      }
    } catch {
      /* file gone (e.g. mid rebuild) — fall through to Vite's own loader */
    }
    return null;
  },
};

async function main() {
  const app = express();
  // The C server injects security headers on its own responses; match that
  // for everything we stream back through the -v proxy so the browser sees
  // one consistent header set. No server fingerprint either.
  app.disable("x-powered-by");
  app.use((req, res, next) => {
    res.setHeader("X-Content-Type-Options", "nosniff");
    res.setHeader("X-Frame-Options", "DENY");
    res.setHeader("Referrer-Policy", "no-referrer");
    next();
  });

  // The internal HTTP server owns Vite's HMR "upgrade" listener too, so the
  // websocket shares this single internal origin (the browser reaches it via
  // the C process's tunnel).
  const httpServer = http.createServer(app);

  const vite = await createViteServer({
    root: SRC_DIR,
    appType: "custom",
    // NOTE: do NOT register @vitejs/plugin-react here — vite.config.js in
    // SRC_DIR already does (double registration broke hydration).
    server: { middlewareMode: "ssr", hmr: { server: httpServer } },
    plugins: [cssAsStringPlugin],
  });

  app.use(vite.middlewares);

  // Dev client entry: client.tsx transformed for the browser.
  app.use("/react/react-ssr.tsx", async (req, res) => {
    try {
      const result = await vite.transformRequest("/client.tsx");
      res.status(200).type("js").send(result ? result.code : "");
    } catch (e) {
      res.status(500).type("text").send("transform failed: " + e.message);
    }
  });

  // SSR for /react/* — same render core as production, HMR-aware.
  app.use("/react", async (req, res) => {
    try {
      const url = req.originalUrl || req.url || "/react";
      const qi = url.indexOf("?");
      const pathname = qi >= 0 ? url.slice(0, qi) : url;
      const query = qi >= 0 ? url.slice(qi + 1) : "";

      const params = {};
      query.split("&").forEach((pair) => {
        if (!pair) return;
        const eq = pair.indexOf("=");
        const k = eq >= 0 ? pair.slice(0, eq) : pair;
        const v = eq >= 0 ? pair.slice(eq + 1) : "";
        try {
          params[decodeURIComponent(k.replace(/\+/g, " "))] = decodeURIComponent(
            v.replace(/\+/g, " ")
          );
        } catch {
          /* ignore malformed pairs */
        }
      });

      const mode = params.mode === "csr" ? "csr" : "ssr";
      delete params.mode;

      const renderModule = await vite.ssrLoadModule("/server/render.tsx");
      const { renderPage } = renderModule;
      const html = await renderPage(params, "GET", pathname || "/react", mode);

      // Swap the prod bundle tag for the HMR-connected dev entry.
      const devHtml = html.replace(
        /<script type="module" src="\/js\/react-ssr\.js"><\/script>/,
        '<script type="module" src="/react/react-ssr.tsx"></script>'
      );
      // react-refresh preamble must be the first script in <head>.
      const devHtml2 = devHtml.replace(
        /<head>/,
        "<head>\n" +
          '<script type="module">import RefreshRuntime from "/@react-refresh";\n' +
          "RefreshRuntime.injectIntoGlobalHook(window);\n" +
          "window.$RefreshReg$ = () => {};\n" +
          "window.$RefreshSig$ = () => (type) => type;\n" +
          "window.__vite_plugin_react_preamble_installed__ = true;</script>"
      );

      res.status(200).type("html").send(devHtml2);
    } catch (error) {
      console.error("[dev-server] SSR error:", error);
      res.status(500).type("text").send("SSR error (details in server log)");
    }
  });

  // Source-watching: invalidate SSR modules and notify browsers.
  let twTimer = null;
  const rebuildTailwind = (reason) => {
    clearTimeout(twTimer);
    twTimer = setTimeout(() => {
      // --minify must match scripts/build-ssr.sh: this writes the TRACKED
      // artifact cgi-bin/react-ssr/tailwind.css, so omitting the flag rewrites
      // it in a different (pretty) form and every dev run leaves a 1400-line
      // spurious diff behind.
      require("child_process").execFile(
        TAILWIND_BIN,
        [
          "-i", path.join(SRC_DIR, "styles/main.css"),
          "-o", path.join(SRC_DIR, "tailwind.css"),
          "--minify",
        ],
        (err) => {
          if (err) {
            console.error("[dev-server] tailwindcss rebuild failed:", err.message);
            return;
          }
          console.log("[dev-server] tailwind.css rebuilt (" + reason + ")");
        }
      );
    }, 120);
  };

  const invalidateSsrModules = () => {
    try {
      for (const env of Object.values(vite.environments || {})) {
        env.moduleGraph?.invalidateAll?.();
      }
      vite.moduleGraph.invalidateAll();
    } catch {
      /* best effort — a restart always fully recovers */
    }
  };

  const pushFullReload = (reason) => {
    invalidateSsrModules();
    vite.ws.send({ type: "full-reload" });
    console.log("[dev-server] full reload:", reason);
  };

  const onFsEvent = (file) => {
    const base = path.basename(file);
    if (base === "tailwind.css") {
      pushFullReload("tailwind.css updated");
    } else if (base === "main.css") {
      rebuildTailwind("styles/main.css edited");
    } else if (base === "App.tsx" || /pages\/.+\.tsx$/.test(file)) {
      rebuildTailwind("class names may have changed in " + base);
      pushFullReload(base + " changed");
    } else if (SSR_RE.test(file)) {
      pushFullReload("SSR module " + base + " changed");
    } else if (base.endsWith(".tsx")) {
      console.log("[dev-server] client module changed (react-refresh hot-swaps it):", base);
    }
  };
  vite.watcher.on("change", onFsEvent);
  vite.watcher.on("add", onFsEvent);
  vite.watcher.on("unlink", onFsEvent);
  vite.watcher.add(SSR_WATCH);

  // The C binary is the only public HTTP server. Spawn it with the internal
  // Vite port so /@, /src and /react SSR pages get proxied here.
  // C stderr/stdout lands here (curl errors, router sync, ...) — private.
  const devLogPath = path.join(ROOT_DIR, ".dev-httpd.log");
  try {
    fs.chmodSync(devLogPath, 0o600);
  } catch {
    /* not created yet — umask applies below via explicit chmod after open */
  }
  const log = fs.openSync(devLogPath, "a");
  try {
    fs.fchmodSync(log, 0o600);
  } catch {
    /* best effort */
  }
  const backend = spawn(
    HTTPD_BIN,
    ["-p", String(PORT), "-n", "-v", String(VITE_PORT)],
    {
      cwd: ROOT_DIR,
      env: spawnEnv(),
      stdio: ["ignore", log, log],
    }
  );
  backend.on("exit", (code) =>
    console.log("[dev-server] agent-httpd exited (" + code + ")")
  );
  await new Promise((resolve) => {
    let tries = 0;
    const ping = () => {
      const req = http.get(
        { host: "127.0.0.1", port: PORT, path: "/", timeout: 500 },
        (res) => {
          res.resume();
          resolve();
        }
      );
      req.on("timeout", () => req.destroy(new Error("timeout")));
      req.on("error", () => {
        if (++tries >= 60) resolve();
        else setTimeout(ping, 100);
      });
    };
    ping();
  });

  // Internal Vite service only — the browser never connects here directly.
  httpServer.listen(VITE_PORT, "127.0.0.1", () => {
    console.log(`AgentHTTPD HMR dev server:  http://localhost:${PORT}`);
    console.log(`  SSR + HMR:   http://localhost:${PORT}/react/?name=Alice`);
    console.log(`  CSR mode:    http://localhost:${PORT}/react/?mode=csr`);
    console.log(`  Health:      http://localhost:${PORT}/health`);
    console.log(`  Vite (internal): 127.0.0.1:${VITE_PORT} (proxied/tunnelled by C)`);
    console.log("  Edit App.tsx / render.tsx / styles/main.css -> browser auto-reloads.");
    console.log("  Client component edits hot-swap without a reload (react-refresh).");
  });

  const shutdown = () => {
    if (backend) backend.kill("SIGTERM");
    httpServer.close(() => process.exit(0));
    setTimeout(() => process.exit(0), 1500).unref();
  };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

// Hand the C backend the repo's .env (it cannot read .env itself). .env is
// authoritative, overriding stale LLM_*/AGENT_* in this shell; an explicitly
// EMPTY value (smoke-test.sh's offline demo engine) wins over the file.
function spawnEnv() {
  const spawnEnv = { ...process.env };
  let uvDir = "";
  if (process.env.UV_DIR) {
    uvDir = process.env.UV_DIR.trim();
  } else {
    const found = spawnSync("/usr/bin/which", ["uv"], { encoding: "utf8" });
    if (found.status === 0 && found.stdout) {
      uvDir = path.dirname(found.stdout.trim().split("\n").pop());
    }
  }
  // Last-resort fallback for a manually unpacked uv (nothing named `uv` on
  // PATH): probe the conventional ~/Software/uv-<triple> location by name
  // instead of hard-coding one machine's absolute path — a committed
  // $HOME/<account>/... would leak the developer's account and break for
  // everyone else. UV_DIR above is the supported override.
  if (!uvDir) {
    const softwareDir = path.join(os.homedir(), "Software");
    try {
      const hit = fs
        .readdirSync(softwareDir)
        .filter((n) => /^uv[-.]/.test(n))
        .sort()
        .pop();
      if (hit && fs.existsSync(path.join(softwareDir, hit, "uv"))) {
        uvDir = path.join(softwareDir, hit);
      }
    } catch {
      /* no ~/Software — nothing to probe */
    }
  }
  if (uvDir) spawnEnv.PATH = (process.env.PATH ? process.env.PATH + ":" : "") + uvDir;
  const envPath = path.join(ROOT_DIR, ".env");
  if (fs.existsSync(envPath)) {
    try {
      fs.chmodSync(envPath, 0o600);
    } catch {
      /* best effort */
    }
    for (const line of fs.readFileSync(envPath, "utf8").split("\n")) {
      const m = /^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$/.exec(line.trimEnd());
      if (!m) continue;
      const key = m[1];
      if (process.env[key] === "") continue; // explicit "" = force this value (demo engine)
      let val = m[2];
      if (val.length >= 2 && val[0] === val[val.length - 1] && (val[0] === '"' || val[0] === "'")) {
        val = val.slice(1, -1);
      }
      spawnEnv[key] = val;
    }
  }
  return spawnEnv;
}

main().catch((error) => {
  console.error("Dev server failed:", error);
  process.exit(1);
});
