(function () {
  // echo query params (the SSR version echoes GET/POST params server-side)
  var qs = new URLSearchParams(location.search);
  var body = document.getElementById('params-body');
  var keys = [];
  qs.forEach(function (v, k) { keys.push([k, v]); });
  if (keys.length) {
    keys.sort(function (a, b) { return a[0] < b[0] ? -1 : 1; });
    keys.forEach(function (kv) {
      var tr = document.createElement('tr');
      var k = document.createElement('td'); k.className = 'key'; k.textContent = kv[0];
      var v = document.createElement('td'); v.textContent = kv[1];
      tr.appendChild(k); tr.appendChild(v); body.appendChild(tr);
    });
    document.getElementById('params-table').style.display = '';
    document.getElementById('params-empty').style.display = 'none';
  }
  // live greeting (hydration stand-in)
  var greet = 'Hello, ' + ((qs.get('name') || '').trim() || 'server-side visitor');
  document.getElementById('v-greet').textContent = greet;
  var name = document.getElementById('f-name');
  name.addEventListener('input', function () {
    document.getElementById('v-greet').textContent =
      'Hello, ' + (name.value.trim() || 'server-side visitor');
  });
  // clock
  document.getElementById('v-clock').textContent =
    new Date().toLocaleString('en-GB', { dateStyle: 'medium', timeStyle: 'medium' });
})();
