<?php
// PHP CGI example for AgentHTTPD. Run by php-cgi (CGI SAPI), which reads the
// CGI environment variables set by the C server and populates $_GET/$_POST.
header("Content-Type: text/html; charset=utf-8");

$method = isset($_SERVER["REQUEST_METHOD"]) ? strtoupper($_SERVER["REQUEST_METHOD"]) : "GET";
$params = array_merge($_GET, $_POST);
$phpv = phpversion();

function h($s) { return htmlspecialchars((string)$s, ENT_QUOTES | ENT_SUBSTITUTE, "UTF-8"); }
?>
<!DOCTYPE html>
<html lang="en">
<head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PHP CGI</title>
<style>
  body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;max-width:640px;margin:40px auto;padding:0 20px}
  code{background:#1e293b;padding:1px 5px;border-radius:4px;color:#fbbf24}
  table{border-collapse:collapse;width:100%}th,td{padding:6px 8px;border-bottom:1px solid #334155;text-align:left}
  .card{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:16px 20px;margin:16px 0}
  h1{color:#fbbf24}
</style>
</head><body>
<h1>PHP CGI</h1>
<p>Executed by <code>php-cgi</code> (CGI SAPI); <code>$_GET</code>/<code>$_POST</code>
auto-populated from server env.</p>
<div class="card"><h3>Request info</h3><table>
  <tr><td>Method</td><td><?php echo h($method); ?></td></tr>
  <tr><td>PHP</td><td><?php echo h($phpv); ?></td></tr>
  <tr><td>Params</td><td><?php echo h(count($params)); ?></td></tr>
</table></div>
<div class="card"><h3>Received <?php echo h($method); ?> parameters</h3>
<?php if (empty($params)): ?>
  <p>No parameters. Try <code>?name=Alice</code>.</p>
<?php else: ?>
  <?php foreach ($params as $k => $v): ?>
    <p><code><?php echo h($k); ?></code>: <?php echo h($v); ?></p>
  <?php endforeach; ?>
<?php endif; ?>
</div>
</body></html>