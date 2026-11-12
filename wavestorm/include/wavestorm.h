/* Wavestorm SDK: C API. Games call into the Wavestorm launcher, which must be running and must have started the game.
 *
 *   if (ws_init() == WS_OK) { ... }               once at startup
 *   uint32_t id = ws_call("friends.list", NULL);   any time; the answer arrives through ws_poll
 *   ws_message m; while (ws_poll(&m)) { ... }     once per frame, on your game thread
 *   ws_shutdown();                                 at exit
 *
 * Results and events are JSON strings (UTF-8). Nothing calls back into your code from another thread.
 * ws_call and ws_send_packet may be used from any thread; everything else belongs to one thread (your game thread).
 * See docs/sdk.md for every method and event.
 */
#ifndef WAVESTORM_H
#define WAVESTORM_H
#include <stdint.h>

#if defined(_WIN32) && defined(WS_BUILD_SHARED)
#  define WS_API __declspec(dllexport)
#elif defined(_WIN32) && defined(WS_USE_SHARED)
#  define WS_API __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#  define WS_API __attribute__((visibility("default")))
#else
#  define WS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WS_OK = 0,
    WS_ERR_NO_RUNTIME = 1,   /* not started by the launcher (no WAVESTORM_RUNTIME): call ws_open_launcher() */
    WS_ERR_CONNECT = 2,      /* the launcher isn't reachable (it was closed) */
    WS_ERR_AUTH = 3,         /* the launcher rejected us (stale token: restart the game from the launcher) */
    WS_ERR_TIMEOUT = 4,
    WS_ERR_PROTOCOL = 5      /* launcher too old/new for this SDK */
};

enum { WS_MSG_RESULT = 1, WS_MSG_EVENT = 2, WS_MSG_PACKET = 3 };

typedef struct ws_message {
    int kind;            /* WS_MSG_RESULT, WS_MSG_EVENT or WS_MSG_PACKET */
    uint32_t id;         /* result: the id ws_call returned */
    int ok;              /* result: 1 = success, 0 = json is {"error": "..."} */
    const char *name;    /* event name ("message", "presence", "join", "disconnected", ...); "" for results */
    const char *json;    /* payload (JSON text, or the raw bytes of a packet); valid until the next ws_poll call */
    uint32_t len;        /* payload length in bytes. WS_MSG_PACKET: id = the sending player's user id, ok = 1 if it was sent reliably */
} ws_message;

WS_API int ws_init(void);                       /* connect and authenticate; blocks up to 5 s */
WS_API void ws_shutdown(void);
WS_API int ws_connected(void);
WS_API const char *ws_hello_json(void);         /* {"user": {...}, "gameId": N, "joinInfo": "..."} after ws_init, else "{}" */
WS_API uint32_t ws_call(const char *method, const char *params_json); /* 0 if not connected; params may be NULL */
WS_API int ws_poll(ws_message *out);            /* 1 if a message was written to *out, 0 if none is waiting */
WS_API int ws_send_packet(uint32_t peer, int reliable, const void *data, uint32_t len); /* game data to a player you did net.connect to; 1 if queued */
WS_API int ws_open_launcher(const char *game_slug); /* ask the launcher to (re)start this game; returns 1 if it could be asked */
/* Steam-style "launcher required": call first thing at startup. Returns 0 when this process was started by the launcher (carry on),
 * 1 when it was not and the launcher has been asked to start the game properly (exit now), -1 if the launcher could not be asked
 * (show ws_strerror(WS_ERR_NO_RUNTIME)). */
WS_API int ws_restart_if_needed(const char *game_slug);
WS_API int ws_has_capability(const char *name); /* 1 if the connected launcher lists it (hello "capabilities"); 0 for older launchers */
WS_API const char *ws_version(void);
WS_API const char *ws_strerror(int code);       /* a sentence for a ws_init result, to show the player */
/* Block until the answer to ws_call id arrives (at most timeout_ms) and return it like ws_poll would; other messages stay
 * queued. 0 on timeout or if that id isn't outstanding. For startup code; in the game loop use ws_poll. */
WS_API int ws_wait(uint32_t id, int timeout_ms, ws_message *out);

/* The overlay drawn by the game itself (works in exclusive fullscreen and on every platform): the launcher renders the
 * overlay into shared memory; draw it as one alpha-blended quad over your frame.
 *   ws_overlay_attach(width, height);             once, and again when your window size changes; blocks up to 3 s
 *   ws_frame f; if (ws_overlay_frame(&f)) upload f.pixels to your texture;   every frame; then draw the texture
 * While the overlay is open (event "overlay" {"open":true}) send the player's input with ws_call("overlay.input", ...),
 * see docs/sdk.md. */
typedef struct ws_frame {
    const uint8_t *pixels;   /* width*height*4 bytes, RGBA with straight (not premultiplied) alpha, top row first; NULL before the first frame */
    uint32_t width, height;
    uint32_t seq;            /* changes with every new frame */
} ws_frame;
WS_API int ws_overlay_attach(uint32_t width, uint32_t height); /* 1 if the launcher now renders the overlay for us */
WS_API void ws_overlay_detach(void);            /* back to the launcher's own overlay window */
WS_API int ws_overlay_frame(ws_frame *out);     /* fills *out with the latest frame; 1 if it changed since the last call */

#ifdef __cplusplus
}
#endif
#endif
