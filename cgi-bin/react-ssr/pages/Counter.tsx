// Counter page — lazy-loaded chunk. The server always renders 0; the
// buttons prove the client bundle hydrated and attached listeners.
import { useState } from "react";
import { Link } from "react-router";

function Counter() {
  const [count, setCount] = useState(0);
  return (
    <section className="rounded-2xl border border-edge bg-surface p-6 text-center shadow-xl shadow-black/20">
      <span className="mb-3 inline-block rounded-full bg-amber-400 px-3 py-0.5 text-[11px] font-bold uppercase tracking-wider text-amber-950">
        Live
      </span>
      <h1 className="mt-0 text-2xl font-bold">Client-side counter</h1>
      <p className="mx-auto mt-2 max-w-md text-sm text-slate-400">
        The server always renders <code className="text-amber-300">0</code>. The
        buttons below only work after hydration — click proof that the listener
        is attached client-side.
      </p>
      <div className="mt-6 bg-gradient-to-br from-sky-400 to-violet-400 bg-clip-text text-7xl font-extrabold text-transparent">
        {count}
      </div>
      <div className="mt-6 flex items-center justify-center gap-3">
        <button
          onClick={() => setCount((c) => c - 1)}
          className="rounded-lg border border-edge bg-ink px-5 py-2.5 font-bold text-slate-200 transition hover:border-accent hover:text-accent"
        >
          &#8722;1
        </button>
        <button
          onClick={() => setCount((c) => c + 1)}
          className="rounded-lg bg-accent px-6 py-2.5 font-bold text-accent-ink transition hover:bg-accent-hi"
        >
          +1
        </button>
        <button
          onClick={() => setCount(0)}
          className="rounded-lg border border-edge bg-ink px-5 py-2.5 font-bold text-slate-400 transition hover:border-accent hover:text-accent"
        >
          Reset
        </button>
      </div>
      <p className="mt-6 text-xs text-slate-500">
        Visited via <code className="text-sky-300">/react/counter</code> &middot;
        use <Link to="/react" className="text-accent hover:underline">Home</Link>{" "}
        to see the SSR page.
      </p>
    </section>
  );
}

export default Counter;