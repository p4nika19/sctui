/**
 * Minimal Headless DRM Satellite Playback Daemon
 * Powered by @castlabs/electron for Google Widevine CDM + VMP support.
 */

const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');
const readline = require('readline');

// Command-line optimization switches for ultra-low resource usage
app.commandLine.appendSwitch('disable-gpu');
app.commandLine.appendSwitch('disable-gpu-compositing');
app.commandLine.appendSwitch('disable-software-rasterizer');
app.commandLine.appendSwitch('disable-dev-shm-usage');
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
app.commandLine.appendSwitch('disable-features', 'PaintHolding,CalculateNativeWinOcclusion,SpareRendererForSitePerProcess');
app.commandLine.appendSwitch('no-sandbox');

// Strict stderr logger to keep stdout clean for NDJSON IPC
function logDebug(...args) {
  process.stderr.write('[Satellite:DEBUG] ' + args.join(' ') + '\n');
}

function logInfo(...args) {
  process.stderr.write('[Satellite:INFO] ' + args.join(' ') + '\n');
}

function logError(...args) {
  process.stderr.write('[Satellite:ERROR] ' + args.join(' ') + '\n');
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
}

// Signal handlers
process.on('SIGPIPE', () => cleanupAndExit(0));
process.on('SIGHUP', () => cleanupAndExit(0));
process.on('SIGTERM', () => cleanupAndExit(0));
process.on('SIGINT', () => cleanupAndExit(0));

function cleanupAndExit(code = 0) {
  try {
    app.quit();
  } catch (e) {}
  process.exit(code);
}

let win = null;

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
      if (win && !win.isDestroyed()) {
        win.webContents.send('cmd-play', cmd);
      }
      break;

    case 'pause':
      if (win && !win.isDestroyed()) {
        win.webContents.send('cmd-pause');
      }
      break;

    case 'resume':
      if (win && !win.isDestroyed()) {
        win.webContents.send('cmd-resume');
      }
      break;

    case 'seek':
      if (win && !win.isDestroyed()) {
        win.webContents.send('cmd-seek', cmd.position_ms);
      }
      break;

    case 'set_volume':
      if (win && !win.isDestroyed()) {
        const vol = typeof cmd.volume === 'number' ? cmd.volume : 1.0;
        win.webContents.send('cmd-volume', vol);
      }
      break;

    case 'stop':
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

// Create headless invisible window when Electron is ready
app.whenReady().then(() => {
  win = new BrowserWindow({
    show: false,
    width: 1,
    height: 1,
    frame: false,
    skipTaskbar: true,
    hasShadow: false,
    focusable: false,
    webPreferences: {
      nodeIntegration: false,
      contextIsolation: true,
      backgroundThrottling: false,
      preload: path.join(__dirname, 'preload.js')
    }
  });

  win.loadFile(path.join(__dirname, 'player.html'));

  win.webContents.on('did-finish-load', () => {
    sendIPC({
      type: 'ready',
      version: '1.0.0',
      cdm: 'widevine-vmp',
      pid: process.pid
    });
    logInfo('Headless DRM satellite initialized with Widevine CDM support. PID: ' + process.pid);
  });

  win.webContents.on('crashed', (event, killed) => {
    logError('Renderer process crashed! killed=' + killed);
    sendIPC({ type: 'error', message: 'Renderer crashed' });
  });
});
