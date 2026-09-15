// Shared React component tree used by BOTH the SSR entry (server.tsx) and
// the hydration client (client.tsx). Both bundle this file separately with
// the SAME route tree; only the router differs:
//   SSR   -> <StaticRouter location={path}> in render.tsx
//   client-> <BrowserRouter> in client.tsx
// so the first client render matches the server markup exactly and
// hydrateRoot() attaches event handlers without a mismatch warning.
//
// Route map:
//   /react            Home        (SSR + params echo + form)
//   /react/about      About       (lazy chunk -> route-level code splitting)
//   /react/counter    Counter     (lazy chunk -> useState interaction)
//   /react/*          NotFound    (lazy chunk)
//   *                 Home        (default for e.g. /cgi-bin/react-ssr.cgi)

import React, {
  Component,
  lazy,
  Suspense,
  use,
  useState,
  useMemo,
  useId,
  useEffect,
} from "react";
import { Routes, Route, Link, NavLink, Outlet, useLocation } from "react-router";
import type { Params, RenderMode } from "./types";

// Route-level code splitting: the three pages below are the ONLY async
// chunks (esbuild --splitting + React.lazy), so the eager Home payload on
// first load stays tiny. On the server these resolve from the inlined CJS
// bundle, so SSR still emits full page content for deep links.
const About = lazy(() => import("./pages/About"));
const Counter = lazy(() => import("./pages/Counter"));
const Chat = lazy(() => import("./pages/Chat"));
const NotFound = lazy(() => import("./pages/NotFound"));

// Everything the component needs to render, supplied on the server and
// replayed verbatim from __SSR_DATA__ on the client (hydration contract).
export interface AppProps {
  params: Params;
  method: string;
  serverTime: string;
  // Node version is resolved server-side ONLY (render.tsx never ships to
  // the browser) and passed through the blob so both sides agree.
  nodeVersion: string;
  // SSR vs CSR mount mode, echoed back from the request (?mode=csr).
  mode: RenderMode;
}

const navLink = (isActive: boolean) =>
  "rounded-md px-3 py-1.5 text-sm font-medium transition " +
  (isActive
    ? "bg-accent/15 text-accent"
    : "text-slate-400 hover:bg-surface hover:text-slate-200");

// Shared page chrome: sticky nav + content <Outlet/>. The server renders
// the same tree, so the badge/links exist before hydration too.
function Layout({ nodeVersion, mode }: { nodeVersion: string; mode: RenderMode }) {
  // The chat page gets the full viewport width; everything else keeps the
  // narrow reading column.
  const { pathname } = useLocation();
  const wide = pathname.startsWith("/react/chat");
  const shell = wide ? "mx-auto flex max-w-6xl items-center gap-2 px-5 py-3" : "mx-auto flex max-w-3xl items-center gap-2 px-5 py-3";
  return (
    <div className="min-h-screen bg-ink font-sans text-slate-200 antialiased">
      <header className="sticky top-0 z-10 border-b border-edge bg-ink/80 backdrop-blur">
        <nav className={shell}>
          <Link
            to="/react"
            className="mr-auto font-bold tracking-tight no-underline text-slate-100 hover:text-accent"
          >
            <code className="text-amber-300">agent-httpd</code>
            <span className="mx-1 text-slate-500">&#215;</span>
            <span className="bg-gradient-to-r from-sky-400 to-violet-400 bg-clip-text text-transparent">
              React
            </span>
          </Link>
          <NavLink to="/react" end className={({ isActive }) => navLink(isActive)}>
            Home
          </NavLink>
          <NavLink to="/react/about" className={({ isActive }) => navLink(isActive)}>
            About
          </NavLink>
          <NavLink to="/react/counter" className={({ isActive }) => navLink(isActive)}>
            Counter
          </NavLink>
          <NavLink to="/react/chat" className={({ isActive }) => navLink(isActive)}>
            Chat
          </NavLink>
          {/* Plain <a>, not <Link>: the site home lives OUTSIDE the /react
              route tree, so leaving it must be a full page load. */}
          <a
            href="/"
            title="Back to the site home page"
            className={navLink(false)}
          >
            &larr; Site
          </a>
        </nav>
      </header>

      <main className={wide ? "mx-auto max-w-6xl px-5 py-6" : "mx-auto max-w-3xl px-5 py-10"}>
        <Outlet />
      </main>

      <footer className={"mx-auto flex items-center justify-between px-5 pb-8 text-xs text-slate-500 " + (wide ? "max-w-6xl" : "max-w-3xl")}>
        <span>
          React {React.version} &middot; Node {nodeVersion}
        </span>
        <span className="flex items-center gap-2">
          {/* Full <a> navigation: the mode is a server-side switch, so it
              must reload the page for the backend to pick it up. */}
          <span>
            Render: {mode === "ssr" ? "SSR" : "CSR"}
            {mode === "ssr" ? (
              <a href="?mode=csr" className="ml-2 text-accent no-underline hover:text-slate-200">
                switch to CSR
              </a>
            ) : (
              <a href="?mode=ssr" className="ml-2 text-accent no-underline hover:text-slate-200">
                switch to SSR
              </a>
            )}
          </span>
        </span>
      </footer>
    </div>
  );
}

// Streaming demo: suspends for `ms` so render.tsx flushes the page shell
// first and this section arrives as a LATER <=stream chunk. Module-level
// promise cache means hydration resolves instantly for the same delay.
const delayPromises = new Map<number, Promise<void>>();
function DelayedSection({ ms }: { ms: number }) {
  let p = delayPromises.get(ms);
  if (!p) {
    p = new Promise<void>((resolve) => setTimeout(resolve, ms));
    delayPromises.set(ms, p);
  }
  use(p);
  return (
    <p className="text-sm text-slate-300">
      This section was rendered after a {ms}ms simulated delay &mdash; the shell
      above arrived first via streaming SSR, then this chunk streamed in.
    </p>
  );
}

const RouteFallback = () => (
  <section className="rounded-2xl border border-edge bg-surface p-6 text-center text-sm text-slate-400 shadow-xl shadow-black/20">
    Loading route chunk&hellip;
  </section>
);

function Home({ params, method, serverTime, nodeVersion }: AppProps) {
  const uid = useId();
  const [name, setName] = useState(() => params.name || "");
  const [now, setNow] = useState(serverTime || "");
  const [hydrated, setHydrated] = useState(false);

  // Streaming demo gate: ?delay=1200 -> show the suspended section.
  const delayRaw = Number(params.delay);
  const delayMs = Number.isFinite(delayRaw) && delayRaw > 0 ? Math.round(delayRaw) : 0;

  // Runs only in the browser, after the server markup is hydrated.
  useEffect(() => {
    setHydrated(true);
    setNow(
      new Date().toLocaleString("en-GB", {
        dateStyle: "medium",
        timeStyle: "medium",
      })
    );
  }, []);

  const keys = useMemo(() => Object.keys(params).sort(), [params]);
  const greeting = useMemo(
    () => `Hello, ${name.trim() || "server-side visitor"}`,
    [name]
  );

  const label = "block text-sm text-slate-400 mt-2 mb-1";
  const input =
    "w-full rounded-lg border border-edge bg-ink px-3 py-2 text-slate-100 " +
    "outline-none transition focus:border-accent focus:ring-2 focus:ring-accent/30";
  const cell = "py-1.5 pr-4 text-right text-slate-500 text-sm";
  const val = "py-1.5 text-slate-200 font-mono";

  return (
    <>
      <section className="text-center">
        <p className="mb-3 text-xs font-bold uppercase tracking-[0.35em] text-accent">
          Server-side React &middot; {React.version}
        </p>
        <h1 className="mt-0 bg-gradient-to-r from-sky-400 via-violet-400 to-fuchsia-400 bg-clip-text text-4xl font-extrabold leading-tight text-transparent">
          React Router &#215; AgentHTTPD
        </h1>
        <p className="mt-2 text-slate-400">
          A client-routed React app, server-rendered by a C web server, then{" "}
          <span className="text-slate-300">hydrated in the browser</span>.
        </p>
      </section>

      {delayMs > 0 && (
        <section className="my-6 rounded-2xl border border-dashed border-sky-500/50 bg-surface p-4 shadow-xl shadow-black/20">
          <Suspense
            fallback={
              <p className="text-sm text-slate-400">
                Streaming demo: waiting for the delayed section&hellip;
              </p>
            }
          >
            <DelayedSection ms={delayMs} />
          </Suspense>
        </section>
      )}

      <section className="my-6 rounded-2xl border border-edge bg-surface p-6 shadow-xl shadow-black/20">
        <span
          className={`mb-3 inline-block rounded-full px-3 py-0.5 text-[11px] font-bold uppercase tracking-wider ${
            hydrated ? "bg-amber-400 text-amber-950" : "bg-emerald-400 text-emerald-950"
          }`}
        >
          {hydrated ? "Hydrated" : "SSR"}
        </span>
        <h2 className="mt-0 text-xl font-semibold">Rendered on the server</h2>
        <p className="text-sm leading-6 text-slate-400">
          This page is produced by a React component tree executed as a CGI
          program behind <code className="rounded bg-ink px-1.5 py-0.5 text-amber-300">agent-httpd</code>{" "}
          using{" "}
          <code className="rounded bg-ink px-1.5 py-0.5 text-amber-300">react-dom/server</code>{" "}
          + <code className="rounded bg-ink px-1.5 py-0.5 text-amber-300">StaticRouter</code>,
          then hydrated with{" "}
          <code className="rounded bg-ink px-1.5 py-0.5 text-amber-300">BrowserRouter</code>.{" "}
          {method} parameters echoed below.
        </p>
        <table className="mt-4 w-full border-collapse">
          <tbody>
            <tr className="border-b border-edge/60">
              <th className={cell + " font-normal"}>Request method</th>
              <td className={val}>{method}</td>
            </tr>
            <tr className="border-b border-edge/60">
              <th className={cell + " font-normal"}>Hooks in use</th>
              <td className={val}>
                useState &middot; useMemo &middot; useId &middot; useEffect
              </td>
            </tr>
            <tr className="border-b border-edge/60">
              <th className={cell + " font-normal"}>Derived greeting</th>
              <td className={val}>{greeting}</td>
            </tr>
            <tr className="border-b border-edge/60">
              <th className={cell + " font-normal"}>Clock</th>
              <td className={val}>
                {now ||
                  (serverTime ? "waiting for hydration&hellip;" : "—")}
              </td>
            </tr>
            <tr className="border-b border-edge/60">
              <th className={cell + " font-normal"}>Router</th>
              <td className={val}>react-router v8 &middot; Browser / Static</td>
            </tr>
          </tbody>
        </table>
      </section>

      <section className="my-6 rounded-2xl border border-edge bg-surface p-6 shadow-xl shadow-black/20">
        <h3 className="mt-0 text-lg font-semibold">
          Received {method} parameters
        </h3>
        {keys.length === 0 ? (
          <p className="text-sm text-slate-400">
            No parameters. Try{" "}
            <code className="rounded bg-ink px-1.5 py-0.5 text-amber-300">?name=Alice</code>{" "}
            or submit the form via POST.
          </p>
        ) : (
          <table className="mt-3 w-full border-collapse">
            <thead>
              <tr className="border-b border-edge text-left text-xs uppercase tracking-wider text-slate-500">
                <th className="py-1.5 pr-4 font-medium">Key</th>
                <th className="py-1.5 font-medium">Value</th>
              </tr>
            </thead>
            <tbody>
              {keys.map((k) => (
                <tr key={k} className="border-b border-edge/40">
                  <td className="py-1.5 pr-4 font-mono text-sky-300">{k}</td>
                  <td className="py-1.5 font-mono text-slate-200">{params[k]}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>

      <section className="my-6 rounded-2xl border border-edge bg-surface p-6 shadow-xl shadow-black/20">
        <h3 className="mt-0 text-lg font-semibold">Try it</h3>
        <p className="text-sm text-slate-400">
          Type in the field and the greeting above updates live — that
          listener only exists because the client bundle hydrated this node.
        </p>
        <form method={method.toLowerCase()} action="/cgi-bin/react-ssr.cgi" className="mt-4">
          <label htmlFor={uid + "-name"} className={label}>
            Name
            <input
              id={uid + "-name"}
              type="text"
              name="name"
              placeholder="Alice"
              value={name}
              onChange={(e) => setName(e.target.value)}
              className={input}
            />
          </label>
          <label htmlFor={uid + "-msg"} className={label}>
            Message
            <input
              id={uid + "-msg"}
              type="text"
              name="message"
              placeholder="Hello from React"
              className={input}
            />
          </label>
          <button
            type="submit"
            className="mt-4 rounded-lg bg-accent px-5 py-2.5 font-bold text-accent-ink transition hover:bg-accent-hi focus:outline-none focus:ring-2 focus:ring-accent/40"
          >
            Submit via {method.toUpperCase()}
          </button>
        </form>
        <p className="mt-4 text-sm">
          <Link
            to="/react/about"
            className="font-medium text-accent no-underline transition hover:text-accent-hi hover:underline"
          >
            Router demo: the About + Counter pages use{" "}
            <code>Link</code> &rarr;
          </Link>
        </p>
      </section>
    </>
  );
}

// The shared route tree. Lazy routes are wrapped in their own Suspense so a
// missing chunk only falls back inside that page, never the whole app.
// Rendered by StaticRouter (SSR) or BrowserRouter (client); unknown
// /react/* paths get NotFound, everything else (e.g. the bare
// /cgi-bin/react-ssr.cgi CGI entry) falls back to Home.

// Top-level error boundary: a render exception in any page (or its lazy
// chunk) shows a recoverable fallback instead of blanking the whole app.
// On SSR a throw would otherwise kill the CGI process producing garbage
// HTML, so the boundary catches mid-render here too.
interface EBState {
  error: Error | null;
}
class ErrorBoundary extends Component<{ children: React.ReactNode }, EBState> {
  state: EBState = { error: null };
  static getDerivedStateFromError(error: Error): EBState {
    return { error };
  }
  render() {
    if (this.state.error) {
      // Self-contained fallback: Layout renders <Outlet/>, which is null
      // outside a route context, so the boundary shows its own full page.
      return (
        <div className="min-h-screen bg-ink font-sans text-slate-200 antialiased">
          <main className="mx-auto max-w-3xl px-5 py-16">
            <section className="rounded-2xl border border-red-500/40 bg-surface p-8 text-center shadow-xl shadow-black/20">
              <p className="text-sm font-semibold text-red-400">Something went wrong rendering this page.</p>
              <p className="mt-2 break-all font-mono text-xs text-slate-400">{String(this.state.error)}</p>
              <a
                href="/react"
                className="mt-5 inline-block rounded-lg bg-accent px-4 py-2 text-sm font-bold text-accent-ink no-underline transition hover:bg-accent-hi"
              >
                Reload
              </a>
            </section>
          </main>
        </div>
      );
    }
    return this.props.children;
  }
}

export function App(props: AppProps) {
  return (
    <ErrorBoundary>
      <Routes>
        <Route element={<Layout nodeVersion={props.nodeVersion} mode={props.mode} />}>
          <Route path="/react" element={<Home {...props} />} />
          <Route
            path="/react/about"
            element={
              <Suspense fallback={<RouteFallback />}>
                <About params={props.params} method={props.method} />
              </Suspense>
            }
          />
          <Route
            path="/react/counter"
            element={
              <Suspense fallback={<RouteFallback />}>
                <Counter />
              </Suspense>
            }
          />
          <Route
            path="/react/chat"
            element={
              <Suspense fallback={<RouteFallback />}>
                <Chat />
              </Suspense>
            }
          />
          <Route
            path="/react/*"
            element={
              <Suspense fallback={<RouteFallback />}>
                <NotFound />
              </Suspense>
            }
          />
          <Route path="*" element={<Home {...props} />} />
        </Route>
      </Routes>
    </ErrorBoundary>
  );
}