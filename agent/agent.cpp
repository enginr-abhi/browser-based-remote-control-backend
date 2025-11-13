// agent.cpp — Robust Windows WSS agent (final)
// Build: cl /EHsc agent.cpp /link winhttp.lib ws2_32.lib user32.lib gdiplus.lib
// Requires nlohmann/json.hpp (https://github.com/nlohmann/json)

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winhttp.h>
#include <gdiplus.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdiplus.lib")

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <sstream>
#include <algorithm>

#include "json.hpp"
using json = nlohmann::json;
using namespace Gdiplus;
using namespace std;

// ---------- Config & State ----------
static std::atomic<bool> keepRunning{ true };
static std::atomic<bool> connected{ false };
static std::mutex sendMutex;

static HINTERNET hSession = NULL;
static HINTERNET hConnect = NULL;
static HINTERNET hWebSocket = NULL;

std::string SERVER_HOST = "browser-based-remote-control-backend.onrender.com";
int SERVER_PORT = 443;
std::string ROOM = "room1";
std::string AGENT_ID = "agent-cpp-001";
int FRAME_MS = 250; // milliseconds (4 FPS default)
int MAX_WIDTH = 1366; // downscale large screens to this width by default
int JPEG_QUALITY = 60; // 0-100

// ---------- Utilities ----------
static inline std::wstring utf8_to_wstring(const std::string& s) {
    if (s.empty()) return std::wstring();
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w; w.resize(sz);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], sz);
    return w;
}
static inline std::string wstring_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s; s.resize(sz);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], sz, NULL, NULL);
    return s;
}

// base64 table
static const char* b64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len) {
        unsigned int val = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(b64_table[(val >> 18) & 0x3F]);
        out.push_back(b64_table[(val >> 12) & 0x3F]);
        out.push_back(b64_table[(val >> 6) & 0x3F]);
        out.push_back(b64_table[val & 0x3F]);
        i += 3;
    }
    if (i < len) {
        unsigned int val = (data[i] << 16);
        if ((i + 1) < len) val |= (data[i + 1] << 8);
        out.push_back(b64_table[(val >> 18) & 0x3F]);
        out.push_back(b64_table[(val >> 12) & 0x3F]);
        if ((i + 1) < len) {
            out.push_back(b64_table[(val >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

// ---------- GDI+ helpers (capture & encode JPEG) ----------
bool GetEncoderClsid(const WCHAR* mime, CLSID* pClsid) {
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0) return false;
    std::vector<BYTE> buffer(size);
    ImageCodecInfo* pInfo = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(num, size, pInfo) != Ok) return false;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(pInfo[i].MimeType, mime) == 0) {
            *pClsid = pInfo[i].Clsid;
            return true;
        }
    }
    return false;
}

// Resize GDI+ Bitmap to target width (maintain aspect)
Bitmap* ResizeBitmap(Bitmap* src, UINT targetW) {
    if (!src) return nullptr;
    UINT w = src->GetWidth();
    UINT h = src->GetHeight();
    if (w <= targetW) return src; // no resize needed; caller must free or handle
    double ratio = (double)targetW / (double)w;
    UINT newW = targetW;
    UINT newH = (UINT)max(1.0, floor(h * ratio));
    Bitmap* dst = new Bitmap(newW, newH, PixelFormat24bppRGB);
    Graphics g(dst);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(src, 0, 0, newW, newH);
    return dst;
}

// Capture primary screen and return JPEG base64 (smaller than PNG)
bool CaptureScreenToJpegBase64(std::string& outBase64, int& outW, int& outH) {
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    outW = screenW; outH = screenH;

    HDC hScreenDC = GetDC(NULL);
    if (!hScreenDC) return false;
    HDC hMemDC = CreateCompatibleDC(hScreenDC);
    if (!hMemDC) { ReleaseDC(NULL, hScreenDC); return false; }
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, screenW, screenH);
    if (!hBitmap) { DeleteDC(hMemDC); ReleaseDC(NULL, hScreenDC); return false; }
    HGDIOBJ oldObj = SelectObject(hMemDC, hBitmap);

    bool captureOk = BitBlt(hMemDC, 0, 0, screenW, screenH, hScreenDC, 0, 0, SRCCOPY | CAPTUREBLT) != 0;

    Bitmap* bmp = nullptr;
    if (captureOk) {
        bmp = new Bitmap(hBitmap, NULL);
    } else {
        SelectObject(hMemDC, oldObj);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        return false;
    }

    // optionally resize if very wide
    Bitmap* toSave = nullptr;
    if (screenW > (UINT)MAX_WIDTH) {
        toSave = ResizeBitmap(bmp, (UINT)MAX_WIDTH);
        outW = toSave ? (int)toSave->GetWidth() : outW;
        outH = toSave ? (int)toSave->GetHeight() : outH;
    } else {
        toSave = bmp;
    }

    // Save to IStream as JPEG with given quality
    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(NULL, TRUE, &pStream) != S_OK) {
        if (toSave != bmp) delete toSave;
        delete bmp;
        SelectObject(hMemDC, oldObj);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        return false;
    }

    CLSID jpgClsid;
    if (!GetEncoderClsid(L"image/jpeg", &jpgClsid)) {
        pStream->Release();
        if (toSave != bmp) delete toSave;
        delete bmp;
        SelectObject(hMemDC, oldObj);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        return false;
    }

    // set quality
    EncoderParameters encParams;
    encParams.Count = 1;
    encParams.Parameter[0].Guid = EncoderQuality;
    encParams.Parameter[0].Type = EncoderParameterValueTypeLong;
    encParams.Parameter[0].NumberOfValues = 1;
    ULONG quality = (ULONG)max(1, min(100, JPEG_QUALITY));
    encParams.Parameter[0].Value = &quality;

    Status s = toSave->Save(pStream, &jpgClsid, &encParams);
    if (s != Ok) {
        pStream->Release();
        if (toSave != bmp) delete toSave;
        delete bmp;
        SelectObject(hMemDC, oldObj);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        return false;
    }

    HGLOBAL hMem = NULL;
    if (GetHGlobalFromStream(pStream, &hMem) != S_OK || !hMem) {
        pStream->Release();
        if (toSave != bmp) delete toSave;
        delete bmp;
        SelectObject(hMemDC, oldObj);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        return false;
    }

    SIZE_T size = GlobalSize(hMem);
    if (size == 0) {
        pStream->Release();
        if (toSave != bmp) delete toSave;
        delete bmp;
        SelectObject(hMemDC, oldObj);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        return false;
    }

    void* pData = GlobalLock(hMem);
    if (!pData) {
        pStream->Release();
        if (toSave != bmp) delete toSave;
        delete bmp;
        SelectObject(hMemDC, oldObj);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        return false;
    }

    outBase64 = base64_encode((const unsigned char*)pData, (size_t)size);

    GlobalUnlock(hMem);
    pStream->Release();

    if (toSave != bmp) delete toSave;
    delete bmp;

    // cleanup GDI objects
    SelectObject(hMemDC, oldObj);
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreenDC);

    return true;
}

// ---------- WinHTTP / WebSocket helpers ----------
void closeWebSocketHandles() {
    std::lock_guard<std::mutex> lk(sendMutex);
    if (hWebSocket) {
        // best-effort close
        WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        WinHttpCloseHandle(hWebSocket);
        hWebSocket = NULL;
    }
    if (hConnect) {
        WinHttpCloseHandle(hConnect);
        hConnect = NULL;
    }
    if (hSession) {
        WinHttpCloseHandle(hSession);
        hSession = NULL;
    }
    connected.store(false);
}

bool sendJsonMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lk(sendMutex);
    if (!hWebSocket) return false;
    HRESULT hr = WinHttpWebSocketSend(hWebSocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        (PVOID)msg.c_str(), (DWORD)msg.size());
    return (hr == S_OK);
}

// handle incoming control packets (viewer -> agent)
void handleControlMessage(const json& p) {
    try {
        std::string type = p.value("type", "");
        if (type.empty()) {
            // server may send under different key 'action' or payload structure
            if (p.contains("action")) type = p.value("action", "");
            // or nested data
            if (type.empty() && p.contains("data") && p["data"].is_object()) {
                type = p["data"].value("type", "");
            }
        }
        if (type == "mousemove") {
            double nx = p.value("x", p.value("nx", 0.0));
            double ny = p.value("y", p.value("ny", 0.0));
            int sw = GetSystemMetrics(SM_CXSCREEN);
            int sh = GetSystemMetrics(SM_CYSCREEN);
            int x = (int)round(nx * sw);
            int y = (int)round(ny * sh);
            SetCursorPos(x, y);
        } else if (type == "click" || type == "mousedown" || type == "mouseup") {
            int btn = p.value("button", 0);
            DWORD downFlag = (btn == 2) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
            DWORD upFlag = (btn == 2) ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
            INPUT inp;
            ZeroMemory(&inp, sizeof(inp));
            inp.type = INPUT_MOUSE;
            if (type == "click") {
                inp.mi.dwFlags = downFlag;
                SendInput(1, &inp, sizeof(INPUT));
                Sleep(18);
                inp.mi.dwFlags = upFlag;
                SendInput(1, &inp, sizeof(INPUT));
            } else if (type == "mousedown") {
                inp.mi.dwFlags = downFlag;
                SendInput(1, &inp, sizeof(INPUT));
            } else {
                inp.mi.dwFlags = upFlag;
                SendInput(1, &inp, sizeof(INPUT));
            }
        } else if (type == "wheel") {
            int delta = p.value("deltaY", 0);
            INPUT i;
            ZeroMemory(&i, sizeof(i));
            i.type = INPUT_MOUSE;
            i.mi.dwFlags = MOUSEEVENTF_WHEEL;
            i.mi.mouseData = (DWORD)delta;
            SendInput(1, &i, sizeof(INPUT));
        } else if (type == "keydown" || type == "keyup") {
            std::string key = p.value("key", "");
            if (!key.empty()) {
                SHORT scan = VkKeyScanA(key[0]);
                if (scan != 0xFFFF) {
                    INPUT k;
                    ZeroMemory(&k, sizeof(k));
                    k.type = INPUT_KEYBOARD;
                    k.ki.wVk = (WORD)LOBYTE(scan);
                    if (type == "keyup") k.ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(1, &k, sizeof(INPUT));
                }
            }
        }
    } catch (...) {
        // ignore malformed control
    }
}

void receiverLoop() {
    const DWORD bufSize = 1024 * 1024; // 1MB buffer
    std::vector<char> buffer(bufSize);
    while (keepRunning.load() && connected.load() && hWebSocket) {
        WINHTTP_WEB_SOCKET_BUFFER_TYPE eType;
        DWORD read = 0;
        HRESULT hr = WinHttpWebSocketReceive(hWebSocket, (PVOID)buffer.data(), (DWORD)buffer.size(), &read, &eType);
        if (FAILED(hr)) {
            // error or connection closed
            break;
        }
        if (eType == WINHTTP_WEB_SOCKET_CLOSE_RECEIVED) {
            // peer closed
            break;
        }
        if (read == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        // parse text message
        try {
            std::string msg(buffer.data(), buffer.data() + read);
            auto j = json::parse(msg);
            // support { action: "control", payload: {...} } OR { type: "control", data: {...} }
            if (j.contains("action") && j["action"].is_string()) {
                std::string action = j["action"].get<std::string>();
                if (action == "control" && j.contains("payload")) {
                    handleControlMessage(j["payload"]);
                } else if (action == "start-stream") {
                    // server asked to start stream; no-op (we stream continuously by default)
                }
            } else if (j.contains("type") && j["type"].is_string()) {
                std::string t = j["type"].get<std::string>();
                if (t == "control" && j.contains("data")) {
                    handleControlMessage(j["data"]);
                } else if (t == "start-stream") {
                    // optional immediate frame
                }
            } else if (j.contains("data") && j["data"].is_object() && j["data"].contains("type")) {
                handleControlMessage(j["data"]);
            }
        } catch (...) {
            // ignore parse error
        }
    }
    // mark disconnected
    connected.store(false);
}

// connectWebSocket with improved error handling
bool connectWebSocket() {
    closeWebSocketHandles();
    // clean server host from scheme if present
    std::string host = SERVER_HOST;
    if (host.rfind("https://", 0) == 0) host = host.substr(8);
    if (host.rfind("http://", 0) == 0) host = host.substr(7);
    if (host.rfind("wss://", 0) == 0) host = host.substr(6);
    if (host.rfind("ws://", 0) == 0) host = host.substr(5);
    // remove trailing slash
    if (!host.empty() && host.back() == '/') host.pop_back();

    hSession = WinHttpOpen(L"RemoteAgent/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        std::cerr << "[!] WinHttpOpen failed\n";
        return false;
    }

    std::wstring hostW = utf8_to_wstring(host);
    hConnect = WinHttpConnect(hSession, hostW.c_str(), SERVER_PORT, 0);
    if (!hConnect) {
        std::cerr << "[!] WinHttpConnect failed\n";
        closeWebSocketHandles();
        return false;
    }

    std::string path = "/ws-agent?room=" + ROOM;
    std::wstring pathW = utf8_to_wstring(path);

    // secure flag if port is 443
    DWORD flags = (SERVER_PORT == 443) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", pathW.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        std::cerr << "[!] WinHttpOpenRequest failed\n";
        closeWebSocketHandles();
        return false;
    }

    // try send + receive
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        std::cerr << "[!] WinHttpSendRequest failed\n";
        WinHttpCloseHandle(hRequest);
        closeWebSocketHandles();
        return false;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        std::cerr << "[!] WinHttpReceiveResponse failed\n";
        WinHttpCloseHandle(hRequest);
        closeWebSocketHandles();
        return false;
    }

    // query status code
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        std::cerr << "[!] WinHttpQueryHeaders failed\n";
        WinHttpCloseHandle(hRequest);
        closeWebSocketHandles();
        return false;
    }
    if (status != 101) {
        std::cerr << "[!] HTTP status not 101 (got " << status << ")\n";
        WinHttpCloseHandle(hRequest);
        closeWebSocketHandles();
        return false;
    }

    HINTERNET hUpgraded = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    if (!hUpgraded) {
        std::cerr << "[!] WinHttpWebSocketCompleteUpgrade failed\n";
        WinHttpCloseHandle(hRequest);
        closeWebSocketHandles();
        return false;
    }

    hWebSocket = hUpgraded;
    connected.store(true);
    return true;
}

// ---------- Main agent loop ----------
void runAgentLoop() {
    int backoff = 3;
    while (keepRunning.load()) {
        std::cerr << "[*] Connecting to " << SERVER_HOST << ":" << SERVER_PORT << " room=" << ROOM << "\n";
        if (!connectWebSocket()) {
            std::cerr << "[!] Connect failed, retrying in " << backoff << "s\n";
            std::this_thread::sleep_for(std::chrono::seconds(backoff));
            backoff = std::min(60, backoff * 2);
            continue;
        }
        backoff = 3;

        // send hello
        json hello = { {"type","hello"}, {"roomId", ROOM}, {"agentId", AGENT_ID} };
        if (!sendJsonMessage(hello.dump())) {
            std::cerr << "[!] Failed to send hello\n";
            closeWebSocketHandles();
            continue;
        }

        // start receiving thread
        std::thread recvThread(receiverLoop);

        // send frames while connected
        while (keepRunning.load() && connected.load()) {
            int w=0,h=0;
            std::string b64;
            bool ok = CaptureScreenToJpegBase64(b64, w, h);
            if (ok && !b64.empty()) {
                json frame = {
                    {"type","frame"},
                    {"roomId", ROOM},
                    {"agentId", AGENT_ID},
                    {"width", w},
                    {"height", h},
                    {"image", b64}
                };
                if (!sendJsonMessage(frame.dump())) {
                    std::cerr << "[!] sendJsonMessage failed (will reconnect)\n";
                    connected.store(false);
                    break;
                }
            } else {
                std::cerr << "[!] Capture failed\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_MS));
        }

        if (recvThread.joinable()) recvThread.join();
        closeWebSocketHandles();
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// -------- config loader --------
void loadConfig() {
    try {
        std::ifstream f("config.json");
        if (!f.good()) return;
        json cfg; f >> cfg;
        if (cfg.contains("roomId")) ROOM = cfg["roomId"].get<std::string>();
        if (cfg.contains("server")) SERVER_HOST = cfg["server"].get<std::string>();
        if (cfg.contains("port")) SERVER_PORT = cfg["port"].get<int>();
        if (cfg.contains("agentId")) AGENT_ID = cfg["agentId"].get<std::string>();
        if (cfg.contains("fps")) {
            int fps = cfg["fps"].get<int>();
            if (fps > 0) FRAME_MS = 1000 / fps;
        }
        if (cfg.contains("maxWidth")) MAX_WIDTH = cfg["maxWidth"].get<int>();
        if (cfg.contains("jpegQuality")) JPEG_QUALITY = cfg["jpegQuality"].get<int>();
    } catch (...) {
        // ignore parse errors
    }
}

// -------- console handler --------
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_BREAK_EVENT) {
        keepRunning.store(false);
        connected.store(false);
        closeWebSocketHandles();
        return TRUE;
    }
    return FALSE;
}

// -------- main --------
int main() {
    SetConsoleTitleA("Remote Agent - C++ (WSS)");
    GdiplusStartupInput gsi;
    ULONG_PTR token;
    if (GdiplusStartup(&token, &gsi, NULL) != Ok) {
        std::cerr << "[!] GdiplusStartup failed\n";
        return 1;
    }

    loadConfig();
    std::cerr << "[*] Agent starting. Server=" << SERVER_HOST << ":" << SERVER_PORT << " Room=" << ROOM << " AgentId=" << AGENT_ID << "\n";
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    runAgentLoop();

    GdiplusShutdown(token);
    std::cerr << "[*] Agent exiting cleanly\n";
    return 0;
}
