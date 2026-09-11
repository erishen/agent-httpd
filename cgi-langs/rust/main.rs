// Rust CGI example for AgentHTTPD.
// Built with: rustc --edition 2021 -O cgi-langs/rust/main.rs -o cgi-bin/rust.cgi
use std::env;
use std::io::{self, Read};

fn main() {
    let method = env::var("REQUEST_METHOD").unwrap_or_else(|_| "GET".into()).to_uppercase();
    let mut param_str = env::var("QUERY_STRING").unwrap_or_default();

    if method == "POST" {
        let len: usize = env::var("CONTENT_LENGTH").unwrap_or_default().parse().unwrap_or(0);
        let mut buf = vec![0u8; len];
        if len > 0 {
            let _ = io::stdin().read_exact(&mut buf);
            param_str = String::from_utf8_lossy(&buf).into_owned();
        }
    }

    let params = parse_qs(&param_str);

    let body = format!(
        "<!DOCTYPE html>\n\
         <html lang=\"en\"><head><meta charset=\"utf-8\">\n\
         <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n\
         <title>Rust CGI</title>\n\
         <style>body{{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;max-width:640px;margin:40px auto;padding:0 20px}}\
         code{{background:#1e293b;padding:1px 5px;border-radius:4px;color:#f472b6}}\
         table{{border-collapse:collapse;width:100%}}th,td{{padding:6px 8px;border-bottom:1px solid #334155;text-align:left}}\
         .card{{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:16px 20px;margin:16px 0}}\
         h1{{color:#f472b6}}</style>\n\
         </head><body>\n\
         <h1>Rust CGI</h1>\n\
         <p><code>rustc -O</code> native binary, executed directly by <code>execl</code>.</p>\n\
         <div class=\"card\"><h3>Request info</h3><table>\n\
         <tr><td>Method</td><td>{method}</td></tr>\n\
         <tr><td>Compiler</td><td>rustc (optimized)</td></tr>\n\
         <tr><td>Query</td><td><code>{param_str}</code></td></tr>\n\
         </table></div>\n\
         <div class=\"card\"><h3>Received {method} parameters</h3>",
    );

    print!("Content-Type: text/html; charset=utf-8\r\n\r\n");
    if params.is_empty() {
        print!("{body}<p>No parameters. Try <code>?name=Alice</code>.</p></div></body></html>\n");
    } else {
        let mut rows = String::new();
        for (k, v) in &params {
            rows.push_str(&format!(
                "<p><code>{}</code>: {}</p>\n",
                esc(k),
                esc(v)
            ));
        }
        print!("{body}{rows}</div></body></html>\n");
    }
}

fn esc(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

fn parse_qs(qs: &str) -> Vec<(String, String)> {
    qs.split('&')
        .filter(|p| !p.is_empty())
        .filter_map(|pair| {
            let idx = pair.find('=')?;
            let k = pct_decode(pair.get(..idx)?);
            let v = pct_decode(pair.get(idx + 1..)?);
            Some((k, v))
        })
        .collect()
}

fn pct_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'+' {
            out.push(b' ');
            i += 1;
        } else if bytes[i] == b'%' && i + 2 < bytes.len() + 1 && i + 2 <= bytes.len() {
            let h = |c: u8| -> Option<u8> {
                match c {
                    b'0'..=b'9' => Some(c - b'0'),
                    b'a'..=b'f' => Some(c - b'a' + 10),
                    b'A'..=b'F' => Some(c - b'A' + 10),
                    _ => None,
                }
            };
            match (h(bytes[i + 1]), h(bytes[i + 2])) {
                (Some(hi), Some(lo)) if i + 2 < bytes.len() + 1 => {
                    out.push((hi << 4) | lo);
                    i += 3;
                }
                _ => {
                    out.push(bytes[i]);
                    i += 1;
                }
            }
        } else {
            out.push(bytes[i]);
            i += 1;
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}