// ✅ FINAL SERVER — Supports Browser + C++ Agent
// ==================================================

const http = require("http");
const express = require("express");
const { Server } = require("socket.io");
const cors = require("cors");
const path = require("path");
const os = require("os");
const fs = require("fs");
const archiver = require("archiver");
const WebSocket = require("ws"); // for agent.exe

// ====== CONFIG ======
const PORT = process.env.PORT || 9000;
const FRONTEND_ORIGIN = "https://browser-based-remote-control-fronte.vercel.app";

// ====== EXPRESS + HTTP ======
const app = express();
app.use(cors({ origin: FRONTEND_ORIGIN }));
app.use(express.json());
const server = http.createServer(app);

// ====== SOCKET.IO (Browser) ======
const io = new Server(server, {
  cors: {
    origin: FRONTEND_ORIGIN,
    methods: ["GET", "POST"],
    credentials: true
  },
});

// ====== STATE ======
const peers = {}; // socket.id -> meta
const users = {}; // socket.id -> { id, name, room, isOnline }
const agentSockets = {}; // roomId -> ws connection (C++ agent)

// ====== HELPERS ======
function broadcastUserList() {
  const list = Object.entries(users).map(([id, u]) => ({
    id,
    name: u.name || "Unknown",
    roomId: u.room || "N/A",
    isOnline: !!u.isOnline,
  }));
  io.emit("peer-list", list);
}

// ====== HEALTH CHECK ======
app.get("/", (req, res) => {
  res.send("✅ Backend running with WebSocket Agent support");
});

// ====== AGENT DOWNLOAD ======
app.get("/download-agent", (req, res) => {
  const roomId = req.query.room || "room1";
  const agentExe = path.join(__dirname, "agent", "agent.exe");
  if (!fs.existsSync(agentExe)) return res.status(404).send("❌ Agent not found.");

  try {
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "agent-"));
    const exeCopy = path.join(tmpDir, "remote-agent.exe");
    fs.copyFileSync(agentExe, exeCopy);
    fs.writeFileSync(
      path.join(tmpDir, "config.json"),
      JSON.stringify({ roomId }, null, 2)
    );

    res.setHeader("Content-Type", "application/zip");
    res.setHeader(
      "Content-Disposition",
      `attachment; filename=remote-agent-${roomId}.zip`
    );

    const archive = archiver("zip", { zlib: { level: 9 } });
    archive.pipe(res);
    archive.file(exeCopy, { name: "agent.exe" });
    archive.file(path.join(tmpDir, "config.json"), { name: "config.json" });
    archive.finalize();
  } catch (err) {
    console.error("⚠️ Error preparing agent:", err);
    res.status(500).send("Error preparing agent");
  }
});

// ====== SOCKET.IO HANDLERS ======
io.on("connection", (socket) => {
  console.log("🔌 Browser connected:", socket.id);

  socket.on("set-name", ({ name }) => {
    peers[socket.id] = { ...(peers[socket.id] || {}), name };
    users[socket.id] = {
      id: socket.id,
      name,
      room: peers[socket.id]?.roomId || null,
      isOnline: true,
    };
    broadcastUserList();
  });

  socket.on("join-room", ({ roomId, name }) => {
    peers[socket.id] = { ...(peers[socket.id] || {}), name, roomId };
    socket.join(roomId);
    users[socket.id] = { id: socket.id, name, room: roomId, isOnline: true };
    socket.to(roomId).emit("peer-joined", { id: socket.id, name });
    broadcastUserList();
  });

  socket.on("request-screen", ({ roomId, from }) => {
    socket.to(roomId).emit("screen-request", {
      from,
      name: peers[socket.id]?.name,
    });
  });

  socket.on("permission-response", ({ to, accepted }) => {
    io.to(to).emit("permission-result", accepted);

    if (accepted) {
      const meta = peers[socket.id];
      const roomId = meta?.roomId;
      // Tell connected C++ agent to start streaming
      const wsAgent = agentSockets[roomId];
      if (wsAgent && wsAgent.readyState === WebSocket.OPEN) {
        wsAgent.send(JSON.stringify({ action: "start-stream" }));
      }
    }
  });

  socket.on("control", (data) => {
    const meta = peers[socket.id];
    if (!meta?.roomId) return;
    const wsAgent = agentSockets[meta.roomId];
    if (wsAgent && wsAgent.readyState === WebSocket.OPEN) {
      wsAgent.send(JSON.stringify({ action: "control", data }));
    }
  });

  socket.on("disconnect", () => {
    const meta = peers[socket.id] || {};
    delete peers[socket.id];
    delete users[socket.id];
    broadcastUserList();
    console.log("❌ Browser disconnected:", socket.id);
  });
});

// ====== WEBSOCKET SERVER for agent.exe ======
const wss = new WebSocket.Server({ noServer: true });

wss.on("connection", (ws, req) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  const roomId = url.searchParams.get("room") || "room1";
  agentSockets[roomId] = ws;
  console.log(`🤖 Agent connected for room: ${roomId}`);

  ws.on("message", (msg) => {
    try {
      const data = JSON.parse(msg.toString());
      if (data.type === "frame") {
        io.in(roomId).emit("screen-frame", {
          agentId: "agent-" + roomId,
          image: data.image,
        });
      }
    } catch (err) {
      console.error("Agent message error:", err);
    }
  });

  ws.on("close", () => {
    console.log(`⚠️ Agent disconnected for room: ${roomId}`);
    delete agentSockets[roomId];
  });
});

// ====== UPGRADE HTTP -> WS ======
server.on("upgrade", (req, socket, head) => {
  if (req.url.startsWith("/ws-agent")) {
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
  console.log("🌐 WebSocket endpoint ready at /ws-agent");
});
