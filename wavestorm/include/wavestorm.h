/* Wavestorm SDK: C API. Games call into the Wavestorm launcher, which must be running and must have started the game.
 *
 *   if (ws_init() == WS_OK) { ... }               once at startup
 *   uint32_t id = ws_call("friends.list", NULL);   any time; the answer arrives through ws_poll
 *   ws_message m; while (ws_poll(&m)) { ... }     once per frame, on your game thread
 *   ws_shutdown();                                 at exit
 *
 * Results and events are JSON strings (UTF-8). Nothing calls back into your code from another thread.
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

enum { WS_MSG_RESULT = 1, WS_MSG_EVENT = 2 };

typedef struct ws_message {
    int kind;            /* WS_MSG_RESULT or WS_MSG_EVENT */
    uint32_t id;         /* result: the id ws_call returned */
    int ok;              /* result: 1 = success, 0 = json is {"error": "..."} */
    const char *name;    /* event name ("message", "presence", "join", "disconnected", ...); "" for results */
    const char *json;    /* payload; valid until the next ws_poll call */
} ws_message;

WS_API int ws_init(void);                       /* connect and authenticate; blocks up to 5 s */
WS_API void ws_shutdown(void);
WS_API int ws_connected(void);
WS_API const char *ws_hello_json(void);         /* {"user": {...}, "gameId": N, "joinInfo": "..."} after ws_init, else "{}" */
WS_API uint32_t ws_call(const char *method, const char *params_json); /* 0 if not connected; params may be NULL */
WS_API int ws_poll(ws_message *out);            /* 1 if a message was written to *out, 0 if none is waiting */
WS_API int ws_open_launcher(const char *game_slug); /* ask the launcher to (re)start this game; returns 1 if it could be asked */
WS_API const char *ws_version(void);

#ifdef __cplusplus
}
#endif
#endif
