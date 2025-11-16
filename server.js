// server.js
const http = require('http');
const express = require("express");
const { Server } = require("socket.io");
const cors = require("cors");
const path = require('path');
const fs = require('fs');
const archiver = require('archiver');
const PORT = process.env.PORT || 9000;

const app = express();
const server = http.createServer(app);

app.use(cors());
app.use(express.static(path.join(__dirname, "public"))); // optional serve frontend

app.get("/", (req, res) => {
  res.send("Backend is LIVE ✅ (agent-frame forwarder)");
});

// Serve a zip: agent.exe + config.json
app.get("/download-agent", (req, res) => {
  const roomId = req.query.room || "room1";
  const agentDir = path.join(__dirname, "agent");
  const exePath = path.join(agentDir, "agent.exe");
  if (!fs.existsSync(exePath)) return res.status(404).send("Agent binary not found");

  res.attachment(`remote-agent-${roomId}.zip`);
  const archive = archiver('zip', { zlib: { level: 9 } });
  archive.on('error', err => { console.error(err); res.status(500).end(); });
  archive.pipe(res);
  archive.file(exePath, { name: "agent.exe" });
  archive.append(JSON.stringify({ roomId }, null, 2), { name: "config.json" });
  archive.finalize();
});

const io = new Server(server, {
  cors: { origin: "*", methods: ["GET", "POST"] },
  maxHttpBufferSize: 10 * 1024 * 1024
});

const peers = {}; // socketId -> { name, roomId, isAgent, isSharing, captureInfo? }
const users = {}; // socketId -> { id, name, room, isOnline }
const allowedControllers = {}; // agentSocketId -> viewerSocketId

function broadcastUserList() {
  const userList = Object.entries(peers).map(([id, p]) => ({
    id,
    name: p.name || "Unknown",
    roomId: p.roomId || "N/A",
    isOnline: true,
    isAgent: !!p.isAgent
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
    if (!actualRoom) return;
    try { socket.leave(actualRoom); } catch (e) {}
    if (peers[socket.id]) peers[socket.id].roomId = null;
    if (users[socket.id]) { users[socket.id].room = null; users[socket.id].isOnline = false; }
    socket.to(actualRoom).emit("peer-left", { id: socket.id, name: name || peers[socket.id]?.name });
    io.emit("update-users", Object.values(users));
    broadcastUserList();
    console.log(`🚪 ${name || peers[socket.id]?.name || socket.id} left room: ${actualRoom}`);
  });

  socket.on("disconnect", () => {
    // cleanup
    delete peers[socket.id];
    delete users[socket.id];
    // remove allowedControllers referencing this socket
    delete allowedControllers[socket.id];
    for (const [aid, vid] of Object.entries(allowedControllers)) if (vid === socket.id) delete allowedControllers[aid];

    io.emit("update-users", Object.values(users));
    broadcastUserList();
    console.log(`❌ Disconnected: ${socket.id}`);
  });

  // Viewer requests owner/agent to share
  socket.on("request-screen", ({ roomId, from }) => {
    // Find first non-agent user in the room other than requester (owner)
    const ownerEntry = Object.entries(peers).find(([id, p]) => p.roomId === roomId && !p.isAgent && id !== from);
    if (ownerEntry) {
      const [ownerId] = ownerEntry;
      io.to(ownerId).emit("screen-request", { from, name: peers[from]?.name || "Viewer" });
      console.log(`➡ screen-request from ${from} to owner ${ownerId} in room ${roomId}`);
    } else {
      // If no owner found, fallback: try any non-requester socket in room
      const anyEntry = Object.entries(peers).find(([id, p]) => p.roomId === roomId && id !== from);
      if (anyEntry) {
        const [someId] = anyEntry;
        io.to(someId).emit("screen-request", { from, name: peers[from]?.name || "Viewer" });
        console.log(`⚠ No explicit owner found; forwarded screen-request from ${from} to ${someId} in room ${roomId}`);
      } else {
        io.to(from).emit("no-agent", { message: "No owner present in that room to accept your request." });
        console.log(`⚠ No potential recipient for screen-request from ${from} in room ${roomId}`);
      }
    }
  });

  // Owner responds (accept/deny)
  socket.on("permission-response", ({ to, accepted }) => {
    // 'socket' here is the owner who clicked accept/reject
    if (accepted && peers[socket.id]) peers[socket.id].isSharing = true;

    // Notify the requesting viewer
    io.to(to).emit("permission-result", accepted);

    // If accepted, find agents in that room
    if (accepted) {
      const roomId = peers[socket.id] && peers[socket.id].roomId;
      const agents = Object.entries(peers).filter(([id,p]) => p.roomId === roomId && p.isAgent).map(([id]) => id);
      if (agents.length === 0) {
        // No agent present — tell the viewer (requester) there's no agent
        io.to(to).emit("no-agent", { message: "No agent present. Please ask owner to download and run the agent." });
        // Also notify the owner so they can offer download link (owner-only)
        io.to(socket.id).emit("offer-download-agent", { roomId });
        console.log(`ℹ No agent in room ${roomId} — notified viewer ${to} and owner ${socket.id}`);
      } else {
        // grant control: pick first agent
        const agentId = agents[0];

        // If that agent was already assigned to another viewer, revoke that previous viewer first
        const prevViewer = allowedControllers[agentId];
        if (prevViewer && prevViewer !== to) {
          // tell previous viewer and agent
          io.to(prevViewer).emit("revoke-control", {});
          console.log(`↩ Revoked previous viewer ${prevViewer} for agent ${agentId}`);
        }

        allowedControllers[agentId] = to;
        io.to(agentId).emit("grant-control", { viewerId: to });
        console.log(`✅ Granted control for agent ${agentId} to viewer ${to} (room ${roomId})`);
        // emit token to viewer (optional)
        const token = Math.random().toString(36).slice(2,10);
        io.to(to).emit("control-token", token);
      }
    } else {
      // denied => revoke any controllers in same room
      const roomId = peers[socket.id] && peers[socket.id].roomId;
      for (const [aid, p] of Object.entries(peers)) {
        if (p.roomId === roomId && p.isAgent && allowedControllers[aid]) {
          delete allowedControllers[aid];
          io.to(aid).emit("revoke-control", {});
        }
      }
      console.log(`❌ Permission denied by ${socket.id} for request to ${to} in room ${roomId}`);
    }
  });

  // Agent sends capture info (optional)
  socket.on("capture-info", info => {
    peers[socket.id] = { ...peers[socket.id], captureInfo: info, roomId: info.roomId };
    // forward to viewers in same room (so they can update UI if needed)
    for (const [id,p] of Object.entries(peers)) {
      if (p.roomId === info.roomId && !p.isAgent) io.to(id).emit("capture-info", info);
    }
  });

  // Agent -> server: frame (binary)
  socket.on("frame", (buffer) => {
    const agentId = socket.id;
    const viewerId = allowedControllers[agentId];
    if (viewerId) {
      // Only forward frames to the specifically allowed viewer
      io.to(viewerId).emit("frame", buffer);
    } else {
      // No viewer allowed — DO NOT broadcast frames to entire room.
      // Keep this silent to avoid leaking screens to everyone.
      // console.log(`Frame received from ${agentId} but no allowed viewer set.`);
    }
  });

  // Viewer -> server: control events => forward only to allowed agent(s)
  socket.on("control", (data) => {
    const { roomId } = peers[socket.id] || {};
    if (!roomId) return;
    for (const [agentId, p] of Object.entries(peers)) {
      if (p.roomId === roomId && p.isAgent && allowedControllers[agentId] === socket.id) {
        io.to(agentId).emit("control", data);
      }
    }
  });

  // resume-with-token (viewer reconnect)
  socket.on("resume-with-token", ({ token }) => {
    // simplistic: not implemented mapping tokens -> agent (left as extension)
    socket.emit("resume-result", { ok: false, reason: "not-supported" });
  });

});

server.listen(PORT, '0.0.0.0',() => {
  console.log(`Server running on http://0.0.0.0:${PORT}`);
});
