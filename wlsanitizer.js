// Executed in QQ's main process; supplied by the experimental preload library.
'use strict';
const fs = require('fs');
const path = require('path');
const { app, BrowserWindow } = require('electron');
const config = JSON.parse(fs.readFileSync(path.join(__dirname, 'config.json'), 'utf8'));
const pending = new WeakMap();
const installed = new WeakSet();

function captureReady() {
  try { return fs.readFileSync(config.flag, 'utf8').trim() === String(process.pid); }
  catch (_) { return false; }
}

function check(win) {
  if (win.isDestroyed()) return;
  const bounds = win.getBounds();
  const min = win.getMinimumSize(), max = win.getMaximumSize();
  const fixed = min[0] > 0 && min[1] > 0 && min[0] === max[0] && min[1] === max[1];
  if (!win.isVisible() || win.getTitle() !== '屏幕共享' || fixed ||
      bounds.width < 600 || bounds.height < 400 || !captureReady()) {
    pending.delete(win);
    return;
  }
  const now = performance.now();
  if (!pending.has(win)) {
    pending.set(win, now);
    console.error(`[qwlss-sanitizer] candidate id=${win.id} ${bounds.width}x${bounds.height}`);
  } else if (now - pending.get(win) >= 2000) {
    // Hide, never close/destroy: QQ retains the window and its share controls.
    win.hide();
    pending.delete(win);
    console.error(`[qwlss-sanitizer] hidden id=${win.id} ${bounds.width}x${bounds.height} visible=${win.isVisible()}`);
  }
}

function track(win) {
  if (installed.has(win)) return;
  installed.add(win);
  for (const event of ['show', 'resize', 'page-title-updated'])
    win.on(event, () => check(win));
  win.on('closed', () => pending.delete(win));
}

app.on('browser-window-created', (_, win) => track(win));
app.whenReady().then(() => {
  const timer = setInterval(() => {
    for (const win of BrowserWindow.getAllWindows()) { track(win); check(win); }
  }, 1000);
  timer.unref();
  app.once('will-quit', () => clearInterval(timer));
});
console.error(`[qwlss-sanitizer] installed pid=${process.pid}`);
require(config.main);
