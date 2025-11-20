const http = require("http");
const express = require("express");
const { Server } = require("socket.io");
const WebSocket = require("ws");
const path = require("path");
const fs = require("fs");
const PORT = process.env.PORT || 9000;

const app = express();
// --- NEW GLOBAL MAP FOR TARGETING ---
// Key: Agent's Room ID (User 2 ka room) -> Value: Viewer's Socket ID (User 1 ka ID)
let viewerMap = new Map(); 

// ✅ FIX 1: Frontend serving start hone ke baad / route ko index.html par redirect karna zaroori hai.
app.get("/", (req, res) => {
    res.send("Backend is LIVE ✅, version: 5 (dynamic agent room)");
});

// --- DOWNLOAD ROUTE ---
app.get("/download-agent", (req, res) => {
    // 1. Path is confirmed: __dirname (backend) + 'agent' folder + 'agent.exe'
    const agentFilePath = path.join(__dirname, "agent", "agent.exe");

    // 2. CHECK: Log the final path the server sees
    console.log(`[DOWNLOAD] Checking path: ${agentFilePath}`);

    // 3. CHECK: Manually verify if the file exists before sending
    if (!fs.existsSync(agentFilePath)) {
        console.error(`[DOWNLOAD ERROR] File NOT FOUND at: ${agentFilePath}`);
        return res
            .status(404)
            .send(
                "File not available on site. Please ensure agent.exe is compiled and located in the /backend/agent/ folder."
            );
    }
    
    // 4. Send the file with the correct name "RemoteAgent.exe"
    res.download(agentFilePath, "RemoteAgent.exe", (err) => {
        if (err) {
            // FIX 3: Agar download fail hota hai, to permission ya lock issue hota hai. 
            // 404 status galat hai. Bas error log karna kafi hai.
            console.error("[DOWNLOAD ERROR] Failed to send file (Permission/Lock):", err.message);
        } else {
            console.log("[DOWNLOAD] File sent successfully.");
        }
    });
});

const server = http.createServer(app);
const io = new Server(server, { 
    cors: {  
      origin: "https://browser-based-remote-control-fronte.vercel.app" , 
      methods: ["GET", "POST"]
    }
});

// Raw WebSocket server (FOR AGENT)
const wss = new WebSocket.Server({ noServer: true });

let agents = new Map(); // roomId → agentSocket

// Handle WS upgrade
server.on("upgrade", (req, socket, head) => {
    if (req.url.startsWith("/agent")) {
        wss.handleUpgrade(req, socket, head, (ws) => {
            wss.emit("connection", ws, req);
        });
    }
});

// AGENT CONNECT
wss.on("connection", (ws, req) => {
    let roomId = new URL(req.url, `http://${req.headers.host}`).searchParams.get(
        "room"
    );
    console.log("Agent joined room:", roomId);

    agents.set(roomId, ws);

    ws.on("message", (msg) => {
        // 🛑 FIX: Room mein broadcast karne ke bajaye, sirf Viewer ko bhej rahe hain.
        let viewerSocketId = viewerMap.get(roomId); 
        
        if (viewerSocketId) {
            io.to(viewerSocketId).emit("agent-frame", msg);
        }
    });

    ws.on("close", () => {
        agents.delete(roomId);
        viewerMap.delete(roomId); // Connection close hone par map se bhi hata do
    });
});

// ✅ FIX 2: EXPRESS STATIC (CRITICAL! Uncomment aur path fix kiya)
// Frontend files (/frontend) parent directory (..) mein hain.
// app.use(express.static(path.join(__dirname, '..', 'frontend')));

// SOCKET.IO (browser)
io.on("connection", (socket) => {
    console.log("User connected:", socket.id);

    socket.on("join", ({ name, room }) => {
        console.log(`socket ${socket.id} joining room ${room} name ${name}`);
        socket.join(room);
        socket.data.name = name;
        socket.data.room = room;

        io.to(room).emit("user-list", getUsers(room));
    });

    socket.on("request-access", ({ target }) => {
        io.to(target).emit("incoming-request", {
            from: socket.id,
            name: socket.data.name,
        });
    });

    socket.on("accept-request", ({ from }) => {
        io.to(from).emit("request-accepted", { ok: true });
        
        // 🎯 FIX: Viewer (request bhejnewala) ki ID store karo
        // from = Viewer (User 1) ka socket ID
        // socket.data.room = Agent (User 2) ka room ID
        viewerMap.set(socket.data.room, from); 
    });

    // socket.on("control", (data) => {
    //     // forward the control object directly to agent (so agent receives {"type":"mouse", ...})
    //     let agent = agents.get(socket.data.room);
    //     if (agent) agent.send(JSON.stringify({ type: "control", data }));
    // });
    socket.on("control", (data) => {
    // forward the control object directly to agent (so agent receives {"type":"mouse", ...})
    const agent = agents.get(socket.data.room);
    if (!agent) return;
    // agent is a WebSocket (ws from wss.on('connection'))
    try {
        agent.send(JSON.stringify(data));
    } catch (err) {
        console.error("Failed to forward control to agent:", err.message);
    }
});

    socket.on("disconnect", () => {
        io.to(socket.data.room).emit("user-list", getUsers(socket.data.room));
    });
});

function getUsers(room) {
    let users = [];
    let list = io.sockets.adapter.rooms.get(room);
    if (!list) return [];

    list.forEach((id) => {
        let s = io.sockets.sockets.get(id);
        if (s?.data?.name) users.push({ id, name: s.data.name });
    });

    return users;
}

server.listen(PORT, '0.0.0.0', () =>
    console.log(`Server running on port http://0.0.0.0:${PORT}`)
);