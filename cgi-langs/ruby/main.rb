#!/usr/bin/env ruby
# Ruby CGI example for AgentHTTPD. Uses the stdlib `cgi` library, which reads
# the CGI environment set by the C server and parses urlencoded + multipart
# bodies. Copied verbatim to cgi-bin/ruby.cgi.

require "cgi"
require "erb"

begin
  method = (ENV["REQUEST_METHOD"] || "GET").upcase
  cgi = CGI.new
  # cgi.params returns arrays; take first value for simple echo
  params = {}
  cgi.params.each { |k, v| params[k] = v.respond_to?(:first) ? v.first.to_s : v.to_s }

  h = ->(s) { ERB::Util.html_escape(s.to_s) }

  body = +""
  body << "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
  body << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
  body << "<title>Ruby CGI</title>"
  body << "<style>body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;max-width:640px;margin:40px auto;padding:0 20px}"
  body << "code{background:#1e293b;padding:1px 5px;border-radius:4px;color:#f472b6}"
  body << "table{border-collapse:collapse;width:100%}th,td{padding:6px 8px;border-bottom:1px solid #334155;text-align:left}"
  body << ".card{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:16px 20px;margin:16px 0}"
  body << "h1{color:#f472b6}</style></head><body>"
  body << "<h1>Ruby CGI</h1>"
  body << "<p>Runs on Ruby <code>#{h.call(RUBY_VERSION)}</code> via stdlib <code>cgi</code>.</p>"
  body << "<div class=\"card\"><h3>Request info</h3><table>"
  body << "<tr><td>Method</td><td>#{h.call(method)}</td></tr>"
  body << "<tr><td>Ruby</td><td>#{h.call(RUBY_VERSION)}</td></tr>"
  body << "<tr><td>Params</td><td>#{h.call(params.size)}</td></tr>"
  body << "</table></div>"
  body << "<div class=\"card\"><h3>Received #{h.call(method)} parameters</h3>"

  if params.empty?
    body << "<p>No parameters. Try <code>?name=Alice</code>.</p>"
  else
    params.each { |k, v| body << "<p><code>#{h.call(k)}</code>: #{h.call(v)}</p>" }
  end
  body << "</div></body></html>\n"

  puts cgi.header("type" => "text/html; charset=utf-8")
  puts body
rescue StandardError => e
  puts "Content-Type: text/plain; charset=utf-8\r\n\r\n"
  puts "Ruby CGI error: #{e.class}: #{e.message}"
  warn e.backtrace.join("\n") if ENV["DEBUG"]
end