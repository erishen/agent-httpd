// Go CGI example for AgentHTTPD. Built with: go build -o cgi-bin/go.cgi main.go
package main

import (
	"fmt"
	"html"
	"net/url"
	"os"
	"runtime"
	"strconv"
	"strings"
)

func main() {
	method := strings.ToUpper(os.Getenv("REQUEST_METHOD"))
	if method == "" {
		method = "GET"
	}

	paramStr := os.Getenv("QUERY_STRING")
	if method == "POST" {
		n, _ := strconv.Atoi(os.Getenv("CONTENT_LENGTH"))
		buf := make([]byte, n)
		if n > 0 {
			os.Stdin.Read(buf)
			paramStr = string(buf)
		}
	}

	params, _ := url.ParseQuery(paramStr)

	fmt.Print("Content-Type: text/html; charset=utf-8\r\n\r\n")
	fmt.Printf(`<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Go CGI</title>
<style>body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;max-width:640px;margin:40px auto;padding:0 20px}code{background:#1e293b;padding:1px 5px;border-radius:4px;color:#38bdf8}table{border-collapse:collapse;width:100%}th,td{padding:6px 8px;border-bottom:1px solid #334155;text-align:left}.card{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:16px 20px;margin:16px 0}h1{color:#34d399}</style>
</head><body>
<h1>Go CGI</h1>
<p>Native binary (<code>go build</code>), executed directly by <code>execl</code>.</p>
<div class="card"><h3>Request info</h3><table>
<tr><td>Method</td><td>%s</td></tr>
<tr><td>Go version</td><td>%s</td></tr>
<tr><td>Query</td><td>%s</td></tr>
</table></div>
<div class="card"><h3>Received %s parameters</h3>`, method, runtime.Version(), html.EscapeString(paramStr), method)

	if len(params) == 0 {
		fmt.Print(`<p>No parameters. Try <code>?name=Alice</code>.</p>`)
	} else {
		for k, vs := range params {
			fmt.Printf(`<p><code>%s</code>: %s</p>`, html.EscapeString(k), html.EscapeString(strings.Join(vs, ", ")))
		}
	}
	fmt.Print("</div></body></html>\n")
}