import java.io.IOException;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;

/** Java CGI example for AgentHTTPD.
 *  Compiled to cgi-bin/../cgi-langs/java/classes and launched by
 *  cgi-bin/java.cgi (a shell wrapper), because JVM is not a native binary.
 *  NOTE: direct CGI spawns a fresh JVM per request (cold start); a long-lived
 *  process or GraalVM native-image is faster for real workloads. */
public class Main {
    public static void main(String[] args) throws Exception {
        String method = env("REQUEST_METHOD", "GET").toUpperCase();
        String params = env("QUERY_STRING", "");
        if (method.equals("POST")) {
            int len = Integer.parseInt(env("CONTENT_LENGTH", "0"));
            byte[] buf = new byte[len];
            if (len > 0) System.in.readNBytes(buf, 0, len);
            params = new String(buf, StandardCharsets.UTF_8);
        }

        Map<String, String> fields = parse(params);

        StringBuilder html = new StringBuilder();
        html.append("Content-Type: text/html; charset=utf-8\r\n\r\n");
        html.append("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">")
            .append("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">")
            .append("<title>Java CGI</title>")
            .append("<style>body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;max-width:640px;margin:40px auto;padding:0 20px}")
            .append("code{background:#1e293b;padding:1px 5px;border-radius:4px;color:#a78bfa}")
            .append("table{border-collapse:collapse;width:100%}th,td{padding:6px 8px;border-bottom:1px solid #334155;text-align:left}")
            .append(".card{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:16px 20px;margin:16px 0}")
            .append("h1{color:#a78bfa}</style></head><body>")
            .append("<h1>Java CGI</h1>")
            .append("<p>Runs a JVM per request via <code>cgi-bin/java.cgi</code> wrapper (cold start).</p>")
            .append("<div class=\"card\"><h3>Request info</h3><table>")
            .append("<tr><td>Method</td><td>").append(esc(method)).append("</td></tr>")
            .append("<tr><td>Java</td><td>").append(esc(System.getProperty("java.version"))).append("</td></tr>")
            .append("</table></div>")
            .append("<div class=\"card\"><h3>Received ").append(esc(method)).append(" parameters</h3>");

        if (fields.isEmpty()) {
            html.append("<p>No parameters. Try <code>?name=Alice</code>.</p>");
        } else {
            for (Map.Entry<String, String> e : fields.entrySet()) {
                html.append("<p><code>").append(esc(e.getKey())).append("</code>: ")
                    .append(esc(e.getValue())).append("</p>");
            }
        }
        html.append("</div></body></html>\n");
        System.out.print(html);
    }

    static String env(String k, String d) {
        String v = System.getenv(k);
        return v != null ? v : d;
    }

    static Map<String, String> parse(String qs) {
        Map<String, String> out = new LinkedHashMap<>();
        if (qs == null || qs.isEmpty()) return out;
        for (String pair : qs.split("&")) {
            if (pair.isEmpty()) continue;
            String[] kv = pair.split("=", 2);
            String k = dec(kv[0]);
            String v = kv.length > 1 ? dec(kv[1]) : "";
            if (!k.isEmpty()) out.put(k, v);
        }
        return out;
    }

    static String dec(String s) {
        return URLDecoder.decode(s, StandardCharsets.UTF_8);
    }

    static String esc(String s) {
        return s.replace("&", "&amp;").replace("<", "&lt;")
                .replace(">", "&gt;").replace("\"", "&quot;");
    }
}