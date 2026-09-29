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

  function concatUint8Arrays(parts) {
    const totalLength = parts.reduce((sum, part) => sum + part.length, 0);
    const merged = new Uint8Array(totalLength);
    let offset = 0;

    for (const part of parts) {
      merged.set(part, offset);
      offset += part.length;
    }

    return merged;
  }

  function encodeBencodeValue(value) {
    if (typeof value === 'number' && Number.isInteger(value)) {
      const text = new TextEncoder().encode(`i${value}e`);
      return text;
    }

    if (typeof value === 'string') {
      const encoded = new TextEncoder().encode(value);
      const length = new TextEncoder().encode(String(encoded.length));
      return concatUint8Arrays([length, new Uint8Array([0x3a]), encoded]);
    }

    if (value instanceof Uint8Array) {
      const length = new TextEncoder().encode(String(value.length));
      return concatUint8Arrays([length, new Uint8Array([0x3a]), value]);
    }

    if (Array.isArray(value)) {
      const parts = [new Uint8Array([0x6c])];
      for (const item of value) {
        parts.push(encodeBencodeValue(item));
      }
      parts.push(new Uint8Array([0x65]));
      return concatUint8Arrays(parts);
    }

    if (value && typeof value === 'object') {
      const keys = Object.keys(value).sort();
      const parts = [new Uint8Array([0x64])];
      for (const key of keys) {
        parts.push(encodeBencodeValue(key));
        parts.push(encodeBencodeValue(value[key]));
      }
      parts.push(new Uint8Array([0x65]));
      return concatUint8Arrays(parts);
    }

    throw new Error('Unsupported Bencode value type');
  }

  function decodeBytes(value) {
    if (value instanceof Uint8Array) {
      return new TextDecoder('utf-8').decode(value);
    }
    return String(value ?? '');
  }

  function parseBString(data, offset) {
    let lengthEnd = offset;
    while (lengthEnd < data.length && data[lengthEnd] >= 0x30 && data[lengthEnd] <= 0x39) {
      lengthEnd += 1;
    }

    if (lengthEnd >= data.length || data[lengthEnd] !== 0x3a) {
      throw new Error('Invalid bencoded string length');
    }

    const length = Number(new TextDecoder('utf-8').decode(data.slice(offset, lengthEnd)));
    const valueStart = lengthEnd + 1;
    const valueEnd = valueStart + length;

    return {
      value: data.slice(valueStart, valueEnd),
      nextIndex: valueEnd,
    };
  }

  function parseBencodeValue(data, offset = 0) {
    const marker = data[offset];

    if (marker === 0x69) {
      const end = data.indexOf(0x65, offset + 1);
      if (end === -1) throw new Error('Invalid integer in torrent metadata');
      const intText = new TextDecoder('utf-8').decode(data.slice(offset + 1, end));
      return { value: Number(intText), nextIndex: end + 1 };
    }

    if (marker >= 0x30 && marker <= 0x39) {
      return parseBString(data, offset);
    }

    if (marker === 0x6c) {
      const list = [];
      let index = offset + 1;
      while (data[index] !== 0x65) {
        const parsed = parseBencodeValue(data, index);
        list.push(parsed.value);
        index = parsed.nextIndex;
      }
      return { value: list, nextIndex: index + 1 };
    }

    if (marker === 0x64) {
      const object = {};
      let index = offset + 1;
      while (data[index] !== 0x65) {
        const keyParsed = parseBencodeValue(data, index);
        const key = decodeBytes(keyParsed.value);
        index = keyParsed.nextIndex;
        const valueParsed = parseBencodeValue(data, index);
        object[key] = valueParsed.value;
        index = valueParsed.nextIndex;
      }
      return { value: object, nextIndex: index + 1 };
    }

    throw new Error('Unsupported torrent metadata format');
  }

  function parseTorrentMetadata(file) {
    const dict = parseBencodeValue(new Uint8Array(file), 0).value;
    const info = dict.info || {};
    const announce = typeof dict.announce === 'string' ? dict.announce : decodeBytes(dict.announce || new Uint8Array());
    const length = typeof info.length === 'number' ? info.length : 0;
    const pieceLength = typeof info['piece length'] === 'number' ? info['piece length'] : 0;
    const pieces = info.pieces instanceof Uint8Array ? info.pieces.length / 20 : 0;
    const name = decodeBytes(info.name || new Uint8Array());

    return {
      name: name || file.name.replace(/\.torrent$/i, ''),
      announce: announce || DEFAULT_ANNOUNCE,
      length,
      pieceLength,
      pieceCount: pieces,
      rawName: file.name,
    };
  }

  function formatBytes(bytes) {
    if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
    const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
    const index = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), sizes.length - 1);
    const value = bytes / (1024 ** index);
    return `${value.toFixed(value >= 10 || index === 0 ? 0 : 1)} ${sizes[index]}`;
  }

  function escapeHtml(value) {
    return String(value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function renderTorrentList() {
    const container = document.getElementById('torrent-list');
    if (!container) return;

    if (!torrentList.length) {
      container.innerHTML = '<div class="torrent-empty">No torrent files added yet.</div>';
      return;
    }

    const cards = torrentList.map((torrent) => {
      const announce = torrent.announce || 'No tracker';
      const size = formatBytes(torrent.length);
      const fileName = torrent.rawName || torrent.name;

      return `
        <article class="torrent-card">
          <div class="torrent-card-top">
            <div class="torrent-file-wrap">
              <div class="torrent-file-icon">T</div>
              <div>
                <h3>${escapeHtml(torrent.name)}</h3>
                <span>${escapeHtml(fileName)}</span>
              </div>
            </div>
            <button class="torrent-remove" data-id="${torrent.id}" aria-label="Remove ${escapeHtml(torrent.name)}">Remove</button>
          </div>
          <div class="torrent-metrics">
            <div>
              <span>Size</span>
              <strong>${size}</strong>
            </div>
            <div>
              <span>Tracker</span>
              <strong>${escapeHtml(announce)}</strong>
            </div>
            <div>
              <span>Pieces</span>
              <strong>${torrent.pieceCount}</strong>
            </div>
          </div>
        </article>
      `;
    }).join('');

    container.innerHTML = cards;

    container.querySelectorAll('.torrent-remove').forEach((button) => {
      button.addEventListener('click', () => {
        const targetId = Number(button.dataset.id);
        const index = torrentList.findIndex((item) => item.id === targetId);
        if (index !== -1) {
          torrentList.splice(index, 1);
          renderTorrentList();
          setStatus('Torrent removed from list.', 'info');
        }
      });
    });
  }

  async function sha1Bytes(data) {
    const hashBuffer = await crypto.subtle.digest('SHA-1', data);
    return new Uint8Array(hashBuffer);
  }

  async function createTorrentBlob(file) {
    const fileBuffer = await file.arrayBuffer();
    const bytes = new Uint8Array(fileBuffer);

    const pieceHashes = [];
    for (let offset = 0; offset < bytes.length; offset += PIECE_LENGTH) {
      const chunk = bytes.slice(offset, offset + PIECE_LENGTH);
      const hash = await sha1Bytes(chunk);
      pieceHashes.push(hash);
    }

    const totalHashLength = pieceHashes.reduce((sum, hash) => sum + hash.length, 0);
    const allPieceHashes = new Uint8Array(totalHashLength);
    let cursor = 0;
    for (const hash of pieceHashes) {
      allPieceHashes.set(hash, cursor);
      cursor += hash.length;
    }

    const info = {
      name: file.name,
      'piece length': PIECE_LENGTH,
      pieces: allPieceHashes,
      length: bytes.length,
    };

    const root = {
      announce: DEFAULT_ANNOUNCE,
      info,
    };

    return new Blob([encodeBencodeValue(root)], { type: 'application/x-bittorrent' });
  }

  async function handleCreateTorrent(file) {
    if (!file) return;

    try {
      setStatus(`Creating torrent for ${file.name}...`);
      const blob = await createTorrentBlob(file);
      const url = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.download = `${file.name}.torrent`;
      document.body.appendChild(link);
      link.click();
      link.remove();
      URL.revokeObjectURL(url);
      setStatus(`Torrent created successfully: ${file.name}.torrent`, 'success');
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
      const buffer = await file.arrayBuffer();
      const metadata = parseTorrentMetadata(buffer);
      const item = {
        id: Date.now() + Math.random(),
        ...metadata,
      };
      torrentList.unshift(item);
      renderTorrentList();
      setStatus(`Added ${metadata.name} to the list.`, 'success');
    } catch (error) {
      setStatus(`Failed to read torrent metadata: ${error.message}`, 'error');
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

  renderTorrentList();
  window.addEventListener('hashchange', showView);
  showView();
  bindFilePicker();
})();
