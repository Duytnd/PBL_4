'use strict';

(() => {
  const VIEWS = ['dashboard', 'torrents', 'speed'];
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

    // Update speed view tiles
    const speedView = document.getElementById('view-speed');
    if (speedView) {
      const values = speedView.querySelectorAll('.tile-value');
      if (values.length >= 2) {
        values[0].textContent = formatSpeed(totalDlSpeed);
        values[1].textContent = formatSpeed(totalUlSpeed);
      }
    }
  }

  const STATE_BADGE_STYLE = {
    seeding: 'background: #dcfce7; color: #166534; font-weight: 600;',
    downloading: 'background: #dbeafe; color: #1e40af; font-weight: 600;',
    waiting: 'background: #fef3c7; color: #92400e; font-weight: 600;',
    checking: 'background: #fef9c3; color: #854d0e;',
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
    if (viewFilter === 'downloading' && !['downloading', 'waiting', 'checking'].includes(kind)) return false;
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
        <div style="display: flex; align-items: center; flex-shrink: 0;">
          <button type="button" class="torrent-copy-magnet btn" style="padding: 4px 8px; font-size: 12px; margin-right: 6px;">Copy Magnet</button>
          <button type="button" class="torrent-remove btn" style="padding: 4px 8px; font-size: 12px; color: #dc2626;">Remove</button>
        </div>
      </div>
      <div role="progressbar" aria-valuemin="0" aria-valuemax="100" data-f="track" style="height: 6px; background: #e5e7eb; border-radius: 3px; overflow: hidden; margin-bottom: 8px;">
        <div data-f="bar" style="width: 0%; height: 100%; background: #2563eb; transition: width 0.4s ease;"></div>
      </div>
      <div data-f="error" hidden style="font-size: 12px; color: #92400e; margin-bottom: 6px;"></div>
      <div style="display: flex; gap: 24px; font-size: 12px; color: var(--muted); flex-wrap: wrap;">
        <div>Down: <strong data-f="dl" style="color: var(--text);"></strong></div>
        <div>Up: <strong data-f="ul" style="color: var(--text);"></strong></div>
        <div>Pieces: <strong data-f="pieces" style="color: var(--text);"></strong></div>
      </div>`;
    return card;
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
    f('meta').textContent =
      `${formatBytes(torrent.length)} • ${progress.toFixed(1)}% • Peers: ${torrent.peers || 0}` +
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

    card.querySelector('.torrent-copy-magnet').hidden = !torrent.magnetUri;
    card.querySelector('.torrent-remove').setAttribute('aria-label', `Remove ${torrent.name}`);
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

      if (button.classList.contains('torrent-remove')) {
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
        setStatus('Torrent removed.', 'info');
      } else if (button.classList.contains('torrent-copy-magnet') && torrent && torrent.magnetUri) {
        try {
          await navigator.clipboard.writeText(torrent.magnetUri);
        } catch (_) {
          setStatus('Clipboard access denied.', 'error');
          return;
        }
        const orig = button.textContent;
        button.textContent = 'Copied!';
        setTimeout(() => { button.textContent = orig === 'Copied!' ? 'Copy Magnet' : orig; }, 1500);
      }
    });
  }

  function bindListControls() {
    const [showSelect, sortSelect] = document.querySelectorAll('#view-torrents .toolbar select');
    if (showSelect) {
      showSelect.addEventListener('change', () => {
        viewFilter = showSelect.value.toLowerCase();
        renderTorrentList();
      });
    }
    if (sortSelect) {
      sortSelect.addEventListener('change', () => {
        const v = sortSelect.value.toLowerCase();
        viewSort = v.startsWith('most') ? 'recent' : v;
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
            uploadRate: item.upload_rate,
            pieceCount: item.total_pieces,
            completedPieces: item.completed_pieces,
            peers: item.connected_peers,
          });
        });
        renderTorrentList();
        updateDashboardTiles();
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

      // Display magnet link in UI box
      if (magnetUri) {
        const magnetBox = document.getElementById('magnet-box');
        const magnetInput = document.getElementById('magnet-input');
        if (magnetBox && magnetInput) {
          magnetInput.value = magnetUri;
          magnetBox.style.display = 'block';
        }
      }

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

  function openFilePicker(mode) {
    activeAction = mode;
    const input = document.getElementById('torrent-source');
    if (!input) return;
    input.accept = mode === 'add' ? '.torrent,application/x-bittorrent' : '*/*';
    input.click();
  }

  function bindFilePicker() {
    const input = document.getElementById('torrent-source');
    const createTorrentButton = document.getElementById('create-torrent-btn');
    const addTorrentButton = document.getElementById('add-torrent-btn');
    const dropzone = document.getElementById('torrent-dropzone');
    const dashboardDropzone = document.getElementById('dashboard-dropzone');
    const copyMagnetBtn = document.getElementById('copy-magnet-btn');

    if (copyMagnetBtn) {
      copyMagnetBtn.addEventListener('click', async () => {
        const magnetInput = document.getElementById('magnet-input');
        if (magnetInput && magnetInput.value) {
          try {
            await navigator.clipboard.writeText(magnetInput.value);
          } catch (_) {
            setStatus('Clipboard access denied.', 'error');
            return;
          }
          copyMagnetBtn.textContent = 'Copied!';
          setTimeout(() => { copyMagnetBtn.textContent = 'Copy Magnet'; }, 2000);
        }
      });
    }

    if (createTorrentButton) {
      createTorrentButton.addEventListener('click', () => openFilePicker('create'));
    }

    if (addTorrentButton) {
      addTorrentButton.addEventListener('click', () => openFilePicker('add'));
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

  // Initial render & polling
  fetchSystemInfo();
  renderTorrentList();
  window.addEventListener('hashchange', showView);
  showView();
  bindFilePicker();
  bindTorrentListActions();
  bindListControls();
  syncTorrentsFromServer();
  setInterval(syncTorrentsFromServer, 2000);
})();
