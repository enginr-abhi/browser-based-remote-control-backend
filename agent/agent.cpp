#include <winsock2.h>
#include <windows.h>
#include <gdiplus.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstdlib> 
#include <algorithm> // for std::transform
#include <cctype> // for std::toupper

// Linker pragma (ensure these are present in your actual file if needed)
// #pragma comment(lib, "ws2_32.lib")
// #pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// --- Global Variables (Hardcoded/Fixed) ---
SOCKET ws;
bool running = true;
int screenW, screenH;
ULONG_PTR token; 

// ✅ FIX: Hardcoded Server Configuration
// NOTE: Live Deployment ke liye "127.0.0.1" ko apne domain name se badalna mat bhoolna!
const std::string SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 9000;
const std::string ROOM_ID = "room1"; 

std::string ip = SERVER_IP; 	
std::string room = ROOM_ID; 	


// --- Helper Functions Forward Declarations ---
void handleControl(const std::string& msg); 
void captureJPEG(std::vector<BYTE>& buf);
void streamThread();
void ws_mask_send(const std::vector<BYTE>& payload, int opcode);
void ws_send(const std::vector<BYTE>& buf);
void listenThread();

// Helper function to get the CLSID of the JPEG encoder
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
	UINT num = 0; 			// number of image encoders
	UINT size = 0; 		 // size of the image encoder array in bytes
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

// Helper: Virtual Key code mapping (Added for keyboard control)
BYTE getVkCode(const std::string& key) {
    if (key.length() == 1 && std::isalpha(key[0])) {
        // Simple letters (A-Z)
        return (BYTE)std::toupper(key[0]);
    }

    if (key.length() == 1 && std::isdigit(key[0])) {
        // Numbers (0-9)
        return (BYTE)(key[0]); // ASCII '0' to '9' corresponds to VK_0 to VK_9
    }

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
    if (key == "F12") return VK_F12; // Example for function keys
    if (key == " ") return VK_SPACE;
    
    // NOTE: Special characters like !, @, # require Shift check (complex)
    return 0; 
}

// ======================================
// 🛠️ WS SEND WITH FRAMING & MASKING
// ======================================

void ws_mask_send(const std::vector<BYTE>& payload, int opcode) {
	if (ws == INVALID_SOCKET || payload.empty()) return;
	
	std::vector<BYTE> frame;
	size_t length = payload.size();

	// 1. First Byte (FIN=1, Opcode) - 0x82 for Binary
	frame.push_back(0x80 | opcode); 

	// 2. Second Byte (Mask=1, Payload Length)
	if (length <= 125) {
		frame.push_back(0x80 | (BYTE)length);
	} else if (length <= 65535) {
		frame.push_back(0x80 | 126);
		frame.push_back((BYTE)(length >> 8));
		frame.push_back((BYTE)length);
	} else {
		frame.push_back(0x80 | 127);
		// 8-byte extended payload length
		for (int i = 7; i >= 0; i--) {
			frame.push_back((BYTE)((length >> (i * 8)) & 0xFF));
		}
	}

	// 3. Masking Key (4 bytes)
	BYTE mask[4];
	for (int i = 0; i < 4; i++) {
		mask[i] = (BYTE)(rand() % 256);
		frame.push_back(mask[i]);
	}

	// 4. Masked Payload
	std::vector<BYTE> masked_payload(length);
	for (size_t i = 0; i < length; i++) {
		masked_payload[i] = payload[i] ^ mask[i % 4]; 
	}
	frame.insert(frame.end(), masked_payload.begin(), masked_payload.end());

	// 5. Send the full frame
	send(ws, (const char*)frame.data(), frame.size(), 0);
}

void ws_send(const std::vector<BYTE>& buf) {
	ws_mask_send(buf, 0x02); // 0x02 = Binary Frame (for JPEG)
}

// ======================================
// 🖼️ CAPTURE AND ENCODE FUNCTION
// ======================================
void captureJPEG(std::vector<BYTE>& buf) {
	// 1. Clear previous buffer data
	buf.clear(); 

	// Get screen device context
	HDC hDesktop = GetDC(NULL);
	HDC hCapture = CreateCompatibleDC(hDesktop);

	// Create a bitmap object
	HBITMAP hBitmap = CreateCompatibleBitmap(hDesktop, screenW, screenH);
	SelectObject(hCapture, hBitmap);

	// Copy screen to memory (hCapture)
	BitBlt(hCapture, 0, 0, screenW, screenH, hDesktop, 0, 0, SRCCOPY | CAPTUREBLT);
	
	// Release resources early
	ReleaseDC(NULL, hDesktop);
	DeleteDC(hCapture);

	// 2. GDI+ Bitmap creation
	Bitmap* pBitmap = new Bitmap(hBitmap, NULL);
	CLSID clsidEncoder;
	
	// Get JPEG Encoder CLSID
	if (GetEncoderClsid(L"image/jpeg", &clsidEncoder) != -1) {
		// Create an IStream to save the JPEG data to memory
		IStream* pStream = NULL;
		if (CreateStreamOnHGlobal(NULL, TRUE, &pStream) == S_OK) {
			
			// 3. Save Bitmap to IStream as JPEG
			// Set Quality to 50
			EncoderParameters encoderParams;
			encoderParams.Count = 1;
			encoderParams.Parameter[0].Guid = EncoderQuality;
			encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
			encoderParams.Parameter[0].NumberOfValues = 1;
			
			ULONG quality = 50; 
			encoderParams.Parameter[0].Value = &quality;

			if (pBitmap->Save(pStream, &clsidEncoder, &encoderParams) == Ok) {
				// 4. Read IStream data into std::vector<BYTE> (buf)
				STATSTG statstg;
				pStream->Stat(&statstg, STATFLAG_NONAME);
				ULARGE_INTEGER ulnSize = statstg.cbSize;
				
				buf.resize((size_t)ulnSize.QuadPart);
				
				LARGE_INTEGER liZero = {0};
				pStream->Seek(liZero, STREAM_SEEK_SET, NULL); 

				pStream->Read(buf.data(), (ULONG)ulnSize.QuadPart, NULL);
			}
			pStream->Release();
		}
	}

	// 5. Cleanup
	delete pBitmap;
	DeleteObject(hBitmap);
}

// ======================================
// 🖱️ MOUSE & ⌨️ KEYBOARD CONTROL HANDLING FUNCTION (FIXED)
// ======================================
void handleControl(const std::string& msg) {
    // NOTE: This uses basic string search/parsing, assuming the JSON format is consistent.
    
    // --- 1. MOUSE CONTROL ---
    if (msg.find("\"type\":\"mouse\"") != std::string::npos) {
        
        // --- 1.1 Extract Mouse Data ---
        size_t x_pos = msg.find("\"x\":");
        size_t y_pos = msg.find("\"y\":");
        size_t act_pos = msg.find("\"action\":\"");
        size_t btn_pos = msg.find("\"button\":");

        if (x_pos == std::string::npos || y_pos == std::string::npos || act_pos == std::string::npos) return;

        // Extract values using simple substring/find (assuming float/int format)
        std::string x_str = msg.substr(x_pos + 4, msg.find(",", x_pos) - (x_pos + 4));
        std::string y_str = msg.substr(y_pos + 4, msg.find(",", y_pos) - (y_pos + 4));
        std::string action_str = msg.substr(act_pos + 10, msg.find("\"", act_pos + 10) - (act_pos + 10));
        std::string button_str = msg.substr(btn_pos + 9, msg.find("}", btn_pos) - (btn_pos + 9)); 

        float x_norm = std::stof(x_str);
        float y_norm = std::stof(y_str);
        int button_code = std::stoi(button_str);

        // --- 1.2 Setup Input Structure ---
        INPUT input = {0};
        input.type = INPUT_MOUSE;

        // 🎯 FIX APPLIED: Normalized coordinates (0.0 to 1.0) ko Absolute Windows Coordinates (0 se 65535) 

         int x_abs = (int)(x_norm * 65536.0f); 
         int y_abs = (int)(y_norm * 65536.0f);


        x_abs = std::min(x_abs, 65535);
        y_abs = std::min(y_abs, 65535);
        
        // --- 1.3 Send Movement (Always needed for accurate click position) ---
        input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
        input.mi.dx = x_abs;
        input.mi.dy = y_abs;
        SendInput(1, &input, sizeof(INPUT));

        // --- 1.4 Handle Button Actions ---
        if (action_str == "down" || action_str == "up") {
            DWORD dwFlag = 0;
            
            if (button_code == 0) { // Left Button (Primary)
                dwFlag = (action_str == "down") ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            } else if (button_code == 2) { // Right Button (Secondary)
                dwFlag = (action_str == "down") ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            } else if (button_code == 1) { // Middle Button
                dwFlag = (action_str == "down") ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            }
            
            if (dwFlag != 0) {
                input.mi.dwFlags = dwFlag;
                // Position ko 0 rakhte hain kyonki yeh position pehle hi MOUSEEVENTF_MOVE se set ho chuki hai.
               input.mi.dx = 0; 
               input.mi.dy = 0;
                SendInput(1, &input, sizeof(INPUT));
            }
        }
    } 

    // --- 2. SCROLL CONTROL ---
    else if (msg.find("\"type\":\"scroll\"") != std::string::npos) {
        size_t delta_pos = msg.find("\"delta\":");
        if (delta_pos == std::string::npos) return;
        
        // Extract delta value
        std::string delta_str = msg.substr(delta_pos + 8, msg.find("}", delta_pos) - (delta_pos + 8));
        int delta = std::stoi(delta_str);

        INPUT input = {0};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        // Invert delta and reduce sensitivity
        input.mi.mouseData = (DWORD)(-delta / 3); 
        
        SendInput(1, &input, sizeof(INPUT));
    }
    
    // --- 3. KEYBOARD CONTROL ---
    else if (msg.find("\"type\":\"key\"") != std::string::npos) {
        size_t key_pos = msg.find("\"key\":\"");
        size_t state_pos = msg.find("\"state\":\"");

        if (key_pos == std::string::npos || state_pos == std::string::npos) return;

        // Extract key and state
        std::string key_str = msg.substr(key_pos + 7, msg.find("\"", key_pos + 7) - (key_pos + 7));
        std::string state_str = msg.substr(state_pos + 9, msg.find("\"", state_pos + 9) - (state_pos + 9));

        BYTE vk = getVkCode(key_str);

        if (vk != 0) {
            INPUT input = {0};
            input.type = INPUT_KEYBOARD;
            
            // Map character keys to Virtual Key codes
            if (vk >= '0' && vk <= '9' || vk >= 'A' && vk <= 'Z') {
                input.ki.wVk = 0; // wVk should be 0 for standard characters to use wScan
                input.ki.wScan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
                input.ki.dwFlags = KEYEVENTF_SCANCODE;
            } else {
                input.ki.wVk = vk; // Use wVk for special/function keys
            }
            
            if (state_str == "up") {
                input.ki.dwFlags |= KEYEVENTF_KEYUP; // Set flag for key release
            }
            // else: dwFlags remains default for key down

            SendInput(1, &input, sizeof(INPUT));
        }
    }
}


void streamThread() {
	std::vector<BYTE> buffer;
	while (running) {
		captureJPEG(buffer);
		if (!buffer.empty()) {
			ws_send(buffer);
		}
		Sleep(33); // ~30 FPS
	}
}
// --------------------------------------------------------

// ======================================
// 🛠️ LISTEN THREAD WITH DEFRAMING
// ======================================
void listenThread() {
	char buf[4096];
	while (running) {
		int len = recv(ws, buf, 4096, 0);
		if (len <= 0) break;

		// Basic WebSocket Deframing Logic (Server->Client: Mask=0)

		// 1. First Byte: Check Opcode (expect Text = 0x01)
		int opcode = buf[0] & 0x0F;
		if (opcode != 0x01 && opcode != 0x08) continue; // 0x01=Text, 0x08=Close

		if (opcode == 0x08) { // Close frame received
			running = false;
			break;
		}

		// 2. Second Byte: Get Payload Length (assuming len < 126)
		size_t payload_len = buf[1] & 0x7F;

		if (payload_len > len - 2) continue; // Not enough data in buffer (incomplete frame)
		
		// 3. Extract payload (Payload starts at buf[2] for small frames)
		std::string msg(buf + 2, payload_len);
		
		handleControl(msg);
	}
	running = false;
}


// ======================================
// 🛠️ MAIN FUNCTION
// ======================================
int main() {
	// 1. Init GDI+ 
	GdiplusStartupInput gdiplusStartupInput;
	GdiplusStartup(&token, &gdiplusStartupInput, NULL);

	// Screen size
	screenW = GetSystemMetrics(SM_CXSCREEN);
	screenH = GetSystemMetrics(SM_CYSCREEN);

	// 2. Setup Winsock and Connection
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	ws = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(SERVER_PORT); 
	inet_pton(AF_INET, ip.c_str(), &addr.sin_addr); 

	if (connect(ws, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
		MessageBoxA(0, ("Failed to connect to server: " + ip + ":" + std::to_string(SERVER_PORT)).c_str(), "Error", 0);
		closesocket(ws);
		WSACleanup();
		GdiplusShutdown(token);
		return 0;
	}

	// 3. Send WebSocket Handshake
	std::string header = 
		"GET /agent?room=" + room + " HTTP/1.1\r\n"
		"Host: " + ip + ":" + std::to_string(SERVER_PORT) + "\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" 
		"Sec-WebSocket-Version: 13\r\n\r\n";
	
	send(ws, header.c_str(), header.size(), 0);

	// 4. Check Handshake Response 
	char response_buf[2048];
	int bytes_received = recv(ws, response_buf, sizeof(response_buf) - 1, 0);
	
	if (bytes_received <= 0) {
		MessageBoxA(0, "Did not receive handshake response from server.", "Error", 0);
		closesocket(ws);
		WSACleanup();
		GdiplusShutdown(token);
		return 0;
	}
	
	response_buf[bytes_received] = '\0'; 

	std::string response(response_buf);

	if (response.find("101 Switching Protocols") == std::string::npos) {
		MessageBoxA(0, "WebSocket Handshake Failed (Server did not accept protocol switch).", "Error", 0);
		closesocket(ws);
		WSACleanup();
		GdiplusShutdown(token);
		return 0;
	}
	
	// 5. Start Threads
	std::thread t1(listenThread);
	std::thread t2(streamThread);

	t1.join();
	t2.join();

	// 6. Cleanup
	closesocket(ws);
	WSACleanup();
	GdiplusShutdown(token);
	return 0;
}