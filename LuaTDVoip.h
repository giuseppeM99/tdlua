#pragma once
#ifndef TGVOIP_USE_CALLBACK_AUDIO_IO
#define TGVOIP_USE_CALLBACK_AUDIO_IO
#endif

#include <queue>
#include <stdio.h>

#include "luajson.h"
//#include "libtgvoip/webrtc_dsp/webrtc/common_audio/wav_file.h"
#include "libtgvoip/VoIPController.h"

class Call {
private:
    bool playing = false;
    tgvoip::VoIPController* controller;
    nlohmann::json call;
    uint32_t id;
    int state;
    void* td;
    void sendAudioFrame(int16_t *data, size_t size);
    void recvAudioFrame(int16_t *data, size_t size);
    std::queue<FILE*> inputFiles;
    std::queue<FILE*> holdFiles;
    size_t readInput;
    size_t readOutput;
    tgvoip::Mutex inputMutex;
    //int r;
    //bool luaCB = false;
    //bool nextFile();
    lua_State *L;
public:
    Call(const nlohmann::json _call, void* _td, lua_State *l);
    ~Call();
    void deInit();
    static Call* NewLua(lua_State *L, const nlohmann::json call, void* _td);
    static void setMeta(lua_State *L);
    static Call* getCall(lua_State *L);
    nlohmann::json getTDCall() const;
    void closeCall();
    bool play(const char* filename);
    bool onHold(nlohmann::json list);
    nlohmann::json getDebug() const;
    //void setLuaCallback();
    //void delLuaCallback();
};
