// Client entry point (runs in the browser). Bundled by esbuild into
// www/js/react-ssr.js. Reads the JSON blob the server inlined, hydrates the
// server markup produced by render.tsx, and from then on the component
// behaves like a normal interactive React app (useEffect, live input,
// live clock).
//
// This bundle must NEVER reference `process` — esbuild replaces
// process.env.NODE_ENV but leaves other globals untouched, and a stray
// ReferenceError would abort hydration. Server-only values (serverTime,
// nodeVersion) arrive inside __SSR_DATA__.

import { createRoot, hydrateRoot } from "react-dom/client";
import { BrowserRouter } from "react-router";
import { App, type AppProps } from "./App";

declare global {
  interface Window {
    __SSR_DATA__?: AppProps;
  }
}

const data: AppProps = window.__SSR_DATA__ ?? {
  params: {},
  method: "GET",
  serverTime: "",
  nodeVersion: "",
  mode: "ssr",
};

const root = document.getElementById("root");
if (root) {
  const app = (
    <BrowserRouter>
      <App {...data} />
    </BrowserRouter>
  );
  // CSR mode: the server sent an empty shell (no renderToString work), so
  // there is nothing to hydrate — mount fresh. SSR mode: adopt the markup
  // render.tsx produced so the first paint matches.
  if (data.mode === "csr") {
    createRoot(root).render(app);
  } else {
    hydrateRoot(root, app);
  }
}