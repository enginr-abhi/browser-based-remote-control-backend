// server.js — Fixed & compatible with the updated frontend (script.js)
// Supports browser clients (socket.io) and C++ agent via WebSocket (/ws-agent)

const http = require("http");
const express = require("express");
const { Server } = require("socket.io");
const cors = require("cors");
const path = require("path");
const os = require("os");
const fs = require("fs");
const archiver = require("archiver");
const WebSocket = require("ws");

// ====== CONFIG ======
const PORT = process.env.PORT || 9000;
// If you deploy frontend to a fixed origin, set FRONTEND_ORIGIN env var.
// If not set, allow all origins for easier local testing.
const FRONTEND_ORIGIN = process.env.FRONTEND_ORIGIN || "*";

// ====== EXPRESS + HTTP ======
const app = express();
app.use(express.json());
if (FRONTEND_ORIGIN === "*") {
  app.use(cors());
} else {
  app.use(cors({ origin: FRONTEND_ORIGIN }));
}
const server = http.createServer(app);

// ====== SOCKET.IO (Browser clients) ======
const io = new Server(server, {
  cors: {
    origin: FRONTEND_ORIGIN === "*" ? true : FRONTEND_ORIGIN,
    methods: ["GET", "POST"],
    credentials: true
  },
});

// ====== STATE ======
// peers: socket.id -> { name, roomId }
const peers = new Map();
// users mirror for easy listing: socket.id -> { id, name, room, isOnline }
const users = new Map();
// agentSockets: roomId -> ws
const agentSockets = new Map();

// ====== HELPERS ======
function getUserListArray() {
  return Array.from(users.entries()).map(([id, u]) => ({
    id,
    name: u.name || "Unknown",
    roomId: u.room || null,
    isOnline: !!u.isOnline,
    isAgent: !!u.isAgent
  }));
}

function broadcastUserList() {
  const list = getUserListArray();
  io.emit("peer-list", list);
}

// ====== HEALTH CHECK ======
app.get("/", (req, res) => {
  res.send("✅ Backend running (socket.io + ws-agent) — ready");
});

// ====== AGENT DOWNLOAD (zip agent + config) ======
app.get("/download-agent", (req, res) => {
  const roomId = req.query.room || "room1";
  const agentExe = path.join(__dirname, "agent", "agent.exe");
  if (!fs.existsSync(agentExe)) return res.status(404).send("❌ Agent not found.");

  try {
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "agent-"));
    const exeCopy = path.join(tmpDir, "remote-agent.exe");
    fs.copyFileSync(agentExe, exeCopy);
    fs.writeFileSync(path.join(tmpDir, "config.json"), JSON.stringify({ roomId }, null, 2));

    res.setHeader("Content-Type", "application/zip");
    res.setHeader("Content-Disposition", `attachment; filename=remote-agent-${roomId}.zip`);

    const archive = archiver("zip", { zlib: { level: 9 } });
    archive.pipe(res);
    archive.file(exeCopy, { name: "agent.exe" });
    archive.file(path.join(tmpDir, "config.json"), { name: "config.json" });
    archive.finalize();
  } catch (err) {
    console.error("Error preparing agent:", err);
    res.status(500).send("Error preparing agent");
  }
});

// ====== SOCKET.IO HANDLERS (browser clients) ======
io.on("connection", (socket) => {
  console.log("🔌 Browser connected:", socket.id);

  // store initial minimal meta
  peers.set(socket.id, { socketId: socket.id });

  socket.on("set-name", ({ name }) => {
    const meta = peers.get(socket.id) || {};
    meta.name = name || meta.name;
    peers.set(socket.id, meta);

    users.set(socket.id, { id: socket.id, name: name || "Unknown", room: meta.roomId || null, isOnline: true });
    broadcastUserList();
  });

  socket.on("join-room", ({ roomId, name, isAgent = false }) => {
    const meta = peers.get(socket.id) || {};
    meta.name = name || meta.name;
    meta.roomId = roomId;
    meta.isAgent = !!isAgent;
    peers.set(socket.id, meta);

    socket.join(roomId);

    users.set(socket.id, { id: socket.id, name: meta.name || name || "Unknown", room: roomId, isOnline: true, isAgent: !!isAgent });

    // notify room that a peer joined
    socket.to(roomId).emit("peer-joined", { id: socket.id, name: meta.name, isAgent: !!isAgent });
    broadcastUserList();

    console.log(`➡️ ${socket.id} joined room ${roomId} (agent=${!!isAgent})`);
  });

  // frontend calls this to get current peers
  socket.on("get-peers", () => {
    socket.emit("peer-list", getUserListArray());
  });

  // viewer requests screen from peers in room (we forward to the room so human can accept)
  socket.on("request-screen", ({ roomId, from }) => {
    if (!roomId) return;
    // forward request to everyone in room (agent or user should prompt)
    socket.to(roomId).emit("screen-request", { from, name: peers.get(socket.id)?.name || "Viewer" });
    console.log(`Request-screen from ${socket.id} for room ${roomId}`);
  });

  // agent / human permission response forwarded to the requester
  // payload: { to, accepted, roomId }
  socket.on("permission-response", ({ to, accepted, roomId }) => {
    try {
      // include agentId: we'll use a stable identifier "agent-<roomId>" if an agent WS exists
      const agentId = agentSockets.has(roomId) ? `agent-${roomId}` : null;
      io.to(to).emit("permission-result", { accepted: !!accepted, agentId });
      console.log(`Permission response from ${socket.id} to ${to}: accepted=${!!accepted}, agentId=${agentId}`);

      // if accepted, instruct the agent (if connected) to start streaming
      if (accepted && agentSockets.has(roomId)) {
        const wsAgent = agentSockets.get(roomId);
        if (wsAgent && wsAgent.readyState === WebSocket.OPEN) {
          wsAgent.send(JSON.stringify({ action: "start-stream" }));
        }
      }
    } catch (err) {
      console.error("Error handling permission-response:", err);
    }
  });

  // viewer control events forwarded to the agent (by room)
  // payload: { type, ...data, roomId, toAgent }
  socket.on("control", (payload) => {
    try {
      const meta = peers.get(socket.id) || {};
      const roomId = meta.roomId;
      if (!roomId) return;
      const wsAgent = agentSockets.get(roomId);
      if (wsAgent && wsAgent.readyState === WebSocket.OPEN) {
        // include who sent it for agent logging/debugging
        wsAgent.send(JSON.stringify({ action: "control", fromViewer: socket.id, payload }));
      }
    } catch (err) {
      console.error("Error forwarding control:", err);
    }
  });

  // agent (or any client) can emit screen-frame to be relayed (but our agent uses WS connection instead)
  socket.on("screen-frame", (frame) => {
    const meta = peers.get(socket.id) || {};
    const roomId = meta.roomId;
    if (!roomId) return;
    io.in(roomId).emit("screen-frame", {
      agentId: socket.id,
      image: frame.image,
      width: frame.width,
      height: frame.height
    });
  });

  socket.on("stop-share", ({ roomId, name }) => {
    if (roomId) {
      io.in(roomId).emit("stop-share", { name: name || peers.get(socket.id)?.name || "unknown" });
    }
  });

  socket.on("leave-room", ({ roomId }) => {
    try {
      const meta = peers.get(socket.id) || {};
      const r = roomId || meta.roomId;
      socket.leave(r);
      if (users.has(socket.id)) {
        const u = users.get(socket.id);
        u.room = null;
        users.set(socket.id, u);
      }
      socket.to(r).emit("peer-left", { id: socket.id });
      broadcastUserList();
    } catch (err) {
      console.error("leave-room error:", err);
    }
  });

  socket.on("disconnect", (reason) => {
    try {
      const meta = peers.get(socket.id) || {};
      const roomId = meta.roomId;
      peers.delete(socket.id);
      users.delete(socket.id);
      if (roomId) socket.to(roomId).emit("peer-left", { id: socket.id });
      broadcastUserList();
      console.log("❌ Browser disconnected:", socket.id, "reason:", reason);
    } catch (err) {
      console.error("disconnect error:", err);
    }
  });
});

// ====== WEBSOCKET SERVER for agent.exe (binary agent connects here) ======
const wss = new WebSocket.Server({ noServer: true });

// agent connects with: ws://HOST/ws-agent?room=<roomId>
wss.on("connection", (ws, req) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);
    const roomId = url.searchParams.get("room") || "room1";

    // store agent connection for this room
    agentSockets.set(roomId, ws);
    console.log(`🤖 Agent WS connected for room: ${roomId}`);

    // send a greeting/ack if agent expects it
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ action: "hello", roomId }));
    }

    ws.on("message", (msg) => {
      try {
        const data = JSON.parse(msg.toString());
        // Agent sends frames as: { type: "frame", image: "<base64>", width, height }
        if (data.type === "frame" && data.image) {
          io.in(roomId).emit("screen-frame", {
            agentId: `agent-${roomId}`,
            image: data.image,
            width: data.width || null,
            height: data.height || null
          });
        }
        // Agent may forward logs or ack; just log
        else if (data.type === "log") {
          console.log(`Agent[${roomId}] log:`, data.msg);
        } else {
          // unknown messages are forwarded to room for debugging if needed
        }
      } catch (err) {
        console.error("Agent message parse error:", err);
      }
    });

    ws.on("close", () => {
      console.log(`⚠️ Agent disconnected for room: ${roomId}`);
      // notify viewers that sharing stopped
      io.in(roomId).emit("stop-share", { name: "agent" });
      agentSockets.delete(roomId);
    });

    ws.on("error", (err) => {
      console.error("Agent WS error:", err);
    });
  } catch (err) {
    console.error("WS connection error:", err);
  }
});

// Upgrade HTTP -> WS for agent connections only at /ws-agent
server.on("upgrade", (req, socket, head) => {
  if (req.url && req.url.startsWith("/ws-agent")) {
    wss.handleUpgrade(req, socket, head, (ws) => {
      wss.emit("connection", ws, req);
    });
  } else {
    socket.destroy();
  }
});

// ====== START SERVER ======
server.listen(PORT, "0.0.0.0", () => {
  console.log(`🚀 Server running on port ${PORT}`);
  console.log(`🌐 WebSocket endpoint for agent: ws(s)://<host>/ws-agent?room=<roomId>`);
});
