// Wavestorm SDK for Lua: shared by the plain Lua module (lua_module.cpp) and the Defold extension.
// The including file defines how callbacks are stored and called:
//   struct WsCallback;  WsCallback *ws_cb_create(lua_State *L, int index);  void ws_cb_destroy(WsCallback *);
//   bool ws_cb_begin(WsCallback *, lua_State **L);  // pushes the function (Defold: and self); false if gone
//   void ws_cb_call(lua_State *L, int nargs);       // calls what ws_cb_begin pushed with nargs more values
//
// Lua API (module table `wavestorm`):
//   ok, err = wavestorm.init()          err: "no_runtime" | "connect" | "auth" | "timeout" | "protocol"
//   wavestorm.run_callbacks()           every frame (the Defold extension does this for you)
//   wavestorm.call(method, params, cb)  cb(ok, result)   raw call, see docs/sdk.md
//   wavestorm.on(event, cb)             cb(data); event "*" gets cb(name, data); nil cb removes
//   wavestorm.user(), wavestorm.game_id(), wavestorm.join_info(), wavestorm.connected()
//   wavestorm.restart_if_needed(slug) -> 0 | 1 (exit now) | -1,  wavestorm.has_capability(name)
//   wavestorm.set_presence(text, join_info), wavestorm.friends(cb), wavestorm.send_chat(user_id, text, cb),
//   wavestorm.invite(user_id, message, cb), wavestorm.open_overlay(), wavestorm.open_launcher(slug), wavestorm.shutdown()
//   wavestorm.workshop_subscribed(cb), workshop_query({q=, tag=, sort=, page=}, cb), workshop_subscribe(id, cb), workshop_unsubscribe(id, cb),
//   wavestorm.workshop_set_enabled(id, on, cb), workshop_create_item({title=, description=}, cb), workshop_upload_version(id, folder, version, changelog, cb)
//   wavestorm.voice_join(room, cb), voice_leave(cb), voice_state(cb), voice_set_muted(bool, cb), voice_set_deafened(bool, cb),
//   wavestorm.voice_set_user_muted(user_id, bool, cb), voice_set_user_volume(user_id, 0..2, cb)
//   wavestorm.net_listen(cb), net_connect(user_id, cb), net_disconnect(user_id, cb), net_send(user_id, bytes, reliable) -> queued?
//   events: wavestorm.on("net.packet", function(user_id, reliable, bytes) end), "net.connected"/"net.disconnected" (d.userId),
//           "voice.joined"/"voice.left" (d.userId), "voice.speaking" (d.userId, d.speaking)
//   events: wavestorm.on("workshop.installed", function(d) -- d.itemId, d.path end), wavestorm.on("workshop.removed", ...)
//   wavestorm.unlock_achievement(api_name, cb), wavestorm.achievements(cb), wavestorm.stats(cb), wavestorm.set_stats({name = value}, cb)
//   wavestorm.leaderboard(stat_api_name, cb)  r.entries[i] = {rank, globalRank, userId, username, value}, r.players
//   wavestorm.game_info(cb)  {gameId, title, slug, version, installPath};  wavestorm.chat_history(user_id, cb)
//   wavestorm.groups(cb), wavestorm.send_group(group_id, text, cb), wavestorm.close_overlay(), wavestorm.net_peers(cb)
//   wavestorm.voice_set_push_to_talk(bool, cb), wavestorm.voice_set_talking(bool, cb)
//   The overlay drawn by the game (see docs/sdk.md): wavestorm.overlay_attach(width, height) -> ok, wavestorm.overlay_detach(),
//   pixels, width, height = wavestorm.overlay_frame()   pixels (RGBA string, straight alpha, top row first) is nil unless the picture changed,
//   wavestorm.overlay_input({type = "move"|"down"|"up"|"wheel"|"key"|"text", x =, y =, button =, dy =, key =, down =, text =})
#pragma once
#include "wavestorm.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#ifndef WS_LUA_INCLUDED // the Defold extension gets Lua from dmsdk/sdk.h instead
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}
#endif

#if LUA_VERSION_NUM < 502
#  define ws_rawlen lua_objlen
#else
#  define ws_rawlen lua_rawlen
#endif

namespace wsl {

static std::map<uint32_t, WsCallback *> g_pending;
static std::map<std::string, WsCallback *> g_handlers;
static std::string g_join;

// ---- JSON <-> Lua
static void encode(lua_State *L, int idx, std::string &out, int depth = 0);
static void encodeString(const char *s, size_t n, std::string &out) {
    out += '"';
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += char(c);
    }
    out += '"';
}
static void encode(lua_State *L, int idx, std::string &out, int depth) {
    if (idx < 0) idx = lua_gettop(L) + idx + 1;
    if (depth > 32) luaL_error(L, "wavestorm: table nested too deeply");
    switch (lua_type(L, idx)) {
    case LUA_TNIL: out += "null"; return;
    case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; return;
    case LUA_TNUMBER: {
        const double d = lua_tonumber(L, idx);
        if (!std::isfinite(d)) { out += "null"; return; }
        char b[32];
        if (d == std::floor(d) && std::fabs(d) < 1e15) std::snprintf(b, sizeof b, "%.0f", d); else std::snprintf(b, sizeof b, "%.17g", d);
        out += b; return;
    }
    case LUA_TSTRING: { size_t n; const char *s = lua_tolstring(L, idx, &n); encodeString(s, n, out); return; }
    case LUA_TTABLE: {
        const size_t n = ws_rawlen(L, idx);
        if (n > 0) { // sequence -> array
            out += '[';
            for (size_t i = 1; i <= n; ++i) { if (i > 1) out += ','; lua_rawgeti(L, idx, int(i)); encode(L, -1, out, depth + 1); lua_pop(L, 1); }
            out += ']'; return;
        }
        out += '{'; bool first = true;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (lua_type(L, -2) == LUA_TSTRING || lua_type(L, -2) == LUA_TNUMBER) {
                if (!first) { out += ','; } first = false;
                lua_pushvalue(L, -2); size_t kn; const char *k = lua_tolstring(L, -1, &kn); encodeString(k, kn, out); lua_pop(L, 1);
                out += ':'; encode(L, -1, out, depth + 1);
            }
            lua_pop(L, 1);
        }
        out += '}'; return;
    }
    default: luaL_error(L, "wavestorm: can't send a %s", luaL_typename(L, idx));
    }
}

struct Parser {
    lua_State *L; const char *p, *end;
    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool str() { // pushes a Lua string
        if (p >= end || *p != '"') return false;
        ++p; std::string s;
        while (p < end && *p != '"') {
            if (*p != '\\') { s += *p++; continue; }
            if (++p >= end) return false;
            const char e = *p++;
            if (e == 'u') {
                if (end - p < 4) return false;
                unsigned cp = unsigned(std::strtoul(std::string(p, 4).c_str(), nullptr, 16)); p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') { // surrogate pair
                    const unsigned lo = unsigned(std::strtoul(std::string(p + 2, 4).c_str(), nullptr, 16));
                    if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                }
                if (cp < 0x80) s += char(cp);
                else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
                else { s += char(0xF0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3F)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
            } else s += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
        }
        if (p >= end) return false;
        ++p;
        lua_pushlstring(L, s.data(), s.size());
        return true;
    }
    bool value(int depth = 0) { // pushes one value
        ws();
        if (p >= end || depth > 64) return false;
        lua_checkstack(L, 4);
        if (*p == '{') {
            ++p; lua_newtable(L); ws();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                ws(); if (!str()) return false; ws();
                if (p >= end || *p++ != ':') return false;
                if (!value(depth + 1)) return false;
                lua_rawset(L, -3); ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return false;
            }
        }
        if (*p == '[') {
            ++p; lua_newtable(L); ws(); int i = 1;
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                if (!value(depth + 1)) return false;
                lua_rawseti(L, -2, i++); ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return false;
            }
        }
        if (*p == '"') return str();
        if (end - p >= 4 && !std::strncmp(p, "true", 4)) { p += 4; lua_pushboolean(L, 1); return true; }
        if (end - p >= 5 && !std::strncmp(p, "false", 5)) { p += 5; lua_pushboolean(L, 0); return true; }
        if (end - p >= 4 && !std::strncmp(p, "null", 4)) { p += 4; lua_pushnil(L); return true; }
        char *e = nullptr;
        const double d = std::strtod(p, &e);
        if (e == p) return false;
        p = e; lua_pushnumber(L, d);
        return true;
    }
};
// Pushes the decoded value (nil on bad JSON).
static void push_json(lua_State *L, const char *json) {
    const int top = lua_gettop(L);
    Parser ps{L, json, json + std::strlen(json)};
    if (!*json || !ps.value()) { lua_settop(L, top); lua_pushnil(L); }
}

// ---- calls
static int call_with(lua_State *L, const char *method, const std::string &params, int cbIndex) {
    const bool wantsAnswer = cbIndex && !lua_isnoneornil(L, cbIndex);
    if (wantsAnswer) luaL_checktype(L, cbIndex, LUA_TFUNCTION); // before sending: an argument error must not leave a call behind
    const uint32_t id = ws_call(method, params.empty() ? nullptr : params.c_str());
    if (wantsAnswer) {
        WsCallback *cb = ws_cb_create(L, cbIndex);
        if (id) g_pending[id] = cb;
        else { // not connected: answer right away (from run_callbacks would be nicer, but this keeps it simple)
            lua_State *CL;
            if (ws_cb_begin(cb, &CL)) { lua_pushboolean(CL, 0); lua_newtable(CL); lua_pushstring(CL, "not connected"); lua_setfield(CL, -2, "error"); ws_cb_call(CL, 2); }
            ws_cb_destroy(cb);
        }
    }
    lua_pushboolean(L, id != 0);
    return 1;
}
static std::string table_json(lua_State *L, int idx) {
    std::string s;
    if (!lua_isnoneornil(L, idx)) { luaL_checktype(L, idx, LUA_TTABLE); encode(L, idx, s); }
    return s;
}

static int l_init(lua_State *L) {
    static const char *codes[] = {"", "no_runtime", "connect", "auth", "timeout", "protocol"};
    const int r = ws_init();
    if (r == WS_OK) {
        // join info from the hello, kept so join_info() works without parsing every time
        push_json(L, ws_hello_json());
        if (lua_istable(L, -1)) { lua_getfield(L, -1, "joinInfo"); g_join = lua_isstring(L, -1) ? lua_tostring(L, -1) : ""; lua_pop(L, 1); }
        lua_pop(L, 1);
        lua_pushboolean(L, 1); return 1;
    }
    lua_pushboolean(L, 0); lua_pushstring(L, r > 0 && r <= 5 ? codes[r] : "unknown"); return 2;
}
static int l_shutdown(lua_State *) {
    ws_shutdown();
    for (auto &kv : g_pending) ws_cb_destroy(kv.second);
    g_pending.clear();
    return 0;
}
static int l_connected(lua_State *L) { lua_pushboolean(L, ws_connected()); return 1; }
static int l_hello_field(lua_State *L, const char *field) {
    push_json(L, ws_hello_json());
    if (!lua_istable(L, -1)) return 1;
    lua_getfield(L, -1, field);
    return 1;
}
static int l_user(lua_State *L) { return l_hello_field(L, "user"); }
static int l_game_id(lua_State *L) { return l_hello_field(L, "gameId"); }
static int l_join_info(lua_State *L) { lua_pushstring(L, g_join.c_str()); return 1; }
static int l_call(lua_State *L) { const char *m = luaL_checkstring(L, 1); return call_with(L, m, table_json(L, 2), 3); }

static int l_on(lua_State *L) {
    const std::string name = luaL_checkstring(L, 1);
    auto it = g_handlers.find(name);
    if (it != g_handlers.end()) { ws_cb_destroy(it->second); g_handlers.erase(it); }
    if (!lua_isnoneornil(L, 2)) { luaL_checktype(L, 2, LUA_TFUNCTION); g_handlers[name] = ws_cb_create(L, 2); }
    return 0;
}

static int l_set_presence(lua_State *L) {
    std::string p = "{";
    if (!lua_isnoneornil(L, 1)) { p += "\"text\":"; size_t n; const char *s = luaL_checklstring(L, 1, &n); encodeString(s, n, p); }
    if (!lua_isnoneornil(L, 2)) { if (p.size() > 1) p += ','; p += "\"joinInfo\":"; size_t n; const char *s = luaL_checklstring(L, 2, &n); encodeString(s, n, p); }
    p += '}';
    return call_with(L, "presence.set", p, 0);
}
static int l_friends(lua_State *L) { return call_with(L, "friends.list", "", 1); }
static int l_send_chat(lua_State *L) {
    std::string p = "{\"userId\":" + std::to_string(luaL_checkinteger(L, 1)) + ",\"text\":";
    size_t n; const char *s = luaL_checklstring(L, 2, &n); encodeString(s, n, p); p += '}';
    return call_with(L, "chat.send", p, 3);
}
static int l_invite(lua_State *L) {
    std::string p = "{\"userId\":" + std::to_string(luaL_checkinteger(L, 1));
    if (!lua_isnoneornil(L, 2)) { p += ",\"message\":"; size_t n; const char *s = luaL_checklstring(L, 2, &n); encodeString(s, n, p); }
    p += '}';
    return call_with(L, "invites.send", p, 3);
}
static int l_unlock_achievement(lua_State *L) {
    std::string p = "{\"apiName\":"; size_t n; const char *s = luaL_checklstring(L, 1, &n); encodeString(s, n, p); p += '}';
    return call_with(L, "achievements.unlock", p, 2);
}
// auth_assertion(audience, cb): cb(ok, {assertion=..., exp=...}), a 5 minute proof of identity for one other service
static int l_auth_assertion(lua_State *L) {
    std::string p = "{\"audience\":"; size_t n; const char *s = luaL_checklstring(L, 1, &n); encodeString(s, n, p); p += '}';
    return call_with(L, "auth.assertion", p, 2);
}
static int l_achievements(lua_State *L) { return call_with(L, "achievements.list", "", 1); }
static int l_stats(lua_State *L) { return call_with(L, "stats.get", "", 1); }
static int l_set_stats(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::string p = "{\"stats\":"; encode(L, 1, p); p += '}';
    return call_with(L, "stats.set", p, 2);
}
// Workshop: tables become the call's JSON params; cb(ok, result).
static int l_workshop_subscribed(lua_State *L) { return call_with(L, "workshop.subscribed", "", 1); }
static int l_workshop_query(lua_State *L) { return call_with(L, "workshop.query", table_json(L, 1), 2); }
static int l_workshop_subscribe(lua_State *L) { return call_with(L, "workshop.subscribe", "{\"itemId\":" + std::to_string(luaL_checkinteger(L, 1)) + "}", 2); }
static int l_workshop_unsubscribe(lua_State *L) { return call_with(L, "workshop.unsubscribe", "{\"itemId\":" + std::to_string(luaL_checkinteger(L, 1)) + "}", 2); }
static int l_workshop_set_enabled(lua_State *L) {
    return call_with(L, "workshop.setEnabled", "{\"itemId\":" + std::to_string(luaL_checkinteger(L, 1)) + ",\"enabled\":" + (lua_toboolean(L, 2) ? "true" : "false") + "}", 3);
}
static int l_workshop_create_item(lua_State *L) { return call_with(L, "workshop.createItem", table_json(L, 1), 2); }
// workshop_upload_version(item_id, folder, version, changelog, cb): zips the folder and uploads it (changelog may be nil)
static int l_workshop_upload_version(lua_State *L) {
    std::string p = "{\"itemId\":" + std::to_string(luaL_checkinteger(L, 1)) + ",\"folder\":"; size_t n; const char *s;
    s = luaL_checklstring(L, 2, &n); encodeString(s, n, p);
    p += ",\"version\":"; s = luaL_checklstring(L, 3, &n); encodeString(s, n, p);
    if (!lua_isnoneornil(L, 4)) { p += ",\"changelog\":"; s = luaL_checklstring(L, 4, &n); encodeString(s, n, p); }
    p += '}';
    return call_with(L, "workshop.uploadVersion", p, 5);
}
// Voice and P2P. Rooms: "game:<gameId>:<name>", up to 8 players. Packets are raw bytes (Lua strings), up to 16000.
static std::string bool_str(lua_State *L, int i) { return lua_toboolean(L, i) ? "true" : "false"; }
static int l_voice_join(lua_State *L) {
    std::string p = "{\"room\":"; size_t n; const char *s = luaL_checklstring(L, 1, &n); encodeString(s, n, p); p += '}';
    return call_with(L, "voice.join", p, 2);
}
static int l_voice_leave(lua_State *L) { return call_with(L, "voice.leave", "", 1); }
static int l_voice_state(lua_State *L) { return call_with(L, "voice.state", "", 1); }
static int l_voice_set_muted(lua_State *L) { return call_with(L, "voice.setMuted", "{\"muted\":" + bool_str(L, 1) + "}", 2); }
static int l_voice_set_deafened(lua_State *L) { return call_with(L, "voice.setDeafened", "{\"deafened\":" + bool_str(L, 1) + "}", 2); }
static int l_voice_set_user_muted(lua_State *L) {
    return call_with(L, "voice.setUserMuted", "{\"userId\":" + std::to_string(luaL_checkinteger(L, 1)) + ",\"muted\":" + bool_str(L, 2) + "}", 3);
}
static int l_voice_set_user_volume(lua_State *L) {
    char b[32]; std::snprintf(b, sizeof b, "%.3f", luaL_checknumber(L, 2));
    return call_with(L, "voice.setUserVolume", "{\"userId\":" + std::to_string(luaL_checkinteger(L, 1)) + ",\"volume\":" + b + "}", 3);
}
static int l_net_listen(lua_State *L) { return call_with(L, "net.listen", "", 1); }
static int l_net_connect(lua_State *L) { return call_with(L, "net.connect", "{\"userId\":" + std::to_string(luaL_checkinteger(L, 1)) + "}", 2); }
static int l_net_disconnect(lua_State *L) { return call_with(L, "net.disconnect", "{\"userId\":" + std::to_string(luaL_checkinteger(L, 1)) + "}", 2); }
static int l_net_send(lua_State *L) {
    const lua_Integer id = luaL_checkinteger(L, 1); size_t n; const char *s = luaL_checklstring(L, 2, &n);
    lua_pushboolean(L, id > 0 && n <= 16000 && ws_send_packet(uint32_t(id), lua_toboolean(L, 3), s, uint32_t(n)));
    return 1;
}
static std::string user_id_json(lua_State *L, int i) { return "{\"userId\":" + std::to_string(luaL_checkinteger(L, i)) + "}"; }
static int l_leaderboard(lua_State *L) {
    std::string p = "{\"stat\":"; size_t n; const char *s = luaL_checklstring(L, 1, &n); encodeString(s, n, p); p += '}';
    return call_with(L, "leaderboards.get", p, 2);
}
static int l_game_info(lua_State *L) { return call_with(L, "game.info", "", 1); }
static int l_chat_history(lua_State *L) { return call_with(L, "chat.history", user_id_json(L, 1), 2); }
static int l_groups(lua_State *L) { return call_with(L, "groups.list", "", 1); }
static int l_send_group(lua_State *L) {
    std::string p = "{\"groupId\":" + std::to_string(luaL_checkinteger(L, 1)) + ",\"text\":";
    size_t n; const char *s = luaL_checklstring(L, 2, &n); encodeString(s, n, p); p += '}';
    return call_with(L, "groups.send", p, 3);
}
static int l_net_peers(lua_State *L) { return call_with(L, "net.peers", "", 1); }
static int l_voice_set_push_to_talk(lua_State *L) { return call_with(L, "voice.setPushToTalk", "{\"enabled\":" + bool_str(L, 1) + "}", 2); }
static int l_voice_set_talking(lua_State *L) { return call_with(L, "voice.setTalking", "{\"talking\":" + bool_str(L, 1) + "}", 2); }
static int l_overlay_attach(lua_State *L) {
    const lua_Integer w = luaL_checkinteger(L, 1), h = luaL_checkinteger(L, 2);
    lua_pushboolean(L, w > 0 && h > 0 && ws_overlay_attach(uint32_t(w), uint32_t(h)));
    return 1;
}
static int l_overlay_detach(lua_State *) { ws_overlay_detach(); return 0; }
static int l_overlay_frame(lua_State *L) {
    ws_frame f;
    if (ws_overlay_frame(&f) && f.pixels) lua_pushlstring(L, reinterpret_cast<const char *>(f.pixels), size_t(f.width) * f.height * 4); else lua_pushnil(L);
    lua_pushinteger(L, lua_Integer(f.width)); lua_pushinteger(L, lua_Integer(f.height));
    return 3;
}
static int l_overlay_input(lua_State *L) { luaL_checktype(L, 1, LUA_TTABLE); return call_with(L, "overlay.input", table_json(L, 1), 0); }
static int l_close_overlay(lua_State *L) { return call_with(L, "overlay.close", "", 0); }
static int l_open_overlay(lua_State *L) { return call_with(L, "overlay.open", "", 0); }
// restart_if_needed(slug) -> 0 running under the launcher, 1 relaunching through it (exit now), -1 could not ask it
static int l_restart_if_needed(lua_State *L) { lua_pushinteger(L, ws_restart_if_needed(luaL_checkstring(L, 1))); return 1; }
static int l_has_capability(lua_State *L) { lua_pushboolean(L, ws_has_capability(luaL_checkstring(L, 1))); return 1; }
static int l_open_launcher(lua_State *L) { lua_pushboolean(L, ws_open_launcher(luaL_checkstring(L, 1))); return 1; }

// Delivers queued results and events. Callback errors are reported, not thrown, so one bad handler can't stall the queue.
static void dispatch_event(const char *name, const char *json) {
    for (const char *key : {name, "*"}) {
        auto it = g_handlers.find(key);
        if (it == g_handlers.end()) continue;
        lua_State *CL;
        if (!ws_cb_begin(it->second, &CL)) continue;
        int n = 0;
        if (!std::strcmp(key, "*")) { lua_pushstring(CL, name); ++n; }
        push_json(CL, json); ++n;
        ws_cb_call(CL, n);
    }
}
static int l_run_callbacks(lua_State *L) {
    ws_message m;
    while (ws_poll(&m)) {
        if (m.kind == WS_MSG_PACKET) { // raw bytes, not JSON: handler(user_id, reliable, bytes)
            auto it = g_handlers.find("net.packet");
            lua_State *CL;
            if (it != g_handlers.end() && ws_cb_begin(it->second, &CL)) {
                lua_pushinteger(CL, lua_Integer(m.id)); lua_pushboolean(CL, m.ok); lua_pushlstring(CL, m.json, m.len);
                ws_cb_call(CL, 3);
            }
            continue;
        }
        if (m.kind == WS_MSG_RESULT) {
            auto it = g_pending.find(m.id);
            if (it == g_pending.end()) continue;
            WsCallback *cb = it->second; g_pending.erase(it);
            lua_State *CL;
            if (ws_cb_begin(cb, &CL)) { lua_pushboolean(CL, m.ok); push_json(CL, m.json); ws_cb_call(CL, 2); }
            ws_cb_destroy(cb);
            continue;
        }
        if (!std::strcmp(m.name, "join")) {
            push_json(L, m.json);
            if (lua_istable(L, -1)) { lua_getfield(L, -1, "joinInfo"); if (lua_isstring(L, -1)) g_join = lua_tostring(L, -1); lua_pop(L, 1); }
            lua_pop(L, 1);
        }
        dispatch_event(m.name, m.json);
    }
    return 0;
}

static const luaL_Reg functions[] = {
    {"init", l_init}, {"shutdown", l_shutdown}, {"connected", l_connected}, {"run_callbacks", l_run_callbacks},
    {"call", l_call}, {"on", l_on}, {"user", l_user}, {"game_id", l_game_id}, {"join_info", l_join_info},
    {"set_presence", l_set_presence}, {"friends", l_friends}, {"send_chat", l_send_chat}, {"invite", l_invite},
    {"unlock_achievement", l_unlock_achievement}, {"achievements", l_achievements}, {"stats", l_stats}, {"set_stats", l_set_stats},
    {"workshop_subscribed", l_workshop_subscribed}, {"workshop_query", l_workshop_query}, {"workshop_subscribe", l_workshop_subscribe},
    {"workshop_unsubscribe", l_workshop_unsubscribe}, {"workshop_set_enabled", l_workshop_set_enabled}, {"workshop_create_item", l_workshop_create_item},
    {"workshop_upload_version", l_workshop_upload_version},
    {"voice_join", l_voice_join}, {"voice_leave", l_voice_leave}, {"voice_state", l_voice_state}, {"voice_set_muted", l_voice_set_muted},
    {"voice_set_deafened", l_voice_set_deafened}, {"voice_set_user_muted", l_voice_set_user_muted}, {"voice_set_user_volume", l_voice_set_user_volume},
    {"net_listen", l_net_listen}, {"net_connect", l_net_connect}, {"net_disconnect", l_net_disconnect}, {"net_send", l_net_send},
    {"leaderboard", l_leaderboard}, {"game_info", l_game_info}, {"chat_history", l_chat_history}, {"groups", l_groups}, {"send_group", l_send_group},
    {"net_peers", l_net_peers}, {"voice_set_push_to_talk", l_voice_set_push_to_talk}, {"voice_set_talking", l_voice_set_talking},
    {"overlay_attach", l_overlay_attach}, {"overlay_detach", l_overlay_detach}, {"overlay_frame", l_overlay_frame}, {"overlay_input", l_overlay_input},
    {"auth_assertion", l_auth_assertion}, {"close_overlay", l_close_overlay}, {"open_overlay", l_open_overlay}, {"open_launcher", l_open_launcher}, {"restart_if_needed", l_restart_if_needed}, {"has_capability", l_has_capability}, {nullptr, nullptr}};

} // namespace wsl
