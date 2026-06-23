const apiResult = document.getElementById("api-result");
const wsLog = document.getElementById("ws-log");
const hint = document.getElementById("hint");
const wsState = document.getElementById("ws-state");

let socket = null;

async function loadApi(path) {
  hint.textContent = `Loading ${path}`;
  try {
    const response = await fetch(path);
    const json = await response.json();
    apiResult.textContent = JSON.stringify(json, null, 2);
    hint.textContent = `Loaded ${path}`;
  } catch (error) {
    apiResult.textContent = String(error);
    hint.textContent = `Failed ${path}`;
  }
}

function appendWsLog(message) {
  if (wsLog.textContent === "No websocket activity yet.") {
    wsLog.textContent = "";
  }
  wsLog.textContent += `${message}\n`;
}

function connectSocket() {
  if (socket && socket.readyState === WebSocket.OPEN) {
    appendWsLog("WebSocket already connected.");
    return;
  }

  socket = new WebSocket("ws://127.0.0.1:7777/ws");
  wsState.textContent = "Connecting";

  socket.addEventListener("open", () => {
    wsState.textContent = "Connected";
    appendWsLog("open");
  });

  socket.addEventListener("message", (event) => {
    appendWsLog(`message: ${event.data}`);
  });

  socket.addEventListener("close", () => {
    wsState.textContent = "Closed";
    appendWsLog("close");
  });

  socket.addEventListener("error", () => {
    wsState.textContent = "Error";
    appendWsLog("error");
  });
}

function sendPing() {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    appendWsLog("cannot send, websocket is not open");
    return;
  }
  const payload = {
    type: "ping",
    at: new Date().toISOString()
  };
  socket.send(JSON.stringify(payload));
  appendWsLog(`send: ${JSON.stringify(payload)}`);
}

document.querySelectorAll("[data-api]").forEach((button) => {
  button.addEventListener("click", () => loadApi(button.dataset.api));
});

document.getElementById("ws-connect").addEventListener("click", connectSocket);
document.getElementById("ws-send").addEventListener("click", sendPing);
