#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <fstream>

#include "LuaTDVoip.h"
#include "tdlua.h"
#include "base64/base64.h"
#include "libtgvoip/VoIPServerConfig.h"

using json = nlohmann::json;
using namespace tgvoip;

/*
static void stackDump (lua_State *L) {
  int i;
  int top = lua_gettop(L);
  for (i = 1; i <= top; i++) {  // repeat for each level
    int t = lua_type(L, i);
    switch (t) {

      case LUA_TSTRING:  // strings
        printf("'%s'", lua_tostring(L, i));
        break;

      case LUA_TBOOLEAN:  // booleans
        printf(lua_toboolean(L, i) ? "true" : "false");
        break;

      case LUA_TNUMBER:  // numbers
        printf("%g", lua_tonumber(L, i));
        break;

      default:  // other values
        printf("%s 0x%x", lua_typename(L, t), lua_topointer(L, i));
        break;

    }
    printf("  ");  // put a separator
  }
  printf("\n");  // end the listing
}
//*/


Call::Call(const json _call, void* _td, lua_State *l)
{
    L = l;
    call = _call;
    td = _td;
    id = call["id"];
    int eps = call["state"]["connections"].size();
    std::vector<Endpoint> endpoints(eps);
    for (int i = 0; i < eps; i++) {
        auto e = call["state"]["connections"][i];
        auto id = std::stol(e["id"].get<std::string>());
        auto port = e["port"].get<uint64_t>();
        auto ipv4 = IPv4Address(e["ip"].get<std::string>());
        auto ipv6 = IPv6Address(e["ipv6"].get<std::string>());
        std::string _peer_tag = base64_decode(e["peer_tag"].get<std::string>());
        auto peer_tag = (unsigned char*) _peer_tag.c_str();
        auto ep = Endpoint(id, port, ipv4, ipv6, Endpoint::TYPE_UDP_RELAY, peer_tag);
        endpoints.push_back(ep);
    }

    auto p2p = call["state"]["protocol"]["udp_p2p"].get<bool>();
    auto layer = call["state"]["protocol"]["max_layer"].get<int32_t>();
    std::map<std::string, std::string> config;
    auto ekey = base64_decode(call["state"]["encryption_key"].get<std::string>());
    bool outgoing = call["is_outgoing"];

    auto jconfig = json::parse(call["state"]["config"].get<std::string>());
    jconfig["audio_max_bitrate"] = 100000;
    jconfig["audio_init_bitrate"] = 100000;
    jconfig["audio_min_bitrate"] = 10000;
    jconfig["audio_congestion_window"] = 4 * 1024;
    for (auto it = jconfig.begin(); it != jconfig.end(); it++) {
        std::string key = it.key();
        auto value = it.value();
        if (value.is_string()) {
            config[key] = value.get<std::string>();
        } else if (value.is_number_integer()){
            config[key] = std::to_string(value.get<int64_t>());
        } else if (value.is_boolean()) {
            config[key] = std::to_string(value.get<bool>());
        }
    }
    ServerConfig::GetSharedInstance()->Update(config);
    VoIPController::Config cfg;
    cfg.enableNS = jconfig["use_system_ns"].get<bool>();
    cfg.enableAEC = jconfig["use_system_aec"].get<bool>();
    cfg.enableNS = false;
    cfg.enableAEC = false;
    cfg.enableAGC = false;
    controller = new VoIPController();
    VoIPController::Callbacks callbacks;
    controller->implData = this;
    callbacks.connectionStateChanged = [](VoIPController *controller, int state) {
        ((Call*) controller->implData)->state = state;
        if (state == STATE_FAILED)
        {
            ((Call*) controller->implData)->deInit();
        }
    };
    callbacks.signalBarCountChanged = NULL;
    callbacks.groupCallKeySent = NULL;
    callbacks.groupCallKeyReceived = NULL;
    callbacks.upgradeToGroupCallRequested = NULL;
    controller->SetCallbacks(callbacks);
    controller->SetAudioDataCallbacks([this](int16_t *buffer, size_t size) {
        this->sendAudioFrame(buffer, size);
    }, [this](int16_t *buffer, size_t size) {
        this->recvAudioFrame(buffer, size);
    });
    controller->SetConfig(cfg);
    controller->SetEncryptionKey((char*)ekey.c_str(), outgoing);
    controller->SetRemoteEndpoints(endpoints, p2p, layer);
    controller->Start();
    controller->Connect();
    controller->DebugCtl(3, p2p);
    controller->DebugCtl(4, 0);
}

json Call::getDebug() const
{
    return json::parse(controller->GetDebugLog());
}

void Call::deInit()
{
    if (controller) {
        controller->Stop();
        delete controller;
        controller = nullptr;
    }
}

Call::~Call()
{
    deInit();
}

Call* Call::getCall(lua_State *L)
{
    if (lua_type(L, 1) == LUA_TUSERDATA) {
        return *(Call**)(lua_touserdata(L,1));
    }
    return nullptr;
}

Call* Call::NewLua(lua_State *L, const json c, void* td) // [{1}, {2}
{
    Call **call = (Call**)(lua_newuserdata(L, sizeof(Call*))); // [{1}, {2}, call]
    *call = new Call(c, td, L);
    Call::setMeta(L);
    return *call;
}

json Call::getTDCall() const
{
    return this->call;
}

bool Call::play(const char* filename)
{
    FILE *tmp = fopen(filename, "rb");

    if (tmp == NULL)
    {
        return false;
    }

    MutexGuard m(inputMutex);
    inputFiles.push(tmp);
    return true;
}
/*
void Call::setLuaCallback()
{
    luaCB = true;
    playing = true;
    r = luaL_ref (L, LUA_REGISTRYINDEX);
}

void Call::delLuaCallback()
{
    if (luaCB) {
        luaL_unref(L, LUA_REGISTRYINDEX, r);
        r = 0;
        luaCB = false;
    }
}

bool Call::nextFile()
{
    lua_rawgeti (L, LUA_REGISTRYINDEX, r);
    lua_pcall(L, 0, 1, 0);
    if (lua_isstring(L, -1)) {
        return play(lua_tostring(L, -1));
    }
    return false;
}
*/
static int play(lua_State *L)
{
    auto call = Call::getCall(L);
    //call->delLuaCallback();
    if (lua_isstring(L, -1)) {
        const char* fileName = lua_tostring(L, -1);
        call->play(fileName);
    } else if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_isstring(L, -1)) {
                const char* fileName = lua_tostring(L, -1);
                call->play(fileName);
            }
            lua_pop(L, 1);
        }
    }/* else if (lua_isfunction(L, -1)) {
        Call::getCall(L)->setLuaCallback();
    }*/
    lua_pushvalue(L, 1);
    return 1;
}

bool Call::onHold(json list)
{
    FILE *tmp = NULL;

    MutexGuard m(inputMutex);
    while (holdFiles.size())
    {
        fclose(holdFiles.front());
        holdFiles.pop();
    }
    for (int i = 0; i < list.size(); i++)
    {
        tmp = fopen(list[i].get<std::string>().c_str(), "rb");
        if (tmp != NULL)
        {
            holdFiles.push(tmp);
        }
    }
    return true;
}

static int onHold(lua_State *L)
{
    if (lua_istable(L, -1)) {
        json list;
        lua_getjson(L, list);
        Call::getCall(L)->onHold(list);
    }
    lua_pushvalue(L, 1);
    return 1;
}

static struct luaL_Reg CallMeta[] = {
    {"play", play},
    {"next", play},
    {"onHold", onHold},
    {"debug", [](lua_State *L) {
        lua_pushjson(L, Call::getCall(L)->getDebug());
        return 1;
    }},
    {nullptr, nullptr}
};

void Call::closeCall()
{
    TDLua* td =  (TDLua*)this->td;
    td->send({
        {"@type", "discardCall"},
        {"call_id", id},
        {"is_disconnected", false},
        {"connection_id", controller->GetPreferredRelayID()},
        {"duration", 0}
    });
}

void Call::setMeta(lua_State *L)
{
    luaL_newmetatable(L, "tdcall");
    lua_pushstring(L, "__index");
    lua_pushvalue(L, -4);
    lua_newtable(L);
    luaL_newlib(L, CallMeta);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_settable(L, -3);
    lua_pushstring(L, "__pairs");
    lua_pushcfunction(L, [](lua_State *L){
        lua_getglobal(L, "next");
        lua_getmetatable(L, 1);
        lua_getfield(L, -1, "__index");
        lua_remove(L, -2);
        lua_pushnil(L);
        return 3;
    });
    lua_settable(L, -3);
    /*
    lua_pushcfunction(L, [](lua_State *L) {
        Call* call = Call::getCall(L);
        TDLua* td = (TDLua*)call->td;
        td->send({
            {"@type", "discardCall"},
            {"call_id", call->id},
            {"is_disconnected", false},
            {"connection_id", call->controller->GetPreferredRelayID()},
            {"duration", 0}
        });
        return 0;
    });
    lua_setfield(L, -2, "__gc");
    */
    lua_setmetatable(L, -2);

}



// https://github.com/danog/php-libtgvoip/blob/master/main.cpp#L97
void Call::sendAudioFrame(int16_t *data, size_t size)
{
    MutexGuard m(inputMutex);
    if (!inputFiles.empty())
    {
        if ((readInput = fread(data, sizeof(int16_t), size, inputFiles.front())) != size)
        {
            fclose(inputFiles.front());
            inputFiles.pop();
            memset(data + (readInput % size), 0, size - (readInput % size));
        }
        playing = true;
    }
    else
    {
        if (!holdFiles.empty())
        {
            if ((readInput = fread(data, sizeof(int16_t), size, holdFiles.front())) != size)
            {
                fseek(this->holdFiles.front(), 0, SEEK_SET);
                this->holdFiles.push(this->holdFiles.front());
                this->holdFiles.pop();
                memset(data + (readInput % size), 0, size - (readInput % size));
            }
        }
    }
}

void Call::recvAudioFrame(int16_t *data, size_t size)
{

}
