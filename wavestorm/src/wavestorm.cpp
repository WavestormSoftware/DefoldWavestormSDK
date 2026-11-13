// Wavestorm SDK core: talks to the launcher's runtime over a local socket (named pipe on Windows).
// Wire format: see apps/launcher/src/Runtime.h. One reader thread queues results/events; ws_poll hands them out.
// Built with -DWS_NO_LAUNCHER (the Defold extension does this for web) the whole file is left out: those platforms have
// no processes or local sockets, so there is no launcher to talk to and the binding provides its own "no runtime" stub.
#ifndef WS_NO_LAUNCHER
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
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <fcntl.h>
#  include <spawn.h>
#  include <sys/mman.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <sys/un.h>
#  include <unistd.h>
extern char **environ;
#endif

#define WS_SDK_VERSION "1.2.5"
static const int kProtocol = 1;
static const uint32_t kMaxFrame = 64u << 20;
static const size_t kMaxQueuedUnreliable = 4096; // a game that stops polling (loading screen) must not pile up position updates
static const size_t kOverlayHeader = 64;         // overlay file: "WSOV", u32 version, u32 width, u32 height, u32 seq (odd while being written), pixels at 64

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

// A read-only view of the launcher's overlay frame file.
struct Mapping {
    const uint8_t *p = nullptr; size_t n = 0;
    bool open(const std::string &path) {
        close();
#ifdef _WIN32
        int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring w(size_t(wn), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], wn);
        HANDLE f = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{}; GetFileSizeEx(f, &size);
        HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
        CloseHandle(f);
        if (!m) return false;
        p = static_cast<const uint8_t *>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0));
        CloseHandle(m);
        n = p ? size_t(size.QuadPart) : 0;
#else
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st{};
        void *m = ::fstat(fd, &st) == 0 && st.st_size > 0 ? ::mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
        ::close(fd);
        if (m != MAP_FAILED) { p = static_cast<const uint8_t *>(m); n = size_t(st.st_size); }
#endif
        return p != nullptr;
    }
    void close() {
        if (!p) return;
#ifdef _WIN32
        UnmapViewOfFile(p);
#else
        ::munmap(const_cast<uint8_t *>(p), n);
#endif
        p = nullptr; n = 0;
    }
};

struct State {
    Conn conn;
    Mapping overlay;                  // game thread only, like the three fields below
    std::vector<uint8_t> frame, scratch;
    uint32_t frameSeq = 0, frameW = 0, frameH = 0;
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
        if (m.kind == WS_MSG_PACKET && !m.ok && s->queue.size() >= kMaxQueuedUnreliable) continue; // unreliable may be dropped
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
    s->overlay.close();
    delete s;
}

// Waits for the answer to one call and takes it out of the queue; everything else stays for ws_poll.
bool takeResult(State *s, uint32_t id, int ms, Msg &answer) {
    std::unique_lock<std::mutex> l(s->m);
    bool gone = false; // neither outstanding nor queued: it was polled already, or never sent
    return s->cv.wait_for(l, std::chrono::milliseconds(ms), [&] {
        for (auto it = s->queue.begin(); it != s->queue.end(); ++it)
            if (it->kind == WS_MSG_RESULT && it->id == id) { answer = std::move(*it); s->queue.erase(it); return true; }
        return gone = !s->pending.count(id);
    }) && !gone;
}

// The value of a string member in the launcher's (compact) JSON. Handles \\ \" \/ only: that is all a file path gets.
std::string jsonString(const std::string &json, const char *key) {
    const std::string k = std::string("\"") + key + "\":\"";
    size_t i = json.find(k);
    std::string out;
    if (i == std::string::npos) return out;
    for (i += k.size(); i < json.size() && json[i] != '"'; ++i) { if (json[i] == '\\' && i + 1 < json.size()) ++i; out += json[i]; }
    return out;
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
    Msg answer;
    if (!takeResult(s, id, 5000, answer)) { stop(s); return WS_ERR_TIMEOUT; }
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
const char *ws_strerror(int code) {
    switch (code) {
    case WS_OK: return "ok";
    case WS_ERR_NO_RUNTIME: return "The game was not started from the Wavestorm launcher.";
    case WS_ERR_CONNECT: return "The Wavestorm launcher is not running.";
    case WS_ERR_AUTH: return "The Wavestorm launcher refused the connection. Start the game from the launcher again.";
    case WS_ERR_TIMEOUT: return "The Wavestorm launcher did not answer.";
    case WS_ERR_PROTOCOL: return "The Wavestorm launcher and this game's SDK are incompatible. Update the launcher.";
    default: return "unknown error";
    }
}

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

int ws_wait(uint32_t id, int timeout_ms, ws_message *out) {
    State *s = g;
    Msg m;
    if (!s || !out || !id || !takeResult(s, id, timeout_ms < 0 ? 0 : timeout_ms, m)) return 0;
    std::lock_guard<std::mutex> l(s->m);
    s->current = std::move(m);
    out->kind = s->current.kind; out->id = s->current.id; out->ok = s->current.ok;
    out->name = s->current.name.c_str(); out->json = s->current.json.c_str(); out->len = uint32_t(s->current.json.size());
    return 1;
}

int ws_overlay_attach(uint32_t width, uint32_t height) {
    State *s = g;
    if (!s || !width || !height) return 0;
    const std::string p = "{\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) + "}";
    const uint32_t id = ws_call("overlay.attach", p.c_str());
    Msg a;
    if (!id || !takeResult(s, id, 3000, a) || !a.ok) return 0;
    s->frameSeq = 0; s->frame.clear();
    if (!s->overlay.open(jsonString(a.json, "path"))) return 0;
    const uint8_t *h = s->overlay.p;
    if (s->overlay.n >= kOverlayHeader && !std::memcmp(h, "WSOV", 4)) {
        s->frameW = get32(reinterpret_cast<const char *>(h + 8)); s->frameH = get32(reinterpret_cast<const char *>(h + 12));
        if (s->frameW && s->frameH && uint64_t(s->frameW) * s->frameH * 4 <= s->overlay.n - kOverlayHeader) return 1;
    }
    s->overlay.close();
    return 0;
}

void ws_overlay_detach(void) {
    State *s = g;
    if (!s) return;
    s->overlay.close(); s->frame.clear(); s->frameSeq = 0;
    ws_call("overlay.detach", nullptr);
}

int ws_overlay_frame(ws_frame *out) {
    State *s = g;
    if (!out) return 0;
    *out = ws_frame{};
    if (!s || !s->overlay.p) return 0;
    // The launcher makes seq odd, writes the pixels, then makes it even again: a copy is good if seq didn't move meanwhile.
    const auto *seq = reinterpret_cast<const std::atomic<uint32_t> *>(s->overlay.p + 16);
    const uint32_t before = seq->load(std::memory_order_acquire);
    int fresh = 0;
    if (before != s->frameSeq && !(before & 1)) {
        const size_t n = size_t(s->frameW) * s->frameH * 4;
        s->scratch.resize(n);
        std::memcpy(s->scratch.data(), s->overlay.p + kOverlayHeader, n);
        if (seq->load(std::memory_order_acquire) == before) { s->frame.swap(s->scratch); s->frameSeq = before; fresh = 1; }
    }
    out->pixels = s->frame.empty() ? nullptr : s->frame.data();
    out->width = s->frameW; out->height = s->frameH; out->seq = s->frameSeq;
    return fresh;
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

int ws_restart_if_needed(const char *slug) {
    if (std::getenv("WAVESTORM_RUNTIME")) return 0;
    return ws_open_launcher(slug) ? 1 : -1;
}

int ws_has_capability(const char *name) {
    if (!name || !g) return 0;
    const std::string &h = g->hello;
    const size_t a = h.find("\"capabilities\":[");
    if (a == std::string::npos) return 0;
    const size_t b = h.find(']', a);
    const size_t at = h.find(std::string("\"") + name + "\"", a);
    return at != std::string::npos && at < b;
}

} // extern "C"

#endif // WS_NO_LAUNCHER
