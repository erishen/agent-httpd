(function () {
  var n = 0;
  var el = document.getElementById('count');
  function render() { el.textContent = String(n); }
  document.getElementById('inc').addEventListener('click', function () { n += 1; render(); });
  document.getElementById('dec').addEventListener('click', function () { n -= 1; render(); });
  document.getElementById('reset').addEventListener('click', function () { n = 0; render(); });
})();
