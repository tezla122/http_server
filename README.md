# Winsock Multithreaded HTTP Server

## Description

This project is a teaching-oriented TCP HTTP server for Windows, implemented in a single C++ file using the native **Winsock2** API (not POSIX sockets).

It walks through the real Windows socket lifecycle: `WSAStartup`, create a listening socket, set options, `bind`, `listen`, then accept clients on the main thread. Each accepted connection is handed to a new `std::thread`, which reads a full HTTP request (headers and optional body), parses the request line (`METHOD PATH VERSION`), and responds. The client socket is then closed (`closesocket`).

Supported methods:

- **GET / HEAD** — serve static files from `./public` (MIME types, path traversal checks, 404 for missing files)
- **POST** — demo endpoint at `/api/echo` that returns the request body as JSON

The server is intentionally minimal — no keep-alive, HTTPS, or thread pool — so the Winsock and multithreading flow stay easy to follow. Ctrl+C triggers a clean shutdown by closing the listen socket, joining worker threads, and calling `WSACleanup`. Inline comments call out where Windows differs from Linux (`SOCKET` vs `int`, `closesocket` vs `close`, `WSAGetLastError` vs `errno`, and so on).

**Short blurb (for GitHub / portfolio):**  
Windows C++ multithreaded HTTP server built with Winsock2 — accept loop on the main thread, one worker thread per connection, static file serving plus a simple POST echo API.

## Requirements

- Windows
- MSVC (`cl.exe`) — Visual Studio or Build Tools with the C++ workload

## Build

From **PowerShell or cmd** in this folder (no Developer Prompt needed):

```powershell
.\build.bat
```

Or open an **x64 Native Tools / Developer Command Prompt for VS** and run:

```bat
cl /EHsc /std:c++17 main.cpp /Fe:http_server.exe
```

`cl` is not on PATH in a normal PowerShell window — that is why a bare `cl ...` fails there.

## Run

In **PowerShell**, you must use the `.\` prefix (PowerShell does not run programs from the current folder by name alone):

```powershell
.\http_server.exe
```

In **cmd**:

```bat
http_server.exe
```

The server listens on [http://127.0.0.1:8080/](http://127.0.0.1:8080/) and serves files from `./public`. Press **Ctrl+C** to stop.

## Test

Use `curl.exe` in PowerShell (plain `curl` is an alias for `Invoke-WebRequest`):

```powershell
# GET static files
curl.exe http://127.0.0.1:8080/
curl.exe http://127.0.0.1:8080/about.txt
curl.exe -I http://127.0.0.1:8080/index.html

# POST echo API
curl.exe -X POST http://127.0.0.1:8080/api/echo -d "hello"
```

Expected POST response:

```json
{"path":"/api/echo","bytes":5,"body":"hello"}
```

You can also open [http://127.0.0.1:8080/](http://127.0.0.1:8080/) in a browser and use the POST demo form on the index page.

## Behavior

| Piece | Detail |
| --- | --- |
| API | Winsock2 (`winsock2.h` / `ws2tcpip.h`), linked with `ws2_32.lib` |
| Concurrency | One `std::thread` per connection (SOCKET passed by value) |
| Static files | `./public`; `/` → `index.html`; MIME lookup by extension |
| Path safety | Rejects `..` segments and drive-like paths (e.g. `/C:/...`) |
| POST | `/api/echo` returns body as JSON; other POST paths → 404 |
| Request read | Headers + body via `Content-Length` (max 1 MiB body) |
| Status codes | 200, 400, 404, 405 |
| Shutdown | Ctrl+C closes the listen socket, joins workers, then `WSACleanup` |

## Known limitations

- No keep-alive (one request per connection)
- No chunked transfer encoding (clients must send `Content-Length`)
- POST only implements `/api/echo`
- Single-file design; no routing framework or thread pool

## Layout

```
main.cpp          Full server implementation
public/           Static files (index.html, style.css, about.txt)
build.bat         MSVC build helper
http_server.exe   Built binary (after compile)
```
