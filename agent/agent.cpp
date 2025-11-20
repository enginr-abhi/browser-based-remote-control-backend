//agent.cpp - FINAL FIXED CODE (Function Order Corrected)
#include <winsock2.h>
#include <windows.h>
#include <gdiplus.h>
#include <ws2tcpip.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <string>
#include <thread>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <random>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;
typedef unsigned char BYTE;

// ---------- CONFIG ----------
const std::string SERVER_HOST = "browser-based-remote-control-backend.onrender.com";
const int SERVER_PORT = 443;
const std::string ROOM_ID = "room1";
// ----------------------------

SOCKET raw_sock = INVALID_SOCKET;
SSL_CTX *ssl_ctx = nullptr;
SSL *ssl = nullptr;

bool running = true;
int screenW = 0, screenH = 0;
ULONG_PTR token_gdiplus = 0;

// Function Prototypes (declaration for main)
void handleControl(const std::string& msg);
void captureJPEG(std::vector<BYTE>& buf);
void streamThread();
void listenThread();

// --- helpers: base64 for Sec-WebSocket-Key ---
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char* data, size_t len) {
    std::string out;
    out.reserve(((len+2)/3)*4);
    size_t i = 0;
    while (i < len) {
        unsigned int a = i < len ? data[i++] : 0;
        unsigned int b = i < len ? data[i++] : 0;
        unsigned int c = i < len ? data[i++] : 0;
        unsigned int triple = (a << 16) + (b << 8) + c;
        out.push_back(b64_table[(triple >> 18) & 0x3F]);
        out.push_back(b64_table[(triple >> 12) & 0x3F]);
        out.push_back(i-1 < len ? b64_table[(triple >> 6) & 0x3F] : '=');
        out.push_back(i   < len ? b64_table[(triple) & 0x3F] : '=');
    }
    return out;
}

std::string make_sec_websocket_key() {
    unsigned char key[16];
    std::random_device rd;
    for (int i = 0; i < 16; ++i) key[i] = (unsigned char)(rd() & 0xFF);
    return base64_encode(key, 16);
}

// ---------- SSL I/O wrappers ----------
int ssl_write_all(const BYTE* data, int len) {
    if (!ssl) return -1;
    int offset = 0;
    while (offset < len) {
        int w = SSL_write(ssl, data + offset, len - offset);
        if (w <= 0) {
            int err = SSL_get_error(ssl, w);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                return -1;
            }
        }
        offset += w;
    }
    return offset;
}

int ssl_read_some(BYTE* buf, int bufsize) {
    if (!ssl) return -1;
    int r = SSL_read(ssl, buf, bufsize);
    if (r <= 0) {
        int err = SSL_get_error(ssl, r);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            return -1;
        }
    }
    return r;
}

// Close current SSL/socket
void close_ssl_connection() {
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
    if (ssl_ctx) { SSL_CTX_free(ssl_ctx); ssl_ctx = nullptr; }
    if (raw_sock != INVALID_SOCKET) { closesocket(raw_sock); raw_sock = INVALID_SOCKET; }
    WSACleanup();
}

// Create TCP connect and SSL_connect (with SNI + verification disabled)
bool open_ssl_connection(const std::string& host, int port) {
    // initialize Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) { return false; }

    // Resolve host and connect
    addrinfo hints = {0}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    std::string portstr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portstr.c_str(), &hints, &res) != 0) { WSACleanup(); return false; }

    raw_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (raw_sock == INVALID_SOCKET) { freeaddrinfo(res); WSACleanup(); return false; }

    if (connect(raw_sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        closesocket(raw_sock); freeaddrinfo(res); WSACleanup(); return false;
    }
    freeaddrinfo(res);

    // Initialize OpenSSL
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    OPENSSL_init_ssl(0, NULL);
#else
    SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();
#endif
    
    // Create SSL Context
    const SSL_METHOD *method = TLS_client_method();
    ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) { closesocket(raw_sock); WSACleanup(); return false; }

    // Certificate Verification ko *Permanently* Disable kar rahe hain (Error fix)
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL); 
    SSL_CTX_set_default_verify_paths(ssl_ctx); // Load paths but verification is NONE

    ssl = SSL_new(ssl_ctx);
    if (!ssl) { SSL_CTX_free(ssl_ctx); closesocket(raw_sock); WSACleanup(); return false; }

    if (!SSL_set_tlsext_host_name(ssl, host.c_str())) { /* warning but continue */ }
    if (!SSL_set_fd(ssl, (int)raw_sock)) { close_ssl_connection(); return false; }

    // Perform TLS handshake
    if (SSL_connect(ssl) != 1) { close_ssl_connection(); return false; }
    return true;
}


// --- STEP 1: HTTP Polling Handshake (to get SID) ---
// Returns: SID string or empty string on failure
std::string http_connect_and_read_sid() {
    if (!open_ssl_connection(SERVER_HOST, SERVER_PORT)) {
        MessageBoxA(0, "Failed to open SSL connection for polling.", "Error", 0);
        return "";
    }

    // Polling Request Path (EIO=4 is standard for modern Socket.IO)
    std::string path = "/socket.io/?room=" + ROOM_ID + "&EIO=4&transport=polling";

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n";
    req << "Host: " << SERVER_HOST << "\r\n";
    req << "User-Agent: RemoteAgent/1.0\r\n";
    req << "Connection: close\r\n"; // Important for HTTP/1.1 response
    req << "\r\n";

    std::string reqs = req.str();
    if (ssl_write_all((const BYTE*)reqs.data(), (int)reqs.size()) <= 0) {
        close_ssl_connection();
        return "";
    }

    // Read response headers and body
    std::string resp;
    const int CHUNK = 4096;
    BYTE buf[CHUNK];
    int r;
    while ((r = ssl_read_some(buf, CHUNK)) > 0) {
        resp.append((char*)buf, r);
    }
    
    // Connection close and cleanup for polling
    close_ssl_connection(); 

    // Check for HTTP 200 OK
    if (resp.find("HTTP/1.1 200 OK") == std::string::npos) {
        std::cerr << "Polling Handshake failed (Non-200 response):\n" << resp.substr(0, 500) << std::endl;
        return "";
    }

    // Find the body (after \r\n\r\n) - Socket.IO payload starts with '0'
    size_t body_pos = resp.find("\r\n\r\n");
    if (body_pos == std::string::npos) return "";

    std::string body = resp.substr(body_pos + 4);

    // Socket.IO Polling response is a JSON string, often prefixed with a length and '0{'
    // Example: 96:0{"sid":"eLpS...","upgrades":["websocket"],"pingInterval":25000,"pingTimeout":5000}
    size_t json_start = body.find('{');
    if (json_start == std::string::npos) return "";

    // Extract JSON part
    std::string json_payload = body.substr(json_start);
    
    // Find SID in the JSON string
    size_t sid_start = json_payload.find("\"sid\":\"");
    if (sid_start == std::string::npos) return "";
    sid_start += 7; // Length of "sid":"

    size_t sid_end = json_payload.find("\"", sid_start);
    if (sid_end == std::string::npos) return "";

    std::string sid = json_payload.substr(sid_start, sid_end - sid_start);
    std::cerr << "Successfully fetched SID: " << sid << std::endl;
    return sid;
}


// --- STEP 2: WebSocket Upgrade Handshake (using SID) ---
bool ws_client_handshake_with_sid(const std::string& host, const std::string& sid, const std::string& key) {
    // Reopen the SSL connection for WebSocket
    if (!open_ssl_connection(host, SERVER_PORT)) {
        MessageBoxA(0, "Failed to reopen SSL connection for WebSocket.", "Error", 0);
        return false;
    }

    // WebSocket Upgrade Request Path (EIO=4, transport=websocket, and the obtained sid)
    std::string path = "/socket.io/?room=" + ROOM_ID + "&EIO=4&transport=websocket&sid=" + sid;

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";
    req << "Upgrade: websocket\r\n";
    req << "Connection: Upgrade\r\n";
    req << "CF-WSS-Proxy: websocket\r\n"; 
    req << "Sec-WebSocket-Key: " << key << "\r\n";
    req << "Sec-WebSocket-Version: 13\r\n";
    req << "User-Agent: RemoteAgent/1.0\r\n";
    req << "\r\n";

    std::string reqs = req.str();
    if (ssl_write_all((const BYTE*)reqs.data(), (int)reqs.size()) <= 0) return false;

    // read response headers (up to header end)
    std::string resp;
    const int CHUNK = 4096;
    BYTE buf[CHUNK];
    int loops = 0;
    while (true) {
        int r = ssl_read_some(buf, CHUNK);
        if (r <= 0) return false;
        resp.append((char*)buf, r);
        if (resp.find("\r\n\r\n") != std::string::npos) break;
        if (++loops > 100) break;
    }
    if (resp.find("101 Switching Protocols") == std::string::npos) {
        std::cerr << "WebSocket Handshake failed (Non-101 response):\n" << resp.substr(0, 500) << std::endl;
        return false;
    }
    return true;
}


// WebSocket client-side mask + send for binary frames (opcode 0x2)
void ws_mask_send_binary(const std::vector<BYTE>& payload) {
    if (!ssl) return;
    std::vector<BYTE> frame;
    size_t length = payload.size();

    // First byte: FIN + opcode(binary)
    frame.push_back(0x80 | 0x02);

    // Mask bit must be 1 for client -> server
    if (length <= 125) {
        frame.push_back(0x80 | (BYTE)length);
    } else if (length <= 65535) {
        frame.push_back(0x80 | 126);
        frame.push_back((BYTE)(length >> 8));
        frame.push_back((BYTE)length);
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) frame.push_back((BYTE)((length >> (i*8)) & 0xFF));
    }

    BYTE mask[4];
    std::random_device rd;
    for (int i=0;i<4;i++) mask[i] = (BYTE)(rd() & 0xFF);
    frame.insert(frame.end(), mask, mask+4);

    std::vector<BYTE> masked(length);
    for (size_t i=0;i<length;i++) masked[i] = payload[i] ^ mask[i % 4];
    frame.insert(frame.end(), masked.begin(), masked.end());

    ssl_write_all(frame.data(), (int)frame.size());
}


// ---------- GDI+ Helper Functions (Correct Order) ----------

// 1. GetEncoderClsid definition (MUST come before captureJPEG)
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    ImageCodecInfo* pImageCodecInfo = NULL;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == NULL) return -1;
    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

// 2. captureJPEG definition (Can now call GetEncoderClsid)
void captureJPEG(std::vector<BYTE>& buf) {
    buf.clear();
    HDC hDesktop = GetDC(NULL);
    HDC hCapture = CreateCompatibleDC(hDesktop);
    HBITMAP hBitmap = CreateCompatibleBitmap(hDesktop, screenW, screenH);
    SelectObject(hCapture, hBitmap);
    BitBlt(hCapture, 0, 0, screenW, screenH, hDesktop, 0, 0, SRCCOPY | CAPTUREBLT);
    ReleaseDC(NULL, hDesktop);
    DeleteDC(hCapture);

    Bitmap* pBitmap = new Bitmap(hBitmap, NULL);
    CLSID clsidEncoder;
    if (GetEncoderClsid(L"image/jpeg", &clsidEncoder) != -1) {
        IStream* pStream = NULL;
        if (CreateStreamOnHGlobal(NULL, TRUE, &pStream) == S_OK) {
            EncoderParameters encoderParams;
            encoderParams.Count = 1;
            encoderParams.Parameter[0].Guid = EncoderQuality;
            encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
            encoderParams.Parameter[0].NumberOfValues = 1;
            ULONG quality = 50;
            encoderParams.Parameter[0].Value = &quality;
            if (pBitmap->Save(pStream, &clsidEncoder, &encoderParams) == Ok) {
                STATSTG statstg;
                pStream->Stat(&statstg, STATFLAG_NONAME);
                ULARGE_INTEGER ulnSize = statstg.cbSize;
                buf.resize((size_t)ulnSize.QuadPart);
                LARGE_INTEGER liZero = {0};
                pStream->Seek(liZero, STREAM_SEEK_SET, NULL);
                ULONG readBytes = 0;
                pStream->Read(buf.data(), (ULONG)ulnSize.QuadPart, &readBytes);
            }
            pStream->Release();
        }
    }
    delete pBitmap;
    DeleteObject(hBitmap);
}

// 3. getVkCode definition
BYTE getVkCode(const std::string& key) {
    if (key.length() == 1 && std::isalpha(key[0])) return (BYTE)std::toupper(key[0]);
    if (key.length() == 1 && std::isdigit(key[0])) return (BYTE)(key[0]);
    if (key == "Enter") return VK_RETURN;
    if (key == "Shift") return VK_SHIFT;
    if (key == "Control") return VK_CONTROL;
    if (key == "Alt") return VK_MENU;
    if (key == "Backspace") return VK_BACK;
    if (key == "Tab") return VK_TAB;
    if (key == "Escape") return VK_ESCAPE;
    if (key == "Delete") return VK_DELETE;
    if (key == "ArrowUp") return VK_UP;
    if (key == "ArrowDown") return VK_DOWN;
    if (key == "ArrowLeft") return VK_LEFT;
    if (key == "ArrowRight") return VK_RIGHT;
    if (key == "Home") return VK_HOME;
    if (key == "End") return VK_END;
    if (key == "PageUp") return VK_PRIOR;
    if (key == "PageDown") return VK_NEXT;
    if (key == "F1") return VK_F1;
    if (key == "F12") return VK_F12;
    if (key == " ") return VK_SPACE;
    return 0;
}

void handleControl(const std::string& msg) {
    // replicate your previous parsing & input logic (mouse/scroll/key) - kept compact
    if (msg.find("\"type\":\"mouse\"") != std::string::npos) {
        size_t x_pos = msg.find("\"x\":"); size_t y_pos = msg.find("\"y\":");
        size_t act_pos = msg.find("\"action\":\""); size_t btn_pos = msg.find("\"button\":");
        if (x_pos == std::string::npos || y_pos == std::string::npos || act_pos == std::string::npos) return;
        std::string x_str = msg.substr(x_pos + 4, msg.find(",", x_pos) - (x_pos + 4));
        std::string y_str = msg.substr(y_pos + 4, msg.find(",", y_pos) - (y_pos + 4));
        std::string action_str = msg.substr(act_pos + 10, msg.find("\"", act_pos + 10) - (act_pos + 10));
        std::string button_str = (btn_pos==std::string::npos) ? "0" : msg.substr(btn_pos + 9, msg.find("}", btn_pos) - (btn_pos + 9));
        float x_norm = std::stof(x_str); float y_norm = std::stof(y_str);
        int button_code = std::stoi(button_str);
        INPUT input = {0}; input.type = INPUT_MOUSE;
        int x_abs = (int)(x_norm * 65536.0f); int y_abs = (int)(y_norm * 65536.0f);
        x_abs = std::min(x_abs, 65535); y_abs = std::min(y_abs, 65535);
        input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        input.mi.dx = x_abs; input.mi.dy = y_abs; SendInput(1, &input, sizeof(INPUT));
        if (action_str == "down" || action_str == "up") {
            DWORD dwFlag = 0;
            if (button_code == 0) dwFlag = (action_str == "down") ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            else if (button_code == 2) dwFlag = (action_str == "down") ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            else if (button_code == 1) dwFlag = (action_str == "down") ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            if (dwFlag != 0) {
                input.mi.dwFlags = dwFlag; input.mi.dx = 0; input.mi.dy = 0; SendInput(1, &input, sizeof(INPUT));
            }
        }
    } else if (msg.find("\"type\":\"scroll\"") != std::string::npos) {
        size_t delta_pos = msg.find("\"delta\":"); if (delta_pos == std::string::npos) return;
        std::string delta_str = msg.substr(delta_pos + 8, msg.find("}", delta_pos) - (delta_pos + 8));
        int delta = std::stoi(delta_str);
        INPUT input = {0}; input.type = INPUT_MOUSE; input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = (DWORD)(-delta / 3); SendInput(1, &input, sizeof(INPUT));
    } else if (msg.find("\"type\":\"key\"") != std::string::npos) {
        size_t key_pos = msg.find("\"key\":\""); size_t state_pos = msg.find("\"state\":\"");
        if (key_pos == std::string::npos || state_pos == std::string::npos) return;
        std::string key_str = msg.substr(key_pos + 7, msg.find("\"", key_pos + 7) - (key_pos + 7));
        std::string state_str = msg.substr(state_pos + 9, msg.find("\"", state_pos + 9) - (state_pos + 9));
        BYTE vk = getVkCode(key_str); if (vk == 0) return;
        INPUT input = {0}; input.type = INPUT_KEYBOARD;
        if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
            input.ki.wVk = 0; input.ki.wScan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC); input.ki.dwFlags = KEYEVENTF_SCANCODE;
        } else input.ki.wVk = vk;
        if (state_str == "up") input.ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(1, &input, sizeof(INPUT));
    }
}

void streamThread() {
    std::vector<BYTE> buffer;
    while (running) {
        captureJPEG(buffer);
        if (!buffer.empty()) {
            ws_mask_send_binary(buffer);
        }
        Sleep(33); // ~30 FPS
    }
}

void listenThread() {
    const int BUF_SZ = 8192;
    std::vector<BYTE> rbuf(BUF_SZ);
    while (running) {
        int r = ssl_read_some(rbuf.data(), BUF_SZ);
        if (r <= 0) { running = false; break; }
        int idx = 0;
        while (idx < r) {
            BYTE b1 = rbuf[idx];
            BYTE opcode = b1 & 0x0F;
            if (opcode == 0x8) { running = false; break; } // close
            if (opcode == 0x1 || opcode == 0x2) {
                if (idx + 1 >= r) break;
                BYTE b2 = rbuf[idx+1];
                bool masked = (b2 & 0x80);
                uint64_t payload_len = b2 & 0x7F;
                int header_len = 2;
                if (payload_len == 126) {
                    if (idx + 4 >= r) break;
                    payload_len = (rbuf[idx+2] << 8) | rbuf[idx+3];
                    header_len += 2;
                } else if (payload_len == 127) {
                    if (idx + 10 >= r) break;
                    payload_len = 0;
                    for (int i=0;i<8;i++) payload_len = (payload_len << 8) | rbuf[idx+2+i];
                    header_len += 8;
                }
                BYTE mask_key[4] = {0,0,0,0};
                if (masked) {
                    if (idx + header_len + 4 > r) break;
                    for (int i=0;i<4;i++) mask_key[i] = rbuf[idx + header_len + i];
                    header_len += 4;
                }
                if (idx + header_len + (int)payload_len > r) break;
                std::string payload; payload.resize(payload_len);
                for (uint64_t i=0;i<payload_len;i++) {
                    BYTE ch = rbuf[idx + header_len + i];
                    if (masked) ch ^= mask_key[i % 4];
                    payload[i] = (char)ch;
                }
                if (opcode == 0x1) {
                    handleControl(payload);
                }
                idx += header_len + (int)payload_len;
            } else {
                idx++;
            }
        }
    }
}


// -------------- MAIN --------------
int main() {
    // init GDI+
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&token_gdiplus, &gdiplusStartupInput, NULL);
    screenW = GetSystemMetrics(SM_CXSCREEN);
    screenH = GetSystemMetrics(SM_CYSCREEN);

    // STEP 1: HTTP Polling to get Session ID (SID)
    std::string session_id = http_connect_and_read_sid();
    if (session_id.empty()) {
        MessageBoxA(0, "Failed to get Socket.IO Session ID. Aborting.", "Error", 0);
        GdiplusShutdown(token_gdiplus);
        return 1;
    }

    // STEP 2: WebSocket handshake using the obtained SID
    std::string swkey = make_sec_websocket_key();
    if (!ws_client_handshake_with_sid(SERVER_HOST, session_id, swkey)) {
        MessageBoxA(0, "WebSocket handshake failed (with SID).", "Error", 0);
        close_ssl_connection();
        GdiplusShutdown(token_gdiplus);
        return 1;
    }

    // Agent is now connected via WebSocket and ready to stream
    MessageBoxA(0, "Successfully connected via Socket.IO WebSocket!", "Success", 0);

    // Start threads
    std::thread t_listen(listenThread);
    std::thread t_stream(streamThread);
    t_listen.join();
    t_stream.join();

    // cleanup
    running = false;
    close_ssl_connection();
    GdiplusShutdown(token_gdiplus);
    return 0;
}