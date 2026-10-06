#include "binding/lua_binding.h"

namespace {

const tdlua_binding::ClientOperations &operations()
{
    return tdlua_backend_operations();
}

tdlua_binding::ClientHandle client(lua_State *L)
{
    return tdlua_binding::get_client(L);
}

static int tdclient_index(lua_State *L);
static int tdclient_newindex(lua_State *L);
static int tdclient_new(lua_State *L);

static int tdclient_receive(lua_State *L) { return operations().receive(L); }
static int tdclient_send(lua_State *L) { return operations().send(L); }
static int tdclient_execute(lua_State *L) { return operations().execute(L); }
static int tdclient_call(lua_State *L) { return operations().call(L); }
static int tdclient_rawexecute(lua_State *L)
{
    return operations().raw_execute(L);
}
static int tdclient_request(lua_State *L) { return operations().request(L); }
static int tdclient_await(lua_State *L) { return operations().await(L); }

static int tdclient_index(lua_State *L)
{
    return tdlua_binding::index(L, client(L), operations(), tdclient_call);
}

static int tdclient_newindex(lua_State *L)
{
    return tdlua_binding::newindex(L, client(L), operations());
}

static int tdclient_on(lua_State *L)
{
    return tdlua_binding::on(L, client(L), operations());
}

static int tdclient_off(lua_State *L)
{
    return tdlua_binding::off(L, client(L), operations());
}

static int tdclient_save(lua_State *L)
{
    return tdlua_binding::save(L, client(L), operations());
}

static int tdclient_clear(lua_State *L)
{
    return tdlua_binding::clear(L, client(L), operations());
}

static int tdclient_close(lua_State *L)
{
    return tdlua_binding::close(L, client(L), operations());
}

static int tdclient_isclosed(lua_State *L)
{
    return tdlua_binding::is_closed(L, client(L), operations());
}

static int tdclient_unload(lua_State *L)
{
    return tdlua_binding::unload(L, client(L), operations());
}

#ifdef TDLUA_TESTING
static int tdclient_pending_count(lua_State *L)
{
    return operations().pending_count(L);
}
#endif

static int tdclient_getcall(lua_State *L)
{
    return luaL_error(L, "TDLua VoIP support has been removed");
}

static int tdclient_setlogpath(lua_State *L)
{
    return operations().set_log_path(L);
}

static int tdclient_setlogmaxsize(lua_State *L)
{
    return operations().set_log_max_size(L);
}

static int tdclient_setlogverbosity(lua_State *L)
{
    return operations().set_log_level(L);
}

static luaL_Reg methods[] = {
    {"receive", tdclient_receive},
    {"poll", tdclient_receive},
    {"send", tdclient_send},
    {"execute", tdclient_execute},
    {"_execute", tdclient_rawexecute},
    {"executeSync", tdclient_rawexecute},
    {"request", tdclient_request},
    {"await", tdclient_await},
    {"on", tdclient_on},
    {"off", tdclient_off},
    {"close", tdclient_close},
    {"destroy", tdclient_close},
    {"isClosed", tdclient_isclosed},
    {"save", tdclient_save},
    {"clearBuffer", tdclient_clear},
    {"getCall", tdclient_getcall},
#ifdef TDLUA_TESTING
    {"pendingCount", tdclient_pending_count},
#endif
    {nullptr, nullptr}
};

static int tdclient_new(lua_State *L)
{
    const auto &ops = operations();
    return tdlua_binding::new_client(
        L, ops.factory, tdclient_index, tdclient_newindex, tdclient_unload,
        methods);
}

static luaL_Reg module_functions[] = {
    {"new", tdclient_new},
    {"setLogPath", tdclient_setlogpath},
    {"setLogMaxSize", tdclient_setlogmaxsize},
    {"setLogLevel", tdclient_setlogverbosity},
    {nullptr, nullptr}
};

}  // namespace

#ifndef TDLUA_VERSION_STRING
#define TDLUA_VERSION_STRING "unknown"
#endif
#ifndef TDLUA_BASE_VERSION
#define TDLUA_BASE_VERSION "unknown"
#endif
#ifndef TDLUA_TDLIB_VERSION
#define TDLUA_TDLIB_VERSION "unknown"
#endif

extern "C" {

LUALIB_API int luaopen_tdlua(lua_State *L)
{
    const auto &ops = operations();
    luaL_newmetatable(L, "tdlua");
    lua_pushstring(L, "__call");
    lua_pushcfunction(L, tdclient_new);
    lua_settable(L, -3);
    luaL_newlib(L, module_functions);
    lua_pushstring(L, TDLUA_VERSION_STRING);
    lua_setfield(L, -2, "version");
    lua_pushstring(L, TDLUA_BASE_VERSION);
    lua_setfield(L, -2, "api_version");
    lua_pushstring(L, TDLUA_TDLIB_VERSION);
    lua_setfield(L, -2, "tdlib_version");
    luaL_setmetatable(L, "tdlua");
    ops.initialize_logging();
    return 1;
}

}  // extern "C"
