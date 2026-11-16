// Defold native extension: the `wavestorm` Lua module, with callbacks pumped automatically every frame.
// Like other Defold extensions, callbacks receive the calling script's `self` first:
//   wavestorm.friends(function(self, ok, list) ... end)
//   wavestorm.on("join", function(self, data) ... end)
// Shared sources (wavestorm_lua.hpp, wavestorm.cpp, wavestorm.h) are copies: run sdk/lua/defold/sync.sh after changing them.
#define EXTENSION_NAME wavestorm
#define LIB_NAME "wavestorm"
#define MODULE_NAME "wavestorm"
#include <dmsdk/sdk.h>

#ifdef WS_NO_LAUNCHER
// Web: there is no launcher to connect to. Same Lua surface ws.lua uses, answering "no runtime", so a game falls back to offline play.
static int ws_stub_init(lua_State *L) { lua_pushboolean(L, 0); lua_pushstring(L, "no_runtime"); return 2; }
static int ws_stub_restart_if_needed(lua_State *L) { lua_pushinteger(L, 0); return 1; } // nothing can restart a web game
static int ws_stub_false(lua_State *L) { lua_pushboolean(L, 0); return 1; }
static int ws_stub_nil(lua_State *L) { lua_pushnil(L); return 1; }
static int ws_stub_none(lua_State *) { return 0; }
static const luaL_reg ws_stub_functions[] = {
    {"init", ws_stub_init}, {"restart_if_needed", ws_stub_restart_if_needed}, {"has_capability", ws_stub_false},
    {"user", ws_stub_nil}, {"join_info", ws_stub_nil}, {"shutdown", ws_stub_none}, {"run_callbacks", ws_stub_none},
    {"on", ws_stub_none}, {"call", ws_stub_none}, {"net_listen", ws_stub_none}, {"net_connect", ws_stub_none},
    {"net_send", ws_stub_false}, {nullptr, nullptr}};
static dmExtension::Result InitializeWavestorm(dmExtension::Params *params) {
    luaL_register(params->m_L, MODULE_NAME, ws_stub_functions);
    lua_pushstring(params->m_L, "web-stub"); lua_setfield(params->m_L, -2, "version");
    lua_pop(params->m_L, 1);
    return dmExtension::RESULT_OK;
}
static dmExtension::Result AppInitializeWavestorm(dmExtension::AppParams *) { return dmExtension::RESULT_OK; }
static dmExtension::Result AppFinalizeWavestorm(dmExtension::AppParams *) { return dmExtension::RESULT_OK; }
static dmExtension::Result FinalizeWavestorm(dmExtension::Params *) { return dmExtension::RESULT_OK; }
DM_DECLARE_EXTENSION(EXTENSION_NAME, LIB_NAME, AppInitializeWavestorm, AppFinalizeWavestorm, InitializeWavestorm, 0, 0, FinalizeWavestorm)
#else
#define WS_LUA_INCLUDED

// Each callback also keeps a registry reference to its function. Defold ties a callback to the script that made the
// request; when that script is deleted (a menu collection unloaded while the launcher was still answering), the answer
// would be lost and the caller would wait forever (a P2P dial looks "in progress" until it goes stale). Such orphaned
// answers are still delivered, with self = nil. ws.lua drops self anyway, so its bookkeeping stays correct.
struct WsCallback { dmScript::LuaCallbackInfo *cb; lua_State *L; int ref; bool orphan; };
static WsCallback *g_current = nullptr;

static WsCallback *ws_cb_create(lua_State *L, int index) {
    lua_pushvalue(L, index);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return new WsCallback{dmScript::CreateCallback(L, index), dmScript::GetMainThread(L), ref, false};
}
static void ws_cb_destroy(WsCallback *c) {
    if (!c) return;
    if (c->cb) dmScript::DestroyCallback(c->cb);
    if (c->ref != LUA_NOREF) luaL_unref(c->L, LUA_REGISTRYINDEX, c->ref);
    delete c;
}
static bool ws_cb_begin(WsCallback *c, lua_State **L) {
    if (c->cb && dmScript::IsCallbackValid(c->cb)) {
        lua_State *CL = dmScript::GetCallbackLuaContext(c->cb);
        if (dmScript::SetupCallback(c->cb)) { // pushes the function and self
            c->orphan = false; *L = CL; g_current = c;
            return true;
        }
    }
    if (c->ref == LUA_NOREF) return false;
    lua_rawgeti(c->L, LUA_REGISTRYINDEX, c->ref);
    lua_pushnil(c->L); // self: the script is gone
    c->orphan = true; *L = c->L; g_current = c;
    return true;
}
static void ws_cb_call(lua_State *L, int nargs) {
    if (g_current->orphan) {
        if (lua_pcall(L, nargs + 1, 0, 0) != 0) { dmLogError("wavestorm callback: %s", lua_tostring(L, -1)); lua_pop(L, 1); }
        return;
    }
    dmScript::PCall(L, nargs + 1, 0); // +1: self; errors are logged by Defold
    dmScript::TeardownCallback(g_current->cb);
}

#include "wavestorm_lua.hpp"

static dmExtension::Result AppInitializeWavestorm(dmExtension::AppParams *) { return dmExtension::RESULT_OK; }
static dmExtension::Result AppFinalizeWavestorm(dmExtension::AppParams *) { return dmExtension::RESULT_OK; }

static dmExtension::Result InitializeWavestorm(dmExtension::Params *params) {
    lua_State *L = params->m_L;
    const int top = lua_gettop(L);
    luaL_register(L, MODULE_NAME, wsl::functions);
    lua_pushstring(L, ws_version()); lua_setfield(L, -2, "version");
    lua_pop(L, 1);
    assert(top == lua_gettop(L));
    return dmExtension::RESULT_OK;
}

static dmExtension::Result UpdateWavestorm(dmExtension::Params *params) {
    wsl::l_run_callbacks(params->m_L);
    return dmExtension::RESULT_OK;
}

static dmExtension::Result FinalizeWavestorm(dmExtension::Params *params) {
    wsl::l_shutdown(params->m_L);
    for (auto &kv : wsl::g_handlers) ws_cb_destroy(kv.second);
    wsl::g_handlers.clear();
    return dmExtension::RESULT_OK;
}

DM_DECLARE_EXTENSION(EXTENSION_NAME, LIB_NAME, AppInitializeWavestorm, AppFinalizeWavestorm, InitializeWavestorm, UpdateWavestorm, 0, FinalizeWavestorm)
#endif // WS_NO_LAUNCHER
