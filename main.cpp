#define WIN32_LEAN_AND_MEAN


#include <winsock2.h>
#include<ws2tcpip.h>

#include<atomic>
#include<iostream>
#include<fstream>
#include<optional>
#include<unordered_map>
#include<mutex>
#include<string>
#include<thread>
#include<vector>


#pragma comment(lib,"ws_32.lib");

namespace {
    constexpr unsigned short kPort = 8080;
    constexpr int kRecvBufferSize = 4096;
    constexpr int kListenBackLog = SOMAXCONN;
    // doc root for static files
    constexpr const char* kRootDir = "./public";



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



    bool SendAll(SOCKET client , const char* data , std:: size_t len) {
        std::size_t sentTotal = 0 ;
        while (sentTotal < len) {
            const int chunk = static_cast <int>((len - sentTotal) > static_cast<std::size_t>(INT_MAX) ? INT_MAX : (len - sentTotal) );
            const int sent = send(client , data + sentTotal , chunk, 0);
            if (sent == SOCKET_ERROR) {
                    printWinsockError("send");
                return false ;
            }
            sentTotal += sent ;
        }
        return true ;
    }

    bool SendHttpResponse(SOCKET client , int stausCode , const std:: string& statusText , const std::string&contentType , const std:: string& body , bool includeBody) {
        const std:: string headers = "HTTP/1.1 " + std::to_string(stausCode) + " " + statusText + "\r\n"
        "Content-Type: " + contentType + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n";
        if (!SendAll(client , headers.data() , headers.size())) {
            return false ;
        }
        if (includeBody && !body.empty()) {
            if (!SendAll(client , body.data() , body.size())) {
                return false;
            }
        }
        return true ;
    }

// used for cheking the correct request from the client
    bool req_line(const std:: string& request , std:: string& method , std::string& path) {
        method.clear();
        path.clear();
        const std:: size_t line_end = request.find("\r\n");
        const std::string first_line = (line_end == std::string::npos) ? request : request.substr(0, line_end);
        if (first_line.empty()) {
            return false;
        }
        const std:: size_t first_space = first_line.find(' ');
        if (first_space == std::string::npos || first_space == 0) {
            return false;
        }
        method = first_line.substr( 0 , first_space);
        const std::size_t second_space = first_line.find(' ' , first_space + 1);
        if (second_space == std::string::npos) {
            return false ;
        }
        path = first_line.substr( first_space + 1 , second_space - first_space - 1 );
        return !method.empty() && !path.empty();
    }
    const std::unordered_map<std::string, std::string>& MimeTable() {
        static const std::unordered_map<std::string, std::string> kTable = {
            {".html", "text/html; charset=utf-8"},
            {".htm", "text/html; charset=utf-8"},
            {".css", "text/css; charset=utf-8"},
            {".js", "application/javascript; charset=utf-8"},
            {".json", "application/json; charset=utf-8"},
            {".png", "image/png"},
            {".jpg", "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".gif", "image/gif"},
            {".svg", "image/svg+xml"},
            {".txt", "text/plain; charset=utf-8"},
            {".ico", "image/x-icon"},
        };
        return kTable;
    }


    std::string ExtensionOf(const std::string & filepath) {
        const std::size_t slash = filepath.find_last_of("/\\");
        const std::size_t name_start = (slash == std::string::npos) ? 0 : slash + 1;
        const std::size_t dot = filepath.find_last_of('.');
        if (dot == std::string::npos || dot < name_start) {
            return {};
        }
        std::string ext = filepath.substr(dot);
        for (char& c : ext) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return ext;
    }

    std::string MimeForPath(const std:: string& filePath) {
        const auto& table = MimeTable();
        const std:: string ext = ExtensionOf(filePath);
        const auto it = table.find(ext);
        if (it != table.end()) {
            return it->second;
        }
        return "application/octet-stream";
    }
    std::optional<std::string>ResolveSafePath(const std::string& url_path) {
        if (url_path.empty()) {
            return std::nullopt;
        }
        std::string cleaned = url_path;
        for (char&c : cleaned) {
            if (c == '\\') {
                c = '/';
            }
        }
        std::string segment;
        for (std:: size_t i = 0; i <= cleaned.length(); ++i) {
            const bool atEnd = (i == cleaned.size());
            const char ch = atEnd ? '/' : cleaned[i];
            if (ch == '/') {
                if (segment == "..") {
                    return std::nullopt;
                }
                if (segment.size() >= 2 && segment[1] == ':') {
                    return std::nullopt;
                }
                segment.clear();
            }else {
                segment.push_back(ch);
            }

        }
        std::string relative = cleaned ;
        if (relative == "/") {
            relative = "/index.html";
        }
        return std::string(kRootDir) + relative ;
    }


    std::optional<std::string>ReadFileFromBinary(const std::string&file_path) {
        std::ifstream in(file_path , std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        in.seekg( 0 ,std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0) {
            return std::nullopt;
        }
        in.seekg( 0, std::ios::beg);
        std::string data;
        data.resize(static_cast<size_t>(size));
        if (size >0) {
            in.read(&data[0] , size);
            if (!in) {
                return std::nullopt;
            }
        }
        return data;
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
        if (!req_line(request, method, path)) {
            const std:: string body ="<!DOCTYPE html><html><body><h1>400 Bad Request</h1>"
            "<p>Malformed request line.</p></body></html>";
            SendHttpResponse(client , 400 , "Bad request" , "text/html; charset=utf-8" , body , true);
            closesocket(client);
            return ;
        }
        if (method != "GET" && method != "HEAD") {
            const std::string body = "<!DOCTYPE html><html><body><h1>405 Method Not Allowed</h1>"
            "<p>Only GET and HEAD are supported.</p></body></html>";
            SendHttpResponse(client , 405 , "Method Not Allowed" , "text/html", body , true);
            closesocket(client);
            return;
        }
        const bool includeBody = (method == "GET");
        const std::optional<std::string> resolved = ResolveSafePath(path);
        if (!resolved) {
            const std:: string body = "<!DOCTYPE html><html><body><h1>400 Bad Request</h1>"
            "<p>Invalid or unsafe path.</p></body></html>";
            SendHttpResponse(client , 400 , "Bad request" , "txt/html", body , true);
            closesocket(client);
            return ;
        }
        const std::optional<std::string> file_bytes = ReadFileFromBinary(*resolved);
        if (!file_bytes) {
            const std::string body =  "<!DOCTYPE html><html><body><h1>404 Not Found</h1>"
            "<p>The requested file was not found.</p></body></html>";
            SendHttpResponse(client, 404, "Not Found", "text/html; charset=utf-8", body, includeBody);
            closesocket(client);
            return;
        }
        const std::string content_type = MimeForPath(*resolved);
        SendHttpResponse(client , 200 , "OK" , content_type , *file_bytes , includeBody);
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
        return 1;
    }
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        std:: cerr << "SetConsoleCtrlHandler failed" << std::endl;
        WSACleanup();
        return 1;
    }
 // create the listing socket
    g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
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