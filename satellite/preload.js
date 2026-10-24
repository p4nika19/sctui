const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('sctuiBridge', {
  onCommand: (channel, callback) => {
    ipcRenderer.on(channel, (event, ...args) => callback(...args));
  },
  sendEvent: (eventObj) => {
    ipcRenderer.send('player-event', eventObj);
  }
});
