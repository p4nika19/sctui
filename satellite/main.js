/**
 * Minimal Headless DRM Satellite Playback Daemon
 * Powered by @castlabs/electron for Google Widevine CDM + VMP support.
 */

'use strict';

const electron = require('electron');
const { app, session, BrowserWindow, ipcMain } = electron;
const components = electron.components;
const http = require('http');
const fs = require('fs');
const path = require('path');
const readline = require('readline');
const util = require('util');

app.setName('sctui-drm-satellite');
process.title = 'sctui-drm-satellite';

// Optimized Chromium flags (consistent with CastLabs / Applejack best practices)
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
app.commandLine.appendSwitch('disable-features', 'HardwareMediaKeyHandling');
app.commandLine.appendSwitch('disk-cache-size', String(256 * 1024 * 1024));
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('unsafely-treat-insecure-origin-as-secure', 'http://127.0.0.1:47823,http://localhost:47823');

// Persistent profile for Widevine CDM keys/certificates
const defaultProfile = path.join(app.getPath('appData'), 'sctui-satellite');
app.setPath('userData', defaultProfile);

// Strict stderr logger to keep stdout 100% clean for NDJSON IPC
function logDebug(...args) {
  process.stderr.write('[Satellite:DEBUG] ' + util.format(...args) + '\n');
}

function logInfo(...args) {
  process.stderr.write('[Satellite:INFO] ' + util.format(...args) + '\n');
}

function logError(...args) {
  process.stderr.write('[Satellite:ERROR] ' + util.format(...args) + '\n');
}

// IPC Output Helper: newline-delimited JSON strictly to stdout
function sendIPC(obj) {
  process.stdout.write(JSON.stringify(obj) + '\n');
}

// Zombie Prevention & Parent Process Monitoring
const parentPid = process.ppid;
if (parentPid > 1) {
  const ppidCheckInterval = setInterval(() => {
    try {
      // process.kill with signal 0 checks for process existence without sending a signal
      process.kill(parentPid, 0);
    } catch (e) {
      logInfo('Parent process (PID ' + parentPid + ') terminated. Exiting satellite daemon.');
      clearInterval(ppidCheckInterval);
      cleanupAndExit(0);
    }
  }, 1000);
  ppidCheckInterval.unref();
}

// Signal handlers
process.on('SIGPIPE', () => cleanupAndExit(0));
process.on('SIGHUP', () => cleanupAndExit(0));
process.on('SIGTERM', () => cleanupAndExit(0));
process.on('SIGINT', () => cleanupAndExit(0));

function cleanupAndExit(code = 0) {
  try {
    if (win && !win.isDestroyed()) {
      win.destroy();
      win = null;
    }
  } catch (e) {}
  try {
    if (localServer) {
      localServer.close();
      localServer = null;
    }
  } catch (e) {}
  try {
    app.exit(code);
  } catch (e) {}
  process.exit(code);
}

let win = null;
let isPageReady = false;
let pendingPlay = null;
let currentVolume = 0.2176;
let localServer = null;
let localPort = 47823;

function startLocalServer() {
  const htmlPath = path.join(__dirname, 'player.html');
  const hlsPath = path.join(__dirname, 'hls.min.js');

  const createHandler = () => (req, res) => {
    const u = req.url || '';
    if (u === '/' || u.startsWith('/player.html')) {
      res.writeHead(200, {
        'Content-Type': 'text/html; charset=utf-8',
        'Cache-Control': 'no-store'
      });
      res.end(fs.readFileSync(htmlPath));
    } else if (u.startsWith('/hls.min.js')) {
      res.writeHead(200, {
        'Content-Type': 'application/javascript; charset=utf-8',
        'Cache-Control': 'no-store'
      });
      res.end(fs.readFileSync(hlsPath));
    } else {
      res.writeHead(404);
      res.end();
    }
  };

  return new Promise((resolve, reject) => {
    const srv = http.createServer(createHandler());
    srv.once('error', (err) => {
      if (err.code === 'EADDRINUSE') {
        const fallback = http.createServer(createHandler());
        fallback.listen(0, '127.0.0.1', () => {
          localServer = fallback;
          localPort = fallback.address().port;
          resolve(`http://127.0.0.1:${localPort}/player.html`);
        });
      } else {
        reject(err);
      }
    });

    srv.listen(localPort, '127.0.0.1', () => {
      localServer = srv;
      resolve(`http://127.0.0.1:${localPort}/player.html`);
    });
  });
}

// Readline interface for NDJSON over stdin
const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  terminal: false
});

rl.on('line', (line) => {
  const trimmed = line.trim();
  if (!trimmed) return;

  try {
    const cmd = JSON.parse(trimmed);
    handleCommand(cmd);
  } catch (e) {
    logError('Malformed IPC command JSON: ' + trimmed + ' (' + e.message + ')');
    sendIPC({ type: 'error', message: 'Malformed JSON: ' + e.message });
  }
});

rl.on('close', () => {
  logInfo('Stdin reached EOF. Terminating satellite daemon.');
  cleanupAndExit(0);
});

function handleCommand(cmd) {
  if (!cmd || typeof cmd !== 'object') return;

  switch (cmd.cmd) {
    case 'ping':
      sendIPC({ type: 'pong', timestamp: Date.now() });
      break;

    case 'play':
      if (typeof cmd.volume === 'number' && !isNaN(cmd.volume)) {
        currentVolume = Math.max(0.0, Math.min(1.0, cmd.volume));
      } else {
        cmd.volume = currentVolume;
      }
      if (isPageReady && win && !win.isDestroyed()) {
        win.webContents.send('cmd-volume', currentVolume);
        win.webContents.send('cmd-play', cmd);
      } else {
        pendingPlay = cmd;
      }
      break;

    case 'pause':
      if (win && !win.isDestroyed()) {
        win.webContents.send('cmd-pause');
      }
      break;

    case 'resume':
      if (win && !win.isDestroyed()) {
        win.webContents.send('cmd-volume', currentVolume);
        win.webContents.send('cmd-resume');
      }
      break;

    case 'seek':
      if (win && !win.isDestroyed()) {
        win.webContents.send('cmd-seek', cmd.position_ms);
      }
      break;

    case 'set_volume':
      if (typeof cmd.volume === 'number' && !isNaN(cmd.volume)) {
        currentVolume = Math.max(0.0, Math.min(1.0, cmd.volume));
      }
      if (isPageReady && win && !win.isDestroyed()) {
        win.webContents.send('cmd-volume', currentVolume);
      }
      break;

    case 'stop':
      pendingPlay = null;
      if (win && !win.isDestroyed()) {
        win.webContents.send('cmd-stop');
      }
      break;

    case 'exit':
      logInfo('Received explicit exit command.');
      sendIPC({ type: 'exited' });
      cleanupAndExit(0);
      break;

    default:
      logError('Unknown command received: ' + cmd.cmd);
      sendIPC({ type: 'error', message: 'Unknown command: ' + cmd.cmd });
      break;
  }
}

// Forward events from renderer to stdout IPC
ipcMain.on('player-event', (event, eventObj) => {
  if (eventObj && typeof eventObj === 'object') {
    sendIPC(eventObj);
  }
});

app.on('window-all-closed', () => {}); // Keep daemon running headless

// Create headless invisible window when Electron is ready
app.whenReady().then(async () => {
  if (components) {
    try {
      await components.whenReady();
      logInfo('CastLabs components ready: ' + JSON.stringify(components.status()));
    } catch (e) {
      logError('Failed waiting for CastLabs components: ' + e.message);
    }
  }

  const sess = session.fromPartition('persist:sctui');

  // Grant mediaKeySystem (EME) permissions for Widevine CDM
  sess.setPermissionRequestHandler((webContents, permission, callback) => {
    if (permission === 'mediaKeySystem' || permission === 'media') {
      return callback(true);
    }
    callback(false);
  });

  sess.setPermissionCheckHandler((webContents, permission) => {
    if (permission === 'mediaKeySystem' || permission === 'media') {
      return true;
    }
    return false;
  });

  // Ensure requests to SoundCloud streaming and license endpoints contain valid Origin headers
  sess.webRequest.onBeforeSendHeaders((details, callback) => {
    const headers = details.requestHeaders || {};
    if (details.url.includes('soundcloud') || details.url.includes('media-streaming')) {
      headers['Origin'] = 'https://soundcloud.com';
      headers['Referer'] = 'https://soundcloud.com/';
      headers['User-Agent'] = 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36';
    }
    callback({ cancel: false, requestHeaders: headers });
  });

  const pageUrl = await startLocalServer();
  logInfo('Local player server ready at ' + pageUrl);

  win = new BrowserWindow({
    show: false,
    width: 1,
    height: 1,
    frame: false,
    skipTaskbar: true,
    hasShadow: false,
    focusable: false,
    transparent: true,
    opacity: 0.0,
    backgroundColor: '#00000000',
    type: 'utility',
    webPreferences: {
      partition: 'persist:sctui',
      nodeIntegration: false,
      contextIsolation: true,
      backgroundThrottling: false,
      spellcheck: false,
      preload: path.join(__dirname, 'preload.js')
    }
  });

  win.webContents.on('console-message', (e) => {
    logDebug('[Page]', e.message);
  });

  win.webContents.on('did-finish-load', async () => {
    // Probe Widevine CDM accessibility
    let emeFunctional = false;
    try {
      emeFunctional = await win.webContents.executeJavaScript(`
        (async () => {
          if (typeof navigator.requestMediaKeySystemAccess !== 'function') return false;
          try {
            await navigator.requestMediaKeySystemAccess('com.widevine.alpha', [{
              initDataTypes: ['cenc'],
              audioCapabilities: [{ contentType: 'audio/mp4; codecs="mp4a.40.2"' }]
            }]);
            return true;
          } catch (e) {
            console.warn('[Probe] Widevine test failed:', e.name, e.message);
            return false;
          }
        })()
      `);
    } catch (e) {
      logDebug('Widevine probe execution failed: ' + e.message);
    }

    if (!emeFunctional && process.platform === 'linux') {
      logInfo('Widevine CDM newly downloaded or not loaded into current sandbox session. Relaunching satellite daemon...');
      sendIPC({ type: 'restart_required' });
      cleanupAndExit(42);
      return;
    }

    isPageReady = true;
    win.webContents.send('cmd-volume', currentVolume);
    sendIPC({
      type: 'ready',
      version: '1.0.0',
      cdm: 'widevine-vmp',
      pid: process.pid
    });
    logInfo('Headless DRM satellite initialized with Widevine CDM support. PID: ' + process.pid + ' (volume: ' + currentVolume + ')');
    if (pendingPlay) {
      logInfo('Dispatching queued play command with volume ' + currentVolume);
      pendingPlay.volume = currentVolume;
      win.webContents.send('cmd-volume', currentVolume);
      win.webContents.send('cmd-play', pendingPlay);
      pendingPlay = null;
    }
  });

  win.webContents.on('crashed', (event, killed) => {
    logError('Renderer process crashed! killed=' + killed);
    sendIPC({ type: 'error', message: 'Renderer crashed' });
  });

  win.loadURL(pageUrl);
});
