'use strict';

(() => {
  const VIEWS = ['dashboard', 'torrents', 'speed', 'files'];
  let currentAnnounceUrl = 'http://127.0.0.1:6969/announce';
  const PIECE_LENGTH = 262144;
  const torrentList = [];
  let activeAction = 'create';

  async function fetchSystemInfo() {
    try {
      const res = await fetch('/api/system/info');
      if (res.ok) {
        const data = await res.json();
        if (data.default_announce) {
          currentAnnounceUrl = data.default_announce;
          const input = document.getElementById('tracker-announce-input');
          if (input && !input.value.trim()) {
            input.value = currentAnnounceUrl;
          }
        }
      }
    } catch (_) {}
    loadDhtConfig();
  }

  async function loadDhtConfig() {
    try {
      const res = await fetch('/api/dht/status');
      if (res.ok) {
        const data = await res.json();
        const input = document.getElementById('dht-bootstrap-input');
        if (input && !input.value.trim() && data.bootstrap_host) {
          input.value = `${data.bootstrap_host}:${data.bootstrap_port || 6881}`;
        }
      }
    } catch (_) {}
  }

  async function handleSaveDhtBootstrap() {
    const input = document.getElementById('dht-bootstrap-input');
    if (!input || !input.value.trim()) return;
    const parts = input.value.trim().split(':');
    const host = parts[0].trim();
    const port = parts[1] ? parseInt(parts[1].trim(), 10) : 6881;
    try {
      setStatus(`Connecting to DHT bootstrap node ${host}:${port}...`);
      const res = await fetch('/api/dht/bootstrap', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ host, port })
      });
      if (res.ok) {
        setStatus(`DHT bootstrap connected to ${host}:${port}`, 'success');
        await syncTorrentsFromServer();
      } else {
        setStatus('Failed to set DHT bootstrap', 'error');
      }
    } catch (err) {
      setStatus(`Failed to set DHT bootstrap: ${err.message}`, 'error');
    }
  }

  function showView() {
    const name = location.hash.replace(/^#\/?/, '');
    const view = VIEWS.includes(name) ? name : 'dashboard';

    for (const section of document.querySelectorAll('.view')) {
      section.hidden = section.id !== `view-${view}`;
    }
    for (const link of document.querySelectorAll('.sidebar a')) {
      if (link.dataset.view === view) link.setAttribute('aria-current', 'page');
      else link.removeAttribute('aria-current');
    }
    if (view === 'files') loadFiles();
  }

  function setStatus(message, kind = 'info') {
    const status = document.getElementById('torrent-status');
    if (!status) return;
    status.textContent = message;
    status.classList.remove('success', 'error');
    if (kind === 'success') status.classList.add('success');
    if (kind === 'error') status.classList.add('error');
  }

  function formatBytes(bytes) {
    if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
    const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
    const index = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), sizes.length - 1);
    const value = bytes / (1024 ** index);
    return `${value.toFixed(value >= 10 || index === 0 ? 0 : 1)} ${sizes[index]}`;
  }

  function formatSpeed(bytesPerSec) {
    return `${formatBytes(bytesPerSec)}/s`;
  }

  function normalizeTorrentState(rawState) {
    const value = String(rawState || 'stopped').trim().toLowerCase();
    if (value === 'seeding') return { label: 'SEEDING', kind: 'seeding' };
    if (value === 'downloading') return { label: 'DOWNLOADING', kind: 'downloading' };
    if (value === 'waiting') return { label: 'WAITING', kind: 'waiting' };
    if (value === 'checking') return { label: 'CHECKING', kind: 'checking' };
    if (value === 'paused') return { label: 'PAUSED', kind: 'paused' };
    if (value === 'queued') return { label: 'QUEUED', kind: 'queued' };
    if (value === 'error') return { label: 'ERROR', kind: 'error' };
    return { label: 'STOPPED', kind: 'stopped' };
  }

  function escapeHtml(value) {
    return String(value ?? '')
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function updateDashboardTiles() {
    let totalDownloaded = 0;
    let totalUploaded = 0;
    let totalDlSpeed = 0;
    let totalUlSpeed = 0;

    torrentList.forEach(t => {
      totalDownloaded += t.downloaded || 0;
      totalUploaded += t.uploaded || 0;
      totalDlSpeed += t.downloadRate || 0;
      totalUlSpeed += t.uploadRate || 0;
    });

    // Update dashboard tiles
    const dashView = document.getElementById('view-dashboard');
    if (dashView) {
      const values = dashView.querySelectorAll('.tile-value');
      if (values.length >= 2) {
        values[0].textContent = formatBytes(totalDownloaded);
        values[1].textContent = formatBytes(totalUploaded);
      }
    }

    recordSpeedSample(totalDlSpeed, totalUlSpeed, totalDownloaded, totalUploaded);
  }

  // Trang Speed: giữ 60 mẫu gần nhất (poll 2 giây/lần => 2 phút), vẽ biểu đồ vùng bằng SVG.
  const SPEED_SAMPLES = 60;
  const speedHistory = { down: [], up: [] };
  const speedPeak = { down: 0, up: 0 };
  const speedLimitKib = { down: 0, up: 0 };
  let speedLimitFetchedAt = 0;

  function drawSpeedChart(dir) {
    const samples = speedHistory[dir];
    const line = document.getElementById(`speed-${dir}-line`);
    const area = document.getElementById(`speed-${dir}-area`);
    if (!line || !area || samples.length === 0) return;
    const width = 300;
    const height = 80;
    const top = 6;  // chừa lề trên để đỉnh không bị cắt
    const maxValue = Math.max(...samples, 1024);  // tối thiểu 1 KB/s để mức rảnh không bị phóng đại
    const step = width / (SPEED_SAMPLES - 1);
    const points = samples.map((value, index) => {
      const x = width - (samples.length - 1 - index) * step;
      const y = height - (value / maxValue) * (height - top);
      return [x.toFixed(1), y.toFixed(1)];
    });
    const path = points.map(([x, y], index) => `${index === 0 ? 'M' : 'L'}${x} ${y}`).join(' ');
    line.setAttribute('d', path);
    area.setAttribute('d', `${path} L${points[points.length - 1][0]} ${height} L${points[0][0]} ${height} Z`);
  }

  function renderSpeedDirection(dir, current, transferred) {
    const samples = speedHistory[dir];
    const set = (suffix, text) => {
      const el = document.getElementById(`speed-${dir}-${suffix}`);
      if (el) el.textContent = text;
    };
    const average = samples.length ? samples.reduce((a, b) => a + b, 0) / samples.length : 0;
    set('value', formatSpeed(current));
    set('avg', formatSpeed(average));
    set('peak', formatSpeed(speedPeak[dir]));
    set('total', formatBytes(transferred));

    const limit = speedLimitKib[dir];
    const chip = document.getElementById(`speed-${dir}-limit`);
    const bar = document.getElementById(`speed-${dir}-bar`);
    if (chip) {
      chip.textContent = limit ? `Limit ${limit.toLocaleString('en-US')} KiB/s` : 'No limit';
      chip.classList.toggle('is-limited', Boolean(limit));
    }
    if (bar) {
      bar.hidden = !limit;
      if (limit) {
        const percent = Math.min(100, (current / (limit * 1024)) * 100);
        bar.firstElementChild.style.width = `${percent.toFixed(0)}%`;
      }
    }
    drawSpeedChart(dir);
  }

  async function refreshSpeedLimits() {
    // Giới hạn chỉ đổi khi người dùng chỉnh trong Settings: tra lại tối đa 10 giây/lần và chỉ khi đang xem trang Speed.
    const view = document.getElementById('view-speed');
    if (!view || view.hidden || Date.now() - speedLimitFetchedAt < 10000) return;
    speedLimitFetchedAt = Date.now();
    try {
      const res = await fetch('/api/settings/bandwidth');
      if (!res.ok) return;
      const data = await res.json();
      speedLimitKib.down = Number(data.down) || 0;
      speedLimitKib.up = Number(data.up) || 0;
    } catch (_) { /* giữ giá trị cũ */ }
  }

  function recordSpeedSample(downRate, upRate, downloaded, uploaded) {
    for (const [dir, rate] of [['down', downRate], ['up', upRate]]) {
      const value = Number.isFinite(rate) && rate > 0 ? rate : 0;
      speedHistory[dir].push(value);
      if (speedHistory[dir].length > SPEED_SAMPLES) speedHistory[dir].shift();
      speedPeak[dir] = Math.max(speedPeak[dir], value);
    }
    renderSpeedDirection('down', speedHistory.down[speedHistory.down.length - 1], downloaded);
    renderSpeedDirection('up', speedHistory.up[speedHistory.up.length - 1], uploaded);
    refreshSpeedLimits();
  }

  const STATE_BADGE_STYLE = {
    seeding: 'background: #dcfce7; color: #166534; font-weight: 600;',
    downloading: 'background: #dbeafe; color: #1e40af; font-weight: 600;',
    waiting: 'background: #fef3c7; color: #92400e; font-weight: 600;',
    checking: 'background: #fef9c3; color: #854d0e;',
    queued: 'background: #ede9fe; color: #5b21b6; font-weight: 600;',
    error: 'background: #fee2e2; color: #991b1b; font-weight: 600;',
  };
  const BADGE_BASE = 'display: inline-block; padding: 2px 8px; border-radius: 4px; font-size: 11px; background: #e5e7eb; color: #374151; ';

  const cardNodes = new Map();      // infoHash -> <article>
  const firstSeen = new Map();      // infoHash -> thứ tự xuất hiện (server trả danh sách không theo thứ tự cố định)
  let viewFilter = 'all';
  let viewSort = 'recent';
  let searchText = '';

  function formatEta(torrent) {
    const remaining = Math.max((torrent.length || 0) - (torrent.downloaded || 0), 0);
    const rate = torrent.downloadRate || 0;
    if (!rate || remaining <= 0) return '';
    const sec = Math.round(remaining / rate);
    if (sec >= 86400) return '> 1d';
    const h = Math.floor(sec / 3600);
    const m = Math.floor((sec % 3600) / 60);
    const s = sec % 60;
    return h ? `${h}h ${m}m` : (m ? `${m}m ${s}s` : `${s}s`);
  }

  function matchesFilter(torrent) {
    const kind = normalizeTorrentState(torrent.state).kind;
    if (viewFilter === 'downloading' && !['downloading', 'waiting', 'checking', 'queued'].includes(kind)) return false;
    if (viewFilter === 'seeding' && kind !== 'seeding') return false;
    if (viewFilter === 'paused' && !['paused', 'stopped'].includes(kind)) return false;
    if (searchText && !String(torrent.name || '').toLowerCase().includes(searchText)) return false;
    return true;
  }

  function visibleTorrents() {
    const list = torrentList.filter(matchesFilter);
    if (viewSort === 'name') {
      list.sort((a, b) => String(a.name).localeCompare(String(b.name)));
    } else if (viewSort === 'size') {
      list.sort((a, b) => (b.length || 0) - (a.length || 0));
    } else {
      list.sort((a, b) => (firstSeen.get(b.infoHash) || 0) - (firstSeen.get(a.infoHash) || 0));
    }
    return list;
  }

  // Icon nét đơn (Feather-style), dùng cho các nút trên thẻ torrent. Chuỗi cố định, không chứa dữ liệu người dùng.
  const svgIcon = (body) => `<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${body}</svg>`;
  const ICONS = {
    copy: svgIcon('<rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/>'),
    check: svgIcon('<polyline points="20 6 9 17 4 12"/>'),
    pause: svgIcon('<rect x="6" y="4" width="4" height="16" rx="1"/><rect x="14" y="4" width="4" height="16" rx="1"/>'),
    play: svgIcon('<polygon points="6 4 20 12 6 20 6 4"/>'),
    files: svgIcon('<path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/>'),
    limit: svgIcon('<path d="M12 14l4-4"/><path d="M3.34 19a10 10 0 1 1 17.32 0"/>'),
    remove: svgIcon('<polyline points="3 6 5 6 21 6"/><path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6"/><path d="M10 11v6"/><path d="M14 11v6"/><path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/>'),
  };

  // Dựng khung card 1 lần; các lần poll sau chỉ cập nhật text/width tại chỗ
  // (không innerHTML lại) để không nháy, không mất hover/focus và thanh tiến độ chạy mượt.
  function buildCard(torrent) {
    const card = document.createElement('article');
    card.className = 'torrent-card';
    card.dataset.hash = torrent.infoHash;
    card.style.cssText = 'margin-bottom: 14px; padding: 14px 18px; border: 1px solid var(--border); border-radius: var(--radius); background: var(--bg);';
    card.innerHTML = `
      <div style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 8px; gap: 12px;">
        <div style="display: flex; align-items: center; gap: 10px; min-width: 0;">
          <span data-f="badge"></span>
          <div style="min-width: 0;">
            <h3 data-f="name" style="margin: 0; font-size: 15px; font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;"></h3>
            <span data-f="meta" style="font-size: 12px; color: var(--muted);"></span>
          </div>
        </div>
        <div class="torrent-actions">
          <button type="button" class="torrent-copy-magnet icon-btn" title="Copy magnet link" aria-label="Copy magnet link">${ICONS.copy}</button>
          <button type="button" class="torrent-pause icon-btn" title="Pause" aria-label="Pause">${ICONS.pause}</button>
          <button type="button" class="torrent-files icon-btn" title="Choose files" aria-label="Choose files">${ICONS.files}</button>
          <button type="button" class="torrent-limit icon-btn" title="Speed limit" aria-label="Speed limit">${ICONS.limit}</button>
          <button type="button" class="torrent-remove icon-btn icon-btn-danger" title="Remove" aria-label="Remove">${ICONS.remove}</button>
        </div>
      </div>
      <div role="progressbar" aria-valuemin="0" aria-valuemax="100" data-f="track" style="height: 6px; background: #e5e7eb; border-radius: 3px; overflow: hidden; margin-bottom: 8px;">
        <div data-f="bar" style="width: 0%; height: 100%; background: #2563eb; transition: width 0.4s ease;"></div>
      </div>
      <div data-f="error" hidden style="font-size: 12px; color: #92400e; margin-bottom: 6px;"></div>
      <div data-f="filepanel" hidden style="margin-bottom: 10px; padding: 8px 10px; border: 1px solid var(--border); border-radius: 6px; font-size: 12px;"></div>
      <div style="display: flex; gap: 24px; font-size: 12px; color: var(--muted); flex-wrap: wrap;">
        <div>Down: <strong data-f="dl" style="color: var(--text);"></strong></div>
        <div>Up: <strong data-f="ul" style="color: var(--text);"></strong></div>
        <div>Pieces: <strong data-f="pieces" style="color: var(--text);"></strong></div>
      </div>`;
    return card;
  }

  // Panel chọn file: dựng bằng DOM + textContent (tên file do torrent quyết định, không được chèn như HTML).
  async function toggleFilePanel(card, hash) {
    const panel = card.querySelector('[data-f="filepanel"]');
    if (!panel.hidden) {
      panel.hidden = true;
      return;
    }
    try {
      const res = await fetch(`/api/torrents/files?hash=${encodeURIComponent(hash)}`);
      if (!res.ok) {
        setStatus(`Failed to load files: server responded ${res.status}`, 'error');
        return;
      }
      renderFilePanel(panel, await res.json());
      panel.hidden = false;
    } catch (_) {
      setStatus('Failed to load files: server unreachable.', 'error');
    }
  }

  function renderFilePanel(panel, files) {
    panel.replaceChildren();
    const single = files.length <= 1;
    files.forEach((file) => {
      const row = document.createElement('label');
      row.style.cssText = 'display: flex; align-items: center; gap: 8px; padding: 2px 0;';
      const box = document.createElement('input');
      box.type = 'checkbox';
      box.checked = !!file.wanted;
      box.disabled = single;
      box.dataset.index = String(file.index);
      const name = document.createElement('span');
      name.style.cssText = 'flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;';
      name.textContent = file.path;
      name.title = file.path;
      const info = document.createElement('span');
      info.style.color = 'var(--muted)';
      const pct = file.length > 0 ? Math.min(100, (file.completed_bytes / file.length) * 100) : 100;
      info.textContent = `${formatBytes(file.length)} • ${pct.toFixed(0)}%`;
      row.append(box, name, info);
      panel.appendChild(row);
    });
    if (!single) {
      const apply = document.createElement('button');
      apply.type = 'button';
      apply.className = 'torrent-files-apply btn';
      apply.style.cssText = 'margin-top: 6px; padding: 4px 10px; font-size: 12px;';
      apply.textContent = 'Apply selection';
      panel.appendChild(apply);
    }
  }

  async function applyFileSelection(card, hash) {
    const panel = card.querySelector('[data-f="filepanel"]');
    const skipped = Array.from(panel.querySelectorAll('input[type="checkbox"]'))
      .filter((box) => !box.checked)
      .map((box) => box.dataset.index);
    try {
      const res = await fetch(
        `/api/torrents/files?hash=${encodeURIComponent(hash)}&skip=${encodeURIComponent(skipped.join(','))}`,
        { method: 'POST' });
      if (!res.ok) {
        setStatus(`Failed to update file selection: ${await res.text()}`, 'error');
        return;
      }
      setStatus('File selection updated.', 'info');
      panel.hidden = true;
    } catch (_) {
      setStatus('Failed to update file selection: server unreachable.', 'error');
    }
  }

  function updateCard(card, torrent) {
    const f = (key) => card.querySelector(`[data-f="${key}"]`);
    const stateInfo = normalizeTorrentState(torrent.state);
    const progress = Math.min(Math.max(Number(torrent.progress) || 0, 0), 100);

    const badge = f('badge');
    badge.textContent = stateInfo.label;
    badge.style.cssText = BADGE_BASE + (STATE_BADGE_STYLE[stateInfo.kind] || '');

    const name = f('name');
    name.textContent = torrent.name;
    name.title = torrent.name;

    const eta = stateInfo.kind === 'downloading' ? formatEta(torrent) : '';
    const dhtStr = (torrent.dhtNodes !== undefined && torrent.dhtNodes > 0) ? ` • DHT: ${torrent.dhtNodes}` : '';
    f('meta').textContent =
      `${formatBytes(torrent.length)} • ${progress.toFixed(1)}% • Peers: ${torrent.peers || 0}${dhtStr}` +
      (eta ? ` • ETA ${eta}` : '');

    f('bar').style.width = `${progress}%`;
    f('bar').style.background = stateInfo.kind === 'seeding' ? '#16a34a' : '#2563eb';
    f('track').setAttribute('aria-valuenow', progress.toFixed(0));

    const errorBox = f('error');
    const showError = !!torrent.error && (stateInfo.kind === 'waiting' || stateInfo.kind === 'error');
    errorBox.hidden = !showError;
    errorBox.textContent = showError ? torrent.error : '';

    f('dl').textContent = formatSpeed(torrent.downloadRate || 0);
    f('ul').textContent = formatSpeed(torrent.uploadRate || 0);
    f('pieces').textContent = `${torrent.completedPieces || 0}/${torrent.pieceCount || 0}`;

    const pauseButton = card.querySelector('.torrent-pause');
    const pauseLabel = torrent.queued ? 'Start now' : (torrent.paused ? 'Resume' : 'Pause');
    const pauseIcon = (torrent.queued || torrent.paused) ? 'play' : 'pause';
    // Chỉ dựng lại SVG khi trạng thái đổi, để poll mỗi giây không làm nháy icon.
    if (pauseButton.dataset.icon !== pauseIcon) {
      pauseButton.innerHTML = ICONS[pauseIcon];
      pauseButton.dataset.icon = pauseIcon;
    }
    pauseButton.title = pauseLabel;
    pauseButton.setAttribute('aria-label', `${pauseLabel} ${torrent.name}`);
    const kib = (bytes) => (bytes ? `${Math.round(bytes / 1024)} KiB/s` : 'unlimited');
    const limitButton = card.querySelector('.torrent-limit');
    limitButton.title = `Speed limit — Download: ${kib(torrent.downloadLimit)} • Upload: ${kib(torrent.uploadLimit)}`;
    limitButton.setAttribute('aria-label', `Speed limit for ${torrent.name}`);

    card.querySelector('.torrent-copy-magnet').hidden = !torrent.magnetUri;
    card.querySelector('.torrent-files').setAttribute('aria-label', `Choose files of ${torrent.name}`);
    card.querySelector('.torrent-remove').setAttribute('aria-label', `Remove ${torrent.name}`);
  }


  function getFileCategory(name) {
    const ext = (name.split('.').pop() || '').toLowerCase();
    if (['png', 'jpg', 'jpeg', 'gif', 'webp', 'svg', 'bmp', 'ico'].includes(ext)) return 'image';
    if (['mp4', 'mov', 'webm', 'mkv', 'avi'].includes(ext)) return 'video';
    if (['mp3', 'wav', 'flac', 'm4a', 'ogg'].includes(ext)) return 'audio';
    if (['doc', 'docx'].includes(ext)) return 'word';
    if (['xls', 'xlsx', 'csv'].includes(ext)) return 'excel';
    if (['ppt', 'pptx'].includes(ext)) return 'powerpoint';
    if (['pdf'].includes(ext)) return 'pdf';
    if (['zip', 'rar', '7z', 'tar', 'gz'].includes(ext)) return 'archive';
    if (['txt', 'md', 'c', 'cpp', 'h', 'py', 'js', 'json', 'html', 'css', 'sh'].includes(ext)) return 'code';
    return 'generic';
  }

  function renderFileIconSvg(category, ext) {
    if (category === 'word') {
      return `
        <div class="file-icon-badge" style="color: #185abd;">
          <svg viewBox="0 0 24 24" fill="currentColor">
            <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8l-6-6z" fill="#e8f0fe"/>
            <path d="M14 2v6h6" fill="#c2d7fe"/>
            <rect x="3" y="8" width="9" height="11" rx="1.5" fill="#185abd"/>
            <text x="7.5" y="16.5" font-size="7.5" font-weight="900" fill="#ffffff" text-anchor="middle" font-family="system-ui, sans-serif">W</text>
          </svg>
          <span class="file-type-pill" style="background: #185abd;">WORD</span>
        </div>`;
    }
    if (category === 'excel') {
      return `
        <div class="file-icon-badge" style="color: #107c41;">
          <svg viewBox="0 0 24 24" fill="currentColor">
            <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8l-6-6z" fill="#e6f4ea"/>
            <path d="M14 2v6h6" fill="#ceead6"/>
            <rect x="3" y="8" width="9" height="11" rx="1.5" fill="#107c41"/>
            <text x="7.5" y="16.5" font-size="7.5" font-weight="900" fill="#ffffff" text-anchor="middle" font-family="system-ui, sans-serif">X</text>
          </svg>
          <span class="file-type-pill" style="background: #107c41;">EXCEL</span>
        </div>`;
    }
    if (category === 'powerpoint') {
      return `
        <div class="file-icon-badge" style="color: #c43e1c;">
          <svg viewBox="0 0 24 24" fill="currentColor">
            <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8l-6-6z" fill="#fce8e6"/>
            <path d="M14 2v6h6" fill="#fad2cf"/>
            <rect x="3" y="8" width="9" height="11" rx="1.5" fill="#c43e1c"/>
            <text x="7.5" y="16.5" font-size="7.5" font-weight="900" fill="#ffffff" text-anchor="middle" font-family="system-ui, sans-serif">P</text>
          </svg>
          <span class="file-type-pill" style="background: #c43e1c;">PPT</span>
        </div>`;
    }
    if (category === 'pdf') {
      return `
        <div class="file-icon-badge" style="color: #dc2626;">
          <svg viewBox="0 0 24 24" fill="currentColor">
            <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8l-6-6z" fill="#fee2e2"/>
            <path d="M14 2v6h6" fill="#fca5a5"/>
            <rect x="3" y="9" width="10" height="9" rx="1.5" fill="#dc2626"/>
            <text x="8" y="15.5" font-size="5" font-weight="900" fill="#ffffff" text-anchor="middle" font-family="system-ui, sans-serif">PDF</text>
          </svg>
          <span class="file-type-pill" style="background: #dc2626;">PDF</span>
        </div>`;
    }
    if (category === 'video') {
      return `
        <div class="file-icon-badge" style="color: #7c3aed;">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <rect x="2" y="4" width="20" height="16" rx="2.5" fill="#f5f3ff"/>
            <polygon points="10 8 16 12 10 16 10 8" fill="#7c3aed" stroke="none"/>
          </svg>
          <span class="file-type-pill" style="background: #7c3aed;">VIDEO</span>
        </div>`;
    }
    if (category === 'audio') {
      return `
        <div class="file-icon-badge" style="color: #db2777;">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <rect x="3" y="3" width="18" height="18" rx="3" fill="#fdf2f8"/>
            <path d="M9 18V5l12-2v13" stroke="#db2777"/>
            <circle cx="6" cy="18" r="3" fill="#db2777"/>
            <circle cx="18" cy="16" r="3" fill="#db2777"/>
          </svg>
          <span class="file-type-pill" style="background: #db2777;">AUDIO</span>
        </div>`;
    }
    if (category === 'archive') {
      return `
        <div class="file-icon-badge" style="color: #d97706;">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M21 8V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v3" fill="#fef3c7"/>
            <path d="M21 8v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8"/>
            <rect x="3" y="8" width="18" height="4" rx="1" fill="#fde68a"/>
            <line x1="10" y1="12" x2="14" y2="12"/>
          </svg>
          <span class="file-type-pill" style="background: #d97706;">ZIP</span>
        </div>`;
    }
    if (category === 'code') {
      return `
        <div class="file-icon-badge" style="color: #0f766e;">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" fill="#f0fdfa"/>
            <polyline points="14 2 14 8 20 8"/>
            <polyline points="9 13 7 15 9 17" stroke="#0f766e"/>
            <polyline points="15 13 17 15 15 17" stroke="#0f766e"/>
          </svg>
          <span class="file-type-pill" style="background: #0f766e;">${(ext || 'TXT').toUpperCase()}</span>
        </div>`;
    }
    return `
      <div class="file-icon-badge" style="color: #64748b;">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M13 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z" fill="#f8fafc"/>
          <polyline points="13 2 13 9 20 9"/>
        </svg>
        <span class="file-type-pill" style="background: #64748b;">${(ext || 'FILE').toUpperCase()}</span>
      </div>`;
  }

  let currentLightboxFile = null;

  async function openFileOnComputer(relativePath) {
    if (!relativePath) return;
    const label = relativePath.split('/').pop();
    setStatus(`Opening "${label}" with the default app...`);
    try {
      const res = await fetch(`/api/files/open?path=${encodeURIComponent(relativePath)}`, { method: 'POST' });
      if (res.ok) {
        setStatus(`Opened "${label}" on this computer.`, 'success');
      } else {
        const err = await res.text();
        setStatus(`Could not open it directly: ${err}`, 'error');
        window.open(`/api/files/content?path=${encodeURIComponent(relativePath)}`, '_blank');
      }
    } catch (err) {
      setStatus(`Error opening file: ${err.message}`, 'error');
    }
  }

  async function openDownloadsFolder() {
    setStatus('Đang mở thư mục Downloads...');
    try {
      const res = await fetch('/api/files/open?folder=1', { method: 'POST' });
      if (res.ok) {
        setStatus('Đã mở thư mục Downloads trên máy tính!', 'success');
      } else {
        setStatus('Không thể mở thư mục downloads', 'error');
      }
    } catch (err) {
      setStatus(`Lỗi khi mở thư mục: ${err.message}`, 'error');
    }
  }

  function openLightbox(relativePath, sizeStr, isVideo = false) {
    currentLightboxFile = relativePath;
    const modal = document.getElementById('lightbox-modal');
    const img = document.getElementById('lightbox-img');
    const video = document.getElementById('lightbox-video');
    const title = document.getElementById('lightbox-filename');
    const size = document.getElementById('lightbox-filesize');
    const downloadBtn = document.getElementById('lightbox-btn-download');

    if (!modal) return;

    const filename = relativePath.split('/').pop();
    const fileUrl = `/api/files/content?path=${encodeURIComponent(relativePath)}`;
    title.textContent = filename;
    size.textContent = sizeStr || '';
    downloadBtn.href = fileUrl;
    downloadBtn.download = filename;

    if (isVideo) {
      img.hidden = true;
      video.hidden = false;
      video.src = fileUrl;
      video.play().catch(() => {});
    } else {
      video.hidden = true;
      video.pause();
      img.hidden = false;
      img.src = fileUrl;
    }

    modal.hidden = false;
  }

  function closeLightbox() {
    const modal = document.getElementById('lightbox-modal');
    const video = document.getElementById('lightbox-video');
    if (modal) modal.hidden = true;
    if (video) {
      video.pause();
      video.src = '';
    }
    currentLightboxFile = null;
  }

  // ---- Trang Files: liệt kê mọi file thật trong thư mục tải, nhóm theo ngày, lọc thời gian, sắp xếp ----
  let filesData = [];
  let filesTruncated = false;
  let filesLoaded = false;
  let filesError = '';
  let filesRange = 'all';
  let filesFrom = '';
  let filesTo = '';
  let filesSort = 'newest';
  let filesRenderedSignature = '';

  const pad2 = (n) => String(n).padStart(2, '0');
  const dayKey = (date) => `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`;
  const addDays = (date, days) => {
    const copy = new Date(date.getFullYear(), date.getMonth(), date.getDate());
    copy.setDate(copy.getDate() + days);
    return copy;
  };

  function dayLabel(key) {
    const today = new Date();
    if (key === dayKey(today)) return 'Today';
    if (key === dayKey(addDays(today, -1))) return 'Yesterday';
    const [y, m, d] = key.split('-').map(Number);
    return new Date(y, m - 1, d).toLocaleDateString('en-GB', { weekday: 'short', day: 'numeric', month: 'short', year: 'numeric' });
  }

  async function loadFiles() {
    try {
      const res = await fetch('/api/files/list');
      if (!res.ok) {
        // 404 = server đang chạy là bản cũ, chưa có API liệt kê file: phải khởi động lại client sau khi cập nhật.
        filesError = res.status === 404
          ? 'This server is an older version without the file list. Restart the client and reload the page.'
          : `Could not load the file list (server responded ${res.status}).`;
        renderFilesList();
        return;
      }
      const data = await res.json();
      filesData = Array.isArray(data.files) ? data.files : [];
      filesTruncated = Boolean(data.truncated);
      filesLoaded = true;
      filesError = '';
      renderFilesList();
    } catch (_) {
      filesError = 'Could not reach the server to load the file list.';
      renderFilesList();
    }
  }

  function filteredFiles() {
    let from = '';
    let to = '';
    const today = new Date();
    if (filesRange === 'today') {
      from = to = dayKey(today);
    } else if (filesRange === '7d') {
      from = dayKey(addDays(today, -6));
    } else if (filesRange === '30d') {
      from = dayKey(addDays(today, -29));
    } else if (filesRange === 'custom') {
      from = filesFrom;
      to = filesTo;
      if (from && to && from > to) [from, to] = [to, from];
    }
    return filesData.filter((file) => {
      const key = dayKey(new Date(file.mtime * 1000));
      return (!from || key >= from) && (!to || key <= to);
    });
  }

  function sortFiles(list) {
    const byName = (a, b) => a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' });
    const sorters = {
      newest: (a, b) => b.mtime - a.mtime || byName(a, b),
      oldest: (a, b) => a.mtime - b.mtime || byName(a, b),
      'name-asc': byName,
      'name-desc': (a, b) => byName(b, a),
      'size-desc': (a, b) => b.size - a.size || byName(a, b),
      'size-asc': (a, b) => a.size - b.size || byName(a, b),
    };
    return list.slice().sort(sorters[filesSort] || sorters.newest);
  }

  function fileCardHtml(file, downloading) {
    const category = getFileCategory(file.name);
    const ext = (file.name.split('.').pop() || '').toLowerCase();
    const contentUrl = `/api/files/content?path=${encodeURIComponent(file.path)}`;
    const when = new Date(file.mtime * 1000);
    const folder = file.path.includes('/') ? file.path.slice(0, file.path.lastIndexOf('/')) : '';

    const previewHtml = category === 'image'
      ? `
          <div class="file-card-preview img-thumb-wrap">
            <img src="${contentUrl}" alt="${escapeHtml(file.name)}" loading="lazy" class="file-thumb-img" onerror="this.style.display='none'; this.nextElementSibling.style.opacity='1';">
            <div class="file-zoom-hint">
              <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="8"/><line x1="21" y1="21" x2="16.65" y2="16.65"/><line x1="11" y1="8" x2="11" y2="14"/><line x1="8" y1="11" x2="14" y2="11"/></svg>
              Zoom
            </div>
          </div>`
      : `
          <div class="file-card-preview">
            ${renderFileIconSvg(category, ext)}
          </div>`;

    const hint = category === 'image' ? 'Click to zoom' : (category === 'video' ? 'Click to play' : 'Click to open');
    return `
        <div class="file-card" data-path="${escapeHtml(file.path)}" data-category="${category}" data-filesize="${formatBytes(file.size)}" title="${hint}">
          ${previewHtml}
          <div class="file-card-body">
            <div class="file-card-name" title="${escapeHtml(file.name)}">${escapeHtml(file.name)}</div>
            ${folder ? `<div class="file-card-path" title="${escapeHtml(folder)}">${escapeHtml(folder)}</div>` : ''}
            <div class="file-card-meta">
              <span>${formatBytes(file.size)}</span>
              <span>${pad2(when.getHours())}:${pad2(when.getMinutes())}</span>
              ${downloading ? '<span class="file-chip">Downloading</span>' : ''}
            </div>
          </div>
        </div>`;
  }

  function renderFilesList() {
    const container = document.getElementById('files-list');
    const countEl = document.getElementById('files-count');
    if (!container) return;

    const visible = sortFiles(filteredFiles());
    const downloadingNames = new Set(torrentList.filter((t) => (Number(t.progress) || 0) < 100).map((t) => t.name));
    const isDownloading = (file) => downloadingNames.has(file.path) || downloadingNames.has(file.path.split('/')[0]);

    const totalBytes = visible.reduce((sum, file) => sum + file.size, 0);
    if (countEl) {
      const filtered = visible.length !== filesData.length;
      countEl.textContent = `${filtered ? `${visible.length} of ${filesData.length}` : visible.length} file${filesData.length === 1 ? '' : 's'} · ${formatBytes(totalBytes)}`;
    }

    // Poll torrent gọi hàm này mỗi 2 giây: chỉ dựng lại DOM khi nội dung hiển thị thật sự đổi, tránh nháy ảnh thu nhỏ.
    const signature = JSON.stringify([filesRange, filesFrom, filesTo, filesSort, filesLoaded, filesTruncated, filesError,
      visible.map((f) => [f.path, f.size, f.mtime, isDownloading(f)])]);
    if (signature === filesRenderedSignature) return;
    filesRenderedSignature = signature;

    if (!filesLoaded) {
      container.innerHTML = filesError
        ? `<div class="files-empty files-error" role="alert">${escapeHtml(filesError)}</div>`
        : '<div class="files-empty">Loading files...</div>';
      return;
    }
    if (!visible.length) {
      container.innerHTML = filesData.length
        ? '<div class="files-empty">No files in this period.</div>'
        : '<div class="files-empty">The download folder is empty. Add a .torrent file or a magnet link to get started.</div>';
      return;
    }

    // Sắp theo ngày (mới/cũ nhất): nhóm theo từng ngày. Sắp theo tên/dung lượng: một danh sách chung cho dễ so sánh.
    const groupByDay = filesSort === 'newest' || filesSort === 'oldest';
    const groups = [];
    if (groupByDay) {
      for (const file of visible) {
        const key = dayKey(new Date(file.mtime * 1000));
        const last = groups[groups.length - 1];
        if (last && last.key === key) last.files.push(file);
        else groups.push({ key, title: dayLabel(key), files: [file] });
      }
    } else {
      groups.push({ key: 'all', title: 'All files', files: visible });
    }

    const html = groups.map((group) => {
      const bytes = group.files.reduce((sum, file) => sum + file.size, 0);
      return `
        <section class="files-day">
          <div class="files-day-head">
            <h3 class="files-day-title">${escapeHtml(group.title)}</h3>
            <span class="files-day-meta">${group.files.length} file${group.files.length === 1 ? '' : 's'} · ${formatBytes(bytes)}</span>
          </div>
          <div class="files-explorer-grid">${group.files.map((file) => fileCardHtml(file, isDownloading(file))).join('')}</div>
        </section>`;
    }).join('');
    container.innerHTML = html + (filesTruncated ? '<p class="files-note">Showing the first 10,000 files only.</p>' : '');

    container.querySelectorAll('.file-card').forEach((card) => {
      card.addEventListener('click', () => {
        const relativePath = card.getAttribute('data-path');
        const category = card.getAttribute('data-category');
        const filesize = card.getAttribute('data-filesize');
        if (category === 'image') {
          openLightbox(relativePath, filesize, false);
        } else if (category === 'video') {
          openLightbox(relativePath, filesize, true);
        } else {
          openFileOnComputer(relativePath);
        }
      });
    });
  }

  function bindFilesControls() {
    const rangeButtons = document.querySelectorAll('#view-files .seg-btn');
    const customRow = document.getElementById('files-custom-range');
    const fromInput = document.getElementById('files-from');
    const toInput = document.getElementById('files-to');

    for (const button of rangeButtons) {
      button.addEventListener('click', () => {
        filesRange = button.dataset.range;
        for (const other of rangeButtons) other.setAttribute('aria-pressed', String(other === button));
        if (customRow) customRow.hidden = filesRange !== 'custom';
        renderFilesList();
      });
    }
    const onDateChange = () => {
      filesFrom = fromInput.value;
      filesTo = toInput.value;
      renderFilesList();
    };
    if (fromInput) fromInput.addEventListener('change', onDateChange);
    if (toInput) toInput.addEventListener('change', onDateChange);
    const clearButton = document.getElementById('files-range-clear');
    if (clearButton) {
      clearButton.addEventListener('click', () => {
        fromInput.value = '';
        toInput.value = '';
        onDateChange();
      });
    }
    const sortSelect = document.getElementById('files-sort');
    if (sortSelect) {
      sortSelect.addEventListener('change', () => {
        filesSort = sortSelect.value;
        renderFilesList();
      });
    }
    const refreshButton = document.getElementById('files-refresh-btn');
    if (refreshButton) refreshButton.addEventListener('click', loadFiles);

    // Danh sách trên đĩa có thể đổi bất cứ lúc nào (tải xong, xoá tay): làm tươi mỗi 10 giây khi đang xem trang này.
    setInterval(() => {
      const view = document.getElementById('view-files');
      if (view && !view.hidden && !document.hidden) loadFiles();
    }, 10000);
  }

  function renderTorrentList() {
    const container = document.getElementById('torrent-list');
    if (!container) return;

    const list = visibleTorrents();
    const keep = new Set(list.map((t) => t.infoHash));

    for (const [hash, node] of cardNodes) {
      if (!keep.has(hash)) {
        node.remove();
        cardNodes.delete(hash);
      }
    }

    let empty = container.querySelector('.torrent-empty');
    if (!list.length) {
      if (!empty) {
        empty = document.createElement('div');
        empty.className = 'torrent-empty';
        empty.style.cssText = 'padding: 24px; text-align: center; color: var(--muted); border: 1px dashed var(--border); border-radius: var(--radius);';
        container.appendChild(empty);
      }
      empty.textContent = torrentList.length ? 'No torrents match the current filter.' : 'No torrent files added yet.';
      return;
    }
    if (empty) empty.remove();

    list.forEach((torrent, index) => {
      let card = cardNodes.get(torrent.infoHash);
      if (!card) {
        card = buildCard(torrent);
        cardNodes.set(torrent.infoHash, card);
      }
      updateCard(card, torrent);
      if (container.children[index] !== card) {
        container.insertBefore(card, container.children[index] || null);
      }
    });
  }

  function bindTorrentListActions() {
    const container = document.getElementById('torrent-list');
    if (!container) return;

    container.addEventListener('click', async (event) => {
      const button = event.target.closest('button');
      const card = event.target.closest('.torrent-card');
      if (!button || !card) return;
      const hash = card.dataset.hash;
      const torrent = torrentList.find((item) => item.infoHash === hash);

      if (button.classList.contains('torrent-pause') && torrent) {
        const action = (torrent.paused || torrent.queued) ? 'resume' : 'pause';  // torrent đang xếp hàng: bấm = chạy ngay
        try {
          const res = await fetch(`/api/torrents/${action}?hash=${encodeURIComponent(hash)}`, { method: 'POST' });
          if (!res.ok) {
            setStatus(`Failed to ${action} torrent: server responded ${res.status}`, 'error');
            return;
          }
          torrent.paused = action === 'pause';
          torrent.queued = false;
          setStatus(torrent.paused ? 'Torrent paused.' : 'Torrent resumed.', 'info');
        } catch (_) {
          setStatus(`Failed to ${action} torrent: server unreachable.`, 'error');
        }
      } else if (button.classList.contains('torrent-files') && torrent) {
        await toggleFilePanel(card, hash);
      } else if (button.classList.contains('torrent-files-apply')) {
        await applyFileSelection(card, hash);
      } else if (button.classList.contains('torrent-limit') && torrent) {
        const toKib = (bytes) => (bytes ? Math.round(bytes / 1024) : 0);
        const down = window.prompt('Download limit in KiB/s (0 = unlimited):', String(toKib(torrent.downloadLimit)));
        if (down === null) return;
        const up = window.prompt('Upload limit in KiB/s (0 = unlimited):', String(toKib(torrent.uploadLimit)));
        if (up === null) return;
        try {
          const res = await fetch(
            `/api/torrents/limit?hash=${encodeURIComponent(hash)}&down=${encodeURIComponent(down.trim())}&up=${encodeURIComponent(up.trim())}`,
            { method: 'POST' });
          if (!res.ok) {
            setStatus(`Failed to set limits: ${await res.text()}`, 'error');
            return;
          }
          torrent.downloadLimit = Number(down) * 1024;
          torrent.uploadLimit = Number(up) * 1024;
          setStatus('Speed limits updated.', 'info');
        } catch (_) {
          setStatus('Failed to set limits: server unreachable.', 'error');
        }
      } else if (button.classList.contains('torrent-remove')) {
        try {
          const res = await fetch(`/api/torrents/remove?hash=${encodeURIComponent(hash)}`, { method: 'POST' });
          if (!res.ok && res.status !== 404) {
            setStatus(`Failed to remove torrent: server responded ${res.status}`, 'error');
            return;
          }
        } catch (_) {
          setStatus('Failed to remove torrent: server unreachable.', 'error');
          return;
        }
        const index = torrentList.findIndex((item) => item.infoHash === hash);
        if (index !== -1) torrentList.splice(index, 1);
        renderTorrentList();
        updateDashboardTiles();
        renderFilesList();
        setStatus('Torrent removed.', 'info');
      } else if (button.classList.contains('torrent-copy-magnet') && torrent && torrent.magnetUri) {
        try {
          await navigator.clipboard.writeText(torrent.magnetUri);
        } catch (_) {
          setStatus('Clipboard access denied.', 'error');
          return;
        }
        button.innerHTML = ICONS.check;
        button.title = 'Copied!';
        button.classList.add('icon-btn-ok');
        setTimeout(() => {
          button.innerHTML = ICONS.copy;
          button.title = 'Copy magnet link';
          button.classList.remove('icon-btn-ok');
        }, 1500);
      }
    });
  }

  function bindListControls() {
    const filterButtons = document.querySelectorAll('#view-torrents .seg-btn');
    for (const button of filterButtons) {
      button.addEventListener('click', () => {
        viewFilter = button.dataset.filter;
        for (const other of filterButtons) {
          other.setAttribute('aria-pressed', String(other === button));
        }
        renderTorrentList();
      });
    }
    const sortSelect = document.getElementById('sort-select');
    if (sortSelect) {
      sortSelect.addEventListener('change', () => {
        viewSort = sortSelect.value;
        renderTorrentList();
      });
    }
    const searchForm = document.querySelector('.topbar .search');
    const searchInput = searchForm && searchForm.querySelector('input');
    if (searchForm) {
      // Enter trong form không có action sẽ tải lại cả trang.
      searchForm.addEventListener('submit', (event) => event.preventDefault());
    }
    if (searchInput) {
      searchInput.addEventListener('input', () => {
        if (location.hash !== '#/torrents') location.hash = '#/torrents';
        searchText = searchInput.value.trim().toLowerCase();
        renderTorrentList();
      });
    }
  }

  let nextSeenOrder = 1;

  function setConnectionLost(lost) {
    const banner = document.getElementById('conn-banner');
    if (banner) banner.hidden = !lost;
  }

  async function syncTorrentsFromServer() {
    try {
      const res = await fetch('/api/torrents');
      if (!res.ok) {
        setConnectionLost(true);
        return;
      }
      setConnectionLost(false);
      const data = await res.json();
      if (Array.isArray(data)) {
        torrentList.length = 0;
        data.forEach(item => {
          if (!firstSeen.has(item.info_hash)) firstSeen.set(item.info_hash, nextSeenOrder++);
          torrentList.push({
            error: item.error || '',
            id: item.info_hash,
            name: item.name,
            rawName: item.name,
            infoHash: item.info_hash,
            magnetUri: item.magnet_uri,
            state: item.state,
            length: item.total_bytes,
            downloaded: item.downloaded_bytes,
            uploaded: item.uploaded_bytes,
            progress: item.progress_percent,
            downloadRate: item.download_rate,
            paused: !!item.paused,
            queued: !!item.queued,
            downloadLimit: item.download_limit || 0,
            uploadLimit: item.upload_limit || 0,
            pickerStrategy: item.picker_strategy || 'rarest_first',
            uploadRate: item.upload_rate,
            pieceCount: item.total_pieces,
            completedPieces: item.completed_pieces,
            peers: item.connected_peers,
            dhtNodes: item.dht_nodes,
          });
        });
        renderTorrentList();
        updateDashboardTiles();
        renderFilesList();
      }
    } catch (_) {
      setConnectionLost(true);
    }
  }

  async function requestTorrentFromServer(file) {
    const announceInput = document.getElementById('tracker-announce-input');
    const announceUrl = (announceInput && announceInput.value.trim()) ? announceInput.value.trim() : currentAnnounceUrl;

    const formData = new FormData();
    formData.append('file', file, file.name);
    formData.append('announce', announceUrl);
    formData.append('piece_length', String(PIECE_LENGTH));

    const dhtInput = document.getElementById('dht-bootstrap-input');
    if (dhtInput && dhtInput.value.trim()) {
      const parts = dhtInput.value.trim().split(':');
      formData.append('dht_bootstrap_host', parts[0].trim());
      if (parts[1]) formData.append('dht_bootstrap_port', parts[1].trim());
    }

    const response = await fetch('/api/torrents/create', {
      method: 'POST',
      body: formData,
    });

    if (!response.ok) {
      const message = await response.text().catch(() => '');
      throw new Error(message || `Server responded with ${response.status}`);
    }

    const disposition = response.headers.get('Content-Disposition') || '';
    const utf8Match = disposition.match(/filename\*=UTF-8''([^;]+)/i);
    const asciiMatch = disposition.match(/filename="?([^";]+)"?/i);
    let downloadName = `${file.name}.torrent`;
    try {
      if (utf8Match) downloadName = decodeURIComponent(utf8Match[1]);
      else if (asciiMatch) downloadName = asciiMatch[1];
    } catch (_) {}
    const magnetUri = response.headers.get('X-Magnet-Uri') || '';
    const blob = await response.blob();
    return { blob, downloadName, magnetUri };
  }

  async function handleCreateTorrent(file) {
    if (!file) return;

    try {
      setStatus(`Creating torrent for ${file.name}...`);
      const { blob, downloadName, magnetUri } = await requestTorrentFromServer(file);

      // Download .torrent file
      const url = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.download = downloadName;
      document.body.appendChild(link);
      link.click();
      link.remove();
      URL.revokeObjectURL(url);

      setStatus(`Torrent created successfully and added to the swarm: ${downloadName}`, 'success');
      await syncTorrentsFromServer();
    } catch (error) {
      setStatus(`Failed to create torrent: ${error.message}`, 'error');
    }
  }

  async function handleAddTorrent(file) {
    if (!file) return;

    const isTorrentFile = file.name.toLowerCase().endsWith('.torrent');
    if (!isTorrentFile) {
      setStatus('Please choose a .torrent file to add.', 'error');
      return;
    }

    try {
      setStatus(`Uploading and starting ${file.name}...`);
      const formData = new FormData();
      formData.append('file', file, file.name);

      const dhtInput = document.getElementById('dht-bootstrap-input');
      if (dhtInput && dhtInput.value.trim()) {
        const parts = dhtInput.value.trim().split(':');
        formData.append('dht_bootstrap_host', parts[0].trim());
        if (parts[1]) formData.append('dht_bootstrap_port', parts[1].trim());
      }
      const res = await fetch('/api/torrents/add', {
        method: 'POST',
        body: formData,
      });

      if (!res.ok) {
        const err = await res.text();
        throw new Error(err || 'Failed to add torrent');
      }

      setStatus(`Added ${file.name} to the swarm. Status will update as peers respond.`, 'success');
      await syncTorrentsFromServer();
    } catch (error) {
      setStatus(`Failed to add torrent: ${error.message}`, 'error');
    }
  }

  async function handleAddMagnet(uri) {
    const magnetUri = (uri || '').trim();
    if (!magnetUri) {
      setStatus('Paste a magnet link first.', 'error');
      return;
    }

    try {
      setStatus('Finding peers and retrieving magnet metadata...');
      const formData = new FormData();
      formData.append('magnet_uri', magnetUri);
      const response = await fetch('/api/torrents/add-magnet', {
        method: 'POST',
        body: formData,
      });
      if (!response.ok) {
        throw new Error(await response.text() || 'Failed to resolve magnet');
      }
      setStatus('Magnet resolved and added to the swarm.', 'success');
      const input = document.getElementById('magnet-add-input');
      if (input) input.value = '';
      await syncTorrentsFromServer();
    } catch (error) {
      setStatus(`Failed to add magnet: ${error.message}`, 'error');
    }
  }

  function openFilePicker(mode) {
    activeAction = mode;
    const input = document.getElementById('torrent-source');
    if (!input) return;
    input.accept = mode === 'add' ? '.torrent,application/x-bittorrent' : '*/*';
    input.click();
  }

  // Popup Settings (Connect / Limit). Công tắc Speed limit và Queue áp dụng ngay khi bật/tắt.
  function bindSettingsDialog() {
    const dialog = document.getElementById('settings-dialog');
    const openButton = document.getElementById('settings-toggle-btn');
    if (!dialog || !openButton || typeof dialog.showModal !== 'function') return;

    const speedToggle = document.getElementById('speed-limit-toggle');
    const speedFields = document.getElementById('speed-limit-fields');
    const speedDown = document.getElementById('speed-down-input');
    const speedUp = document.getElementById('speed-up-input');
    const queueToggle = document.getElementById('queue-toggle');
    const queueFields = document.getElementById('queue-fields');
    const queueMax = document.getElementById('queue-max-input');
    const DEFAULT_SPEED_KIB = 1024;
    const DEFAULT_QUEUE = 3;

    const showPane = (name) => {
      for (const tab of dialog.querySelectorAll('.settings-tab')) {
        tab.setAttribute('aria-selected', String(tab.dataset.pane === name));
      }
      for (const pane of dialog.querySelectorAll('.settings-pane')) {
        pane.hidden = pane.id !== `pane-${name}`;
      }
    };
    for (const tab of dialog.querySelectorAll('.settings-tab')) {
      tab.addEventListener('click', () => showPane(tab.dataset.pane));
    }

    const refreshLimits = async () => {
      try {
        const [bandwidthRes, queueRes] = await Promise.all([
          fetch('/api/settings/bandwidth'), fetch('/api/settings/queue')]);
        if (bandwidthRes.ok) {
          const data = await bandwidthRes.json();
          const on = Boolean(data.down || data.up);
          speedToggle.checked = on;
          speedFields.hidden = !on;
          if (on) {
            speedDown.value = String(data.down || 0);
            speedUp.value = String(data.up || 0);
          }
        }
        if (queueRes.ok) {
          const data = await queueRes.json();
          const on = Boolean(data.max_active);
          queueToggle.checked = on;
          queueFields.hidden = !on;
          if (on) queueMax.value = String(data.max_active);
        }
      } catch (_) { /* server chưa sẵn sàng: giữ nguyên trạng thái hiện có */ }
    };

    const saveSpeed = async (down, up) => {
      if (!/^\d{1,7}$/.test(down) || !/^\d{1,7}$/.test(up)) {
        setStatus('Limits must be whole numbers of KiB/s.', 'error');
        return false;
      }
      try {
        const res = await fetch(`/api/settings/bandwidth?down=${down}&up=${up}`, { method: 'POST' });
        setStatus(res.ok ? (down === '0' && up === '0' ? 'Speed limit turned off.' : 'Speed limit saved.')
          : `Could not save speed limit: server responded ${res.status}`, res.ok ? 'success' : 'error');
        return res.ok;
      } catch (_) {
        setStatus('Could not save speed limit: server unreachable.', 'error');
        return false;
      }
    };

    const saveQueue = async (value) => {
      if (!/^\d{1,2}$/.test(value) || Number(value) > 64) {
        setStatus('Queue size must be a whole number from 1 to 64.', 'error');
        return false;
      }
      try {
        const res = await fetch(`/api/settings/queue?max_active=${value}`, { method: 'POST' });
        setStatus(res.ok ? (value === '0' ? 'Queue turned off.' : 'Queue setting saved.')
          : `Could not save queue setting: server responded ${res.status}`, res.ok ? 'success' : 'error');
        return res.ok;
      } catch (_) {
        setStatus('Could not save queue setting: server unreachable.', 'error');
        return false;
      }
    };

    speedToggle.addEventListener('change', async () => {
      if (speedToggle.checked) {
        if (!Number(speedDown.value) && !Number(speedUp.value)) {
          speedDown.value = String(DEFAULT_SPEED_KIB);
          speedUp.value = String(DEFAULT_SPEED_KIB);
        }
        speedFields.hidden = false;
        if (!(await saveSpeed(speedDown.value.trim(), speedUp.value.trim()))) await refreshLimits();
      } else {
        speedFields.hidden = true;
        if (!(await saveSpeed('0', '0'))) await refreshLimits();
      }
    });
    document.getElementById('speed-apply-btn').addEventListener('click', async () => {
      const down = speedDown.value.trim() || '0';
      const up = speedUp.value.trim() || '0';
      if (down === '0' && up === '0') {
        setStatus('Enter a limit above 0, or switch the speed limit off.', 'error');
        return;
      }
      if (!(await saveSpeed(down, up))) await refreshLimits();
    });

    queueToggle.addEventListener('change', async () => {
      if (queueToggle.checked) {
        if (!Number(queueMax.value)) queueMax.value = String(DEFAULT_QUEUE);
        queueFields.hidden = false;
        if (!(await saveQueue(queueMax.value.trim()))) await refreshLimits();
      } else {
        queueFields.hidden = true;
        if (!(await saveQueue('0'))) await refreshLimits();
      }
    });
    document.getElementById('queue-apply-btn').addEventListener('click', async () => {
      if (!(await saveQueue(queueMax.value.trim() || '0')) ) await refreshLimits();
    });

    // ---- Strategy Selector ----
    const strategySelect   = document.getElementById('strategy-torrent-select');
    const strategyStatus   = document.getElementById('strategy-status');
    const strategyApplyBtn = document.getElementById('strategy-apply-btn');

    // Điền danh sách torrent vào dropdown khi mở pane Strategy
    function populateStrategyTorrents() {
      if (!strategySelect) return;
      // Giữ lại giá trị đang chọn nếu còn hợp lệ
      const prev = strategySelect.value;
      strategySelect.innerHTML = '<option value="">— Select torrent —</option>' +
        '<option value="all">All torrents (and new ones)</option>';
      for (const t of torrentList) {
        const opt = document.createElement('option');
        opt.value = t.infoHash;
        opt.textContent = t.name || t.infoHash.slice(0, 12) + '…';
        strategySelect.appendChild(opt);
      }
      if (prev && strategySelect.querySelector(`option[value="${prev}"]`)) {
        strategySelect.value = prev;
      } else if (torrentList.length > 0) {
        // Tự động chọn torrent đầu tiên nếu chưa chọn
        strategySelect.value = torrentList[0].infoHash;
        const current = torrentList[0].pickerStrategy || 'rarest_first';
        const radio = document.querySelector(`input[name="picker-strategy"][value="${current}"]`);
        if (radio) radio.checked = true;
      }
    }

    // Khi chọn torrent: tải chiến lược hiện tại và cập nhật radio
    if (strategySelect) {
      strategySelect.addEventListener('change', async () => {
        const hash = strategySelect.value;
        if (!hash) return;
        try {
          const res = await fetch(`/api/settings/strategy?hash=${encodeURIComponent(hash)}`);
          if (!res.ok) return;
          const data = await res.json();
          const current = data.strategy || 'rarest_first';
          const radio = document.querySelector(`input[name="picker-strategy"][value="${current}"]`);
          if (radio) radio.checked = true;
        } catch (_) { /* bỏ qua lỗi mạng */ }
      });
    }

    // Áp dụng chiến lược
    if (strategyApplyBtn) {
      strategyApplyBtn.addEventListener('click', async () => {
        const hash = strategySelect ? strategySelect.value : '';
        if (!hash) {
          if (strategyStatus) {
            strategyStatus.textContent = 'Please select a torrent first.';
            strategyStatus.className = 'strategy-status err';
          }
          return;
        }
        const radio = document.querySelector('input[name="picker-strategy"]:checked');
        const strategy = radio ? radio.value : 'rarest_first';
        const label = strategy === 'sequential' ? 'Sequential' : 'Rarest-First';
        try {
          const body = new URLSearchParams({ hash, strategy });
          const res = await fetch('/api/settings/strategy', { method: 'POST', body });
          if (res.ok) {
            let message = '✓ Switched to ' + label;
            if (hash === 'all') {
              for (const item of torrentList) item.pickerStrategy = strategy;
              let applied = torrentList.length;
              try { applied = (await res.json()).applied ?? applied; } catch (_) { /* giữ số đếm cục bộ */ }
              message = `✓ ${label} set for ${applied} torrent(s) and all new ones`;
            } else {
              const found = torrentList.find(item => item.infoHash === hash);
              if (found) found.pickerStrategy = strategy;
            }
            if (strategyStatus) {
              strategyStatus.textContent = message;
              strategyStatus.className = 'strategy-status ok';
            }
          } else {
            const text = await res.text();
            if (strategyStatus) {
              strategyStatus.textContent = 'Error: ' + text;
              strategyStatus.className = 'strategy-status err';
            }
          }
        } catch (e) {
          if (strategyStatus) {
            strategyStatus.textContent = 'Connection error.';
            strategyStatus.className = 'strategy-status err';
          }
        }
        // Xóa thông báo sau 4 giây
        setTimeout(() => {
          if (strategyStatus) { strategyStatus.textContent = ''; strategyStatus.className = 'strategy-status'; }
        }, 4000);
      });
    }

    // Cập nhật danh sách torrent khi mở tab strategy
    for (const tab of dialog.querySelectorAll('.settings-tab')) {
      if (tab.dataset.pane === 'strategy') {
        tab.addEventListener('click', populateStrategyTorrents);
      }
    }

    openButton.addEventListener('click', () => {
      showPane('connect');
      dialog.showModal();
      refreshLimits();
    });
    document.getElementById('settings-close-btn').addEventListener('click', () => dialog.close());
    // Bấm ra vùng nền mờ (chính phần tử <dialog>, ngoài hộp nội dung) thì đóng.
    dialog.addEventListener('click', (event) => {
      if (event.target === dialog) {
        const box = dialog.getBoundingClientRect();
        const inside = event.clientX >= box.left && event.clientX <= box.right
          && event.clientY >= box.top && event.clientY <= box.bottom;
        if (!inside) dialog.close();
      }
    });
  }

  function bindFilePicker() {
    bindSettingsDialog();
    const input = document.getElementById('torrent-source');
    const createTorrentButton = document.getElementById('create-torrent-btn');
    const addTorrentButton = document.getElementById('add-torrent-btn');
    const addMagnetButton = document.getElementById('add-magnet-btn');
    const magnetAddInput = document.getElementById('magnet-add-input');
    const dropzone = document.getElementById('torrent-dropzone');
    const dashboardDropzone = document.getElementById('dashboard-dropzone');
    const saveDhtBtn = document.getElementById('save-dht-btn');
    if (saveDhtBtn) {
      saveDhtBtn.addEventListener('click', handleSaveDhtBootstrap);
    }

    if (createTorrentButton) {
      createTorrentButton.addEventListener('click', () => openFilePicker('create'));
    }

    if (addTorrentButton) {
      addTorrentButton.addEventListener('click', () => openFilePicker('add'));
    }

    if (addMagnetButton) {
      addMagnetButton.addEventListener('click', () => {
        handleAddMagnet(magnetAddInput ? magnetAddInput.value : '');
      });
    }

    if (input) {
      input.addEventListener('change', async (event) => {
        const file = event.target.files && event.target.files[0];
        if (activeAction === 'add') {
          await handleAddTorrent(file);
        } else {
          await handleCreateTorrent(file);
        }
        input.value = '';
      });
    }

    const bindDropzone = (zone, mode) => {
      if (!zone) return;
      zone.addEventListener('click', () => openFilePicker(mode));
      zone.addEventListener('dragover', (event) => {
        event.preventDefault();
        zone.classList.add('is-active');
      });
      zone.addEventListener('dragleave', () => {
        zone.classList.remove('is-active');
      });
      zone.addEventListener('drop', async (event) => {
        event.preventDefault();
        zone.classList.remove('is-active');
        const file = event.dataTransfer && event.dataTransfer.files && event.dataTransfer.files[0];
        // Kéo thả file .torrent vào vùng "create" là để thêm torrent, không phải tạo torrent từ chính nó.
        const isTorrent = !!file && file.name.toLowerCase().endsWith('.torrent');
        if (mode === 'add' || isTorrent) {
          await handleAddTorrent(file);
        } else {
          await handleCreateTorrent(file);
        }
      });
    };

    bindDropzone(dropzone, 'create');
    bindDropzone(dashboardDropzone, 'create');
  }

  function bindFilesViewEvents() {
    const openFolderBtn = document.getElementById('open-downloads-folder-btn');
    if (openFolderBtn) {
      openFolderBtn.addEventListener('click', openDownloadsFolder);
    }

    const closeBtn = document.getElementById('lightbox-close');
    if (closeBtn) {
      closeBtn.addEventListener('click', closeLightbox);
    }

    const modal = document.getElementById('lightbox-modal');
    if (modal) {
      const backdrop = modal.querySelector('.lightbox-backdrop');
      if (backdrop) {
        backdrop.addEventListener('click', closeLightbox);
      }
    }

    const btnOpenApp = document.getElementById('lightbox-btn-open-app');
    if (btnOpenApp) {
      btnOpenApp.addEventListener('click', () => {
        if (currentLightboxFile) {
          openFileOnComputer(currentLightboxFile);
        }
      });
    }

    window.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') {
        closeLightbox();
      }
    });
  }

  // Initial render & polling
  fetchSystemInfo();
  renderTorrentList();
  window.addEventListener('hashchange', showView);
  showView();
  bindFilePicker();
  bindTorrentListActions();
  bindListControls();
  bindFilesControls();
  bindFilesViewEvents();
  syncTorrentsFromServer();
  setInterval(syncTorrentsFromServer, 2000);
})();
