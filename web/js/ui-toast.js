// toast notifications
export function toast(title, message, isError = false, ttl = 5000) {
  const box = document.getElementById('toasts');
  const el = document.createElement('div');
  el.className = 'toast' + (isError ? ' error' : '');
  const t = document.createElement('div');
  t.className = 't-title';
  t.textContent = title;
  const m = document.createElement('div');
  m.textContent = message;
  el.append(t, m);
  box.appendChild(el);
  setTimeout(() => el.remove(), ttl);
  if (isError) console.warn('[toast]', title, message);
}
