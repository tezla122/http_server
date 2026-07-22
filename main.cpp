#include <winsock2.h>
#include<ws2tcpip.h>
#include<atomic>
#include<iostream>
#include<mutex>
#include<string>
#include<thread>
#include<vector>
#define WIN32_LEAN_AND_MEAN


#pragma comment(lib,"ws_32.lib");

namespace {
    constexpr unsigned short kPort = 8080;
    constexpr int kRecvBufferSize = 4096;
    constexpr int kListenBackLog = SOMAXCONN;

    SOCKET g_listenSocket = INVALID_SOCKET;
    std:: atomic<bool> g_running{true};
    std::mutex g_workersMutex;
    std::vector<std::thread> g_workers;

    void printWinsockError(const char*what) {
        const int err = WSAGetLastError();
        std::cerr << what << ": " << err << std::endl;
    }

    BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
        if (ctrlType == CTRL_C_EVENT ||  ctrlType == CTRL_BREAK_EVENT ||
            ctrlType == CTRL_CLOSE_EVENT) {
            std:: cout << "\n Shutting down\n" << std::endl;
            g_running = false;

            if (g_listenSocket  != INVALID_SOCKET) {
                closesocket(g_listenSocket);
                g_listenSocket = INVALID_SOCKET;
            }
            return TRUE;
        }
        return FALSE;
    }

    void ParseRequestLine(const std::string& request , std:: string& method , std::string& path) {
        method = "GET" ;
        path = "/";
        const std:: size_t line_end = request.find("\r\n");
        const std:: string  first_line = (line_end == std::string::npos) ? request : request.substr(0, line_end);
        const std::size_t first_space = first_line.find(" ");
        if (first_space == std::string::npos) {
            return ;
        }
        method = first_line.substr(0, first_space);
        const std::size_t second_space = first_line.find(' ' , first_space + 1);
        if (second_space == std::string::npos) {
            path = first_line.substr(first_space + 1);
            return ;
        }
        path = first_line.substr(first_space + 1 , second_space - first_space - 1 );
    }
    void HandleClient(SOCKET client) {
        char buffer[kRecvBufferSize];
        const int received = recv(client, buffer, sizeof(buffer) - 1, 0);
        if (received == SOCKET_ERROR) {
            printWinsockError(buffer);
            closesocket(client);
            return ;
        }
        if (received == 0) {
            closesocket(client);
            return ;
        }
        buffer[received] = '\0';
        const std:: string request(buffer , static_cast<size_t>(received));
        std::string method ;
        std:: string path ;

        ParseRequestLine(request , method, path);

        const std::string body =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Winsock HTTP</title></head>"
        "<body><h1>Hello from Winsock</h1>"
        "<p>Method: " +
        method + "</p><p>Path: " + path + "</p></body></html>";

        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " +
            std::to_string(body.size()) +
            "\r\n"
            "Connection: close\r\n"
            "\r\n" +
            body;

        const char* data = response.data();
        int remaining = static_cast<int>(response.size());
        while (remaining > 0) {
            const int sent = send(client, data, remaining, 0);
            if (sent == SOCKET_ERROR) {
                printWinsockError("send");
                break;
            }
            data += sent ;
            remaining -= sent;

        }
        closesocket(client);
    }

};


int main() {
    std:: cout << "Winsock init " << std::endl;

    // init the data
    WSADATA wsaData{};
    const int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0) {
        std:: cerr << "WSAStartup failed: " << wsaResult << std::endl;
    }
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        std:: cerr << "SetConsoleCtrlHandler failed" << std::endl;
        WSACleanup();
        return 1;
    }
 // create the listing socket
    g_listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listenSocket == INVALID_SOCKET) {
        printWinsockError("socket");
        WSACleanup();
        return 1;

    }
    // socket options
    BOOL reuse = TRUE ;
    if (setsockopt(g_listenSocket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse)) == SOCKET_ERROR) {
        printWinsockError("setsockopt(SO_REUSEADDR)");
        closesocket(g_listenSocket);
        WSACleanup();
        return 1;
    }
// bind to address and port
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 0.0.0.0  all interfaces
    if (bind(g_listenSocket , reinterpret_cast<sockaddr*>(&addr) , sizeof(addr)) == SOCKET_ERROR) {
        printWinsockError("bind");
        closesocket(g_listenSocket);
        WSACleanup();
        return 1;
    }
    // listening for connections
    if (listen(g_listenSocket, kListenBackLog) == SOCKET_ERROR) {
        printWinsockError("listen");
        closesocket(g_listenSocket);
        WSACleanup();
        return 1;
    }
    std::cout << "Listening  on http://127.0.0.1:" << kPort << std::endl;
   // accept loop
    while (g_running) {
        sockaddr_in clientAddr{};
        int clientAsddrLength = sizeof(clientAddr);
        const SOCKET client = accept(g_listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientAsddrLength);
        if (client == INVALID_SOCKET) {
            if (!g_running) {
                break; // for ctrl c
            }
            printWinsockError("accept");
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_workersMutex);
            g_workers.emplace_back(HandleClient , client);
        }
    }

// joining workers
    {
        std:: lock_guard<std::mutex> lock(g_workersMutex);
        for (std::thread& t : g_workers) {
            if (t.joinable()) {
                t.join();
            }
        }
        g_workers.clear();
    }
if (g_listenSocket != INVALID_SOCKET) {
    closesocket(g_listenSocket);
    g_listenSocket = INVALID_SOCKET;
}
    WSACleanup();
    std::cout<<"Server stopped" << std::endl;


    return 0;
}