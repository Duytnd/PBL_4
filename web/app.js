'use strict';

// Chuyển giữa 3 trang (Dashboard / Torrents / Speed) theo phần # của địa chỉ.
(() => {
  const VIEWS = ['dashboard', 'torrents', 'speed'];

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

  window.addEventListener('hashchange', showView);
  showView();
})();
