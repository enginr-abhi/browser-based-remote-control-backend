// agent.cpp — Windows-only simple JPEG streaming agent
// Matches your backend protocol
// Requirements: g++ with -lws2_32 -lgdi32 -lgdiplus -luser32 -lole32 -lcrypt32
// SSL verification disabled for simplicity.

#include <winsock2.h>
#include <windows.h>
#include <gdiplus.h>
#include <wincrypt.h>
#include <string>
#include <thread>
#include <vector>
#include <iostream>
#include <sstream>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "crypt32.lib")

// ----------------- GLOBALS -------------------
ULONG_PTR gdiplusToken;
SOCKET sock = INVALID_SOCKET;
HWND desktop = GetDesktopWindow();
bool running = true;

// ----------------------------------------------
// Base64 decode (for JSON control data if needed)
// ----------------------------------------------

// Simple JSON parsing (only small objects)
#include <iomanip>
#include <map>

std::map<std::string, std::string> parseJson(const std::string &s){
    std::map<std::string, std::string> out;
    std::string key, val;
    bool inKey=false, inVal=false;
    for(size_t i=0;i<s.size();++i){
        if(s[i]=='"'){
            size_t j=s.find('"', i+1);
            if(j==std::string::npos) break;
            std::string str=s.substr(i+1, j-i-1);
            i=j;
            if(!inKey){ key=str; inKey=true; }
            else if(!inVal){ val=str; out[key]=val; inVal=true; inKey=false; }
            continue;
        }
        if(s[i]==',' || s[i]=='}'){ inVal=false; }
    }
    return out;
}

// ------------------------------------------------
// Screen capture -> JPEG -> memory buffer
// ------------------------------------------------
bool captureScreenJPEG(std::vector<BYTE> &out){
    HDC hScreen = GetDC(NULL);
    HDC hDC = CreateCompatibleDC(hScreen);

    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);

    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, w, h);
    SelectObject(hDC, hBitmap);
    BitBlt(hDC,0,0,w,h,hScreen,0,0,SRCCOPY);

    Gdiplus::Bitmap bmp(hBitmap, NULL);
    CLSID clsid;
    UINT num=0,size=0;
    Gdiplus::GetImageEncodersSize(&num,&size);
    std::vector<BYTE> buf(size);
    auto *enc=(Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num,size,enc);
    for(UINT i=0;i<num;i++) if(wcscmp(enc[i].MimeType,L"image/jpeg")==0) clsid=enc[i].Clsid;

    Gdiplus::EncoderParameters ep;
    ep.Count=1;
    ep.Parameter[0].Guid = Gdiplus::EncoderQuality;
    ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    ULONG q=50;
    ep.Parameter[0].NumberOfValues=1;
    ep.Parameter[0].Value=&q;

    IStream *stream=NULL;
    CreateStreamOnHGlobal(NULL, TRUE, &stream);
    bmp.Save(stream,&clsid,&ep);

    HGLOBAL hGlobal=NULL;
    GetHGlobalFromStream(stream,&hGlobal);
    SIZE_T sz=GlobalSize(hGlobal);
    BYTE *p=(BYTE*)GlobalLock(hGlobal);
    out.assign(p,p+sz);
    GlobalUnlock(hGlobal);
    stream->Release();

    DeleteObject(hBitmap);
    DeleteDC(hDC);
    ReleaseDC(NULL,hScreen);
    return true;
}

// ------------------------------------------------
// Send raw binary frame
// ------------------------------------------------
bool sendFrame(){
    std::vector<BYTE> jpg;
    if(!captureScreenJPEG(jpg)) return false;

    uint32_t size = htonl((uint32_t)jpg.size());
    send(sock, (char*)&size, 4, 0);
    send(sock, (char*)jpg.data(), jpg.size(), 0);
    return true;
}

// ------------------------------------------------
// Input events
// ------------------------------------------------
void handleControl(const std::string &json){
    auto d = parseJson(json);
    if(d.find("type") == d.end()) return;

    if(d["type"] == "mouse"){
        int x = std::stoi(d["x"]);
        int y = std::stoi(d["y"]);
        SetCursorPos(x,y);
        if(d["event"] == "down"){ mouse_event(MOUSEEVENTF_LEFTDOWN,0,0,0,0); }
        if(d["event"] == "up"){ mouse_event(MOUSEEVENTF_LEFTUP,0,0,0,0); }
    }
    if(d["type"] == "keyboard"){
        INPUT ip={0};
        ip.type=INPUT_KEYBOARD;
        ip.ki.wVk = VkKeyScanA(d["key"][0]);
        if(d["event"]=="down") ip.ki.dwFlags=0;
        if(d["event"]=="up") ip.ki.dwFlags=KEYEVENTF_KEYUP;
        SendInput(1,&ip,sizeof(ip));
    }
    if(d["type"] == "scroll"){
        int delta = std::stoi(d["deltaY"]);
        mouse_event(MOUSEEVENTF_WHEEL,0,0,delta,0);
    }
}

// ------------------------------------------------
// Reader thread
// ------------------------------------------------
void readerThread(){
    while(running){
        char hdr[4];
        int r = recv(sock, hdr, 4, 0);
        if(r<=0){ running=false; break; }
        int sz = *(int*)hdr;
        sz = ntohl(sz);
        if(sz<=0 || sz>2000000){ running=false; break; }
        std::string buf(sz, '\0');
        int got=0;
        while(got<sz){
            int x = recv(sock, &buf[got], sz-got, 0);
            if(x<=0){ running=false; break;} 
            got+=x;
        }
        handleControl(buf);
    }
}

// ------------------------------------------------
// MAIN
// ------------------------------------------------
int main(){
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&gdiplusToken,&gsi,NULL);
    WSADATA w;
    WSAStartup(MAKEWORD(2,2), &w);

    std::string room;
    std::cout << "Enter room ID: ";
    std::cin >> room;

    std::string host = "browser-based-remote-control-backend.onrender.com";
    std::string port = "443";

    struct addrinfo hints={}, *res;
    hints.ai_socktype=SOCK_STREAM;
    hints.ai_family=AF_INET;
    hints.ai_protocol=IPPROTO_TCP;

    if(getaddrinfo(host.c_str(), port.c_str(), &hints, &res)!=0){
        std::cout<<"DNS failed";
        return 0;
    }

    sock = socket(res->ai_family,res->ai_socktype,res->ai_protocol);
    if(connect(sock,res->ai_addr,(int)res->ai_addrlen)!=0){
        std::cout<<"Connect failed";
        return 0;
    }
    freeaddrinfo(res);

    // ------------------------------
    // Disable TLS (Render allows raw WS over SSL terminator)
    // ------------------------------
    std::string req =
        "GET /agent?room=" + room + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\n\r\n";

    send(sock, req.c_str(), req.size(),0);

    char resp[2048];
    recv(sock, resp, sizeof(resp), 0);

    std::thread t(readerThread);

    while(running){
        sendFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    t.join();
    closesocket(sock);
    WSACleanup();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}