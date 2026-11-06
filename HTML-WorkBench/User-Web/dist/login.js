const form = document.querySelector('#loginForm');
const username = document.querySelector('#username');
const password = document.querySelector('#password');
const submit = document.querySelector('#submitLogin');
const status = document.querySelector('#loginStatus');
const showPassword = document.querySelector('#showPassword');
const prefix = '/robot-studio/';
let csrfToken = '';
let submitting = false;

function nextLocation() {
  const requested = new URLSearchParams(location.search).get('next') || prefix;
  // The backend independently validates this path before returning redirectTo.
  let target;
  try { target = new URL(requested, location.origin); } catch { return prefix + location.hash; }
  if (target.origin !== location.origin || !target.pathname.startsWith(prefix) || /\/auth\//.test(target.pathname) || /\/login\.(html|js|css)$/.test(target.pathname)) return prefix + location.hash;
  if (!target.hash && location.hash) target.hash = location.hash;
  return target.pathname + target.search + target.hash;
}

function message(text, error = false) {
  status.textContent = text;
  status.classList.toggle('is-error', error);
}

async function session() {
  const response = await fetch('./auth/session', {credentials: 'same-origin', cache: 'no-store', headers: {'Accept': 'application/json'}});
  const data = await response.json();
  if (response.status !== 200 && response.status !== 401) throw new Error('暂时无法连接，请刷新页面重试。');
  csrfToken = data.csrfToken || '';
  if (!csrfToken) throw new Error('登录初始化失败，请刷新后重试。');
  return data;
}

showPassword.addEventListener('click', () => {
  const shown = password.type === 'password';
  password.type = shown ? 'text' : 'password';
  showPassword.textContent = shown ? '隐藏' : '显示';
  showPassword.setAttribute('aria-pressed', String(shown));
});

form.addEventListener('submit', async event => {
  event.preventDefault();
  if (submitting || !form.reportValidity()) return;
  submitting = true;
  submit.disabled = true;
  username.removeAttribute('aria-invalid');
  password.removeAttribute('aria-invalid');
  message('正在登录…');
  try {
    if (!csrfToken) await session();
    const response = await fetch('./auth/login', {
      method: 'POST', credentials: 'same-origin', headers: {'Content-Type': 'application/json', 'X-CSRF-Token': csrfToken},
      body: JSON.stringify({username: username.value.trim(), password: password.value, next: nextLocation()})
    });
    const data = await response.json();
    if (!response.ok) {
      if (response.status === 401) {username.setAttribute('aria-invalid', 'true'); password.setAttribute('aria-invalid', 'true'); password.focus();}
      if (response.status === 403) {csrfToken = ''; await session();}
      throw new Error(data.error || '登录失败，请稍后重试。');
    }
    password.value = '';
    message('登录成功，正在打开工作台…');
    location.replace(data.redirectTo || prefix);
  } catch (error) {
    message(error instanceof TypeError ? '连接中断，请检查网络后重试。' : error.message, true);
    submitting = false;
    submit.disabled = false;
  }
});

try {
  const data = await session();
  if (data.authenticated) location.replace(nextLocation());
  else {message('请输入账号和密码。'); submit.disabled = false; username.focus();}
} catch (error) {
  message(error instanceof TypeError ? '连接中断，请刷新页面重试。' : error.message, true);
  submit.disabled = false;
}
