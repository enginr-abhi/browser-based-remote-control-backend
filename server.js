const http = require('http');
const express = require("express");
const { Server } = require("socket.io");
const cors = require("cors");
const path = require('path');
const fs = require('fs');
const PORT = process.env.PORT || 9000;

const app = express();
const server = http.createServer(app);

app.use(cors());

app.get("/", (req, res) => {
  res.send("Backend is LIVE ✅, version: 5 (dynamic agent room)");
});

// ✅ Dynamic agent download (with room info)
app.get("/download-agent", (req, res) => {
  const roomId = req.query.room || "room1";
  const agentDir = path.join(__dirname, "agent");

  // Write config.json dynamically for the agent
  try {
    const configPath = path.join(agentDir, "config.json");
    fs.writeFileSync(configPath, JSON.stringify({ roomId }, null, 2));
    console.log(`📝 Created config.json for room: ${roomId}`);
  } catch (err) {
    console.error("⚠️ Failed to write config.json:", err);
  }

  const filePath = path.join(agentDir, "agent.exe");
  res.download(filePath, "remote-agent.exe", err => {
    if (err) {
      console.error("Download error:", err);
      res.status(500).send("File not found");
    }
  });
});

const io = new Server(server, {
  cors: { origin: "*", methods: ["GET", "POST"] }
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

  // set-name: store name, mark online
  socket.on("set-name", ({ name }) => {
    peers[socket.id] = { ...peers[socket.id], name };
    users[socket.id] = { id: socket.id, name, room: peers[socket.id]?.roomId || null, isOnline: true };
    io.emit("update-users", Object.values(users));
    broadcastUserList();
  });

  // join-room: put socket in room, mark user online and in room
  socket.on("join-room", ({ roomId, name, isAgent = false }) => {
    peers[socket.id] = { ...peers[socket.id], name, roomId, isAgent, isSharing: false };
    socket.join(roomId);
    users[socket.id] = { id: socket.id, name: name || peers[socket.id]?.name || "Unknown", room: roomId, isOnline: true };

    socket.to(roomId).emit("peer-joined", { id: socket.id, name: peers[socket.id].name, isAgent });
    io.emit("update-users", Object.values(users));
    broadcastUserList();

    console.log(`👤 ${name || 'Unknown'} joined room: ${roomId} (Agent: ${isAgent})`);
  });

  // Client requests the latest peers
  socket.on("get-peers", () => broadcastUserList());

  // leave-room: user voluntarily leaves the room (stays connected)
  socket.on("leave-room", ({ roomId, name }) => {
    const actualRoom = roomId || (peers[socket.id] && peers[socket.id].roomId);
    if (actualRoom) {
      // make socket leave the room
      try { socket.leave(actualRoom); } catch (e) { /* ignore */ }

      // update server state
      if (peers[socket.id]) {
        peers[socket.id].roomId = null;
      }
      if (users[socket.id]) {
        users[socket.id].room = null;
        // mark offline for presence dot (you wanted offline presence after leaving)
        users[socket.id].isOnline = false;
      }

      // notify the room that the peer left
      socket.to(actualRoom).emit("peer-left", { id: socket.id, name: name || peers[socket.id]?.name });

      // broadcast updated user list to everyone
      io.emit("update-users", Object.values(users));
      broadcastUserList();

      console.log(`🚪 ${name || peers[socket.id]?.name || socket.id} left room: ${actualRoom}`);
    }
  });

  // disconnect: remove peer and mark offline / clean up
  socket.on("disconnect", () => {
    const { roomId, isSharing } = peers[socket.id] || {};
    // Remove server-side records for the socket
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

  // ---- Screen request & permission ----
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

  // ---- WebRTC signaling ----
  socket.on("signal", ({ roomId, desc, candidate }) => {
    socket.to(roomId).emit("signal", { desc, candidate });
  });

  // ---- Capture info (resolution & scaling) ----
  socket.on("capture-info", info => {
    peers[socket.id] = { ...peers[socket.id], captureInfo: info, roomId: info.roomId };
    for (const [id, p] of Object.entries(peers)) {
      if (p.roomId === info.roomId && p.isAgent) io.to(id).emit("capture-info", info);
    }
  });

  // ---- Remote control events ----
  socket.on("control", data => {
    const { roomId } = peers[socket.id] || {};
    if (!roomId) return;
    for (const [id, p] of Object.entries(peers)) {
      if (p.roomId === roomId && p.isAgent) io.to(id).emit("control", data);
    }
  });
});

server.listen(PORT, '0.0.0.0',() => {
  console.log(`Server running on http://0.0.0.0:${PORT}`);
});