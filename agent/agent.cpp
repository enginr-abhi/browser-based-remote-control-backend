// ✅ agent.cpp (Windows) - Persistent Remote Agent (Final)
// Build (MSVC):
//   cl /EHsc agent.cpp /link winhttp.lib ws2_32.lib user32.lib
//
// Connects to: ws://<SERVER_HOST>:<SERVER_PORT>/ws-agent?room=<ROOM>
// Works with persistent backend (Render/Vercel backend setup)
// ----------------------------------------------------------------------

#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <csignal>
#include <cmath>
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

// ------------------ CONFIG ------------------
string SERVER_HOST = "browser-based-remote-control-backend.onrender.com";
int SERVER_PORT = 9000;
string ROOM = "room1";

atomic<bool> keepRunning{true};
HINTERNET hSession = nullptr;
HINTERNET hConnect = nullptr;
HINTERNET hRequest = nullptr;
mutex sendMutex;

void cleanupHandles() {
    if (hRequest) { WinHttpCloseHandle(hRequest); hRequest = nullptr; }
    if (hConnect) { WinHttpCloseHandle(hConnect); hConnect = nullptr; }
    if (hSession) { WinHttpCloseHandle(hSession); hSession = nullptr; }
}

void signalHandler(int sig) {
    cout << "\n🛑 Caught signal, shutting down...\n";
    keepRunning.store(false);
    cleanupHandles();
    exit(0);
}

// ------------------ LOAD CONFIG ------------------
void loadConfig() {
    try {
        ifstream f("config.json");
        if (f.is_open()) {
            json cfg; f >> cfg;
            if (cfg.contains("roomId")) ROOM = cfg["roomId"].get<string>();
            if (cfg.contains("server")) SERVER_HOST = cfg["server"].get<string>();
            if (cfg.contains("port")) SERVER_PORT = cfg["port"].get<int>();
        }
    } catch (...) {
        cerr << "⚠️ Failed to read config.json, using defaults\n";
    }
}

// ------------------ SEND MESSAGE ------------------
bool sendJsonMessage(const string &msg) {
    lock_guard<mutex> lk(sendMutex);
    if (!hRequest) return false;

    DWORD dwError = WinHttpWebSocketSend(
        hRequest,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        (PVOID)msg.c_str(),
        (DWORD)msg.size()
    );
    return (dwError == S_OK);
}

// ------------------ HANDLE CONTROL ------------------
void handleControl(const json &data) {
    string ctype = data.value("type", "");

    if (ctype == "mousemove") {
        double x = data.value("x", 0.0);
        double y = data.value("y", 0.0);
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        SetCursorPos((int)round(x * screenW), (int)round(y * screenH));
    } 
    else if (ctype == "click") {
        int btn = data.value("button", 0);
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_MOUSE;
        inputs[1].type = INPUT_MOUSE;
        if (btn == 2) {
            inputs[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        } else {
            inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        }
        SendInput(2, inputs, sizeof(INPUT));
    } 
    else if (ctype == "dblclick") {
        INPUT d1[2] = {};
        d1[0].type = INPUT_MOUSE; d1[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        d1[1].type = INPUT_MOUSE; d1[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, d1, sizeof(INPUT));
        this_thread::sleep_for(chrono::milliseconds(60));
        SendInput(2, d1, sizeof(INPUT));
    } 
    else if (ctype == "wheel") {
        int dy = data.value("deltaY", 0);
        INPUT i = {};
        i.type = INPUT_MOUSE;
        i.mi.dwFlags = MOUSEEVENTF_WHEEL;
        i.mi.mouseData = (DWORD)dy;
        SendInput(1, &i, sizeof(INPUT));
    } 
    else if (ctype == "keydown" || ctype == "keyup") {
        string key = data.value("key", "");
        SHORT vk = VkKeyScanA(key.empty() ? '\0' : key[0]);
        if (vk != 0xFFFF) {
            INPUT ki = {};
            ki.type = INPUT_KEYBOARD;
            ki.ki.wVk = (WORD)LOBYTE(vk);
            if (ctype == "keyup") ki.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &ki, sizeof(INPUT));
        }
    }
}

// ------------------ RECEIVE LOOP ------------------
void wsReceiveLoop() {
    const DWORD bufSize = 64 * 1024;
    vector<char> buffer(bufSize);

    while (keepRunning.load()) {
        if (!hRequest) break;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE eType;
        DWORD read = 0;

        HRESULT hr = WinHttpWebSocketReceive(
            hRequest,
            (PVOID)buffer.data(),
            (DWORD)buffer.size(),
            &read,
            &eType
        );

        if (FAILED(hr)) {
            cerr << "❌ WebSocket receive failed, hr=" << hr << "\n";
            break;
        }

        if (read == 0) {
            this_thread::sleep_for(chrono::milliseconds(50));
            continue;
        }

        string msg(buffer.data(), buffer.data() + read);
        try {
            auto j = json::parse(msg);
            string type = j.value("type", "");
            if (type == "control" && j.contains("data")) handleControl(j["data"]);
        } catch (exception &e) {
            cerr << "JSON error: " << e.what() << " -> " << msg << "\n";
        }
    }

    cerr << "⚠️ Exiting receive loop (disconnected)\n";
}

// ------------------ CONNECT ------------------
bool connectWebSocket() {
    cleanupHandles();

    WCHAR hostW[512];
    MultiByteToWideChar(CP_UTF8, 0, SERVER_HOST.c_str(), -1, hostW, 512);

    hSession = WinHttpOpen(L"RemoteAgent/1.0",
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    hConnect = WinHttpConnect(hSession, hostW, SERVER_PORT, 0);
    if (!hConnect) return false;

    string path = "/ws-agent?room=" + ROOM;
    WCHAR pathW[512];
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, pathW, 512);

    hRequest = WinHttpOpenRequest(hConnect, L"GET", pathW, NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) return false;

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return false;
    if (!WinHttpReceiveResponse(hRequest, NULL)) return false;

    HINTERNET hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    if (!hWebSocket) return false;

    hRequest = hWebSocket;
    cout << "✅ Connected to /ws-agent successfully.\n";
    return true;
}

// ------------------ RUN AGENT LOOP ------------------
void runAgent() {
    int reconnectDelay = 5;
    int failedHeartbeats = 0;

    while (keepRunning.load()) {
        cout << "\n🔄 Connecting to server...\n";
        if (!connectWebSocket()) {
            cerr << "❌ Connection failed. Retrying in " << reconnectDelay << "s...\n";
            cleanupHandles();
            this_thread::sleep_for(chrono::seconds(reconnectDelay));
            reconnectDelay = min(reconnectDelay * 2, 60); // backoff
            continue;
        }

        reconnectDelay = 5; // reset delay after success
        json hello = { {"type","hello"}, {"roomId",ROOM}, {"agentId","agent-cpp-001"} };
        sendJsonMessage(hello.dump());

        thread recvThread(wsReceiveLoop);

        // Heartbeat
        while (keepRunning.load() && hRequest) {
            json hb = { {"type","ping"}, {"ts",(uint64_t)chrono::duration_cast<chrono::milliseconds>(
                        chrono::system_clock::now().time_since_epoch()).count()} };
            bool ok = sendJsonMessage(hb.dump());
            if (!ok) {
                failedHeartbeats++;
                if (failedHeartbeats >= 3) {
                    cerr << "⚠️ Lost connection, restarting...\n";
                    break;
                }
            } else {
                failedHeartbeats = 0;
            }
            this_thread::sleep_for(chrono::seconds(10));
        }

        recvThread.join();
        cleanupHandles();
        this_thread::sleep_for(chrono::seconds(3));
    }
}

// ------------------ MAIN ------------------
int main() {
    signal(SIGINT, signalHandler);
    cout << "🚀 Starting persistent remote agent\n";
    loadConfig();
    cout << "Server: " << SERVER_HOST << ":" << SERVER_PORT << " | Room: " << ROOM << "\n";

    runAgent();
    cleanupHandles();
    cout << "🟢 Agent exited cleanly.\n";
    return 0;
}
