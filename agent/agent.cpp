// agent.cpp  (Windows)
// Compile: g++ agent.cpp -o agent.exe -Iinclude -lWs2_32 -lUser32
// Requirements: include/json.hpp (nlohmann single header) in agent/include/json.hpp

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <sstream>
#include "json.hpp"

#pragma comment(lib, "Ws2_32.lib")
using json = nlohmann::json;
using namespace std;

string SERVER_HOST = "127.0.0.1"; // change to deployed server domain when needed
int SERVER_PORT = 3001;
string ROOM = "room1";

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

bool sendLine(SOCKET s, const string &line) {
    string out = line + "\n";
    int total = 0;
    int len = (int)out.size();
    const char *buf = out.c_str();
    while (total < len) {
        int sent = send(s, buf + total, len - total, 0);
        if (sent == SOCKET_ERROR) return false;
        total += sent;
    }
    return true;
}

WORD mapKey(const string &k) {
    string key = k;
    for (auto &c : key) c = tolower(c);
    if (key.size() == 1) {
        SHORT v = VkKeyScanA(key[0]);
        if (v != 0xFFFF) return LOBYTE(v);
    }
    if (key == "enter") return VK_RETURN;
    if (key == "escape") return VK_ESCAPE;
    if (key == "backspace") return VK_BACK;
    if (key == "tab") return VK_TAB;
    if (key == "delete") return VK_DELETE;
    if (key == "control" || key == "ctrl") return VK_CONTROL;
    if (key == "shift") return VK_SHIFT;
    if (key == "alt") return VK_MENU;
    if (key == "arrowup") return VK_UP;
    if (key == "arrowdown") return VK_DOWN;
    if (key == "arrowleft") return VK_LEFT;
    if (key == "arrowright") return VK_RIGHT;
    if (key == "space") return VK_SPACE;
    return 0;
}

void doMouseMove(double nx, double ny) {
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (int)round(nx * screenW);
    int y = (int)round(ny * screenH);
    SetCursorPos(x, y);
}

void doMouseClick(int button) {
    if (button == 2) {
        mouse_event(MOUSEEVENTF_RIGHTDOWN, 0,0,0,0);
        mouse_event(MOUSEEVENTF_RIGHTUP, 0,0,0,0);
    } else if (button == 1) {
        mouse_event(MOUSEEVENTF_MIDDLEDOWN, 0,0,0,0);
        mouse_event(MOUSEEVENTF_MIDDLEUP, 0,0,0,0);
    } else {
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0,0,0,0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0,0,0,0);
    }
}

void doMouseWheel(int deltaY) {
    int amount = (int)round(deltaY);
    mouse_event(MOUSEEVENTF_WHEEL, 0,0,amount,0);
}

void doKeyDown(const string &key) {
    WORD vk = mapKey(key);
    if (vk) keybd_event(vk, 0, 0, 0);
    else if (!key.empty() && key.size() == 1) {
        SHORT v = VkKeyScanA(key[0]);
        if (v != 0xFFFF) { WORD vk2 = LOBYTE(v); keybd_event(vk2,0,0,0); }
    }
}

void doKeyUp(const string &key) {
    WORD vk = mapKey(key);
    if (vk) keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
    else if (!key.empty() && key.size() == 1) {
        SHORT v = VkKeyScanA(key[0]);
        if (v != 0xFFFF) { WORD vk2 = LOBYTE(v); keybd_event(vk2,0,KEYEVENTF_KEYUP,0); }
    }
}

int main() {
    cout << "🚀 Starting C++ Agent (Windows)\n";
    loadConfig();
    cout << "Room: " << ROOM << "  Server: " << SERVER_HOST << ":" << SERVER_PORT << endl;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        cerr << "WSAStartup failed\n";
        return -1;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portbuf[16];
    sprintf(portbuf, "%d", SERVER_PORT);

    if (getaddrinfo(SERVER_HOST.c_str(), portbuf, &hints, &res) != 0) {
        cerr << "getaddrinfo failed\n";
        WSACleanup();
        return -1;
    }

    SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        cerr << "socket() failed\n";
        freeaddrinfo(res);
        WSACleanup();
        return -1;
    }

    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        cerr << "Connection failed\n";
        closesocket(sock);
        freeaddrinfo(res);
        WSACleanup();
        return -1;
    }
    freeaddrinfo(res);

    json jhello = { {"type","hello"}, {"roomId", ROOM} };
    if (!sendLine(sock, jhello.dump())) {
        cerr << "Failed to send hello\n";
        closesocket(sock);
        WSACleanup();
        return -1;
    }
    cout << "✅ Connected and registered to room: " << ROOM << "\n";

    string buffer;
    char recvbuf[4096];
    int recvlen = 0;
    while (true) {
        recvlen = recv(sock, recvbuf, sizeof(recvbuf)-1, 0);
        if (recvlen <= 0) { cout << "Server closed or network error\n"; break; }
        recvbuf[recvlen] = '\0';
        buffer += string(recvbuf);

        size_t pos;
        while ((pos = buffer.find('\n')) != string::npos) {
            string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (line.size() == 0) continue;
            try {
                json m = json::parse(line);
                string t = m.value("type", "");
                if (t == "control") {
                    json data = m["data"];
                    string ctype = data.value("type", "");
                    if (ctype == "mousemove") {
                        double x = data.value("x", 0.0);
                        double y = data.value("y", 0.0);
                        doMouseMove(x, y);
                    } else if (ctype == "click") {
                        int btn = data.value("button", 0);
                        doMouseClick(btn);
                    } else if (ctype == "dblclick") {
                        doMouseClick(0);
                        this_thread::sleep_for(chrono::milliseconds(50));
                        doMouseClick(0);
                    } else if (ctype == "wheel") {
                        int dy = data.value("deltaY", 0);
                        doMouseWheel(dy);
                    } else if (ctype == "keydown") {
                        string key = data.value("key", "");
                        doKeyDown(key);
                    } else if (ctype == "keyup") {
                        string key = data.value("key", "");
                        doKeyUp(key);
                    }
                } else if (t == "hello-ack") {
                    cout << "Server acked hello for room: " << m.value("roomId", "") << "\n";
                } else if (t == "capture-info") {
                    cout << "Capture info: " << m.dump() << "\n";
                } else if (t == "stop-share") {
                    cout << "Stop-share received\n";
                }
            } catch (exception &e) {
                cerr << "JSON parse error: " << e.what() << " | line: " << line << "\n";
            }
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
