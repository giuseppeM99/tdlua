/**
 * @author Giuseppe Marino
 * ©Giuseppe Marino 2018 - 2018
 * This file is under GPLv3 license see LICENCE
 */

#pragma once
#ifndef TGVOIP_USE_CALLBACK_AUDIO_IO
#define TGVOIP_USE_CALLBACK_AUDIO_IO
#endif

#include <queue>

#include "luajson.h"
#include "libtgvoip/VoIPController.h"

class TDLua;

class Call {
private:
    bool playing = false;
    tgvoip::VoIPController* controller;
    nlohmann::json call;
    uint32_t id;
    int state;
    TDLua* td;
    void sendAudioFrame(int16_t *data, size_t size);
    void recvAudioFrame(int16_t *data, size_t size);
    std::queue<std::string> inputFiles;
    std::queue<std::string> holdFiles;
    size_t readInput;
    size_t readOutput;
    tgvoip::Mutex inputMutex;
    FILE* input = NULL;
    //int r;
    //bool luaCB = false;
    //bool nextFile();
    lua_State *L;
public:
    Call(const nlohmann::json _call, TDLua* _td, lua_State *l);
    ~Call();
    void deInit();
    static Call* NewLua(lua_State *L, const nlohmann::json call, TDLua* _td);
    static void setMeta(lua_State *L);
    static Call* getCall(lua_State *L);
    nlohmann::json getTDCall() const;
    void closeCall();
    bool play(const char* filename);
    bool onHold(nlohmann::json list);
    nlohmann::json getDebug() const;
    void stop();
    //void setLuaCallback();
    //void delLuaCallback();
};
