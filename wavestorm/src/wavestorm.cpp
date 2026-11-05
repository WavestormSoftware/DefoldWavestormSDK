// Wavestorm SDK core: talks to the launcher's runtime over a local socket (named pipe on Windows).
// Wire format: see apps/launcher/src/Runtime.h. One reader thread queues results/events; ws_poll hands them out.
#include "wavestorm.h"
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <spawn.h>
#  include <sys/socket.h>
#  include <sys/wait.h>
#  include <sys/un.h>
#  include <unistd.h>
extern char **environ;
#endif

#define WS_SDK_VERSION "1.0.0"
static const int kProtocol = 1;
static const uint32_t kMaxFrame = 64u << 20;

namespace {

struct Msg { int kind = 0; uint32_t id = 0; int ok = 0; std::string name, json; };

// ---- transport: a byte stream to the launcher
struct Conn {
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
    bool open(const char *name) {
        int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
        std::wstring w(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name, -1, &w[0], n);
        for (int attempt = 0; attempt < 2; ++attempt) {
            h = CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h != INVALID_HANDLE_VALUE) return true;
            if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(w.c_str(), 2000)) return false;
        }
        return false;
    }
    // Overlapped I/O: with a synchronous pipe handle a pending read would block every write.
    bool io(bool write, void *buf, uint32_t len) {
        char *p = static_cast<char *>(buf);
        while (len > 0) {
            OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            DWORD done = 0;
            BOOL ok = write ? WriteFile(h, p, len, nullptr, &ov) : ReadFile(h, p, len, nullptr, &ov);
            if (!ok && GetLastError() != ERROR_IO_PENDING) { CloseHandle(ov.hEvent); return false; }
            ok = GetOverlappedResult(h, &ov, &done, TRUE);
            CloseHandle(ov.hEvent);
            if (!ok || done == 0) return false;
            p += done; len -= done;
        }
        return true;
    }
    bool read(void *b, uint32_t n) { return io(false, b, n); }
    bool write(const void *b, uint32_t n) { return io(true, const_cast<void *>(b), n); }
    void interrupt() { if (h != INVALID_HANDLE_VALUE) CancelIoEx(h, nullptr); }
    void close() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
#else
    int fd = -1;
    bool open(const char *path) {
        sockaddr_un a{}; a.sun_family = AF_UNIX;
        if (std::strlen(path) >= sizeof(a.sun_path)) return false;
        std::strcpy(a.sun_path, path);
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
#  ifdef SO_NOSIGPIPE
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); // macOS: no SIGPIPE if the launcher went away
#  endif
        if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) { ::close(fd); fd = -1; return false; }
        return true;
    }
    bool read(void *b, uint32_t n) {
        char *p = static_cast<char *>(b);
        while (n) { ssize_t r = ::recv(fd, p, n, 0); if (r <= 0) return false; p += r; n -= uint32_t(r); }
        return true;
    }
    bool write(const void *b, uint32_t n) {
#  ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#  else
        const int flags = 0;
#  endif
        const char *p = static_cast<const char *>(b);
        while (n) { ssize_t r = ::send(fd, p, n, flags); if (r <= 0) return false; p += r; n -= uint32_t(r); }
        return true;
    }
    void interrupt() { if (fd >= 0) ::shutdown(fd, SHUT_RDWR); }
    void close() { if (fd >= 0) ::close(fd); fd = -1; }
#endif
};

struct State {
    Conn conn;
    std::mutex m, wm;                 // m: queue/pending, wm: one writer at a time
    std::condition_variable cv;
    std::deque<Msg> queue;
    std::set<uint32_t> pending;       // ids still waiting for an answer: failed locally if the launcher goes away
    std::atomic<bool> connected{false};
    std::atomic<uint32_t> nextId{1};
    std::thread reader;
    std::string hello = "{}";
    Msg current;                      // the message ws_poll last returned (keeps its strings alive)
};
State *g = nullptr;
std::mutex g_life; // init/shutdown

void put32(std::string &s, uint32_t v) { for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xff); }
void put16(std::string &s, uint16_t v) { s += char(v & 0xff); s += char(v >> 8); }
uint32_t get32(const char *p) { const auto *u = reinterpret_cast<const unsigned char *>(p); return u[0] | (u[1] << 8) | (u[2] << 16) | (uint32_t(u[3]) << 24); }
uint16_t get16(const char *p) { const auto *u = reinterpret_cast<const unsigned char *>(p); return uint16_t(u[0] | (u[1] << 8)); }

bool sendRequest(State *s, uint32_t id, const char *method, const char *params) {
    const size_t ml = std::strlen(method), pl = params ? std::strlen(params) : 0;
    if (ml > 0xffff || 4 + 1 + 4 + 2 + ml + pl > kMaxFrame) return false;
    std::string f;
    put32(f, uint32_t(1 + 4 + 2 + ml + pl));
    f += char(1);
    put32(f, id); put16(f, uint16_t(ml)); f.append(method, ml);
    if (pl) f.append(params, pl);
    std::lock_guard<std::mutex> l(s->wm);
    return s->conn.write(f.data(), uint32_t(f.size()));
}

void readLoop(State *s) {
    std::string body;
    for (;;) {
        char hdr[4];
        if (!s->conn.read(hdr, 4)) break;
        const uint32_t len = get32(hdr);
        if (len < 1 || len > kMaxFrame) break;
        body.resize(len);
        if (!s->conn.read(&body[0], len)) break;
        Msg m;
        const char type = body[0];
        if (type == 2 && len >= 6) {
            m.kind = WS_MSG_RESULT; m.id = get32(&body[1]); m.ok = body[5] != 0; m.json.assign(body, 6, std::string::npos);
        } else if (type == 3 && len >= 3) {
            const uint16_t nl = get16(&body[1]);
            if (3u + nl > len) break;
            m.kind = WS_MSG_EVENT; m.name.assign(body, 3, nl); m.json.assign(body, 3 + nl, std::string::npos);
        } else if (type == 4 && len >= 6) {
            m.kind = WS_MSG_PACKET; m.id = get32(&body[1]); m.ok = body[5] & 1; m.json.assign(body, 6, std::string::npos);
        } else continue; // unknown frame type from a newer launcher: skip it
        std::lock_guard<std::mutex> l(s->m);
        if (m.kind == WS_MSG_RESULT) s->pending.erase(m.id);
        s->queue.push_back(std::move(m));
        s->cv.notify_all();
    }
    // Launcher closed (or quit): fail what's outstanding and tell the game once.
    s->connected = false;
    std::lock_guard<std::mutex> l(s->m);
    for (uint32_t id : s->pending) { Msg e; e.kind = WS_MSG_RESULT; e.id = id; e.json = "{\"error\":\"disconnected\"}"; s->queue.push_back(e); }
    s->pending.clear();
    Msg d; d.kind = WS_MSG_EVENT; d.name = "disconnected"; d.json = "{}";
    s->queue.push_back(d);
    s->cv.notify_all();
}

void stop(State *s) {
    s->conn.interrupt();
    if (s->reader.joinable()) s->reader.join();
    s->conn.close();
    delete s;
}

} // namespace

extern "C" {

int ws_init(void) {
    std::lock_guard<std::mutex> life(g_life);
    if (g && g->connected) return WS_OK;
    if (g) { stop(g); g = nullptr; }
    const char *path = std::getenv("WAVESTORM_RUNTIME"), *token = std::getenv("WAVESTORM_RUNTIME_TOKEN");
    if (!path || !*path || !token || !*token) return WS_ERR_NO_RUNTIME;
    for (const char *c = token; *c; ++c) if (!std::isxdigit(static_cast<unsigned char>(*c))) return WS_ERR_AUTH;

    auto *s = new State;
    if (!s->conn.open(path)) { delete s; return WS_ERR_CONNECT; }
    s->connected = true;
    s->reader = std::thread(readLoop, s);
    const uint32_t id = s->nextId++;
    { std::lock_guard<std::mutex> l(s->m); s->pending.insert(id); }
    const std::string params = std::string("{\"token\":\"") + token + "\",\"sdk\":\"" WS_SDK_VERSION "\"}";
    if (!sendRequest(s, id, "hello", params.c_str())) { stop(s); return WS_ERR_CONNECT; }

    // Wait for the hello answer only; anything else that arrives stays queued for ws_poll.
    std::unique_lock<std::mutex> l(s->m);
    Msg answer;
    const bool got = s->cv.wait_for(l, std::chrono::seconds(5), [&] {
        for (auto it = s->queue.begin(); it != s->queue.end(); ++it)
            if (it->kind == WS_MSG_RESULT && it->id == id) { answer = *it; s->queue.erase(it); return true; }
        return false;
    });
    l.unlock();
    if (!got) { stop(s); return WS_ERR_TIMEOUT; }
    if (!answer.ok) { stop(s); return answer.json.find("disconnected") != std::string::npos ? WS_ERR_CONNECT : WS_ERR_AUTH; }
    const size_t p = answer.json.find("\"protocol\":");
    if (p == std::string::npos || std::atoi(answer.json.c_str() + p + 11) != kProtocol) { stop(s); return WS_ERR_PROTOCOL; }
    s->hello = answer.json;
    g = s;
    return WS_OK;
}

void ws_shutdown(void) {
    std::lock_guard<std::mutex> life(g_life);
    if (g) { stop(g); g = nullptr; }
}

int ws_connected(void) { return g && g->connected ? 1 : 0; }
const char *ws_hello_json(void) { return g ? g->hello.c_str() : "{}"; }
const char *ws_version(void) { return WS_SDK_VERSION; }

uint32_t ws_call(const char *method, const char *params_json) {
    State *s = g;
    if (!s || !s->connected || !method || !*method) return 0;
    uint32_t id = s->nextId++;
    if (id == 0) id = s->nextId++; // 0 means "not sent"
    { std::lock_guard<std::mutex> l(s->m); s->pending.insert(id); }
    if (!sendRequest(s, id, method, params_json)) { std::lock_guard<std::mutex> l(s->m); s->pending.erase(id); return 0; }
    return id;
}

int ws_poll(ws_message *out) {
    State *s = g;
    if (!s || !out) return 0;
    std::lock_guard<std::mutex> l(s->m);
    if (s->queue.empty()) return 0;
    s->current = std::move(s->queue.front());
    s->queue.pop_front();
    out->kind = s->current.kind; out->id = s->current.id; out->ok = s->current.ok;
    out->name = s->current.name.c_str(); out->json = s->current.json.c_str(); out->len = uint32_t(s->current.json.size());
    return 1;
}

int ws_send_packet(uint32_t peer, int reliable, const void *data, uint32_t len) {
    State *s = g;
    if (!s || !s->connected || !data || len == 0 || len > 16000) return 0;
    std::string f;
    put32(f, 1 + 4 + 1 + len);
    f += char(4); put32(f, peer); f += char(reliable ? 1 : 0);
    f.append(static_cast<const char *>(data), len);
    std::lock_guard<std::mutex> l(s->wm);
    return s->conn.write(f.data(), uint32_t(f.size())) ? 1 : 0;
}

int ws_open_launcher(const char *slug) {
    if (!slug || !*slug) return 0;
    for (const char *c = slug; *c; ++c) if (!std::isalnum(static_cast<unsigned char>(*c)) && *c != '-' && *c != '_') return 0;
    const std::string url = std::string("wavestorm://run/") + slug;
#ifdef _WIN32
    std::wstring w(url.begin(), url.end()); // ASCII only (checked above)
    return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
#  ifdef __APPLE__
    const char *opener = "open";
#  else
    const char *opener = "xdg-open";
#  endif
    char *argv[] = {const_cast<char *>(opener), const_cast<char *>(url.c_str()), nullptr};
    pid_t pid;
    if (posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) != 0) return 0;
    std::thread([pid] { waitpid(pid, nullptr, 0); }).detach(); // reap it; no zombie left in the game's process table
    return 1;
#endif
}

} // extern "C"
