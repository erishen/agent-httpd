// About page. Split into its own module so the client bundle can lazy-load
// it as a separate chunk (route-based code splitting via React.lazy).
import { Link } from "react-router";
import type { Params } from "../types";

function About({ params, method }: { params: Params; method: string }) {
  return (
    <section className="rounded-2xl border border-edge bg-surface p-6 shadow-xl shadow-black/20">
      <span className="mb-3 inline-block rounded-full bg-emerald-400 px-3 py-0.5 text-[11px] font-bold uppercase tracking-wider text-emerald-950">
        SSR deep-link
      </span>
      <h1 className="mt-0 text-2xl font-bold">How the routing works here</h1>
      <p className="text-sm leading-6 text-slate-400">
        You reached this page via <code className="rounded bg-ink px-1.5 py-0.5 text-amber-300">{method}</code>.
        The route tree is shared, only the router wrapper differs:
      </p>
      <ol className="mt-4 space-y-2 text-sm">
        <li className="rounded-lg border border-edge/60 bg-ink/40 p-3">
          <span className="font-semibold text-slate-200">1. Browser hits <code className="text-sky-300">/react/about</code></span>
          <span className="block text-slate-400">
            agent-httpd relays it via FastCGI to the resident backend (-R).
          </span>
        </li>
        <li className="rounded-lg border border-edge/60 bg-ink/40 p-3">
          <span className="font-semibold text-slate-200">2. Server renders</span>
          <span className="block text-slate-400">
            <code className="text-amber-300">StaticRouter</code> with that path
            produces the exact About markup (SSR deep-link support).
          </span>
        </li>
        <li className="rounded-lg border border-edge/60 bg-ink/40 p-3">
          <span className="font-semibold text-slate-200">3. Client hydrates</span>
          <span className="block text-slate-400">
            <code className="text-amber-300">BrowserRouter</code> takes over;
            NavLinks below switch without a page reload.
          </span>
        </li>
      </ol>
      <div className="mt-4 flex gap-3 text-sm">
        <Link to="/react" className="text-accent hover:text-accent-hi hover:underline">Home</Link>
        <Link to="/react/counter" className="text-accent hover:text-accent-hi hover:underline">Counter</Link>
      </div>
      {params && Object.keys(params).length > 0 && (
        <p className="mt-4 text-xs text-slate-500">
          Got params on this route:{" "}
          {Object.entries(params).map(([k, v]) => `${k}=${v}`).join(", ")}.
        </p>
      )}
    </section>
  );
}

export default About;