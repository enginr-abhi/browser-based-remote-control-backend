#!/usr/bin/env node
const os = require("os");
const path = require("path");
const fs = require("fs");
const { io } = require("socket.io-client");
const screenshot = require("screenshot-desktop");
const { mouse, keyboard, Key, Point, Button, screen } = require("@nut-tree-fork/nut-js");
const { execSync } = require("child_process");

// simple permission hint
if (os.platform() === "darwin") console.warn("macOS: give Accessibility permission to this app for input injection");

// read config.json or argv
let ROOM = "room1";
try {
  const cfg = path.join(__dirname, "config.json");
  if (fs.existsSync(cfg)) {
    const obj = JSON.parse(fs.readFileSync(cfg, "utf8"));
    ROOM = obj.roomId || ROOM;
  } else if (process.argv[2]) ROOM = process.argv[2];
  else if (process.env.ROOM) ROOM = process.env.ROOM;
} catch (e) { console.warn("config read failed", e); }

const SERVER = process.env.SERVER || "http://localhost:9000";
const socket = io(SERVER, { transports: ["websocket"], reconnection: true, maxHttpBufferSize: 10 * 1024 * 1024 });

const FPS = 8;
const INTERVAL_MS = Math.round(1000 / FPS);

socket.on("connect", () => {
  console.log("Agent connected", socket.id, "room:", ROOM);
  socket.emit("join-room", { roomId: ROOM, isAgent: true, name: "agent-"+socket.id });
});

// grant control hint
socket.on("grant-control", ({ viewerId }) => {
  console.log("Granted control for viewer:", viewerId);
});
socket.on("revoke-control", () => {
  console.log("Control revoked");
});
socket.on("disconnect", () => console.log("Agent disconnected"));

let streaming = false;
async function startStreaming() {
  if (streaming) return;
  streaming = true;
  console.log("Starting capture loop at", FPS, "FPS");
  while (streaming) {
    try {
      // screenshot-desktop returns a Buffer (png/jpg) - ask jpg for smaller size
      const img = await screenshot({ format: "jpg" });
      // send binary buffer
      socket.emit("frame", img);
      await new Promise(r => setTimeout(r, INTERVAL_MS));
    } catch (e) {
      console.error("capture error", e);
      await new Promise(r => setTimeout(r, 1000));
    }
  }
}
startStreaming().catch(console.error);

// control handling (use nut-js)
const keyMap = { enter: Key.Enter, tab: Key.Tab, escape: Key.Escape, backspace: Key.Backspace, delete: Key.Delete, space: Key.Space, ctrl: Key.LeftControl, control: Key.LeftControl, shift: Key.LeftShift, alt: Key.LeftAlt };
const mapBtn = b => b === 2 ? Button.RIGHT : (b === 1 ? Button.MIDDLE : Button.LEFT);

socket.on("control", async (data) => {
  try {
    // mouse events: x,y are ratios
    if (["mousemove","click","mousedown","mouseup","dblclick","wheel"].includes(data.type)) {
      const displayW = await screen.width();
      const displayH = await screen.height();
      const absX = Math.round((data.x || 0) * displayW);
      const absY = Math.round((data.y || 0) * displayH);
      await mouse.setPosition(new Point(absX, absY));
      if (data.type === "click") await mouse.click(mapBtn(data.button));
      else if (data.type === "dblclick") await mouse.doubleClick(mapBtn(data.button));
      else if (data.type === "mousedown") await mouse.pressButton(mapBtn(data.button));
      else if (data.type === "mouseup") await mouse.releaseButton(mapBtn(data.button));
      else if (data.type === "wheel") { if (data.deltaY > 0) await mouse.scrollDown(200); else await mouse.scrollUp(200); }
    }
    if (["keydown","keyup"].includes(data.type)) {
      const k = (data.key || "").toString().toLowerCase();
      const mapped = keyMap[k];
      if (data.type === "keydown") {
        if (mapped) await keyboard.pressKey(mapped);
        else if (data.key && data.key.length === 1) await keyboard.type(data.key);
      } else {
        if (mapped) await keyboard.releaseKey(mapped);
      }
    }
  } catch (e) {
    console.error("control error", e);
  }
});

// graceful exit
process.on("SIGINT", () => process.exit());
