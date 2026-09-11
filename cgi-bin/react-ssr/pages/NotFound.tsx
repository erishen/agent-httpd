// NotFound page — lazy-loaded chunk for unknown /react/* paths.
import { Link } from "react-router";

function NotFound() {
  return (
    <section className="rounded-2xl border border-edge bg-surface p-10 text-center shadow-xl shadow-black/20">
      <div className="bg-gradient-to-r from-sky-400 to-fuchsia-400 bg-clip-text text-6xl font-extrabold text-transparent">
        404
      </div>
      <p className="mt-3 text-sm text-slate-400">
        Nothing is routed to this URL under <code className="text-sky-300">/react/*</code>.
      </p>
      <Link
        to="/react"
        className="mt-5 inline-block rounded-lg bg-accent px-5 py-2.5 font-bold text-accent-ink no-underline transition hover:bg-accent-hi"
      >
        Back to Home
      </Link>
    </section>
  );
}

export default NotFound;