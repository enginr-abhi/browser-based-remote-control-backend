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
app.use(cors());
app.use(express.json());

const server = http.createServer(app);
const io = new Server(server, { cors: { origin: "https://browser-based-remote-control-fronte.vercel.app/" } });

/*
  peers: socketId -> { name, roomId, isAgent, isSharing, captureInfo? }
  users: socketId -> { id, name, room, isOnline }
  tcpAgentsByRoom: roomId -> Set(tcpSocket)
*/
const peers = {};
const users = {};
const tcpAgentsByRoom = {};

// simple user list broadcast
function broadcastUserList() {
  const list = Object.entries(users).map(([id, u]) => ({
    id, name: u.name || "Unknown", roomId: u.room || "N/A", isOnline: !!u.isOnline
  }));
  io.emit("peer-list", list);
}

app.get("/", (req, res) => {
  res.send("Backend is LIVE ✅");
});

// download-agent (zip agent.exe + config.json)
app.get("/download-agent", (req, res) => {
  const roomId = req.query.room || "room1";
  const agentExe = path.join(__dirname, "agent", "agent.exe");
  if (!fs.existsSync(agentExe)) return res.status(404).send("Agent not found");

  try {
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'agent-'));
    const exePath = path.join(tmpDir, "remote-agent.exe");
    fs.copyFileSync(agentExe, exePath);
    fs.writeFileSync(path.join(tmpDir, "config.json"), JSON.stringify({ roomId }, null, 2));

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

// Socket.IO events
io.on("connection", socket => {
  console.log("Connected:", socket.id);

  socket.on("set-name", ({ name }) => {
    peers[socket.id] = { ...(peers[socket.id]||{}), name };
    users[socket.id] = { id: socket.id, name, room: peers[socket.id]?.roomId || null, isOnline: true };
    broadcastUserList();
  });

  socket.on("join-room", ({ roomId, name, isAgent = false }) => {
    peers[socket.id] = { ...(peers[socket.id]||{}), name, roomId, isAgent, isSharing: false };
    socket.join(roomId);
    users[socket.id] = { id: socket.id, name: name || peers[socket.id]?.name || "Unknown", room: roomId, isOnline: true };

    socket.to(roomId).emit("peer-joined", { id: socket.id, name: peers[socket.id].name, isAgent });
    broadcastUserList();
    console.log(`${name || 'Unknown'} joined room: ${roomId} (agent=${isAgent})`);
  });

  socket.on("get-peers", () => broadcastUserList());

  socket.on("leave-room", ({ roomId, name }) => {
    const actualRoom = roomId || peers[socket.id]?.roomId;
    if (actualRoom) {
      try { socket.leave(actualRoom); } catch (e) {}
      if (peers[socket.id]) peers[socket.id].roomId = null;
      if (users[socket.id]) { users[socket.id].room = null; users[socket.id].isOnline = false; }
      socket.to(actualRoom).emit("peer-left", { id: socket.id, name: name || peers[socket.id]?.name });
      broadcastUserList();
      console.log(`${name || peers[socket.id]?.name || socket.id} left room: ${actualRoom}`);
    }
  });

  socket.on("disconnect", () => {
    const meta = peers[socket.id] || {};
    delete peers[socket.id];
    delete users[socket.id];
    broadcastUserList();

    if (meta.roomId) {
      socket.to(meta.roomId).emit("peer-left", { id: socket.id });
      if (meta.isSharing) socket.to(meta.roomId).emit("stop-share");
    }
    console.log("Disconnected:", socket.id);
  });

  // request screen from controllers -> target UIs
  socket.on("request-screen", ({ roomId, from }) => {
    socket.to(roomId).emit("screen-request", { from, name: peers[socket.id]?.name });
  });

  // permission-response: accepted -> notify requester; instruct agents to start streaming
  socket.on("permission-response", ({ to, accepted }) => {
    if (accepted && peers[socket.id]) peers[socket.id].isSharing = true;
    io.to(to || socket.id).emit("permission-result", accepted);

    const meta = peers[socket.id];
    if (accepted && meta && meta.roomId) {
      // socket.io agents
      for (const [id, p] of Object.entries(peers)) {
        if (p.roomId === meta.roomId && p.isAgent) {
          io.to(id).emit("start-stream", { roomId: meta.roomId });
        }
      }
      // tcp agents
      const set = tcpAgentsByRoom[meta.roomId];
      if (set && set.size) {
        for (const tcpSock of Array.from(set)) {
          try { tcpSock.write(JSON.stringify({ type: 'start-stream', roomId: meta.roomId }) + '\n'); }
          catch (e) { console.warn('TCP start-stream write failed', e); }
        }
      }
    }
  });

  // stop-share -> notify all and tell agents to stop
  socket.on("stop-share", ({ roomId, name }) => {
    if (peers[socket.id]) peers[socket.id].isSharing = false;
    io.in(roomId).emit("stop-share", { name });

    for (const [id, p] of Object.entries(peers)) {
      if (p.roomId === roomId && p.isAgent) io.to(id).emit("stop-stream", { roomId });
    }
    const set = tcpAgentsByRoom[roomId];
    if (set && set.size) {
      for (const tcpSock of Array.from(set)) {
        try { tcpSock.write(JSON.stringify({ type: 'stop-stream', roomId }) + '\n'); } catch (e) {}
      }
    }
  });

  // WebRTC signaling relay (if you use it)
  socket.on("signal", ({ roomId, desc, candidate }) => {
    socket.to(roomId).emit("signal", { desc, candidate });
  });

  // capture-info forwarded to agents (optional)
  socket.on("capture-info", (info) => {
    peers[socket.id] = { ...(peers[socket.id]||{}), captureInfo: info, roomId: info.roomId };
    for (const [id, p] of Object.entries(peers)) {
      if (p.roomId === info.roomId && p.isAgent) io.to(id).emit("capture-info", info);
    }
  });

  // receive frames from agent (socket.io agent)
  socket.on("screen-frame", ({ roomId, agentId, image }) => {
    if (!roomId) return;
    // broadcast to all except sender
    socket.to(roomId).emit("screen-frame", { agentId, image });
  });

  // control events from controller -> forward to socket.io agents and tcp agents
  socket.on("control", (data) => {
    const { roomId } = peers[socket.id] || {};
    if (!roomId) return;

    // socket.io agents
    for (const [id, p] of Object.entries(peers)) {
      if (p.roomId === roomId && p.isAgent) io.to(id).emit("control", data);
    }
    // tcp agents
    const set = tcpAgentsByRoom[roomId];
    if (set && set.size) {
      const payload = JSON.stringify({ type: 'control', data }) + '\n';
      for (const tcpSock of Array.from(set)) {
        try { tcpSock.write(payload); } catch (e) { console.warn('TCP write failed', e); try { tcpSock.destroy(); } catch {} set.delete(tcpSock); }
      }
    }
  });
});

// -------------------- TCP agent server --------------------
const tcpAgentServer = net.createServer((tcpSocket) => {
  tcpSocket.setEncoding('utf8');
  let agentRoom = null;
  console.log('TCP agent connected from', tcpSocket.remoteAddress);

  // Expect a hello JSON message first
  tcpSocket.once('data', (chunk) => {
    try {
      const msg = chunk.toString().trim();
      const j = JSON.parse(msg);
      if (j.type === 'hello' && j.roomId) {
        agentRoom = j.roomId;
        tcpAgentsByRoom[agentRoom] = tcpAgentsByRoom[agentRoom] || new Set();
        tcpAgentsByRoom[agentRoom].add(tcpSocket);
        console.log(`TCP agent registered for room: ${agentRoom}`);
        tcpSocket.write(JSON.stringify({ type: 'hello-ack', roomId: agentRoom }) + '\n');
      } else {
        console.warn('Invalid tcp hello:', msg);
        tcpSocket.end();
      }
    } catch (e) {
      console.warn('Error parsing tcp hello:', e);
      tcpSocket.end();
    }
  });

  tcpSocket.on('data', (data) => {
    try {
      const t = data.toString().trim();
      if (t.length) console.log('From TCP Agent (raw):', t);
      // Optionally parse JSON lines from agent for frames/control etc.
    } catch (e) { console.warn(e); }
  });

  tcpSocket.on('close', () => {
    console.log('TCP agent closed', tcpSocket.remoteAddress);
    if (agentRoom && tcpAgentsByRoom[agentRoom]) {
      tcpAgentsByRoom[agentRoom].delete(tcpSocket);
      if (tcpAgentsByRoom[agentRoom].size === 0) delete tcpAgentsByRoom[agentRoom];
    }
  });

  tcpSocket.on('error', (err) => console.log('TCP agent socket error', err));
});

tcpAgentServer.listen(3001, () => console.log("Agent TCP server listening on 3001"));

// start http/socket server
server.listen(PORT, () => console.log(`Server running on http://localhost:${PORT}`));
