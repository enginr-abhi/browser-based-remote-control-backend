#!/usr/bin/env node
const os = require("os");
const process = require("process");
const fs = require("fs");
const path = require("path");
const screenshot = require("screenshot-desktop");
const { io } = require("socket.io-client");
const { mouse, keyboard, Key, Point, Button, screen } = require("@nut-tree-fork/nut-js");
const { execSync } = require("child_process");

// ---- Permission check (best-effort) ----
function checkPermissions() {
  const platform = os.platform();
  if (platform === "win32") {
    try {
      execSync("net session", { stdio: "ignore" });
      console.log("✅ Running as Administrator");
    } catch {
      console.warn("⚠️ Not running as Admin! Mouse/keyboard may need elevation.");
    }
  } else if (platform === "darwin") {
    console.warn("⚠️ macOS requires Screen Recording and Accessibility permissions.");
  } else if (platform === "linux" && process.getuid && process.getuid() !== 0) {
    console.warn("⚠️ It may require sudo on some Linux setups.");
  }
}
checkPermissions();

// nut.js config
mouse.config.mouseSpeed = 1200;
keyboard.config.autoDelayMs = 0;

// read ROOM from config.json placed next to exe or env var
let ROOM = "room1";
try {
  const cfg = path.join(__dirname, "config.json");
  if (fs.existsSync(cfg)) {
    const j = JSON.parse(fs.readFileSync(cfg, "utf8"));
    ROOM = j.roomId || ROOM;
  } else if (process.env.ROOM) ROOM = process.env.ROOM;
} catch (e) {
  console.warn("Failed to read config.json", e);
}

const SERVER = "https://browser-based-remote-control-backend.onrender.com" ;
const socket = io(SERVER, { transports: ["websocket"], reconnection: true });

let captureInfo = null;
let streaming = false;
const FPS = parseInt(process.env.FPS || "3", 10);
const intervalMs = Math.max(50, Math.round(1000 / Math.max(1, FPS)));
let lastMoveTs = 0;
const MOVE_THROTTLE_MS = 10;

socket.on("connect", () => {
  console.log("Agent connected:", socket.id, "room:", ROOM);
  socket.emit("join-room", { roomId: ROOM, isAgent: true, name: os.userInfo().username || "agent" });
});

socket.on("disconnect", () => console.log("Agent disconnected"));

// server instructs to start/stop streaming
socket.on("start-stream", ({ roomId }) => {
  if (roomId === ROOM) {
    streaming = true;
    console.log("Agent: start-stream received");
  }
});
socket.on("stop-stream", ({ roomId }) => {
  if (roomId === ROOM) {
    streaming = false;
    console.log("Agent: stop-stream received");
  }
});

// optional capture-info from UI
socket.on("capture-info", (info) => {
  captureInfo = info;
  console.log("Agent: got capture-info", info);
});

// --- control handler (your existing nut-js logic) ---
const keyMap = {
  enter: Key.Enter, escape: Key.Escape, tab: Key.Tab,
  backspace: Key.Backspace, delete: Key.Delete,
  control: Key.LeftControl, ctrl: Key.LeftControl,
  shift: Key.LeftShift, alt: Key.LeftAlt, meta: Key.LeftSuper,
  arrowup: Key.Up, arrowdown: Key.Down, arrowleft: Key.Left, arrowright: Key.Right,
  space: Key.Space,
  f1: Key.F1, f2: Key.F2, f3: Key.F3, f4: Key.F4, f5: Key.F5, f6: Key.F6, f7: Key.F7, f8: Key.F8,
  f9: Key.F9, f10: Key.F10, f11: Key.F11, f12: Key.F12
};
const mapBtn = btn => btn === 2 ? Button.RIGHT : (btn === 1 ? Button.MIDDLE : Button.LEFT);
function isPrintableChar(s) { return typeof s === "string" && s.length === 1; }
function clamp(v, a, b) { return Math.max(a, Math.min(b, v)); }

socket.on("control", async (data) => {
  if (!captureInfo) {
    // if captureInfo not provided, we still attempt but be cautious
  }
  try {
    if (["mousemove", "click", "mousedown", "mouseup", "dblclick", "wheel"].includes(data.type)) {
      const now = Date.now();
      if (data.type === "mousemove" && now - lastMoveTs < MOVE_THROTTLE_MS) return;
      lastMoveTs = now;

      // map normalized coords [0..1] to screen
      const w = (captureInfo?.captureWidth || 1280) * (captureInfo?.devicePixelRatio || 1);
      const h = (captureInfo?.captureHeight || 720) * (captureInfo?.devicePixelRatio || 1);
      const srcX = typeof data.x === "number" ? Math.round(data.x * w) : null;
      const srcY = typeof data.y === "number" ? Math.round(data.y * h) : null;

      const displayWidth = await screen.width();
      const displayHeight = await screen.height();
      const absX = srcX !== null ? clamp(Math.round(srcX * (displayWidth / Math.max(1, w))), 0, displayWidth - 1) : null;
      const absY = srcY !== null ? clamp(Math.round(srcY * (displayHeight / Math.max(1, h))), 0, displayHeight - 1) : null;

      if (absX !== null && absY !== null) await mouse.setPosition(new Point(absX, absY));

      if (data.type === "click") await mouse.click(mapBtn(data.button));
      else if (data.type === "dblclick") await mouse.doubleClick(mapBtn(data.button));
      else if (data.type === "mousedown") await mouse.pressButton(mapBtn(data.button));
      else if (data.type === "mouseup") await mouse.releaseButton(mapBtn(data.button));
      else if (data.type === "wheel") {
        if (data.deltaY > 0) await mouse.scrollDown(200);
        else await mouse.scrollUp(200);
      }
    }

    if (["keydown", "keyup"].includes(data.type)) {
      const rawKey = (data.key || "").toString();
      const keyName = rawKey.toLowerCase();
      const mapped = keyMap[keyName];
      if (data.type === "keydown") {
        if (mapped) await keyboard.pressKey(mapped);
        else if (isPrintableChar(rawKey)) await keyboard.type(rawKey);
      } else if (data.type === "keyup") {
        if (mapped) await keyboard.releaseKey(mapped);
      }
    }
  } catch (err) {
    console.error("Control handling error:", err);
  }
});

// ---- capture loop (screenshot-desktop) ----
(async function captureLoop() {
  while (true) {
    try {
      if (streaming) {
        const imgBuf = await screenshot({ format: 'png' });
        socket.emit('screen-frame', { roomId: ROOM, agentId: socket.id, image: imgBuf.toString('base64') });
      }
    } catch (err) {
      console.error("Capture error:", err && err.message);
    }
    await new Promise(r => setTimeout(r, intervalMs));
  }
})();

// keep alive
process.stdin.resume();
console.log("Agent ready — waiting for start-stream and control events...");
