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

struct WsCallback { dmScript::LuaCallbackInfo *cb; };
static WsCallback *g_current = nullptr;

static WsCallback *ws_cb_create(lua_State *L, int index) { return new WsCallback{dmScript::CreateCallback(L, index)}; }
static void ws_cb_destroy(WsCallback *c) { if (!c) return; if (c->cb) dmScript::DestroyCallback(c->cb); delete c; }
static bool ws_cb_begin(WsCallback *c, lua_State **L) {
    // The script that registered it may have been deleted (collection unloaded): skip instead of crashing.
    if (!c->cb || !dmScript::IsCallbackValid(c->cb)) return false;
    lua_State *CL = dmScript::GetCallbackLuaContext(c->cb);
    if (!dmScript::SetupCallback(c->cb)) return false; // pushes the function and self
    *L = CL; g_current = c;
    return true;
}
static void ws_cb_call(lua_State *L, int nargs) {
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
