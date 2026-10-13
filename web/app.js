'use strict';

(() => {
  const VIEWS = ['dashboard', 'torrents', 'speed'];
  const DEFAULT_ANNOUNCE = 'http://localhost:6969/announce';
  const PIECE_LENGTH = 262144;
  const torrentList = [];
  let activeAction = 'create';

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

  function renderTorrentList() {
    const container = document.getElementById('torrent-list');
    if (!container) return;

    if (!torrentList.length) {
      container.innerHTML = '<div class="torrent-empty" style="padding: 24px; text-align: center; color: var(--muted); border: 1px dashed var(--border); border-radius: var(--radius);">No torrent files added yet.</div>';
      return;
    }

    const cards = torrentList.map((torrent) => {
      const size = formatBytes(torrent.length);
      const state = (torrent.state || 'stopped').toUpperCase();
      const progress = Number(torrent.progress || 0).toFixed(1);
      const dlSpeed = formatSpeed(torrent.downloadRate || 0);
      const ulSpeed = formatSpeed(torrent.uploadRate || 0);
      const peers = torrent.peers || 0;

      let stateBadgeStyle = 'background: #e5e7eb; color: #374151;';
      if (state === 'SEEDING') stateBadgeStyle = 'background: #dcfce7; color: #166534; font-weight: 600;';
      else if (state === 'DOWNLOADING') stateBadgeStyle = 'background: #dbeafe; color: #1e40af; font-weight: 600;';
      else if (state === 'CHECKING') stateBadgeStyle = 'background: #fef9c3; color: #854d0e;';

      const copyMagnetHtml = torrent.magnetUri
        ? `<button class="torrent-copy-magnet btn" data-magnet="${escapeHtml(torrent.magnetUri)}" style="padding: 4px 8px; font-size: 12px; margin-right: 6px;">Copy Magnet</button>`
        : '';

      return `
        <article class="torrent-card" style="margin-bottom: 14px; padding: 14px 18px; border: 1px solid var(--border); border-radius: var(--radius); background: var(--bg);">
          <div class="torrent-card-top" style="display: flex; align-items: center; justify-content: space-between; margin-bottom: 8px;">
            <div class="torrent-file-wrap" style="display: flex; align-items: center; gap: 10px;">
              <span style="display: inline-block; padding: 2px 8px; border-radius: 4px; font-size: 11px; ${stateBadgeStyle}">${state}</span>
              <div>
                <h3 style="margin: 0; font-size: 15px; font-weight: 600;">${escapeHtml(torrent.name)}</h3>
                <span style="font-size: 12px; color: var(--muted);">${size} • ${progress}% • Peers: ${peers}</span>
              </div>
            </div>
            <div style="display: flex; align-items: center;">
              ${copyMagnetHtml}
              <button class="torrent-remove btn" data-hash="${escapeHtml(torrent.infoHash || torrent.id)}" aria-label="Remove ${escapeHtml(torrent.name)}" style="padding: 4px 8px; font-size: 12px; color: #dc2626;">Remove</button>
            </div>
          </div>
          <div style="height: 6px; background: #e5e7eb; border-radius: 3px; overflow: hidden; margin-bottom: 8px;">
            <div style="width: ${progress}%; height: 100%; background: #2563eb; transition: width 0.3s ease;"></div>
          </div>
          <div class="torrent-metrics" style="display: flex; gap: 24px; font-size: 12px; color: var(--muted);">
            <div>Down: <strong style="color: var(--text);">${dlSpeed}</strong></div>
            <div>Up: <strong style="color: var(--text);">${ulSpeed}</strong></div>
            <div>Pieces: <strong style="color: var(--text);">${torrent.completedPieces || 0}/${torrent.pieceCount || 0}</strong></div>
          </div>
        </article>
      `;
    }).join('');

    container.innerHTML = cards;

    // Bind remove buttons
    container.querySelectorAll('.torrent-remove').forEach((button) => {
      button.addEventListener('click', async () => {
        const hash = button.dataset.hash;
        try {
          await fetch(`/api/torrents/remove?hash=${encodeURIComponent(hash)}`, { method: 'POST' });
        } catch (_) {}
        const index = torrentList.findIndex((item) => (item.infoHash || item.id) === hash);
        if (index !== -1) {
          torrentList.splice(index, 1);
          renderTorrentList();
          updateDashboardTiles();
          setStatus('Torrent removed.', 'info');
        }
      });
    });

    // Bind copy magnet buttons
    container.querySelectorAll('.torrent-copy-magnet').forEach((button) => {
      button.addEventListener('click', async () => {
        const uri = button.dataset.magnet;
        if (uri) {
          await navigator.clipboard.writeText(uri);
          const orig = button.textContent;
          button.textContent = 'Copied!';
          setTimeout(() => { button.textContent = orig; }, 1500);
        }
      });
    });
  }

  async function syncTorrentsFromServer() {
    try {
      const res = await fetch('/api/torrents');
      if (!res.ok) return;
      const data = await res.json();
      if (Array.isArray(data)) {
        torrentList.length = 0;
        data.forEach(item => {
          torrentList.push({
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
    } catch (_) {}
  }

  async function requestTorrentFromServer(file) {
    const formData = new FormData();
    formData.append('file', file, file.name);
    formData.append('announce', DEFAULT_ANNOUNCE);
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
    const downloadName = utf8Match
      ? decodeURIComponent(utf8Match[1])
      : (asciiMatch ? asciiMatch[1] : `${file.name}.torrent`);
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

      setStatus(`Torrent created successfully & is now Seeding: ${downloadName}`, 'success');
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

      setStatus(`Added ${file.name} to downloading swarm!`, 'success');
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
          await navigator.clipboard.writeText(magnetInput.value);
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
        if (mode === 'add') {
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
  renderTorrentList();
  window.addEventListener('hashchange', showView);
  showView();
  bindFilePicker();
  syncTorrentsFromServer();
  setInterval(syncTorrentsFromServer, 2000);
})();
