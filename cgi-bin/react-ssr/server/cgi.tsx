// CGI entry point (renders server-side, one process per request). Bundled
// by esbuild into cgi-bin/react-ssr.cgi. Reads the request from CGI
// environment variables and prints a full HTML document. The heavier
// resident live server is server-main.tsx (FastCGI).

import { isRenderMode, parseQuery, renderPage } from "./render";

const REQUEST_METHOD = process.env.REQUEST_METHOD || "GET";
const QUERY_STRING = process.env.QUERY_STRING || "";
const CONTENT_LENGTH = parseInt(process.env.CONTENT_LENGTH || "0", 10);

// REQUEST_URI (full path, may carry "?query") drives router matching.
const rawUri = process.env.REQUEST_URI || "";
const pathname = (rawUri.split("?")[0] || "/").replace(/\/+$/, "");

const METHOD = REQUEST_METHOD.toUpperCase();
const isPost = METHOD === "POST";
const methodLabel = isPost ? "POST" : "GET";

function readStdin(length: number): Promise<string> {
  return new Promise((resolve) => {
    if (!length) {
      resolve("");
      return;
    }
    let data = "";
    process.stdin.on("data", (chunk: Buffer) => (data += chunk));
    process.stdin.on("end", () => resolve(data));
  });
}

function toQueryString(raw: string): string {
  return METHOD === "POST" ? raw : QUERY_STRING;
}

readStdin(CONTENT_LENGTH)
  .then((body) => {
    const params = parseQuery(toQueryString(body));
    // ?mode=csr|ssr drives which render path renderPage takes; never show
    // the switch itself in the params table (it is server metadata).
    const mode = isRenderMode(params.mode) ? params.mode : "ssr";
    delete params.mode;
    return renderPage(params, methodLabel, pathname, mode);
  })
  .then((html) => {
    process.stdout.write("Content-Type: text/html; charset=utf-8\n\n");
    process.stdout.write(html);
  })
  .catch((err: Error) => {
    process.stdout.write("Content-Type: text/plain; charset=utf-8\n\n");
    process.stdout.write("React SSR CGI error: " + (err && err.message) + "\n");
    process.exit(1);
  });