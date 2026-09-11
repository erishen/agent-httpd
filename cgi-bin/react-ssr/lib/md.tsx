// Minimal Markdown → React renderer. No dependency, no dangerouslySetInnerHTML:
// every token becomes a React element, so text is always escaped and the same
// code runs for SSR, CSR and hydration. Supports the block constructs LLM
// replies actually use — fenced code, headings, blockquotes, bullet & numbered
// lists, horizontal rules, paragraphs — plus inline code/bold/italic/links.
// Everything else (unclosed fences, stray markers) degrades to plain text so a
// mid-stream partial reply never looks broken.
import { createElement, Fragment, type ReactNode } from "react";

function inlineToNodes(text: string, keyBase = "i"): ReactNode[] {
  const out: ReactNode[] = [];
  // Order matters: code first (`` ` ``), then links, then bold, then italic.
  const re = /(`[^`]+`)|(\*\*[^*]+\*\*)|(\*[^*\n]+\*)|(\[[^\]]+\]\([^)\s]+\))/g;
  let last = 0;
  let m: RegExpExecArray | null;
  let i = 0;
  while ((m = re.exec(text)) !== null) {
    if (m.index > last) out.push(text.slice(last, m.index));
    const tok = m[0];
    let node: ReactNode = tok;
    if (m[1]) node = <code key={i} className="rounded bg-surface px-1 py-0.5 font-mono text-[0.85em] text-sky-300">{tok.slice(1, -1)}</code>;
    else if (m[2]) node = <strong key={i} className="font-semibold text-slate-100">{tok.slice(2, -2)}</strong>;
    else if (m[3]) node = <em key={i} className="italic">{tok.slice(1, -1)}</em>;
    else if (m[4]) {
      const sep = tok.indexOf("](");
      const label = tok.slice(1, sep);
      const href = tok.slice(sep + 2, -1);
      // Only safe schemes may become clickable: javascript:/data:/vbscript:
      // in an <a href> execute on click. Anything else renders as plain text.
      if (/^(https?:|mailto:)/i.test(href.trim())) {
        node = <a key={i} href={href.trim()} target="_blank" rel="noreferrer" className="text-accent underline underline-offset-2 hover:text-accent-hi">{label}</a>;
      } else {
        node = <>{tok}</>;
      }
    }
    out.push(node);
    last = re.lastIndex;
    i++;
  }
  if (last < text.length) out.push(text.slice(last));
  if (out.length === 0) out.push(text);
  return out;
}

function renderLine(text: string): ReactNode {
  const trimmed = text.trim();
  if (!trimmed) return null;
  return <>{inlineToNodes(trimmed, "l")}</>;
}

export default function Markdown({ text }: { text: string }) {
  const lines = text.replace(/\r\n/g, "\n").split("\n");
  const blocks: ReactNode[] = [];
  let para: string[] = [];
  let k = 0;

  const flushPara = (): void => {
    if (para.length === 0) return;
    blocks.push(
      <p key={"p" + k++} className="my-1.5 first:mt-0 last:mb-0 leading-relaxed">
        <>{para.map((l, j) => (
          <Fragment key={j}>
            {renderLine(l)}
            {j < para.length - 1 && <br />}
          </Fragment>
        ))}</>
      </p>
    );
    para = [];
  };

  const flushCode = (code: string, lang: string): void => {
    blocks.push(
      <pre key={"c" + k++} className="my-2 overflow-x-auto rounded-lg border border-edge/50 bg-ink px-3 py-2 text-[12.5px] leading-relaxed">
        {lang && <div className="mb-1 font-mono text-[10px] uppercase tracking-wider text-slate-500">{lang}</div>}
        <code className="font-mono text-slate-200">{code}</code>
      </pre>
    );
  };

  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    const trimmed = line.trim();
    const isBlank = trimmed === "";

    // Fenced code block
    const fence = /^```/.exec(trimmed);
    if (fence) {
      flushPara();
      const lang = trimmed.slice(3).trim();
      const codeLines: string[] = [];
      i++;
      while (i < lines.length && !/^```/.test(lines[i].trim())) {
        codeLines.push(lines[i]);
        i++;
      }
      i++; // consume closing fence (or run off the end → keep partial)
      flushCode(codeLines.join("\n"), lang);
      continue;
    }

    // Heading
    const head = /^(#{1,6})\s+(.*)$/.exec(trimmed);
    if (head) {
      flushPara();
      const level = head[1].length;
      const bold = head[2].replace(/\*\*/g, "");
      const cls = "my-1.5 font-semibold text-slate-100 " + (level <= 2 ? "text-[15px]" : "text-[13.5px]");
      blocks.push(createElement(`h${Math.min(level + 2, 6)}`, { key: "h" + k++, className: cls }, inlineToNodes(bold, "h")));
      i++;
      continue;
    }

    // Horizontal rule
    if (/^(-{3,}|\*{3,}|_{3,})$/.test(trimmed)) {
      flushPara();
      blocks.push(<hr key={"r" + k++} className="my-2 border-edge/50" />);
      i++;
      continue;
    }

    // Blockquote
    if (trimmed.startsWith(">")) {
      flushPara();
      const q: string[] = [];
      while (i < lines.length && lines[i].trim().startsWith(">")) {
        q.push(lines[i].trim().replace(/^>\s?/, ""));
        i++;
      }
      blocks.push(
        <blockquote key={"q" + k++} className="my-1.5 border-l-2 border-accent/50 pl-3 italic text-slate-400">
          {q.map((l, j) => <div key={j}>{inlineToNodes(l, "bq")}</div>)}
        </blockquote>
      );
      continue;
    }

    // Lists (bullet or numbered) — gather the whole consecutive run
    const bullet = /^[-*+]\s+(.+)$/.exec(trimmed);
    const numbered = /^\d+[.)]\s+(.+)$/.exec(trimmed);
    if (bullet || numbered) {
      flushPara();
      const items: ReactNode[] = [];
      let ordered = false;
      while (i < lines.length) {
        const t = lines[i].trim();
        const b = /^[-*+]\s+(.+)$/.exec(t);
        const n = /^\d+[.)]\s+(.+)$/.exec(t);
        if (b) {
          ordered = ordered || false;
          items.push(<li key={i} className="flex gap-2"><span className="mt-px text-slate-500">•</span><span>{inlineToNodes(b[1], "li")}</span></li>);
        } else if (n) {
          ordered = true;
          items.push(<li key={i} className="flex gap-2"><span className="mt-px font-mono text-slate-500">{n[0].slice(0, -1)}.</span><span>{inlineToNodes(n[1], "li")}</span></li>);
        } else {
          break;
        }
        i++;
      }
      blocks.push(
        <div key={"u" + k++} className={"my-1.5 space-y-0.5 flex flex-col gap-0.5"}>
          {items}
        </div>
      );
      continue;
    }

    // Table-ish line groups (simple "a | b" rows) get a light tabular layout
    if (trimmed.includes("|") && trimmed.split("|").length >= 3 && !trimmed.includes("```")) {
      const headerNext = /^\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)+/.test(lines[i + 1]?.trim() ?? "");
      if (headerNext) {
        flushPara();
        const rows: string[][] = [];
        while (i < lines.length && lines[i].includes("|")) {
          const cells = lines[i]
            .trim()
            .replace(/^\||\|$/g, "")
            .split("|")
            .map((c) => c.trim());
          if (cells.length >= 2 && !/^-+$|^:?-+:?$/.test(cells.join(""))) rows.push(cells);
          i++;
        }
        if (rows.length) {
          const headers = rows[0];
          blocks.push(
            <div key={"t" + k++} className="my-2 overflow-x-auto rounded-lg border border-edge/50">
              <table className="w-full text-[12.5px]">
                <thead>
                  <tr className="bg-ink/60">
                    {headers.map((h, j) => <th key={j} className="px-2.5 py-1 text-left font-semibold text-slate-300">{h.replace(/\*\*/g, "")}</th>)}
                  </tr>
                </thead>
                <tbody>
                  {rows.slice(1).map((r, j) => (
                    <tr key={j} className="border-t border-edge/40">
                      {r.map((c, h) => <td key={h} className="px-2.5 py-1 text-slate-300">{inlineToNodes(c, "td")}</td>)}
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          );
          continue;
        }
      }
    }

    // Plain line → paragraph buffer
    if (isBlank) {
      flushPara();
      i++;
      continue;
    }
    para.push(line);
    i++;
  }
  flushPara();
  if (blocks.length === 0) blocks.push(<p key="p" className="leading-relaxed">{inlineToNodes(text, "0")}</p>);
  return <div className="space-y-1">{blocks}</div>;
}