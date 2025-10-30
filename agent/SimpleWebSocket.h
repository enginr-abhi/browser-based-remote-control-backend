// ===== SimpleWebSocket.h =====
#pragma once
#include <iostream>
#include <thread>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

class SimpleWebSocket {
private:
    SOCKET sock;
    std::thread recvThread;
    bool connected = false;
    std::string serverAddr;
    int serverPort;

public:
    SimpleWebSocket(std::string addr, int port)
        : serverAddr(addr), serverPort(port), sock(INVALID_SOCKET) {}

    bool connectToServer() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::cerr << "WSAStartup failed\n";
            return false;
        }

        struct addrinfo *result = NULL, hints;
        ZeroMemory(&hints, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        std::string portStr = std::to_string(serverPort);
        if (getaddrinfo(serverAddr.c_str(), portStr.c_str(), &hints, &result) != 0) {
            std::cerr << "getaddrinfo failed\n";
            WSACleanup();
            return false;
        }

        sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (sock == INVALID_SOCKET) {
            std::cerr << "Socket creation failed\n";
            freeaddrinfo(result);
            WSACleanup();
            return false;
        }

        if (::connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
            std::cerr << "Connection failed\n";
            closesocket(sock);
            WSACleanup();
            return false;
        }

        freeaddrinfo(result);
        connected = true;
        std::cout << "✅ Connected to server!\n";

        recvThread = std::thread([this]() {
            char buffer[2048];
            while (connected) {
                int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
                if (bytes > 0) {
                    buffer[bytes] = '\0';
                    std::cout << "📩 Server: " << buffer << std::endl;
                }
                else break;
            }
        });

        return true;
    }

    void sendMessage(std::string msg) {
        if (!connected) return;
        send(sock, msg.c_str(), (int)msg.length(), 0);
    }

    void closeSocket() {
        connected = false;
        closesocket(sock);
        WSACleanup();
        if (recvThread.joinable()) recvThread.join();
    }

    ~SimpleWebSocket() {
        closeSocket();
    }
};
