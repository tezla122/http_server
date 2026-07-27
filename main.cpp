#define WIN32_LEAN_AND_MEAN


#include <winsock2.h>
#include<ws2tcpip.h>

#include<atomic>
#include<cstring>
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
    constexpr std:: size_t kMaxBodySize = 1024 * 1024;
    constexpr DWORD kKeepAliveTimeoutMs = 5000;
    constexpr int kMaxKeepAliveRequests = 100 ;


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

    bool SendHttpResponse(SOCKET client , int statusCode , const std:: string& statusText , const std::string&contentType , const std:: string& body , bool includeBody , bool keep_alive) {
        const std::string headers =
       "HTTP/1.1 " + std::to_string(statusCode) + " " + statusText + "\r\n"
       "Content-Type: " + contentType + "\r\n"
       "Content-Length: " + std::to_string(body.size()) + "\r\n"
       "Connection: " + std::string(keep_alive ? "keep-alive" : "close") + "\r\n"
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
    bool req_line(const std:: string& request , std:: string& method , std::string& path , std::string&version) {
        method.clear();
        path.clear();
        version.clear();
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
        version = first_line.substr(second_space + 1);
        return !method.empty() && !path.empty() && !version.empty();
    }
    std::string Trim(const std::string& value) {
        const std::size_t start = value.find_first_not_of("\t");
        if (start == std::string::npos) {
            return{};
        }
        const std::size_t end = value.find_last_not_of("\t");
        return value.substr(start, end - start + 1);
    }

    bool HeaderNameEquals(const std::string&name , const char*target) {
        const std::size_t target_len = std::strlen(target);
        if (name.size() != target_len) {
            return false;
        }
        for (std::size_t i = 0 ; i < target_len ; ++i) {
            char c = name[i];
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != target[i]) {
                return false;
            }
        }
        return true ;
    }

    std::size_t HeaderEndOffset(const std::string& request) {
        const std:: size_t pos = request.find("\r\n\r\n");
        return (pos == std::string::npos) ? std::string::npos : pos + 4;
    }
    /*
    std::optional<std::size_t> ParseContentLength(const std::string&headers) {
        std::size_t pos = 0;
        while (pos < headers.length()) {
            const std::size_t line_end = headers.find("\r\n");
            if (line_end == std::string::npos) {
                break;
            }
            const std:: string line = headers.substr(pos , line_end  - pos);
            pos = line_end + 2 ;
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue ;
            }
            const std::string name = line.substr(0 , colon);
            if (!HeaderNameEquals(name , "content-length")) {
                continue;
            }
            const std:: string value = Trim(line.substr(colon + 1));
            if (value.empty()) {
                return std::nullopt;
            }
            try {
                const unsigned long long parsed = std::stoull(value);
                if (parsed > kMaxBodySize) {
                    return std::nullopt;
                }
            }catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }
*/


    std::optional<std::string> FindHeaderValue(const std::string& headers , const char* name) {
        std::size_t pos = 0;
        while (pos < headers.size()) {
            const std::size_t line_end = headers.find("\r\n");
            if (line_end == std::string::npos) {
                break;
            }
            const std::string line = headers.substr(pos , line_end  - pos);
            pos = line_end + 2 ;
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) {
                continue ;
            }
            if (!HeaderNameEquals(line.substr(0,colon) , name)) {
                continue;
            }
            return Trim(line.substr(colon + 1));
        }
        return std::nullopt;
    }

    bool ParseContentLength(const std::string& headers , std::size_t& body_length) {
        const std::optional<std::string> value = FindHeaderValue(headers , "content-length");
        if (!value) {
            body_length = 0;
            return true ;
        }else {
            if (value->empty()) {
                return false ;
            }
            try {
                const unsigned long long parsed = std::stoull(*value);
                if (parsed > kMaxBodySize) {
                    return false ;
                }
                body_length = static_cast<std::size_t>(parsed);
                return true ;
            }catch (...) {
                return false ;
            }
        }
    }
    bool WantsKeepAlive(const std::string&version , const std::string& headers) {
        const std::optional<std::string> connection = FindHeaderValue(headers , "connection");
        if (connection) {
            std::string lower = *connection ;
            for (char& c : lower) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            if (lower.find("close") != std::string::npos) {
                return false ;
            }
            if (lower.find("keep-alive") != std::string::npos) {
                return true ;
            }
        }
        return version == "HTTP/1.1";
    }
    bool RecvAppend(SOCKET client , std::string& buffer) {
        char chunk[kRecvBufferSize];
        const int received = recv(client ,chunk , sizeof(chunk) , 0);
        if (received == SOCKET_ERROR) {
            printWinsockError("recv");
            return false;
        }
        if (received == 0) {
            return false;
        }
        buffer.append(chunk , static_cast<std::size_t>(received));
        return true ;
    }
    bool ReadHTTPRequest(SOCKET client ,std::string& leftover,  std::string&request) {
        request.clear();
        while (true) {
            const std::size_t header_end = HeaderEndOffset(leftover);
            if (header_end != std::string::npos) {
                const std::string headers = leftover.substr(0,header_end);
                std::size_t body_length = 0;
                if (!ParseContentLength(headers , body_length)) {
                    return false;
                }
                const std::size_t total_needed = header_end + body_length;
                while (leftover.size() < total_needed) {
                    if (!RecvAppend(client , leftover)) {
                        return false;
                    }
                }
                request = leftover.substr(0,header_end);
                leftover.erase(0, total_needed);
                return true ;
            }
            if (!RecvAppend(client , request)) {
                return false;
            }
            if (leftover.size()  > kMaxBodySize + 8912) {
                return false ;
            }
        }
    }
    std::string RequestBody(const std::string& request) {
        const std::size_t header_end = HeaderEndOffset(request);
        if (header_end == std::string::npos || header_end == 0) {
            return {};
        }
        return request.substr( header_end);
    }
    std::string JsonEscape(const std::string& value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (const char c : value) {
            switch (c) {
                case '"':
                    escaped += "\\\"";
                    break;
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    escaped.push_back(c);
                    break;
            }
        }
        return escaped;
    }
    bool HandlePost(SOCKET client, const std::string& path, const std::string& body , bool keepAlive) {
        if (path != "/api/echo") {
            const std::string responseBody =
                "<!DOCTYPE html><html><body><h1>404 Not Found</h1>"
                "<p>No POST handler for this path.</p></body></html>";
            return SendHttpResponse(client, 404, "Not Found", "text/html; charset=utf-8",
                                    responseBody, true , keepAlive);
        }

        const std::string responseBody =
            "{\"path\":\"/api/echo\",\"bytes\":" + std::to_string(body.size()) +
            ",\"body\":\"" + JsonEscape(body) + "\"}";
        return SendHttpResponse(client, 200, "OK", "application/json; charset=utf-8", responseBody,
                                true , keepAlive);
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
    const DWORD recvTimeoutMs = kKeepAliveTimeoutMs;
    if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&recvTimeoutMs),
                   sizeof(recvTimeoutMs)) == SOCKET_ERROR) {
        printWinsockError("setsockopt(SO_RCVTIMEO)");
        closesocket(client);
        return;
    }

    std::string leftover;
    int requestsHandled = 0;

    while (g_running) {
        std::string request;
        if (!ReadHTTPRequest(client, leftover, request)) {
            break;
        }

        std::string method;
        std::string path;
        std::string version;
        if (!req_line(request, method, path, version)) {
            const std::string body =
                "<!DOCTYPE html><html><body><h1>400 Bad Request</h1>"
                "<p>Malformed request line.</p></body></html>";
            SendHttpResponse(client, 400, "Bad Request", "text/html; charset=utf-8", body, true,
                             false);
            break;
        }

        const std::size_t headerEnd = HeaderEndOffset(request);
        const std::string headers =
            (headerEnd == std::string::npos) ? std::string{} : request.substr(0, headerEnd - 4);

        bool keepAlive = WantsKeepAlive(version, headers);
        if (requestsHandled + 1 >= kMaxKeepAliveRequests) {
            keepAlive = false;
        }

        if (method == "POST") {
            const std::string body = RequestBody(request);
            if (!HandlePost(client, path, body, keepAlive)) {
                break;
            }
        } else if (method == "GET" || method == "HEAD") {
            const bool includeBody = (method == "GET");

            const std::optional<std::string> resolved = ResolveSafePath(path);
            if (!resolved) {
                const std::string body =
                    "<!DOCTYPE html><html><body><h1>400 Bad Request</h1>"
                    "<p>Invalid or unsafe path.</p></body></html>";
                if (!SendHttpResponse(client, 400, "Bad Request", "text/html; charset=utf-8", body,
                                      includeBody, keepAlive)) {
                    break;
                }
            } else {
                const std::optional<std::string> fileBytes = ReadFileFromBinary(*resolved);
                if (!fileBytes) {
                    const std::string body =
                        "<!DOCTYPE html><html><body><h1>404 Not Found</h1>"
                        "<p>The requested file was not found.</p></body></html>";
                    if (!SendHttpResponse(client, 404, "Not Found", "text/html; charset=utf-8", body,
                                          includeBody, keepAlive)) {
                        break;
                    }
                } else {
                    const std::string contentType = MimeForPath(*resolved);
                    if (!SendHttpResponse(client, 200, "OK", contentType, *fileBytes, includeBody,
                                          keepAlive)) {
                        break;
                    }
                }
            }
        } else {
            const std::string body =
                "<!DOCTYPE html><html><body><h1>405 Method Not Allowed</h1>"
                "<p>Only GET, HEAD, and POST are supported.</p></body></html>";
            if (!SendHttpResponse(client, 405, "Method Not Allowed", "text/html; charset=utf-8",
                                  body, true, keepAlive)) {
                break;
            }
        }

        ++requestsHandled;
        if (!keepAlive) {
            break;
        }
    }

    // Windows: closesocket(). Linux/POSIX: close().
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