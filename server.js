// ✅ FIXED server.js (Render compatible)
const http = require('http');
const express = require("express");
const { Server } = require("socket.io");
const cors = require("cors");
const path = require('path');
const os = require("os");
const archiver = require("archiver");
const fs = require('fs');

const PORT = process.env.PORT || 9000;
const app = express();
app.use(cors());
app.use(express.json());

const server = http.createServer(app);
const io = new Server(server, {
  cors: {
    origin: "https://browser-based-remote-control-fronte.vercel.app",
    methods: ["GET", "POST"]
  }
});

// ---------------------- In-memory state ----------------------
const peers = {};
const users = {};

// simple user list broadcast
function broadcastUserList() {
  const list = Object.entries(users).map(([id, u]) => ({
    id,
    name: u.name || "Unknown",
    roomId: u.room || "N/A",
    isOnline: !!u.isOnline
  }));
  io.emit("peer-list", list);
}

// ✅ Root route for Render health check
app.get("/", (req, res) => {
  res.send("✅ Backend is LIVE on Render!");
});

// ✅ Download agent (bundle with room config)
app.get("/download-agent", (req, res) => {
  const roomId = req.query.room || "room1";
  const agentExe = path.join(__dirname, "agent", "agent.exe");
  if (!fs.existsSync(agentExe)) return res.status(404).send("Agent not found");

  try {
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'agent-'));
    const exePath = path.join(tmpDir, "remote-agent.exe");
    fs.copyFileSync(agentExe, exePath);
    fs.writeFileSync(
      path.join(tmpDir, "config.json"),
      JSON.stringify({ roomId }, null, 2)
    );

    res.setHeader("Content-Type", "application/zip");
    res.setHeader("Content-Disposition", `attachment; filename=remote-agent-${roomId}.zip`);
    const archive = archiver("zip", { zlib: { level: 9 } });
    archive.pipe(res);
    archive.file(exePath, { name: "agent.exe" });
    archive.file(path.join(tmpDir, "config.json"), { name: "config.json" });
    archive.finalize();
  } catch (err) {
    console.error("Error preparing agent:", err);
    res.status(500).send("Error preparing agent");
  }
});

// ---------------------- SOCKET.IO HANDLERS ----------------------
io.on("connection", (socket) => {
  console.log("🔌 Connected:", socket.id);

  socket.on("set-name", ({ name }) => {
    peers[socket.id] = { ...(peers[socket.id] || {}), name };
    users[socket.id] = { id: socket.id, name, room: peers[socket.id]?.roomId || null, isOnline: true };
    broadcastUserList();
  });

  socket.on("join-room", ({ roomId, name, isAgent = false }) => {
    peers[socket.id] = { ...(peers[socket.id] || {}), name, roomId, isAgent, isSharing: false };
    socket.join(roomId);
    users[socket.id] = { id: socket.id, name, room: roomId, isOnline: true };

    socket.to(roomId).emit("peer-joined", { id: socket.id, name, isAgent });
    broadcastUserList();
    console.log(`${name || 'Unknown'} joined room: ${roomId} (agent=${isAgent})`);
  });

  socket.on("get-peers", () => broadcastUserList());

  socket.on("leave-room", ({ roomId, name }) => {
    const actualRoom = roomId || peers[socket.id]?.roomId;
    if (actualRoom) {
      socket.leave(actualRoom);
      if (peers[socket.id]) peers[socket.id].roomId = null;
      if (users[socket.id]) users[socket.id].isOnline = false;
      socket.to(actualRoom).emit("peer-left", { id: socket.id, name });
      broadcastUserList();
      console.log(`${name || socket.id} left room: ${actualRoom}`);
    }
  });

  socket.on("disconnect", () => {
    const meta = peers[socket.id] || {};
    delete peers[socket.id];
    delete users[socket.id];
    broadcastUserList();
    if (meta.roomId) socket.to(meta.roomId).emit("peer-left", { id: socket.id });
    console.log("❌ Disconnected:", socket.id);
  });

  // Controller requests screen access
  socket.on("request-screen", ({ roomId, from }) => {
    socket.to(roomId).emit("screen-request", { from, name: peers[socket.id]?.name });
  });

  // Permission granted/denied
  socket.on("permission-response", ({ to, accepted }) => {
    if (accepted && peers[socket.id]) peers[socket.id].isSharing = true;
    io.to(to).emit("permission-result", accepted);

    const meta = peers[socket.id];
    if (accepted && meta?.roomId) {
      for (const [id, p] of Object.entries(peers)) {
        if (p.roomId === meta.roomId && p.isAgent) io.to(id).emit("start-stream", { roomId: meta.roomId });
      }
    }
  });

  socket.on("stop-share", ({ roomId, name }) => {
    if (peers[socket.id]) peers[socket.id].isSharing = false;
    io.in(roomId).emit("stop-share", { name });
    for (const [id, p] of Object.entries(peers)) {
      if (p.roomId === roomId && p.isAgent) io.to(id).emit("stop-stream", { roomId });
    }
  });

  socket.on("signal", ({ roomId, desc, candidate }) => {
    socket.to(roomId).emit("signal", { desc, candidate });
  });

  socket.on("capture-info", (info) => {
    peers[socket.id] = { ...(peers[socket.id] || {}), captureInfo: info, roomId: info.roomId };
    for (const [id, p] of Object.entries(peers)) {
      if (p.roomId === info.roomId && p.isAgent) io.to(id).emit("capture-info", info);
    }
  });

  socket.on("screen-frame", ({ roomId, agentId, image }) => {
    if (roomId) socket.to(roomId).emit("screen-frame", { agentId, image });
  });

  socket.on("control", (data) => {
    const { roomId } = peers[socket.id] || {};
    if (!roomId) return;
    for (const [id, p] of Object.entries(peers)) {
      if (p.roomId === roomId && p.isAgent) io.to(id).emit("control", data);
    }
  });
});

// ---------------------- Start HTTP server ----------------------
server.listen(PORT, '0.0.0.0', () => {
  console.log(`🚀 Server running on http://0.0.0.0:${PORT}`);
});
