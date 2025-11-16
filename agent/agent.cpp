// agent.cpp
// Compile:
// cl /EHsc agent.cpp /link winhttp.lib gdiplus.lib user32.lib gdi32.lib ole32.lib
//
// Requires Windows (WinHTTP, GDI+, SendInput). Place a config.json next to exe.
//
// Basic socket.io (engine.io v4) minimal implementation over WebSocket (WinHTTP).
// Sends "set-name" and "join-room" (isAgent=true), captures screen to JPEG and
// sends as binary attachment for event "frame". Handles "grant-control" and "control" events.

#include <windows.h>
#include <winhttp.h>
#include <gdiplus.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp> // include nlohmann/json.hpp in include path

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")

using json = nlohmann::json;
using namespace Gdiplus;

std::string configServer = "https://browser-based-remote-control-backend.onrender.com";
std::string configRoom = "room1";
std::string configName = "agent-pc";
int configFps = 5;
bool sendCursor = true;

std::atomic<bool> keepRunning(true);
std::atomic<bool> granted(false);
std::mutex sockMutex;

HINTERNET hSession = NULL;
HINTERNET hConnect = NULL;
HINTERNET hRequest = NULL;
HINTERNET hWebSocket = NULL;

// Utility: read config.json
void loadConfig() {
    std::ifstream f("config.json");
    if (!f.is_open()) return;
    try {
        json j; f >> j;
        if (j.contains("server")) configServer = j["server"].get<std::string>();
        if (j.contains("roomId")) configRoom = j["roomId"].get<std::string>();
        if (j.contains("name")) configName = j["name"].get<std::string>();
        if (j.contains("fps")) configFps = j["fps"].get<int>();
        if (j.contains("sendCursor")) sendCursor = j["sendCursor"].get<bool>();
    } catch (...) {
        std::cout << "Failed parsing config.json\n";
    }
}

// Helper: parse host and path from server URL
bool parseUrl(const std::string& url, std::wstring& hostOut, std::wstring& pathOut, INTERNET_PORT& portOut, bool& secure) {
    // supports https://host[:port]/path
    std::string u = url;
    secure = false;
    if (u.rfind("https://",0) == 0) { secure = true; u = u.substr(8); portOut = INTERNET_DEFAULT_HTTPS_PORT; }
    else if (u.rfind("http://",0) == 0) { secure = false; u = u.substr(7); portOut = INTERNET_DEFAULT_HTTP_PORT; }
    else { return false; }
    size_t slash = u.find('/');
    std::string host = (slash == std::string::npos) ? u : u.substr(0, slash);
    std::string path = (slash == std::string::npos) ? std::string("/") : u.substr(slash);
    // remove trailing /
    hostOut = std::wstring(host.begin(), host.end());
    pathOut = std::wstring(path.begin(), path.end());
    return true;
}

// Send text websocket frame (WinHTTP) - wrapper
bool wsSendText(const std::string& msg) {
    std::lock_guard<std::mutex> lk(sockMutex);
    if (!hWebSocket) return false;
    BOOL ok = WinHttpWebSocketSend(hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (LPVOID)msg.c_str(), (DWORD)msg.size());
    return ok == TRUE;
}

// Send binary websocket frame
bool wsSendBinary(const std::vector<unsigned char>& data) {
    std::lock_guard<std::mutex> lk(sockMutex);
    if (!hWebSocket) return false;
    BOOL ok = WinHttpWebSocketSend(hWebSocket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (LPVOID)data.data(), (DWORD)data.size());
    return ok == TRUE;
}

// GDI+ capture to JPEG in memory
std::vector<unsigned char> captureScreenJpeg(int& outW, int& outH, ULONG quality = 60) {
    std::vector<unsigned char> out;
    HDC hScreenDC = GetDC(NULL);
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    outW = width; outH = height;

    HDC hMemDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    HGDIOBJ oldBmp = SelectObject(hMemDC, hBitmap);
    BitBlt(hMemDC, 0, 0, width, height, hScreenDC, 0, 0, SRCCOPY | CAPTUREBLT);
    SelectObject(hMemDC, oldBmp);

    // GDI+ Bitmap from HBITMAP
    Bitmap bmp(hBitmap, NULL);
    // Save to IStream
    IStream* istream = NULL;
    if (CreateStreamOnHGlobal(NULL, TRUE, &istream) == S_OK) {
        CLSID clsidEncoder;
        // find jpeg CLSID
        UINT num = 0, size = 0;
        GetImageEncodersSize(&num, &size);
        if (size > 0) {
            ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
            if (pImageCodecInfo) {
                GetImageEncoders(num, size, pImageCodecInfo);
                for (UINT j = 0; j < num; ++j) {
                    if (wcscmp(pImageCodecInfo[j].MimeType, L"image/jpeg") == 0) {
                        clsidEncoder = pImageCodecInfo[j].Clsid;
                        break;
                    }
                }
                free(pImageCodecInfo);
                // encoder parameters
                EncoderParameters encoderParams;
                encoderParams.Count = 1;
                encoderParams.Parameter[0].Guid = EncoderQuality;
                encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
                encoderParams.Parameter[0].NumberOfValues = 1;
                encoderParams.Parameter[0].Value = &quality;
                if (bmp.Save(istream, &clsidEncoder, &encoderParams) == Ok) {
                    // read stream into buffer
                    LARGE_INTEGER zero = {}; ULARGE_INTEGER size2 = {};
                    IStream_GetSize(istream, &size2);
                    // Rewind
                    LARGE_INTEGER liZero; liZero.QuadPart = 0;
                    istream->Seek(liZero, STREAM_SEEK_SET, NULL);
                    // read
                    STATSTG statstg;
                    if (istream->Stat(&statstg, STATFLAG_NONAME) == S_OK) {
                        ULONG toRead = (ULONG)statstg.cbSize.QuadPart;
                        out.resize(toRead);
                        ULONG actuallyRead = 0;
                        istream->Read(out.data(), toRead, &actuallyRead);
                        out.resize(actuallyRead);
                    }
                }
            }
        }
        istream->Release();
    }

    // cleanup
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreenDC);
    return out;
}

// Simple JSON escape for small usage (we'll rely on nlohmann to produce payloads)
std::string jsonEmitText(const std::string& event, const json& payload) {
    json arr = json::array();
    arr.push_back(event);
    if (payload.is_null()) arr.push_back(json::object());
    else arr.push_back(payload);
    std::string s = "42" + arr.dump();
    return s;
}

// Send event with single binary attachment: text '42["frame", {"_placeholder":true,"num":0}]' then binary image
bool sendFrameAsSocketIOBinary(const std::vector<unsigned char>& jpeg) {
    // text frame describing event with placeholder for one binary attachment
    json arr = json::array();
    arr.push_back("frame");
    json placeholder = { { "_placeholder", true }, { "num", 0 } };
    arr.push_back(placeholder);
    std::string text = "42" + arr.dump();
    if (!wsSendText(text)) return false;
    // then send binary payload as separate binary websocket message
    if (!wsSendBinary(jpeg)) return false;
    return true;
}

// Handle inbound text messages from socket.io/engine.io
void handleTextMessage(const std::string& msg) {
    if (msg.size() == 0) return;
    char t = msg[0];
    if (t == '0') {
        // open packet: msg = 0{...}
        std::string jsonPart = msg.substr(1);
        std::cout << "[engine.io open] " << jsonPart << "\n";
        // after open, send '40' to connect socket.io namespace
        wsSendText("40");
        // emit set-name and join-room
        json jn = { {"name", configName} };
        wsSendText(jsonEmitText("set-name", jn));
        json jr = { {"roomId", configRoom}, {"name", configName}, {"isAgent", true} };
        wsSendText(jsonEmitText("join-room", jr));
        std::cout << "Sent join-room\n";
    } else if (t == '3') {
        // pong or pong ack
        // ignore
    } else if (t == '2') {
        // ping from server; reply with '3'
        wsSendText("3");
    } else if (t == '4') {
        // Socket.IO message: starts with '4' then second char is engine packet type; usually '42' for event
        if (msg.size() >= 2 && msg[1] == '2') {
            std::string jsonPart = msg.substr(2);
            try {
                json arr = json::parse(jsonPart);
                if (!arr.is_array() || arr.size() < 1) return;
                std::string event = arr[0].get<std::string>();
                json data = arr.size() >= 2 ? arr[1] : json();
                std::cout << "[event] " << event << " -> " << data.dump() << "\n";
                if (event == "grant-control") {
                    // server tells agent to allow frames for viewer
                    granted = true;
                    std::cout << "Granted control to viewer: " << data.dump() << "\n";
                } else if (event == "revoke-control") {
                    granted = false;
                    std::cout << "Revoke control\n";
                } else if (event == "control") {
                    // viewer control event: move/click/keyboard
                    // data may contain type, x,y,button, captureWidth/captureHeight
                    std::string typ = data.value("type", "");
                    if (typ == "mousemove") {
                        double rx = data.value("x", 0.0);
                        double ry = data.value("y", 0.0);
                        int screenW = GetSystemMetrics(SM_CXSCREEN);
                        int screenH = GetSystemMetrics(SM_CYSCREEN);
                        LONG x = (LONG)round(rx * screenW);
                        LONG y = (LONG)round(ry * screenH);
                        // set mouse position
                        INPUT in = {};
                        in.type = INPUT_MOUSE;
                        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                        // Convert to absolute (0..65535)
                        in.mi.dx = (LONG)(x * 65535 / (screenW - 1));
                        in.mi.dy = (LONG)(y * 65535 / (screenH - 1));
                        SendInput(1, &in, sizeof(INPUT));
                    } else if (typ == "click" || typ == "mousedown" || typ == "mouseup" || typ == "dblclick") {
                        int btn = data.value("button", 0);
                        INPUT in[2] = {};
                        if (btn == 0) { // left
                            if (typ == "mousedown") {
                                in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                                SendInput(1, &in[0], sizeof(INPUT));
                            } else if (typ == "mouseup") {
                                in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                                SendInput(1, &in[0], sizeof(INPUT));
                            } else if (typ == "click") {
                                in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                                in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                                SendInput(2, in, sizeof(INPUT));
                            } else if (typ == "dblclick") {
                                in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                                in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                                SendInput(2, in, sizeof(INPUT));
                                Sleep(40);
                                SendInput(2, in, sizeof(INPUT));
                            }
                        } else if (btn == 2) { // right
                            if (typ == "click") {
                                in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
                                in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
                                SendInput(2, in, sizeof(INPUT));
                            }
                        }
                    } else if (typ == "wheel") {
                        int deltaY = data.value("deltaY", 0);
                        INPUT in = {};
                        in.type = INPUT_MOUSE;
                        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
                        in.mi.mouseData = (DWORD)deltaY;
                        SendInput(1, &in, sizeof(INPUT));
                    } else if (typ == "keydown" || typ == "keyup") {
                        std::string key = data.value("key", "");
                        // simple mapping for common keys (letters/digits/enter/esc)
                        SHORT vk = VkKeyScanA(key.size() ? key[0] : 0) & 0xff;
                        if (vk) {
                            INPUT in = {};
                            in.type = INPUT_KEYBOARD;
                            in.ki.wVk = vk;
                            if (typ == "keyup") in.ki.dwFlags = KEYEVENTF_KEYUP;
                            SendInput(1, &in, sizeof(INPUT));
                        }
                    }
                }
            } catch (std::exception& ex) {
                std::cout << "JSON parse error: " << ex.what() << "\n";
            }
        } else {
            // other socket.io messages
        }
    } else {
        // other engine.io frames
    }
}

// Reader thread: receive websocket messages
void recvLoop() {
    const DWORD buffSize = 8192;
    std::vector<char> buffer(buffSize);
    while (keepRunning) {
        DWORD length = 0;
        DWORD flags = 0;
        BOOL res = FALSE;
        {
            std::lock_guard<std::mutex> lk(sockMutex);
            if (!hWebSocket) break;
            res = WinHttpWebSocketReceive(hWebSocket, (LPVOID)buffer.data(), (DWORD)buffer.size(), &length, &flags);
        }
        if (!res) {
            DWORD err = GetLastError();
            std::cout << "WebSocket receive failed: " << err << "\n";
            break;
        }
        if (length == 0 && flags == WINHTTP_WEB_SOCKET_CLOSE_FLAG) {
            std::cout << "WebSocket closed by server\n";
            break;
        }
        if (flags & WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            std::string msg(buffer.data(), buffer.data() + length);
            handleTextMessage(msg);
        } else if (flags & WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            // binary message - in our design, binary frames correspond to attachments or ping - ignore
            // we don't expect server->binary except if server forwards something binary (unlikely)
            // optionally print size
            std::cout << "[binary message] size=" << length << "\n";
        }
    }
}

// Main connect routine (engine.io websocket)
bool connectSocketIO() {
    std::wstring host, path;
    INTERNET_PORT port = 0; bool secure = true;
    if (!parseUrl(configServer, host, path, port, secure)) {
        std::cout << "Invalid server URL in config\n";
        return false;
    }
    // append socket.io query params
    std::wstring fullPath = path + L"/socket.io/?EIO=4&transport=websocket";
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = (LPWSTR)host.c_str();
    uc.dwHostNameLength = (DWORD)host.size();
    const wchar_t* userAgent = L"AgentClient/1.0";

    hSession = WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { std::cout << "WinHttpOpen failed\n"; return false; }

    hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) { std::cout << "WinHttpConnect failed\n"; return false; }

    hRequest = WinHttpOpenRequest(hConnect, L"GET", fullPath.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { std::cout << "WinHttpOpenRequest failed\n"; return false; }

    // upgrade to websocket
    BOOL ok = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
    if (!ok) {
        // In practice, you must call WinHttpSendRequest/ReceiveResponse before CompleteUpgrade
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            std::cout << "WinHttpSendRequest failed\n"; return false;
        }
        if (!WinHttpReceiveResponse(hRequest, NULL)) {
            std::cout << "WinHttpReceiveResponse failed\n"; return false;
        }
        hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
        if (!hWebSocket) { std::cout << "WinHttpWebSocketCompleteUpgrade failed\n"; return false; }
    } else {
        // rarely used path
        hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, NULL);
        if (!hWebSocket) { std::cout << "WinHttpWebSocketCompleteUpgrade failed (2)\n"; return false; }
    }

    std::cout << "WebSocket connected\n";
    return true;
}

int wmain(int argc, wchar_t* argv[]) {
    // init GDI+
    ULONG_PTR gdiplusToken;
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    loadConfig();
    std::cout << "Server: " << configServer << "\nRoom: " << configRoom << "\nName: " << configName << "\nFPS: " << configFps << "\n";

    if (!connectSocketIO()) {
        std::cout << "Failed to connect to server\n";
        return 1;
    }

    // start receiver thread
    std::thread recvThread(recvLoop);

    // Main loop: capture & send frames when granted
    while (keepRunning) {
        if (granted) {
            int w=0,h=0;
            auto img = captureScreenJpeg(w,h, 55);
            if (!img.empty()) {
                // send via socket.io binary mechanism
                if (!sendFrameAsSocketIOBinary(img)) {
                    std::cout << "Failed to send frame\n";
                    // continue but maybe break
                } else {
                    std::cout << "Sent frame size=" << img.size() << " " << w << "x" << h << "\n";
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / max(1, configFps)));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    // cleanup
    if (hWebSocket) {
        WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        WinHttpCloseHandle(hWebSocket);
    }
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);

    GdiplusShutdown(gdiplusToken);
    if (recvThread.joinable()) recvThread.join();
    return 0;
}
