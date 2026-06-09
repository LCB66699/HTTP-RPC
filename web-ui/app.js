const API = '/api';
let currentUser = null;
let authToken = null;
// 用 window 避免 let TDZ 问题
window.currentSheetId = null;
window.currentSheetData = null;
window.currentSheetHeaders = null;

// ---- Pagination state ----
const PAGE_SIZE = 20;
let sheetsPage = 0;
let sheetsTotal = 0;
let filesPage = 0;
let filesTotal = 0;

// ---- Idempotency helpers ----
// Generates a RFC-4122 v4 UUID using crypto.getRandomValues
function uuidv4() {
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  bytes[6] = (bytes[6] & 0x0f) | 0x40;  // version 4
  bytes[8] = (bytes[8] & 0x3f) | 0x80;  // variant 1
  const hex = [...bytes].map(b => b.toString(16).padStart(2, '0'));
  return hex.slice(0,4).join('') + '-' + hex[4] + hex[5] + '-' + hex[6] + hex[7] + '-'
       + hex[8] + hex[9] + '-' + hex.slice(10).join('');
}

// ---- In-flight guards (prevent double-submit) ----
let isSavingSheet = false;
let isCreatingSheet = false;
let isUploadingFile = false;

// ============================================================
//  AVATAR MANAGEMENT
// ============================================================

const AVATAR_PRESETS = [
  { bg: 'linear-gradient(135deg, #6366f1, #8b5cf6)', init: 'A' },
  { bg: 'linear-gradient(135deg, #ec4899, #f43f5e)', init: 'B' },
  { bg: 'linear-gradient(135deg, #10b981, #06b6d4)', init: 'C' },
  { bg: 'linear-gradient(135deg, #f59e0b, #ef4444)', init: 'D' },
  { bg: 'linear-gradient(135deg, #3b82f6, #6366f1)', init: 'E' },
  { bg: 'linear-gradient(135deg, #8b5cf6, #ec4899)', init: 'F' },
];

function getAvatarData() {
  const stored = localStorage.getItem('rpc_avatar');
  if (stored) return stored;
  return null;
}

function saveAvatarData(dataUrl) {
  localStorage.setItem('rpc_avatar', dataUrl);
}

function generateDefaultAvatar(username) {
  const initial = (username || 'U').charAt(0).toUpperCase();
  const idx = initial.charCodeAt(0) % AVATAR_PRESETS.length;
  const preset = AVATAR_PRESETS[idx];
  const canvas = document.createElement('canvas');
  canvas.width = 200;
  canvas.height = 200;
  const ctx = canvas.getContext('2d');

  // Background gradient
  const grad = ctx.createLinearGradient(0, 0, 200, 200);
  grad.addColorStop(0, '#6366f1');
  grad.addColorStop(1, '#8b5cf6');
  ctx.fillStyle = grad;
  ctx.beginPath();
  ctx.arc(100, 100, 100, 0, Math.PI * 2);
  ctx.fill();

  // Initial letter
  ctx.fillStyle = '#fff';
  ctx.font = 'bold 80px "Inter", "Segoe UI", system-ui, sans-serif';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(initial, 100, 104);

  return canvas.toDataURL('image/png');
}

function loadAvatar() {
  let dataUrl = getAvatarData();
  if (!dataUrl) {
    dataUrl = generateDefaultAvatar(currentUser?.username || 'U');
    saveAvatarData(dataUrl);
  }
  applyAvatar(dataUrl);
}

function applyAvatar(dataUrl) {
  const avatarImg = document.getElementById('avatar-img');
  const headerAvatar = document.getElementById('header-avatar');
  if (avatarImg) avatarImg.src = dataUrl;
  if (headerAvatar) headerAvatar.src = dataUrl;
}

function switchToProfile() {
  document.querySelectorAll('.tab').forEach(b => b.classList.remove('active'));
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  const profileTab = document.querySelector('[data-tab="profile"]');
  if (profileTab) profileTab.classList.add('active');
  document.getElementById('panel-profile').classList.add('active');
  initProfile();
}

// ---- Avatar Modal ----
function buildPresetGrid() {
  const grid = document.getElementById('avatar-preset-grid');
  if (!grid) return;
  grid.innerHTML = AVATAR_PRESETS.map((p, i) => {
    return `<div class="avatar-preset" style="background:${p.bg};" data-preset="${i}" title="预设头像 ${i + 1}"></div>`;
  }).join('');

  grid.querySelectorAll('.avatar-preset').forEach(el => {
    el.addEventListener('click', () => {
      const idx = parseInt(el.dataset.preset);
      const preset = AVATAR_PRESETS[idx];
      const canvas = document.createElement('canvas');
      canvas.width = 200; canvas.height = 200;
      const ctx = canvas.getContext('2d');
      // Parse gradient
      const gradStr = preset.bg;
      const colors = gradStr.match(/#[0-9a-fA-F]{6}/g) || ['#6366f1', '#8b5cf6'];
      const grad = ctx.createLinearGradient(0, 0, 200, 200);
      colors.forEach((c, i) => grad.addColorStop(i / (colors.length - 1 || 1), c));
      ctx.fillStyle = grad;
      ctx.beginPath();
      ctx.arc(100, 100, 100, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = '#fff';
      ctx.font = 'bold 80px "Inter", "Segoe UI", system-ui, sans-serif';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(preset.init, 100, 104);
      const dataUrl = canvas.toDataURL('image/png');
      saveAvatarData(dataUrl);
      applyAvatar(dataUrl);
      closeAvatarModal();
    });
  });
}

function openAvatarModal() {
  buildPresetGrid();
  document.getElementById('avatar-modal').classList.remove('hidden');
}

function closeAvatarModal() {
  document.getElementById('avatar-modal').classList.add('hidden');
}

document.getElementById('avatar-wrapper')?.addEventListener('click', openAvatarModal);
document.getElementById('btn-avatar-cancel')?.addEventListener('click', closeAvatarModal);
document.getElementById('avatar-modal')?.addEventListener('click', function(e) {
  if (e.target === this) closeAvatarModal();
});

// Upload avatar
document.getElementById('btn-upload-avatar')?.addEventListener('click', () => {
  document.getElementById('avatar-file-input').click();
});

document.getElementById('avatar-file-input')?.addEventListener('change', function() {
  const file = this.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = function(e) {
    const img = new Image();
    img.onload = function() {
      const canvas = document.createElement('canvas');
      const size = Math.min(img.width, img.height);
      canvas.width = 200; canvas.height = 200;
      const ctx = canvas.getContext('2d');
      // Crop to center square
      const sx = (img.width - size) / 2;
      const sy = (img.height - size) / 2;
      // Draw circular clip
      ctx.beginPath();
      ctx.arc(100, 100, 100, 0, Math.PI * 2);
      ctx.clip();
      ctx.drawImage(img, sx, sy, size, size, 0, 0, 200, 200);
      const dataUrl = canvas.toDataURL('image/png');
      saveAvatarData(dataUrl);
      applyAvatar(dataUrl);
      closeAvatarModal();
    };
    img.src = e.target.result;
  };
  reader.readAsDataURL(file);
  this.value = '';
});

// ============================================================
//  AUTH
// ============================================================

async function checkAuth() {
  try {
    // 用 /api/me 验证 HttpOnly Cookie 是否有效（而非仅依赖 localStorage）
    const res = await fetch(API + '/me', { credentials: 'same-origin' });
    if (res.status === 200) {
      const data = await res.json();
      const stored = localStorage.getItem('rpc_user');
      const local = stored ? JSON.parse(stored) : {};
      const joinDate = local.joinDate || new Date().toLocaleDateString('zh-CN');
      currentUser = { username: data.username, joinDate, role: data.role };
      localStorage.setItem('rpc_user', JSON.stringify(currentUser));
      authToken = '1';
      showMainApp();
      const lastTab = localStorage.getItem('rpc_last_tab') || 'sheets';
      if (currentUser.role === 'admin') {
        document.querySelectorAll('.admin-only').forEach(el => el.style.display = '');
      }
      const tab = document.querySelector(`[data-tab="${lastTab}"]`);
      if (tab) tab.click();
      else loadSheets();
    } else {
      // Cookie 无效 → 清除本地状态，显示登录页
      localStorage.removeItem('rpc_user');
      showLoginModal();
    }
  } catch (e) {
    // 网络错误等 → 如果 localStorage 有数据，尝试显示（离线兜底）
    const user = localStorage.getItem('rpc_user');
    if (user) {
      try {
        currentUser = JSON.parse(user);
        authToken = '1';
        showMainApp();
        const lastTab = localStorage.getItem('rpc_last_tab') || 'services';
        const tab = document.querySelector(`[data-tab="${lastTab}"]`);
        if (tab) tab.click();
        else loadServices();
      } catch (_) {
        localStorage.removeItem('rpc_user');
        showLoginModal();
      }
    } else {
      showLoginModal();
    }
  }
}

function showLoginModal() {
  const video = document.getElementById('video-bg');
  if (!video.src) video.src = 'bg-video.mp4';
  video.style.display = '';
  video.play().catch(() => {});
  document.getElementById('video-main').style.display = 'none';
  document.getElementById('login-modal').classList.remove('hidden');
  document.getElementById('register-modal').classList.add('hidden');
  document.getElementById('main-header').classList.add('hidden');
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  document.querySelector('main').classList.add('hidden');
}

function showRegisterModal() {
  document.getElementById('login-modal').classList.add('hidden');
  document.getElementById('register-modal').classList.remove('hidden');
}

function showMainApp() {
  document.getElementById('login-modal').classList.add('hidden');
  document.getElementById('register-modal').classList.add('hidden');
  document.getElementById('main-header').classList.remove('hidden');
  document.querySelector('main').classList.remove('hidden');
  document.getElementById('user-display').textContent = currentUser.username;
  document.getElementById('profile-username').textContent = currentUser.username;
  document.getElementById('profile-join-date').textContent = currentUser.joinDate || new Date().toLocaleDateString('zh-CN');
  // 隐藏非管理员功能
  const isAdmin = currentUser && currentUser.role === 'admin';
  document.querySelector('[data-tab="monitor"]').style.display = isAdmin ? '' : 'none';
  if (!isAdmin && document.getElementById('panel-monitor').classList.contains('active')) {
    document.getElementById('panel-monitor').classList.remove('active');
    document.getElementById('panel-sheets').classList.add('active');
    document.querySelector('[data-tab="sheets"]').classList.add('active');
  }
  loadAvatar();
  const video = document.getElementById('video-bg');
  video.pause();
  video.style.display = 'none';
  const mainVid = document.getElementById('video-main');
  mainVid.src = 'anime-bg.mp4';
  mainVid.load();
  mainVid.style.display = 'block';
  mainVid.play().catch(() => {});
}

function logout() {
  if (monitorTimer) { clearTimeout(monitorTimer); monitorTimer = null; }
  // Ask the server to clear the HttpOnly cookie (Max-Age=0)
  fetch(API + '/logout', { method: 'POST', credentials: 'same-origin' }).catch(() => {});
  localStorage.removeItem('rpc_user');
  currentUser = null;
  authToken = null;
  refreshToken = null;
  refreshUsername = null;
  refreshPromise = null;
  showLoginModal();
  document.getElementById('login-error').classList.add('hidden');
  document.getElementById('login-password').value = '';
}

// ---- Login Form ----
document.getElementById('login-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const username = document.getElementById('login-username').value.trim();
  const password = document.getElementById('login-password').value;
  const errorDiv = document.getElementById('login-error');

  if (!username || !password) {
    errorDiv.textContent = '请输入用户名和密码';
    errorDiv.classList.remove('hidden');
    return;
  }

  try {
    // credentials:'same-origin' 让浏览器在同源请求中自动携带并接收 Cookie
    const res = await fetch(API + '/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username, password }),
      credentials: 'same-origin'
    });
    const data = await res.json();
    if (data.success) {
      // JWT 已通过 Set-Cookie: rpc_token HttpOnly 设置，JS 无法读取，也不需要存储
      authToken = '1';   // 仅作登录状态标志
      if (data._rt) { refreshToken = data._rt; refreshUsername = data.username; }
      const loginUsername = data.username || username;
      const joinDate = localStorage.getItem('rpc_join_date_' + loginUsername) || new Date().toLocaleDateString('zh-CN');
      const role = data._role || 'user';
      currentUser = { username: loginUsername, joinDate, role };
      localStorage.setItem('rpc_user', JSON.stringify(currentUser));
      if (!localStorage.getItem('rpc_join_date_' + loginUsername)) {
        localStorage.setItem('rpc_join_date_' + loginUsername, joinDate);
      }
      showMainApp();
      loadServices();
    } else {
      errorDiv.textContent = data.error || '用户名或密码错误';
      errorDiv.classList.remove('hidden');
    }
  } catch (err) {
    errorDiv.textContent = '网络错误，请检查服务是否启动';
    errorDiv.classList.remove('hidden');
  }
});

document.getElementById('btn-show-register').addEventListener('click', showRegisterModal);
document.getElementById('btn-cancel-register').addEventListener('click', showLoginModal);

// ---- Register Form ----
document.getElementById('register-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const username = document.getElementById('reg-username').value.trim();
  const password = document.getElementById('reg-password').value;
  const confirmPassword = document.getElementById('reg-password-confirm').value;
  const errorDiv = document.getElementById('register-error');

  if (!username || !password) {
    errorDiv.textContent = '请填写完整信息';
    errorDiv.classList.remove('hidden');
    return;
  }
  if (password !== confirmPassword) {
    errorDiv.textContent = '两次输入的密码不一致';
    errorDiv.classList.remove('hidden');
    return;
  }
  if (password.length < 6) {
    errorDiv.textContent = '密码长度至少为 6 位';
    errorDiv.classList.remove('hidden');
    return;
  }

  try {
    const res = await fetch(API + '/register', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username, password }),
      credentials: 'same-origin'
    });
    const data = await res.json();
    if (data.success) {
      authToken = '1';   // JWT 在 HttpOnly cookie 中，JS 不存储
      if (data._rt) { refreshToken = data._rt; refreshUsername = data.username; }
      const regUsername = data.username || username;
      const joinDate = new Date().toLocaleDateString('zh-CN');
      currentUser = { username: regUsername, joinDate };
      localStorage.setItem('rpc_user', JSON.stringify(currentUser));
      localStorage.setItem('rpc_join_date_' + regUsername, joinDate);
      showMainApp();
      loadServices();
    } else {
      errorDiv.textContent = data.error || '注册失败';
      errorDiv.classList.remove('hidden');
    }
  } catch (err) {
    errorDiv.textContent = '网络错误，请检查服务是否启动';
    errorDiv.classList.remove('hidden');
  }
});

document.getElementById('btn-logout').addEventListener('click', logout);

// ============================================================
//  NAVIGATION
// ============================================================

document.querySelectorAll('.tab').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
    btn.classList.add('active');
    const panelId = 'panel-' + btn.dataset.tab;
    document.getElementById(panelId).classList.add('active');
    // 记住当前标签页
    localStorage.setItem('rpc_last_tab', btn.dataset.tab);
    if (btn.dataset.tab === 'services') loadServices();
    if (btn.dataset.tab === 'sheets') loadSheets();
    if (btn.dataset.tab === 'files') loadFiles();
    if (btn.dataset.tab === 'monitor') loadMonitor();
    if (btn.dataset.tab === 'search') initSearch();
    if (btn.dataset.tab === 'profile') initProfile();
  });
});

// ============================================================
//  SEARCH
// ============================================================

let searchPage = 1;
let searchTotal = 0;
let searchTimer = null;

function initSearch() {
  const input = document.getElementById('search-input');
  if (input) {
    input.focus();
    input.addEventListener('input', () => {
      clearTimeout(searchTimer);
      searchTimer = setTimeout(doSearch, 300);
    });
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { clearTimeout(searchTimer); doSearch(); }
    });
  }
  const btn = document.getElementById('btn-search');
  if (btn) btn.addEventListener('click', doSearch);
  const btnPrev = document.getElementById('btn-search-prev');
  if (btnPrev) btnPrev.addEventListener('click', () => {
    if (searchPage > 1) { searchPage--; doSearch(); }
  });
  const btnNext = document.getElementById('btn-search-next');
  if (btnNext) btnNext.addEventListener('click', () => {
    if (searchPage * PAGE_SIZE < searchTotal) { searchPage++; doSearch(); }
  });
}

async function doSearch() {
  const input = document.getElementById('search-input');
  if (!input) return;
  const q = input.value.trim();
  if (!q) return;

  // 收集 scope
  const scopes = [];
  document.querySelectorAll('.scope-chk:checked').forEach(cb => scopes.push(cb.value));
  const sortEl = document.getElementById('search-sort');
  const sort = sortEl ? sortEl.value : 'relevance';

  const loading = document.getElementById('search-loading');
  const emptyEl = document.getElementById('search-empty');
  const container = document.getElementById('search-results-container');
  const summary = document.getElementById('search-summary');
  if (!container) return;

  if (loading) loading.classList.remove('hidden');
  if (emptyEl) emptyEl.classList.add('hidden');
  container.innerHTML = '';

  try {
    const res = await apiPost('/search', {
      q, scope: scopes.join(','), sort,
      page: searchPage, page_size: PAGE_SIZE
    });
    if (loading) loading.classList.add('hidden');
    if (res.error) {
      if (summary) summary.textContent = '搜索出错: ' + res.error;
      return;
    }
    searchTotal = res.total || 0;
    if (summary) summary.textContent = `"${q}" — 找到 ${searchTotal} 条结果`;

    const sugg = document.getElementById('search-suggestion');
    if (sugg) {
      if (res.suggestion && res.suggestion !== q) {
        sugg.classList.remove('hidden');
        sugg.innerHTML = `您是不是要找：<a href="#" onclick="var inp=document.getElementById('search-input');if(inp)inp.value='${res.suggestion}';searchPage=1;doSearch();return false;">${res.suggestion}</a>`;
      } else {
        sugg.classList.add('hidden');
      }
    }

    renderSearchResults(res.results || []);

    const pageInfo = document.getElementById('search-page-info');
    const btnPrev = document.getElementById('btn-search-prev');
    const btnNext = document.getElementById('btn-search-next');
    const pagination = document.getElementById('search-pagination');
    if (pageInfo) pageInfo.textContent = `第 ${searchPage} 页 / 共 ${Math.ceil(searchTotal / PAGE_SIZE) || 1} 页`;
    if (btnPrev) btnPrev.disabled = (searchPage <= 1);
    if (btnNext) btnNext.disabled = (searchPage * PAGE_SIZE >= searchTotal);
    if (pagination) pagination.classList.toggle('hidden', searchTotal <= PAGE_SIZE);

  } catch (e) {
    if (loading) loading.classList.add('hidden');
    if (summary) summary.textContent = '搜索请求失败，请稍后重试';
  }
}

function renderSearchResults(results) {
  const container = document.getElementById('search-results-container');
  if (!results.length) {
    container.innerHTML = '<div class="search-empty"><p>😕 没有找到相关结果</p><p style="font-size:0.85rem;color:var(--text-muted);">尝试其他关键词</p></div>';
    return;
  }
  container.innerHTML = results.map(r => {
    const isSheet = r.type === 'sheet';
    const title = isSheet ? (r.name || '未命名表格') : (r.original_name || '未知文件');
    const subtitle = isSheet
      ? `表格 · ${r.username} · ${r.row_count || 0}行 × ${r.col_count || 0}列`
      : `文件 · ${r.username} · ${formatFileSize(r.size)} · ${r.mime_type || ''}`;
    const date = isSheet ? r.updated_at : r.created_at;

    // 高亮片段
    let snippet = '';
    if (r.highlight) {
      const parts = [];
      for (const [field, hl] of Object.entries(r.highlight)) {
        if (Array.isArray(hl)) parts.push(...hl);
      }
      snippet = parts.slice(0, 3).join(' ... ');
    }

    return `
      <div class="search-result-card">
        <div class="search-result-type">${isSheet ? '📊' : '📄'} ${isSheet ? '表格' : '文件'}</div>
        <div class="search-result-title">
          ${isSheet
            ? `<a href="#" onclick="window.openSheetById('${r.id}');return false;">${highlightText(title)}</a>`
            : `<span>${highlightText(title)}</span>`}
        </div>
        <div class="search-result-meta">${subtitle} · ${formatSearchDate(date)}</div>
        ${snippet ? `<div class="search-result-snippet">${snippet}</div>` : ''}
      </div>
    `;
  }).join('');
}

function highlightText(text) {
  return String(text).replace(/<em>/g, '<em class="search-highlight">').replace(/<\/em>/g, '</em>');
}

function formatSearchDate(d) {
  if (!d) return '';
  try { return new Date(d).toLocaleDateString('zh-CN'); } catch(e) { return d; }
}

function formatFileSize(bytes) {
  if (!bytes) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  let i = 0, s = bytes;
  while (s >= 1024 && i < units.length-1) { s /= 1024; i++; }
  return s.toFixed(i > 0 ? 1 : 0) + ' ' + units[i];
}

// 从搜索结果跳转到表格详情
window.openSheetById = function(sheetId) {
  document.querySelectorAll('.tab').forEach(b => b.classList.remove('active'));
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  const sheetTab = document.querySelector('[data-tab="sheets"]');
  if (sheetTab) sheetTab.classList.add('active');
  document.getElementById('panel-sheets').classList.add('active');
  localStorage.setItem('rpc_last_tab', 'sheets');
  loadSheets().then(() => {
    // 尝试定位到该表格
    setTimeout(() => {
      if (typeof window.currentSheetId !== 'undefined') {
        window.currentSheetId = sheetId;
      }
    }, 500);
  });
};

// ============================================================
//  TOKEN REFRESH
// ============================================================
let refreshToken = null;
let refreshUsername = null;
let refreshPromise = null;  // 防并发刷新

async function tryRefreshToken() {
  if (!refreshToken || !refreshUsername) return false;
  // 已有刷新在进行中，等它完成
  if (refreshPromise) {
    await refreshPromise;
    return true;
  }
  refreshPromise = (async () => {
    try {
      const res = await fetch(API + '/refresh', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        credentials: 'same-origin',
        body: JSON.stringify({ username: refreshUsername, refresh_token: refreshToken })
      });
      if (res.ok) {
        authToken = '1';
        return true;
      }
      return false;
    } catch (_) {
      return false;
    } finally {
      refreshPromise = null;
    }
  })();
  return refreshPromise;
}

// ============================================================
//  API HELPERS (自动 401 → 刷新 AT → 重试)
// ============================================================

async function apiGet(path) {
  if (!authToken) { showLoginModal(); throw new Error('未登录'); }
  const res = await fetch(API + path, { credentials: 'same-origin' });
  if (res.status === 401) {
    if (await tryRefreshToken()) {
      const retry = await fetch(API + path, { credentials: 'same-origin' });
      if (retry.status === 401) { logout(); throw new Error('会话已过期'); }
      return retry.json();
    }
    logout(); throw new Error('会话已过期');
  }
  return res.json();
}

async function apiPost(path, body, extraHeaders = {}) {
  if (!authToken && path !== '/login' && path !== '/register') {
    showLoginModal();
    throw new Error('未登录');
  }
  const headers = { 'Content-Type': 'application/json', ...extraHeaders };
  const res = await fetch(API + path, {
    method: 'POST', headers, credentials: 'same-origin',
    body: JSON.stringify(body)
  });
  if (res.status === 401 && path !== '/login' && path !== '/register') {
    if (await tryRefreshToken()) {
      const retry = await fetch(API + path, {
        method: 'POST', headers, credentials: 'same-origin',
        body: JSON.stringify(body)
      });
      if (retry.status === 401) { logout(); throw new Error('会话已过期'); }
      return retry.json();
    }
    logout(); throw new Error('会话已过期');
  }
  return res.json();
}

// ============================================================
//  SERVICES PANEL
// ============================================================

async function loadServices() {
  const data = await apiGet('/services');
  const container = document.getElementById('services-list');
  if (!data.services || data.services.length === 0) {
    container.innerHTML = '<div class="card empty-state"><div class="empty-icon">📦</div><p>暂无已注册的服务</p></div>';
    return;
  }
  container.innerHTML = data.services.map(svc => `
    <div class="service-card">
      <h3>${esc(svc.name)}</h3>
      <div class="svc-desc">${esc(svc.description)}</div>
      <div class="methods-row">
        ${svc.methods.map(m => `<span class="method-tag" title="${esc(m.description)}">${esc(m.name)}(${m.params.map(p => p.name + ':' + p.type).join(', ')})</span>`).join('')}
      </div>
    </div>
  `).join('');
}

document.getElementById('btn-refresh-svc').addEventListener('click', loadServices);
document.getElementById('btn-show-svc-register').addEventListener('click', () => {
  document.getElementById('svc-register-form').classList.remove('hidden');
});
document.getElementById('btn-cancel-svc-register').addEventListener('click', () => {
  document.getElementById('svc-register-form').classList.add('hidden');
});
document.getElementById('btn-register-svc').addEventListener('click', async () => {
  const name = document.getElementById('reg-svc-name').value.trim();
  const desc = document.getElementById('reg-svc-desc').value.trim();
  let methods;
  try {
    methods = JSON.parse(document.getElementById('reg-svc-methods').value.trim());
  } catch (e) {
    alert('方法定义 JSON 格式错误: ' + e.message);
    return;
  }
  if (!name) { alert('请输入服务名称'); return; }

  const data = await apiPost('/services', { name, description: desc, methods });
  if (data.success) {
    alert('服务注册成功');
    document.getElementById('svc-register-form').classList.add('hidden');
    document.getElementById('reg-svc-name').value = '';
    document.getElementById('reg-svc-desc').value = '';
    document.getElementById('reg-svc-methods').value = '';
    loadServices();
  } else {
    alert('注册失败: ' + data.error);
  }
});

// ============================================================
//  CALL PANEL
// ============================================================

async function loadServicesForCall() {
  const data = await apiGet('/services');
  const sel = document.getElementById('call-service');
  sel.innerHTML = '<option value="">-- 选择服务 --</option>';
  if (data.services) {
    data.services.forEach(svc => {
      sel.innerHTML += `<option value="${esc(svc.name)}">${esc(svc.name)}</option>`;
    });
  }
  sel._services = data.services || [];
}

document.getElementById('call-service')?.addEventListener('change', function() {
  const svcName = this.value;
  const methodSel = document.getElementById('call-method');
  const paramsContainer = document.getElementById('call-params-container');

  if (!svcName) {
    methodSel.innerHTML = '<option value="">-- 先选择服务 --</option>';
    paramsContainer.innerHTML = '';
    return;
  }

  const svc = this._services.find(s => s.name === svcName);
  if (!svc) return;

  methodSel.innerHTML = '<option value="">-- 选择方法 --</option>';
  methodSel._methodDefs = {};
  svc.methods.forEach(m => {
    methodSel.innerHTML += `<option value="${esc(m.name)}">${esc(m.name)}</option>`;
    methodSel._methodDefs[m.name] = m;
  });
  paramsContainer.innerHTML = '';
});

document.getElementById('call-method')?.addEventListener('change', function() {
  const def = this._methodDefs?.[this.value];
  const container = document.getElementById('call-params-container');
  if (!def || !def.params || def.params.length === 0) {
    container.innerHTML = '<div class="card" style="padding:14px;text-align:center;color:var(--text-muted);font-size:0.85rem;">该方法无需参数</div>';
    return;
  }
  container.innerHTML = def.params.map(p => `
    <div class="param-row">
      <span class="param-label">${esc(p.name)} ${p.required ? '<span style="color:var(--error)">*</span>' : ''}</span>
      <span class="param-type">${esc(p.type)}</span>
      <input type="text" id="param-${esc(p.name)}" placeholder="${esc(p.description || '')}${p.required ? ' (必填)' : ''}">
    </div>
  `).join('');
});

document.getElementById('btn-call')?.addEventListener('click', async () => {
  const service = document.getElementById('call-service').value;
  const method = document.getElementById('call-method').value;
  if (!service || !method) { alert('请选择服务和方法'); return; }

  const params = {};
  const rows = document.querySelectorAll('.param-row');
  rows.forEach(row => {
    const input = row.querySelector('input');
    if (input) {
      const name = input.id.replace('param-', '');
      const typeEl = row.querySelector('.param-type');
      const type = typeEl ? typeEl.textContent : 'string';
      const val = input.value.trim();
      if (val === '') return;
      if (type === 'int') params[name] = parseInt(val);
      else if (type === 'float') params[name] = parseFloat(val);
      else if (type === 'bool') params[name] = val === 'true';
      else if (type === 'object' || type === 'array') {
        try { params[name] = JSON.parse(val); }
        catch (e) { params[name] = val; }
      }
      else params[name] = val;
    }
  });

  const data = await apiPost('/call', { service, method, params });
  const resultDiv = document.getElementById('call-result');
  resultDiv.classList.remove('hidden');

  const statusEl = document.getElementById('call-result-status');
  if (data.success) {
    statusEl.className = 'success';
    statusEl.textContent = '调用成功';
  } else {
    statusEl.className = 'error';
    statusEl.textContent = '调用失败: ' + (data.error || '未知错误');
  }
  document.getElementById('call-result-body').textContent = JSON.stringify(data, null, 2);
  document.getElementById('call-result-duration').textContent = '耗时: ' + (data.duration_us != null ? (data.duration_us / 1000).toFixed(2) + ' ms' : 'N/A');
});

// ============================================================
//  HISTORY (inside Personal Center)
// ============================================================

// ---- History state ----
let historyStore = []; // in-memory store for showDetail lookup

async function loadHistoryUsers() {
  const sel = document.getElementById('history-user-select');
  try {
    const data = await apiGet('/history/users');
    sel.innerHTML = '<option value="">— 选择用户 —</option>' +
      (data.users || []).map(u => `<option value="${esc(u)}">${esc(u)}</option>`).join('');
  } catch (e) {
    sel.innerHTML = '<option value="">加载失败</option>';
  }
}

async function loadHistory() {
  const isAdmin = currentUser && currentUser.role === 'admin';
  const userSelect = document.getElementById('history-user-select');
  const titleUser = document.getElementById('history-title-user');
  const tbody = document.getElementById('history-tbody');
  const serviceFilter = document.getElementById('filter-service').value.trim();
  const methodFilter = document.getElementById('filter-method').value.trim();

  // Admin UI: show user selector
  if (isAdmin) {
    userSelect.style.display = '';
    if (userSelect.options.length <= 1) await loadHistoryUsers();
  } else {
    userSelect.style.display = 'none';
  }

  const selUser = isAdmin ? userSelect.value : '';
  let url = '/history?limit=100';
  if (selUser) url += '&user=' + encodeURIComponent(selUser);

  let data;
  try {
    data = await apiGet(url);
  } catch (e) {
    tbody.innerHTML = '<tr><td colspan="7" style="color:var(--text-muted);text-align:center;padding:32px 16px;">加载失败</td></tr>';
    updateProfileStats([]);
    return;
  }

  if (!data.history || data.history.length === 0) {
    tbody.innerHTML = '<tr><td colspan="7" style="color:var(--text-muted);text-align:center;padding:32px 16px;">暂无调用记录</td></tr>';
    updateProfileStats([]);
    // Hide user column for non-admin empty state
    return;
  }

  // Store for showDetail lookup
  historyStore = data.history;

  // Client-side filtering
  let filtered = data.history;
  if (serviceFilter) {
    const sf = serviceFilter.toLowerCase();
    filtered = filtered.filter(e => (e.service || '').toLowerCase().includes(sf));
  }
  if (methodFilter) {
    const mf = methodFilter.toLowerCase();
    filtered = filtered.filter(e => (e.method || '').toLowerCase().includes(mf));
  }

  titleUser.textContent = selUser ? '— ' + selUser : (isAdmin ? '— 全部用户' : '');

  tbody.innerHTML = filtered.map(e => {
    const entryId = e.id || '';
    const username = e.username || '';
    return `
    <tr>
      <td style="font-weight:500;">${esc(username)}</td>
      <td>${esc(e.timestamp)}</td>
      <td><span style="font-weight:500;">${esc(e.service)}</span></td>
      <td><span style="font-family:monospace;font-size:0.8rem;color:var(--accent-light);">${esc(e.method)}</span></td>
      <td><span class="status-badge ${e.success ? 'success' : 'error'}">${e.success ? '成功' : '失败'}</span></td>
      <td style="font-family:monospace;font-size:0.8rem;">${(e.duration_us / 1000).toFixed(2)} ms</td>
      <td><button class="btn" onclick="showDetail('${esc(entryId)}')" style="padding:4px 14px;font-size:0.75rem;">详情</button></td>
    </tr>
  `}).join('');

  updateProfileStats(filtered);
}

document.getElementById('btn-refresh-history').addEventListener('click', loadHistory);
document.getElementById('filter-service').addEventListener('input', debounce(loadHistory, 400));
document.getElementById('filter-method').addEventListener('input', debounce(loadHistory, 400));
document.getElementById('history-user-select').addEventListener('change', loadHistory);

// ---- System Monitor ----
let monitorTimer = null;
async function loadMonitor() {
  if (!authToken) { monitorTimer = null; return; }
  try {
    const data = await apiGet('/system/status');
    renderMonitor(data);
  } catch (e) {
    console.error('[monitor] load error:', e);
  }
  // 加载熔断器滑动窗口数据
  try {
    const bs = await apiGet('/breaker/stats');
    renderBreakerStats(bs);
  } catch(e) { console.error('[breaker-stats]', e); }

  // 10s 自动刷新
  if (monitorTimer) clearTimeout(monitorTimer);
  monitorTimer = setTimeout(loadMonitor, 10000);
}

function renderMonitor(data) {
  // 刷新时间
  document.getElementById('monitor-refresh-time').textContent =
    '更新于 ' + new Date().toLocaleTimeString('zh-CN');

  // 错误卡片
  ['auth', 'spreadsheet', 'file'].forEach(svc => {
    const errs = (data.errors && data.errors[svc]) ? data.errors[svc] : 0;
    const el = document.getElementById('mc-err-' + svc);
    const card = document.getElementById('mc-' + svc);
    if (el) el.textContent = errs;
    if (card) {
      card.classList.remove('ok', 'warn', 'err');
      if (errs === 0) card.classList.add('ok');
      else if (errs < 5) card.classList.add('warn');
      else card.classList.add('err');
    }
  });

  // 服务连通 + 熔断器
  const svcDiv = document.getElementById('monitor-services');
  if (svcDiv && data.services) {
    const chanColor = (s, b) => {
	      if (b === 'OPEN' || b === 'HALF_OPEN') return '#f43f5e';
	      if (s === 'READY') return '#10b981';
	      if (s === 'FAILED') return '#f59e0b';
	      return '#6b7280';
	    };
    const breakerColor = b => b === 'CLOSED' ? '#10b981' : b === 'OPEN' ? '#f43f5e' : '#6b7280';
    svcDiv.innerHTML = Object.entries(data.services).map(([name, s]) =>
      `${name.padEnd(10)} channel: <span style="color:${chanColor(s.channel, s.breaker)};font-weight:500;">${s.channel.padEnd(11)}</span>` +
      ` breaker: <span style="color:${breakerColor(s.breaker)};font-weight:500;">${s.breaker}</span>`
    ).join('<br>');
  }

  // 状态指示
  ['auth', 'spreadsheet', 'file'].forEach(svc => {
    const st = document.getElementById('mc-st-' + svc);
    if (st && data.services && data.services[svc]) {
      const s = data.services[svc];
      let label, color;
      if (s.breaker === 'OPEN' || s.breaker === 'HALF_OPEN') {
        label = '● 熔断'; color = '#f43f5e';
      } else if (s.channel === 'READY') {
        label = '● 正常'; color = '#10b981';
      } else if (s.channel === 'FAILED') {
        label = '● 失联'; color = '#f59e0b';
      } else {
        label = '● 就绪中'; color = '#6b7280';
      }
      st.innerHTML = `<span style="color:${color};">${label}</span>`;
    }
  });
}

function renderBreakerStats(data) {
  const el = document.getElementById('breaker-stats');
  if (!el || !data) return;
  const keys = ['auth', 'sheet', 'file'];
  el.innerHTML = keys.map(k => {
    const s = data[k];
    if (!s) return '';
    const errRate = s.total > 0 ? (s.failed / s.total * 100).toFixed(1) : '0.0';
    const slowRate = s.total > 0 ? (s.slow / s.total * 100).toFixed(1) : '0.0';
    const color = s.state === 'CLOSED' ? '#10b981' : s.state === 'OPEN' ? '#f43f5e' : '#f59e0b';
    return `<span style="color:${color};font-weight:500;">${k.padEnd(7)}</span>` +
      `state=<b>${s.state.padEnd(10)}</b>` +
      `total=${s.total} failed=${s.failed}(${errRate}%) slow=${s.slow}(${slowRate}%) ` +
      `P99=${s.p99_ms}ms fails=${s.local_fails}`;
  }).join('<br>');
}

// 压测按钮
document.getElementById('btn-stress-run').addEventListener('click', async () => {
  const status = document.getElementById('stress-status');
  const result = document.getElementById('stress-result');
  status.textContent = '压测运行中...  (等待 2-3s)';
  result.style.display = 'none';
  try {
    const res = await fetch(API + '/stress/run', { method: 'POST', credentials: 'same-origin' });
    const data = await res.json();
    status.textContent = '完成 ✓';
    result.style.display = 'block';
    result.textContent =
      `QPS:      ${data.qps || 'N/A'}\n` +
      `P50:      ${data.p50 || 'N/A'}\n` +
      `P95:      ${data.p95 || 'N/A'}\n` +
      `P99:      ${data.p99 || 'N/A'}\n` +
      `Failed:   ${data.failed || '0'}\n` +
      `Non-2xx:  ${data.non_2xx || '0'}`;
  } catch(e) {
    status.textContent = '压测失败';
    console.error('[stress]', e);
  }
});

document.getElementById('btn-refresh-monitor').addEventListener('click', () => {
  if (monitorTimer) clearTimeout(monitorTimer);
  loadMonitor();
});

function showDetail(id) {
  const entry = historyStore.find(e => String(e.id) === String(id));
  if (!entry) return;
  const div = document.getElementById('history-detail');
  div.classList.remove('hidden');
  const content = document.getElementById('history-detail-content');
  content.innerHTML = `
    <div class="detail-section">
      <h4>用户</h4><div>${esc(entry.username || '')}</div>
    </div>
    <div class="detail-section">
      <h4>时间</h4><div>${esc(entry.timestamp)}</div>
    </div>
    <div class="detail-section">
      <h4>服务 / 方法</h4><div>${esc(entry.service)} :: ${esc(entry.method)}</div>
    </div>
    <div class="detail-section">
      <h4>状态 / 耗时</h4>
      <div><span class="status-badge ${entry.success ? 'success' : 'error'}">${entry.success ? '成功' : '失败'}</span> ${(entry.duration_us / 1000).toFixed(2)} ms</div>
    </div>
    <div class="detail-section">
      <h4>参数</h4><pre>${JSON.stringify(entry.params || {}, null, 2)}</pre>
    </div>
    <div class="detail-section">
      <h4>结果</h4><pre>${JSON.stringify(entry.result || {}, null, 2)}</pre>
    </div>
  `;
  div.scrollIntoView({ behavior: 'smooth' });
}

// ============================================================
//  PERSONAL CENTER
// ============================================================

function updateProfileStats(history) {
  const total = history.length;
  const successCount = history.filter(e => e.success).length;
  const successRate = total > 0 ? Math.round((successCount / total) * 100) : 0;
  const avgLatency = total > 0
    ? (history.reduce((sum, e) => sum + (e.duration_us || 0), 0) / total / 1000)
    : 0;

  // Most used service
  const svcCounts = {};
  history.forEach(e => {
    svcCounts[e.service] = (svcCounts[e.service] || 0) + 1;
  });
  let topService = '--';
  let topCount = 0;
  for (const [svc, count] of Object.entries(svcCounts)) {
    if (count > topCount) { topCount = count; topService = svc; }
  }

  document.getElementById('pstat-total').textContent = total;
  document.getElementById('pstat-success-rate').textContent = successRate + '%';
  document.getElementById('pstat-avg-latency').textContent = avgLatency.toFixed(2) + ' ms';
  document.getElementById('pstat-top-service').textContent = topService;
}

function initProfile() {
  document.getElementById('profile-username').textContent = currentUser?.username || '--';
  document.getElementById('profile-join-date').textContent = currentUser?.joinDate || new Date().toLocaleDateString('zh-CN');
  loadAvatar();
  loadHistory();
}

// ============================================================
//  UTILS
// ============================================================

function esc(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function debounce(fn, delay) {
  let timer;
  return function(...args) {
    clearTimeout(timer);
    timer = setTimeout(() => fn.apply(this, args), delay);
  };
}

// ============================================================
//  STRESS TEST
// ============================================================

let stressRunning = false;
let stressAbort = null;
let stressStats = { total: 0, success: 0, fail: 0, latencies: [], errors: [], startTime: 0 };

document.querySelector('[data-tab="stress"]').addEventListener('click', () => {
  loadStressServices();
});

async function loadStressServices() {
  const data = await apiGet('/services');
  const sel = document.getElementById('stress-service');
  sel.innerHTML = '<option value="">-- 选择服务 --</option>';
  (data.services || []).forEach(svc => {
    sel.innerHTML += `<option value="${esc(svc.name)}">${esc(svc.name)}</option>`;
  });
  sel._services = data.services || [];
  document.getElementById('stress-method').innerHTML = '<option value="">-- 先选择服务 --</option>';
  document.getElementById('stress-params-container').innerHTML = '';
}

document.getElementById('stress-service').addEventListener('change', function () {
  const svcName = this.value;
  const methodSel = document.getElementById('stress-method');
  const pc = document.getElementById('stress-params-container');
  if (!svcName) {
    methodSel.innerHTML = '<option value="">-- 先选择服务 --</option>';
    pc.innerHTML = '';
    return;
  }
  const svc = this._services.find(s => s.name === svcName);
  if (!svc) return;
  methodSel.innerHTML = '<option value="">-- 选择方法 --</option>';
  svc.methods.forEach(m => {
    methodSel.innerHTML += `<option value="${esc(m.name)}">${esc(m.name)}</option>`;
  });
  methodSel._methods = svc.methods;
  pc.innerHTML = '';
});

document.getElementById('stress-method').addEventListener('change', function () {
  const methodName = this.value;
  const methods = this._methods || [];
  const def = methods.find(m => m.name === methodName);
  const pc = document.getElementById('stress-params-container');
  if (!def || !def.params || def.params.length === 0) {
    pc.innerHTML = '<p style="color:var(--text-muted);font-size:0.85rem;margin-top:12px;">该方法无需参数</p>';
    return;
  }
  pc.innerHTML = '<div class="form-row" style="margin-top:12px;">' +
    def.params.map(p => `
      <div class="form-group flex-1">
        <label>${esc(p.name)} (${esc(p.type)}) ${p.required ? '<span style="color:var(--error)">*</span>' : ''}</label>
        <input type="text" id="stress-param-${esc(p.name)}" placeholder="${esc(p.description || '')}">
      </div>`).join('') + '</div>';
});

function parseStressParams() {
  const params = {};
  const methodName = document.getElementById('stress-method').value;
  const methods = document.getElementById('stress-method')._methods || [];
  const def = methods.find(m => m.name === methodName);
  if (!def) return params;
  (def.params || []).forEach(p => {
    const input = document.getElementById('stress-param-' + p.name);
    if (!input) return;
    const val = input.value.trim();
    if (val === '') return;
    switch (p.type) {
      case 'int': params[p.name] = parseInt(val); break;
      case 'float': params[p.name] = parseFloat(val); break;
      case 'bool': params[p.name] = val === 'true'; break;
      case 'object': case 'array':
        try { params[p.name] = JSON.parse(val); } catch (_) { params[p.name] = val; }
        break;
      default: params[p.name] = val;
    }
  });
  return params;
}

function resetStressStats() {
  stressStats = { total: 0, success: 0, fail: 0, latencies: [], errors: [], startTime: 0 };
  document.getElementById('stat-total').textContent = '0';
  document.getElementById('stat-success').textContent = '0';
  document.getElementById('stat-fail').textContent = '0';
  document.getElementById('stat-qps').textContent = '0';
  document.getElementById('stat-avg').textContent = '0 ms';
  document.getElementById('stat-p99').textContent = '0 ms';
  document.getElementById('stress-progress-fill').style.width = '0%';
  document.getElementById('stress-progress-text').textContent = '0 / 0';
  document.getElementById('latency-bars').innerHTML = '';
  document.getElementById('stress-errors').innerHTML = '';
  document.getElementById('stress-stats').classList.add('hidden');
  document.getElementById('stress-progress-wrap').classList.add('hidden');
  document.getElementById('stress-chart-wrap').classList.add('hidden');
  document.getElementById('stress-errors-wrap').classList.add('hidden');
}

function updateStressUI() {
  const s = stressStats;
  document.getElementById('stat-total').textContent = s.total;
  document.getElementById('stat-success').textContent = s.success;
  document.getElementById('stat-fail').textContent = s.fail;

  const elapsed = (performance.now() - s.startTime) / 1000;
  document.getElementById('stat-qps').textContent = elapsed > 0 ? (s.total / elapsed).toFixed(1) : '0';

  if (s.latencies.length > 0) {
    const sorted = [...s.latencies].sort((a, b) => a - b);
    const avg = s.latencies.reduce((a, b) => a + b, 0) / s.latencies.length;
    const p99 = sorted[Math.floor(sorted.length * 0.99)];
    document.getElementById('stat-avg').textContent = avg.toFixed(2) + ' ms';
    document.getElementById('stat-p99').textContent = (p99 || sorted[sorted.length - 1]).toFixed(2) + ' ms';
  }

  const totalTarget = parseInt(document.getElementById('stress-total').value) || 1;
  const pct = Math.min(100, (s.total / totalTarget) * 100);
  document.getElementById('stress-progress-fill').style.width = pct + '%';
  document.getElementById('stress-progress-text').textContent = s.total + ' / ' + totalTarget;

  if (s.latencies.length > 0) {
    const buckets = [
      { label: '<1ms', max: 1 },
      { label: '1-5', max: 5 },
      { label: '5-10', max: 10 },
      { label: '10-50', max: 50 },
      { label: '50-100', max: 100 },
      { label: '100-500', max: 500 },
      { label: '500+', max: Infinity }
    ];
    const counts = buckets.map(() => 0);
    s.latencies.forEach(l => {
      for (let i = 0; i < buckets.length; i++) {
        if (l <= buckets[i].max) { counts[i]++; break; }
      }
    });
    const maxCount = Math.max(1, ...counts);
    document.getElementById('latency-bars').innerHTML = counts.map((c, i) => {
      const h = Math.max(2, (c / maxCount) * 170);
      return `<div class="bar-col"><div class="bar-fill" style="height:${h}px;"></div><div class="bar-count">${c}</div></div>`;
    }).join('');
  }

  const displayed = s.errors.slice(-20);
  document.getElementById('stress-errors').innerHTML = displayed.length === 0
    ? '<p style="color:var(--text-muted);text-align:center;padding:16px;">暂无错误</p>'
    : displayed.map((e, i) => `<div style="padding:4px 0;border-bottom:1px solid rgba(255,255,255,0.04);color:var(--error);">#${s.total - displayed.length + i + 1} ${esc(e)}</div>`).join('');
}

async function runStressTest() {
  const service = document.getElementById('stress-service').value;
  const method = document.getElementById('stress-method').value;
  if (!service || !method) { alert('请选择服务和方法'); return; }

  const concurrency = Math.max(1, parseInt(document.getElementById('stress-concurrency').value) || 10);
  const totalTarget = Math.max(1, parseInt(document.getElementById('stress-total').value) || 1000);
  const timeout = Math.max(500, parseInt(document.getElementById('stress-timeout').value) || 10000);

  resetStressStats();
  stressRunning = true;
  stressAbort = new AbortController();

  document.getElementById('btn-stress-start').style.display = 'none';
  document.getElementById('btn-stress-stop').style.display = '';
  document.getElementById('stress-stats').classList.remove('hidden');
  document.getElementById('stress-progress-wrap').classList.remove('hidden');
  document.getElementById('stress-chart-wrap').classList.remove('hidden');
  document.getElementById('stress-errors-wrap').classList.remove('hidden');

  stressStats.startTime = performance.now();
  let completed = 0;
  const params = parseStressParams();

  const updateTimer = setInterval(() => {
    if (!stressRunning) { clearInterval(updateTimer); return; }
    updateStressUI();
  }, 200);

  async function worker(workerId) {
    while (stressRunning && completed < totalTarget) {
      const current = ++completed;
      if (current > totalTarget) break;

      const start = performance.now();
      try {
        const res = await fetch(API + '/call', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          credentials: 'same-origin',
          body: JSON.stringify({ service, method, params }),
          signal: stressAbort.signal
        });
        const data = await res.json();
        const elapsed = performance.now() - start;
        stressStats.total++;
        stressStats.latencies.push(elapsed);
        if (data.success) {
          stressStats.success++;
        } else {
          stressStats.fail++;
          stressStats.errors.push(`${data.error || 'unknown error'} (${elapsed.toFixed(1)}ms)`);
          if (stressStats.errors.length > 200) stressStats.errors = stressStats.errors.slice(-200);
        }
      } catch (err) {
        if (err.name === 'AbortError') break;
        const elapsed = performance.now() - start;
        stressStats.total++;
        stressStats.fail++;
        stressStats.errors.push(`${err.message} (${elapsed.toFixed(1)}ms)`);
        if (stressStats.errors.length > 200) stressStats.errors = stressStats.errors.slice(-200);
      }
    }
  }

  const workers = [];
  for (let i = 0; i < concurrency; i++) workers.push(worker(i));
  await Promise.all(workers);

  stressRunning = false;
  clearInterval(updateTimer);
  updateStressUI();
  document.getElementById('btn-stress-start').style.display = '';
  document.getElementById('btn-stress-stop').style.display = 'none';
}

document.getElementById('btn-stress-start').addEventListener('click', () => {
  runStressTest().catch(err => {
    console.error('Stress test error:', err);
    stressRunning = false;
    document.getElementById('btn-stress-start').style.display = '';
    document.getElementById('btn-stress-stop').style.display = 'none';
  });
});

document.getElementById('btn-stress-stop').addEventListener('click', () => {
  stressRunning = false;
  if (stressAbort) stressAbort.abort();
  document.getElementById('btn-stress-start').style.display = '';
  document.getElementById('btn-stress-stop').style.display = 'none';
});

document.getElementById('btn-stress-reset').addEventListener('click', () => {
  if (stressRunning) {
    stressRunning = false;
    if (stressAbort) stressAbort.abort();
    document.getElementById('btn-stress-start').style.display = '';
    document.getElementById('btn-stress-stop').style.display = 'none';
  }
  resetStressStats();
});

// ============================================================
//  SPREADSHEET MANAGEMENT
// ============================================================

async function apiPut(path, body) {
  if (!authToken) { showLoginModal(); throw new Error('未登录'); }
  const res = await fetch(API + path, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    credentials: 'same-origin',
    body: JSON.stringify(body)
  });
  if (res.status === 401) { logout(); throw new Error('会话已过期'); }
  return res.json();
}

async function apiDelete(path) {
  if (!authToken) { showLoginModal(); throw new Error('未登录'); }
  const res = await fetch(API + path, { method: 'DELETE', credentials: 'same-origin' });
  if (res.status === 401) { logout(); throw new Error('会话已过期'); }
  return res.json();
}


// ---- Load sheet list ----
async function loadSheets(page = 0) {
  sheetsPage = page;
  console.log('[sheets] loadSheets called, page=' + page);
  try {
    const data = await apiGet('/sheets?page=' + page + '&page_size=' + PAGE_SIZE);
    console.log('[sheets] API response:', data);
    sheetsTotal = data.total || 0;
    renderSheetList(data);
  } catch (e) {
    console.error('loadSheets error:', e);
  }
}

function renderSheetList(data) {
  const listEl = document.getElementById('sheets-list');
  const emptyEl = document.getElementById('sheets-empty');
  const sheets = data.sheets || [];
  const total = data.total || 0;

  if (sheets.length === 0 && sheetsPage === 0) {
    listEl.innerHTML = '';
    emptyEl.style.display = '';
    renderPagination('sheets-pagination', total, sheetsPage, loadSheets);
    return;
  }
  emptyEl.style.display = 'none';

  listEl.innerHTML = sheets.map(s => `
    <div class="sheet-card" data-id="${s.id}">
      <div class="sheet-card-info">
        <h3>${esc(s.name)}</h3>
        <p class="sheet-card-desc">${esc(s.description || '无备注')}</p>
        <div class="sheet-card-meta">
          <span>${s.row_count} 行 × ${s.col_count} 列</span>
          <span>更新于 ${esc(s.updated_at || '--')}</span>
        </div>
      </div>
      <div class="sheet-card-actions">
        <button class="btn btn-primary btn-sm" onclick="openSheet('${s.id}')">打开</button>
        <button class="btn btn-sm" onclick="deleteSheet('${s.id}', '${esc(s.name)}')">删除</button>
      </div>
    </div>
  `).join('');

  renderPagination('sheets-pagination', total, sheetsPage, loadSheets);
}

function renderPagination(containerId, total, currentPage, loadFn) {
  const el = document.getElementById(containerId);
  if (!el) return;
  const totalPages = Math.max(1, Math.ceil(total / PAGE_SIZE));
  if (totalPages <= 1) { el.innerHTML = ''; return; }
  const hasPrev = currentPage > 0;
  const hasNext = currentPage < totalPages - 1;
  el.innerHTML = `
    <div class="pagination">
      <button class="btn btn-sm" ${hasPrev ? '' : 'disabled'} onclick="${loadFn.name}(${currentPage - 1})">上一页</button>
      <span class="pagination-info">第 ${currentPage + 1} / ${totalPages} 页 · 共 ${total} 条</span>
      <button class="btn btn-sm" ${hasNext ? '' : 'disabled'} onclick="${loadFn.name}(${currentPage + 1})">下一页</button>
    </div>
  `;
}

// ---- Create blank sheet ----
async function createBlankSheet() {
  if (isCreatingSheet) return;
  console.log('[sheets] createBlankSheet called');
  const name = prompt('请输入表格名称：', '新建表格');
  if (!name || !name.trim()) { console.log('[sheets] createBlankSheet cancelled'); return; }
  const desc = prompt('请输入备注（可选）：', '');

  // 自定义行列数
  const cols = parseInt(prompt('列数：', '5')) || 5;
  const rows = parseInt(prompt('行数：', '10')) || 10;
  const colCount = Math.max(1, Math.min(cols, 50));
  const rowCount = Math.max(1, Math.min(rows, 5000));

  // 生成表头和数据
  const headers = [];
  for (let i = 1; i <= colCount; i++) headers.push('列' + i);
  const data = [];
  for (let r = 0; r < rowCount; r++) {
    data.push(new Array(colCount).fill(''));
  }

  isCreatingSheet = true;
  const idemKey = uuidv4();
  try {
    const res = await apiPost('/sheets', {
      name: name.trim(),
      description: desc || '',
      headers_json: JSON.stringify(headers),
      data_json: JSON.stringify(data)
    }, { 'X-Idempotency-Key': idemKey });
    console.log('[sheets] create API response:', res);
    if (res.success) {
      loadSheets();
      openSheet(res.id);
    } else {
      alert('创建失败: ' + (res.error || '未知错误'));
    }
  } catch (e) {
    console.error('[sheets] create error:', e);
    alert('网络错误: ' + e.message);
  } finally {
    isCreatingSheet = false;
  }
}

// ---- Upload xlsx ----
function triggerXlsxUpload() {
  console.log('[sheets] triggerXlsxUpload called');
  const el = document.getElementById('xlsx-file-input');
  if (!el) { console.error('[sheets] xlsx-file-input not found'); return; }
  el.click();
}

function handleXlsxFile(file) {
  console.log('[sheets] handleXlsxFile:', file.name);
  if (typeof XLSX === 'undefined') {
    alert('XLSX 库加载失败，请检查网络连接');
    console.error('[sheets] XLSX library not loaded');
    return;
  }
  const reader = new FileReader();
  reader.onload = function(e) {
    try {
      const workbook = XLSX.read(e.target.result, { type: 'array' });
      const firstSheet = workbook.SheetNames[0];
      const worksheet = workbook.Sheets[firstSheet];
      const rows = XLSX.utils.sheet_to_json(worksheet, { header: 1, defval: '' });

      if (rows.length === 0) { alert('表格为空'); return; }
      const headers = rows[0].map(h => String(h));
      const data = rows.slice(1).map(row => {
        const filled = [];
        for (let i = 0; i < headers.length; i++) {
          filled.push(row[i] != null ? String(row[i]) : '');
        }
        return filled;
      });

      const name = prompt('请输入表格名称：', firstSheet || '导入表格');
      if (!name || !name.trim()) return;

      createFromXlsx(name.trim(), headers, data);
    } catch (err) {
      alert('解析 xlsx 失败: ' + err.message);
    }
  };
  reader.readAsArrayBuffer(file);
}

async function createFromXlsx(name, headers, data) {
  if (isCreatingSheet) return;
  isCreatingSheet = true;
  try {
    const res = await apiPost('/sheets', {
      name: name,
      description: '从 xlsx 导入',
      headers_json: JSON.stringify(headers),
      data_json: JSON.stringify(data)
    }, { 'X-Idempotency-Key': uuidv4() });
    if (res.success) {
      loadSheets();
      openSheet(res.id);
    } else {
      alert('导入失败: ' + (res.error || '未知错误'));
    }
  } catch (e) {
    alert('网络错误');
  } finally {
    isCreatingSheet = false;
  }
}

// ---- Open sheet (editor view) ----
async function openSheet(id) {
  window.currentSheetId = id;
  console.log('[sheets] openSheet id:', id, 'authToken:', !!authToken);
  try {
    const data = await apiGet('/sheets/' + id);
    console.log('[sheets] openSheet response:', data);
    if (!data.success) { alert('获取表格失败: ' + (data.error || '')); return; }

    const s = data.spreadsheet;
    window.currentSheetData = Array.isArray(s.data_json) ? s.data_json : JSON.parse(s.data_json || '[[]]');
    window.currentSheetHeaders = Array.isArray(s.headers_json) ? s.headers_json : JSON.parse(s.headers_json || '[]');

    document.getElementById('sheets-list-view').style.display = 'none';
    document.getElementById('sheets-editor-view').style.display = '';
    document.getElementById('sheet-edit-name').value = s.name;
    document.getElementById('sheet-edit-desc').value = s.description || '';
    document.getElementById('cache-source-text').textContent = data.cache_source === 'redis' ? 'Redis 缓存' : 'MySQL 数据库';
    document.getElementById('cache-source-text').style.color = data.cache_source === 'redis' ? '#10b981' : '#f59e0b';

    // 同步行列输入框
    document.getElementById('sheet-set-rows').value = window.currentSheetData.length;
    document.getElementById('sheet-set-cols').value = window.currentSheetHeaders.length;

    renderGrid();
  } catch (e) {
    console.error('[sheets] openSheet error:', e);
    alert('网络错误: ' + e.message);
  }
}

function closeEditor() {
  document.getElementById('sheets-list-view').style.display = '';
  document.getElementById('sheets-editor-view').style.display = 'none';
  window.currentSheetId = null;
  window.currentSheetData = null;
  window.currentSheetHeaders = null;
}

// ---- Grid rendering ----
function renderGrid() {
  const table = document.getElementById('sheet-grid');
  const headers = window.currentSheetHeaders || [];
  const data = window.currentSheetData || [];

  let html = '<thead><tr><th class="row-num">#</th>';
  headers.forEach((h, i) => {
    html += `<th contenteditable="true" data-col="${i}" class="col-header" onblur="onHeaderChange(${i}, this.textContent)">${esc(h)}</th>`;
  });
  html += '</tr></thead><tbody>';
  data.forEach((row, ri) => {
    html += '<tr><td class="row-num">' + (ri + 1) + '</td>';
    row.forEach((cell, ci) => {
      html += `<td contenteditable="true" data-row="${ri}" data-col="${ci}" onblur="onCellChange(${ri}, ${ci}, this.textContent)">${esc(String(cell))}</td>`;
    });
    html += '</tr>';
  });
  html += '</tbody>';
  table.innerHTML = html;
}

function onHeaderChange(colIdx, text) {
  if (window.currentSheetHeaders && colIdx < window.currentSheetHeaders.length) {
    window.currentSheetHeaders[colIdx] = text;
  }
}

function onCellChange(row, col, text) {
  if (window.currentSheetData && row < window.currentSheetData.length && col < window.currentSheetData[row].length) {
    window.currentSheetData[row][col] = text;
  }
}

// ---- Add / Delete rows & cols ----
function addRow() {
  if (!window.currentSheetHeaders || window.currentSheetHeaders.length === 0) return;
  const newRow = new Array(window.currentSheetHeaders.length).fill('');
  window.currentSheetData.push(newRow);
  renderGrid();
}

function addCol() {
  window.currentSheetHeaders.push('新列' + (window.currentSheetHeaders.length + 1));
  window.currentSheetData.forEach(row => row.push(''));
  renderGrid();
}

function deleteRow() {
  if (window.currentSheetData.length <= 1) return;
  window.currentSheetData.pop();
  renderGrid();
}

function deleteCol() {
  if (window.currentSheetHeaders.length <= 1) return;
  window.currentSheetHeaders.pop();
  window.currentSheetData.forEach(row => row.pop());
  renderGrid();
}

// ---- Resize grid ----
function resizeGrid() {
  const targetRows = parseInt(document.getElementById('sheet-set-rows').value) || 10;
  const targetCols = parseInt(document.getElementById('sheet-set-cols').value) || 5;
  const newRows = Math.max(1, Math.min(targetRows, 5000));
  const newCols = Math.max(1, Math.min(targetCols, 50));

  // 调整列
  while (window.currentSheetHeaders.length < newCols) {
    window.currentSheetHeaders.push('新列' + (window.currentSheetHeaders.length + 1));
  }
  window.currentSheetHeaders.length = newCols;

  // 调整行
  window.currentSheetData.forEach(row => {
    while (row.length < newCols) row.push('');
    row.length = newCols;
  });
  while (window.currentSheetData.length < newRows) {
    window.currentSheetData.push(new Array(newCols).fill(''));
  }
  window.currentSheetData.length = newRows;

  document.getElementById('sheet-set-rows').value = newRows;
  document.getElementById('sheet-set-cols').value = newCols;
  renderGrid();
}

// ---- Save sheet ----
async function saveSheet() {
  if (!window.currentSheetId || isSavingSheet) return;
  isSavingSheet = true;
  const btn = document.getElementById('btn-sheet-save');
  const origText = btn ? btn.textContent : '保存';
  if (btn) { btn.disabled = true; btn.textContent = '保存中…'; }

  try {
    const res = await apiPut('/sheets', {
      id: window.currentSheetId,
      name: document.getElementById('sheet-edit-name').value.trim(),
      description: document.getElementById('sheet-edit-desc').value.trim(),
      headers_json: JSON.stringify(window.currentSheetHeaders),
      data_json: JSON.stringify(window.currentSheetData)
    });
    if (res.success) {
      // Re-fetch to display cache source badge
      const data = await apiPost('/sheets/get', { id: window.currentSheetId });
      if (data.success) {
        document.getElementById('cache-source-text').textContent = data.cache_source === 'redis' ? 'Redis 缓存' : 'MySQL 数据库';
        document.getElementById('cache-source-text').style.color = data.cache_source === 'redis' ? '#10b981' : '#f59e0b';
      }
    } else {
      alert('保存失败: ' + (res.error || '未知错误'));
    }
  } catch (e) {
    alert('网络错误');
  } finally {
    isSavingSheet = false;
    if (btn) { btn.disabled = false; btn.textContent = origText; }
  }
}

// ---- Export xlsx ----
function exportXlsx() {
  if (!window.currentSheetHeaders) return;
  const rows = [window.currentSheetHeaders, ...window.currentSheetData];
  const ws = XLSX.utils.aoa_to_sheet(rows);
  const wb = XLSX.utils.book_new();
  XLSX.utils.book_append_sheet(wb, ws, 'Sheet1');
  const name = document.getElementById('sheet-edit-name').value || '表格';
  XLSX.writeFile(wb, name + '.xlsx');
}

// ---- Delete sheet ----
async function deleteSheet(id, name) {
  if (!confirm('确定删除表格 "' + name + '" 吗？此操作不可恢复。')) return;
  try {
    const res = await apiDelete('/sheets/' + id);
    if (!res.success) { alert('删除失败: ' + (res.error || '未知错误')); return; }
  } catch (e) { alert('网络错误'); return; }
  // Stay on current page; fall back to previous page if it becomes empty
  const totalAfter = sheetsTotal - 1;
  const maxPage = Math.max(0, Math.ceil(totalAfter / PAGE_SIZE) - 1);
  loadSheets(Math.min(sheetsPage, maxPage));
}

// ---- Event bindings (wait for DOM ready) ----
// 按钮事件已通过 HTML onclick 绑定，无需 JS addEventListener

// ============================================================
// ============================================================
//  FILE MANAGEMENT
// ============================================================

async function loadFiles(page = 0) {
  console.log('[files] loadFiles called, page=' + page);
  filesPage = page;
  try {
    const data = await apiGet('/files?page=' + page + '&page_size=' + PAGE_SIZE);
    console.log('[files] API response:', data);
    filesTotal = data.total || 0;
    renderFileList(data);
  } catch (e) {
    console.error('loadFiles error:', e);
  }
}

function renderFileList(data) {
  const listEl = document.getElementById('files-list');
  const emptyEl = document.getElementById('files-empty');
  const files = data.files || [];
  const total = data.total || 0;

  if (files.length === 0 && filesPage === 0) {
    listEl.innerHTML = '';
    emptyEl.style.display = '';
    renderPagination('files-pagination', total, filesPage, loadFiles);
    return;
  }
  emptyEl.style.display = 'none';

  listEl.innerHTML = files.map(f => {
    const sizeStr = f.size < 1024 ? f.size + ' B'
      : f.size < 1048576 ? (f.size / 1024).toFixed(1) + ' KB'
      : (f.size / 1048576).toFixed(1) + ' MB';
    return `<div class="sheet-card">
      <div class="sheet-card-info">
        <h3>${esc(f.original_name)}</h3>
        <div class="sheet-card-meta">
          <span>${sizeStr}</span>
          <span>${esc(f.mime_type)}</span>
          <span>${esc(f.created_at || '')}</span>
        </div>
      </div>
      <div class="sheet-card-actions">
        <button class="btn btn-sm" onclick="downloadFile('${f.id}', '${esc(f.original_name)}')">下载</button>
        <button class="btn btn-sm" onclick="deleteFile('${f.id}', '${esc(f.original_name)}')">删除</button>
      </div>
    </div>`;
  }).join('');

  renderPagination('files-pagination', total, filesPage, loadFiles);
}

async function uploadFile(file) {
  if (!file || isUploadingFile) return;
  isUploadingFile = true;
  const btn = document.getElementById('btn-file-upload-trigger');
  const origText = btn ? btn.textContent : '上传文件';
  if (btn) { btn.disabled = true; btn.textContent = '上传中…'; }

  const formData = new FormData();
  formData.append('file', file);

  try {
    const res = await fetch(API + '/files/upload', {
      method: 'POST',
      headers: { 'X-Idempotency-Key': uuidv4() },
      credentials: 'same-origin',
      body: formData
    });
    if (res.status === 401) { logout(); return; }
    const data = await res.json();
    if (data.success) {
      loadFiles(0);
    } else {
      alert('上传失败: ' + (data.error || '未知错误'));
    }
  } catch (e) {
    console.error('uploadFile error:', e);
    alert('上传失败: ' + e.message);
  } finally {
    isUploadingFile = false;
    if (btn) { btn.disabled = false; btn.textContent = origText; }
  }
}

async function downloadFile(id, filename) {
  try {
    // 用 fetch 触发自动刷新，拿到 blob 后通过 URL 下载
    var res = await fetch(API + '/files/download?id=' + encodeURIComponent(id), { credentials: 'same-origin' });
    if (res.status === 401) { logout(); return; }
    if (!res.ok) { alert('下载失败 (HTTP ' + res.status + ')'); return; }
    var blob = await res.blob();
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url;
    a.download = filename || 'download';
    document.body.appendChild(a);
    a.click();
    setTimeout(function() {
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
    }, 100);
  } catch (e) {
    alert('下载失败: ' + e.message);
  }
}

async function deleteFile(id, name) {
  if (!confirm('确定删除文件 "' + name + '" 吗？此操作不可恢复。')) return;
  try {
    const res = await apiDelete('/files/' + id);
    if (!res.success) { alert('删除失败: ' + (res.error || '未知错误')); return; }
  } catch (e) { alert('网络错误: ' + e.message); return; }
  const totalAfter = filesTotal - 1;
  const maxPage = Math.max(0, Math.ceil(totalAfter / PAGE_SIZE) - 1);
  loadFiles(Math.min(filesPage, maxPage));
}

// ============================================================
//  INIT
// ============================================================

checkAuth();
