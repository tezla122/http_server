# Winsock Multithreaded HTTP Server

A small TCP HTTP server for Windows written in C++ with the Winsock2 API. The main thread accepts connections; each client is handled on its own `std::thread`.

## Requirements

- Windows
- MSVC (`cl.exe`) — Visual Studio or Build Tools with the C++ workload
## Run

```bat
http_server.exe
```

The server listens on `http://127.0.0.1:8080/` (all interfaces, port `8080`). Press **Ctrl+C** to shut down cleanly.

## Test

```bat
curl http://127.0.0.1:8080/
curl http://127.0.0.1:8080/hello
```

You should get `HTTP/1.1 200 OK` with a short HTML page that echoes the request method and path.

## Behavior

| Piece | Detail |
| --- | --- |
| API | Winsock2 (`winsock2.h` / `ws2tcpip.h`), linked with `ws2_32.lib` |
| Concurrency | One `std::thread` per connection (SOCKET passed by value) |
| HTTP | Minimal request-line parse; single response then connection close |
| Shutdown | Ctrl+C closes the listen socket, joins workers, then `WSACleanup` |

## Layout

```
main.cpp          Full server implementation
http_server.exe   Built binary (after compile)
```
