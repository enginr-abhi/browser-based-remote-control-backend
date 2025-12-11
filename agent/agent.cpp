#include <winsock2.h>
#include <windows.h>
#include <gdiplus.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <stdint.h>
#include <sstream>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Gdiplus.lib")

using namespace Gdiplus;

std::string SERVER_HOST = "localhost";
int SERVER_PORT = 9000;
std::string ROOM_ID = "room1";

SOCKET sockGlobal;

// -------------------- BASE64 --------------------
std::string base64_encode(const unsigned char* data, int len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -6;
    for (int i = 0; i < len; i++) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(tbl[(val >> valb) & 63]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(tbl[((val << 8) >> (valb + 8)) & 63]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

// 🔥 WebSocket random key
std::string random_key() {
    unsigned char temp[16];
    for (int i = 0; i < 16; i++) temp[i] = rand() % 255;
    return base64_encode(temp, 16);
}

// -------------------- CONNECT --------------------
bool websocket_connect() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    sockGlobal = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sockGlobal, (sockaddr*)&serverAddr, sizeof(serverAddr)) != 0) {
        std::cout << "❌ TCP connect failed\n";
        return false;
    }

    std::string key = random_key();

    std::string req =
        "GET /agent?room=" + ROOM_ID + " HTTP/1.1\r\n"
        "Host: " + SERVER_HOST + ":" + std::to_string(SERVER_PORT) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    send(sockGlobal, req.c_str(), req.size(), 0);

    char buffer[2048];
    int r = recv(sockGlobal, buffer, 2048, 0);

    if (r <= 0 || std::string(buffer).find("101") == std::string::npos) {
        std::cout << "❌ WS handshake failed\n";
        return false;
    }

    std::cout << "✅ WebSocket Connected to backend!\n";
    return true;
}

// -------------------- SCREEN CAPTURE --------------------
bool capture_screen(std::vector<unsigned char>& outJpg) {
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);

    HDC hScreen = GetDC(NULL);
    HDC hDC = CreateCompatibleDC(hScreen);

    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, w, h);
    SelectObject(hDC, hBitmap);

    BitBlt(hDC, 0, 0, w, h, hScreen, 0, 0, SRCCOPY);

    CLSID clsid;
    UINT num, size;
    GetImageEncodersSize(&num, &size);
    ImageCodecInfo* pInfo = (ImageCodecInfo*)(malloc(size));
    GetImageEncoders(num, size, pInfo);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(pInfo[i].MimeType, L"image/jpeg") == 0) {
            clsid = pInfo[i].Clsid;
            break;
        }
    }
    free(pInfo);

    IStream* stream = NULL;
    CreateStreamOnHGlobal(NULL, TRUE, &stream);

    Bitmap bmp(hBitmap, NULL);
    bmp.Save(stream, &clsid, NULL);

    HGLOBAL hMem;
    GetHGlobalFromStream(stream, &hMem);
    SIZE_T sizeJ = GlobalSize(hMem);

    void* data = GlobalLock(hMem);
    outJpg.assign((unsigned char*)data, (unsigned char*)data + sizeJ);
    GlobalUnlock(hMem);

    stream->Release();
    DeleteObject(hBitmap);
    DeleteDC(hDC);
    ReleaseDC(NULL, hScreen);

    return true;
}

// -------------------- SEND MASKED WS FRAME --------------------
void send_ws_binary(const std::vector<unsigned char>& data) {
    std::vector<unsigned char> frame;

    frame.push_back(0x82); // FIN + binary

    size_t len = data.size();
    unsigned char mask_key[4];
    for (int i = 0; i < 4; i++) mask_key[i] = rand() % 256;

    // payload length + MASK bit
    if (len <= 125) {
        frame.push_back(0x80 | (unsigned char)len);
    } else if (len <= 65535) {
        frame.push_back(0x80 | 126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--)
            frame.push_back((len >> (8 * i)) & 0xFF);
    }

    // mask key
    frame.insert(frame.end(), mask_key, mask_key + 4);

    // masked payload
    for (size_t i = 0; i < len; i++) {
        frame.push_back(data[i] ^ mask_key[i % 4]);
    }

    send(sockGlobal, (char*)frame.data(), frame.size(), 0);
}

// -------------------- HANDLE CONTROL --------------------
void handle_control(const std::string& json) {
    if (json.find("\"type\":\"mouse\"") != std::string::npos) {
        int x, y;
        sscanf(json.c_str(), "{\"type\":\"mouse\",\"x\":%d,\"y\":%d}", &x, &y);
        SetCursorPos(x, y);
    }
}

// -------------------- WS LISTENER --------------------
void ws_listener() {
    char buf[8192];
    while (true) {
        int r = recv(sockGlobal, buf, sizeof(buf), 0);
        if (r <= 0) break;

        size_t pos = 0;
        while (pos < r) {
            unsigned char b1 = buf[pos];
            unsigned char b2 = buf[pos + 1];

            bool masked = (b2 & 0x80) != 0;
            uint64_t payload_len = b2 & 0x7F;
            size_t header_len = 2;

            if (payload_len == 126) {
                payload_len = ((unsigned char)buf[pos + 2] << 8) | (unsigned char)buf[pos + 3];
                header_len += 2;
            } else if (payload_len == 127) {
                payload_len = 0;
                for (int i = 0; i < 8; i++) {
                    payload_len = (payload_len << 8) | (unsigned char)buf[pos + 2 + i];
                }
                header_len += 8;
            }

            unsigned char mask_key[4] = {0,0,0,0};
            if (masked) {
                for (int i = 0; i < 4; i++) mask_key[i] = buf[pos + header_len + i];
                header_len += 4;
            }

            std::string payload;
            for (uint64_t i = 0; i < payload_len; i++) {
                unsigned char byte = buf[pos + header_len + i];
                if (masked) byte ^= mask_key[i % 4];
                payload.push_back(byte);
            }

            // handle text frame only
            if ((b1 & 0x0F) == 1) handle_control(payload);

            pos += header_len + payload_len;
        }
    }
}

// -------------------- MAIN --------------------
int main() {
    GdiplusStartupInput gpsi;
    ULONG_PTR token;
    GdiplusStartup(&token, &gpsi, NULL);

    if (!websocket_connect()) {
        std::cout << "Exiting due to WS failure\n";
        return 0;
    }

    std::thread(ws_listener).detach();

    while (true) {
        std::vector<unsigned char> jpg;
        capture_screen(jpg);
        send_ws_binary(jpg);
        Sleep(80); // ~12 FPS
    }

    return 0;
}
