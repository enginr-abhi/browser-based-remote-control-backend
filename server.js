// server.js
const http = require('http');
const express = require("express");
const { Server } = require("socket.io");
const cors = require("cors");
const path = require('path');
const os = require("os");
const crypto = require("crypto");
const archiver = require("archiver");
const fs = require('fs');
const net = require("net");
const PORT = process.env.PORT || 9000;

const app = express();
const server = http.createServer(app);

app.use(cors());

app.get("/", (req, res) => {
  res.send("Backend is LIVE ✅, version: 5 (dynamic agent room)");
});

app.get("/download-agent", async (req, res) => {
  const roomId = req.query.room || "room1";
  const agentExe = path.join(__dirname, "agent", "agent.exe");

  if (!fs.existsSync(agentExe)) {
    return res.status(404).send("Agent.exe not found on server");
  }

  try {
    const tmpDir = path.join(os.tmpdir(), `agent_${crypto.randomBytes(4).toString("hex")}`);
    fs.mkdirSync(tmpDir);

    const exePath = path.join(tmpDir, "remote-agent.exe");
    fs.copyFileSync(agentExe, exePath);

    const configPath = path.join(tmpDir, "config.json");
    fs.writeFileSync(configPath, JSON.stringify({ roomId }, null, 2));

    res.setHeader("Content-Type", "application/zip");
    res.setHeader("Content-Disposition", `attachment; filename=remote-agent-${roomId}.zip`);

    const archive = archiver("zip", { zlib: { level: 9 } });
    archive.pipe(res);
    archive.directory(tmpDir, false);
    await archive.finalize();

    console.log(`✅ Agent ready for room: ${roomId}`);
  } catch (err) {
    console.error("❌ Error:", err);
    res.status(500).send("Error preparing agent");
  }
});

const io = new Server(server, {
  cors: { origin: "https://browser-based-remote-control-fronte.vercel.app/", methods: ["GET", "POST"] }
});

const peers = {}; // socketId -> { name, roomId, isAgent, isSharing, captureInfo? }
const users = {}; // socketId -> { id, name, room, isOnline }

// Helper: broadcast full user list to everyone
function broadcastUserList() {
  const userList = Object.entries(users).map(([id, u]) => ({
    id,
    name: u.name || "Unknown",
    roomId: u.room || "N/A",
    isOnline: !!u.isOnline
  }));
  io.emit("peer-list", userList);
}

io.on("connection", socket => {
  console.log("Connected:", socket.id);

  socket.on("set-name", ({ name }) => {
    peers[socket.id] = { ...peers[socket.id], name };
    users[socket.id] = { id: socket.id, name, room: peers[socket.id]?.roomId || null, isOnline: true };
    io.emit("update-users", Object.values(users));
    broadcastUserList();
  });

  socket.on("join-room", ({ roomId, name, isAgent = false }) => {
    peers[socket.id] = { ...peers[socket.id], name, roomId, isAgent, isSharing: false };
    socket.join(roomId);
    users[socket.id] = { id: socket.id, name: name || peers[socket.id]?.name || "Unknown", room: roomId, isOnline: true };

    socket.to(roomId).emit("peer-joined", { id: socket.id, name: peers[socket.id].name, isAgent });
    io.emit("update-users", Object.values(users));
    broadcastUserList();

    console.log(`👤 ${name || 'Unknown'} joined room: ${roomId} (Agent: ${isAgent})`);
  });

  socket.on("get-peers", () => broadcastUserList());

  socket.on("leave-room", ({ roomId, name }) => {
    const actualRoom = roomId || (peers[socket.id] && peers[socket.id].roomId);
    if (actualRoom) {
      try { socket.leave(actualRoom); } catch (e) {}
      if (peers[socket.id]) peers[socket.id].roomId = null;
      if (users[socket.id]) { users[socket.id].room = null; users[socket.id].isOnline = false; }

      socket.to(actualRoom).emit("peer-left", { id: socket.id, name: name || peers[socket.id]?.name });
      io.emit("update-users", Object.values(users));
      broadcastUserList();

      console.log(`🚪 ${name || peers[socket.id]?.name || socket.id} left room: ${actualRoom}`);
    }
  });

  socket.on("disconnect", () => {
    const { roomId, isSharing } = peers[socket.id] || {};
    delete peers[socket.id];
    delete users[socket.id];

    io.emit("update-users", Object.values(users));
    broadcastUserList();

    if (roomId) {
      socket.to(roomId).emit("peer-left", { id: socket.id });
      if (isSharing) socket.to(roomId).emit("stop-share");
    }
    console.log(`❌ Disconnected: ${socket.id}`);
  });

  // Screen request & permission
  socket.on("request-screen", ({ roomId, from }) => {
    socket.to(roomId).emit("screen-request", { from, name: peers[socket.id]?.name });
  });

  socket.on("permission-response", ({ to, accepted }) => {
    if (accepted && peers[socket.id]) peers[socket.id].isSharing = true;
    io.to(to).emit("permission-result", accepted);
  });

  socket.on("stop-share", ({ roomId, name }) => {
    if (peers[socket.id]) peers[socket.id].isSharing = false;
    io.in(roomId).emit("stop-share", { name });
  });

  // WebRTC signaling
  socket.on("signal", ({ roomId, desc, candidate }) => {
    socket.to(roomId).emit("signal", { desc, candidate });
  });

  // Capture info
  socket.on("capture-info", info => {
    peers[socket.id] = { ...peers[socket.id], captureInfo: info, roomId: info.roomId };
    for (const [id, p] of Object.entries(peers)) {
      if (p.roomId === info.roomId && p.isAgent) io.to(id).emit("capture-info", info);
    }
  });

  // Remote control events (socket.io + TCP agents)
  socket.on("control", data => {
    const { roomId } = peers[socket.id] || {};
    if (!roomId) return;

    // Forward to socket.io agents (existing behavior)
    for (const [id, p] of Object.entries(peers)) {
      if (p.roomId === roomId && p.isAgent) io.to(id).emit("control", data);
    }

    // Forward to native TCP agents if present
    const list = tcpAgentsByRoom[roomId];
    if (list && list.size) {
      const payload = JSON.stringify({ type: 'control', data }) + '\n';
      for (const tcpSocket of Array.from(list)) {
        try {
          tcpSocket.write(payload);
        } catch (e) {
          console.warn('Failed to write to TCP agent, removing', e);
          try { tcpSocket.destroy(); } catch {}
          list.delete(tcpSocket);
        }
      }
    }
  });
});

// -------------------- TCP Agent server --------------------
const tcpAgentsByRoom = {}; // roomId -> Set of tcp sockets

const tcpAgentServer = net.createServer((tcpSocket) => {
  tcpSocket.setEncoding('utf8');
  let agentRoom = null;

  console.log('TCP agent connected from', tcpSocket.remoteAddress);

  // Expect first message to be a join/hello JSON like: {"type":"hello","roomId":"room1"}
  tcpSocket.once('data', (chunk) => {
    try {
      const msg = chunk.toString().trim();
      const j = JSON.parse(msg);
      if (j.type === 'hello' && j.roomId) {
        agentRoom = j.roomId;
        tcpAgentsByRoom[agentRoom] = tcpAgentsByRoom[agentRoom] || new Set();
        tcpAgentsByRoom[agentRoom].add(tcpSocket);
        console.log(`✅ TCP agent registered for room: ${agentRoom}`);
        tcpSocket.write(JSON.stringify({ type: 'hello-ack', roomId: agentRoom }) + '\n');
      } else {
        console.warn('⚠️ TCP agent sent invalid hello:', msg);
        tcpSocket.end();
      }
    } catch (e) {
      console.warn('⚠️ Error parsing tcp hello:', e);
      tcpSocket.end();
    }
  });

  tcpSocket.on('data', (data) => {
    try {
      const trimmed = data.toString().trim();
      if (trimmed.length) console.log('📩 From Agent (raw):', trimmed);
    } catch {}
  });

  tcpSocket.on('close', () => {
    console.log('TCP agent connection closed', tcpSocket.remoteAddress);
    if (agentRoom && tcpAgentsByRoom[agentRoom]) {
      tcpAgentsByRoom[agentRoom].delete(tcpSocket);
      if (tcpAgentsByRoom[agentRoom].size === 0) delete tcpAgentsByRoom[agentRoom];
    }
  });

  tcpSocket.on('error', (err) => {
    console.log('TCP agent socket error', err);
  });
});

tcpAgentServer.listen(3001, () => {
  console.log("🧠 Agent TCP server running on port 3001");
});

// start http/socket.io server

server.listen(PORT, '0.0.0.0',() => {
  console.log(`Server running on http://0.0.0.0:${PORT}`);
});
