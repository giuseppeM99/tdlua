// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#include "tdlua/common/receiver.h"
#include "tdlua/lua_compat.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <iostream>
#include <stdexcept>
#include <string>

void check(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}
void run(lua_State *L, const char *code)
{
    if (luaL_dostring(L, code) != LUA_OK) throw std::runtime_error(lua_tostring(L, -1));
}
namespace {
enum class CreationStage : int {
    BeforeClientId = 0,
    AfterClientId = 1,
    AfterLedgerRegistration = 2,
    AfterEngineRegistration = 3,
    BeforeDispatcherConstruction = 5,
    AfterDispatcherSetup = 6,
    NativeClientCreation = 11
};

enum class ControlAction : int {
    PauseSource = 0,
    ReleaseSource = 1,
    WaitForSource = 2,
    HoldTerminal = 3,
    ReleaseTerminal = 4,
    FailBeforePublish = 5,
    FailAfterPublish = 6,
    PauseCleanup = 7,
    ReleaseCleanup = 8,
    WaitForCleanup = 9,
    WaitForNoOwners = 10,
    FailNextClientCreation = 11,
    FailNextSend = 12,
    WaitForTerminalAck = 13,
    MarkTerminalAcks = 14
};

constexpr int value(CreationStage stage) { return static_cast<int>(stage); }
constexpr int value(ControlAction action) { return static_cast<int>(action); }

int construction_stage = -1;
void failConstruction(int stage)
{
    if (stage == construction_stage) throw std::bad_alloc();
}
}
int main(int argc, char **argv)
{
    lua_State *L = nullptr;
    try {
        check(argc == 3, "usage: module-directory backend");
        const std::string directory = argv[1], backend = argv[2];
        L = luaL_newstate();
        check(L != nullptr, "could not create Lua state");
        luaL_openlibs(L);
        lua_getglobal(L, "package");
#ifdef _WIN32
        lua_pushstring(L, (directory + "/?.dll").c_str());
#else
        lua_pushstring(L, (directory + "/?.so").c_str());
#endif
        lua_setfield(L, -2, "cpath"); lua_pop(L, 1);
        run(L, "td=require('tdlua'); td.setLogLevel(0)");
        run(L, "function checkAuthorization() "
               " local done=false; local co=coroutine.create(function() "
               " assert(client:getAuthorizationState():wait(2)); done=true end); "
               " assert(coroutine.resume(co)); "
               " for i=1,200 do if done then break end client:receive(0.01) end; "
               " assert(done) end");
#ifdef _WIN32
        HMODULE dso = nullptr;
        check(GetModuleHandleExW(0, L"tdlua.dll", &dso) != 0,
              "module was not loaded through require");
#else
        void *dso = dlopen((directory + "/tdlua.so").c_str(), RTLD_NOW | RTLD_NOLOAD);
        check(dso, "module was not loaded through require");
#endif
        using Control = int (*)(int);
        using Ledger = tdlua::receiver::FailureCleanup::Stats (*)();
        using Stats = tdlua::receiver::RuntimeStats (*)();
#ifdef _WIN32
        const auto control = reinterpret_cast<Control>(GetProcAddress(dso, ("tdlua_" + backend + "_test_control").c_str()));
        const auto ledger = reinterpret_cast<Ledger>(GetProcAddress(dso, ("tdlua_" + backend + "_test_cleanup_stats").c_str()));
        const auto stats = reinterpret_cast<Stats>(GetProcAddress(dso, ("tdlua_" + backend + "_test_stats").c_str()));
#else
        const auto control = reinterpret_cast<Control>(dlsym(dso, ("tdlua_" + backend + "_test_control").c_str()));
        const auto ledger = reinterpret_cast<Ledger>(dlsym(dso, ("tdlua_" + backend + "_test_cleanup_stats").c_str()));
        const auto stats = reinterpret_cast<Stats>(dlsym(dso, ("tdlua_" + backend + "_test_stats").c_str()));
#endif
        check(control && ledger && stats, "testing hooks unavailable");
        // Query TLS in the module that owns it. Mach-O exposes a TLV descriptor
        // for a thread_local symbol, not the calling thread's counter storage.
        using CoreCount = std::size_t (*)();
#ifdef _WIN32
        const auto cores = reinterpret_cast<CoreCount>(
            GetProcAddress(dso, "tdlua_test_live_scheduler_cores"));
#else
        const auto cores = reinterpret_cast<CoreCount>(
            dlsym(dso, "tdlua_test_live_scheduler_cores"));
#endif
        using CreationHook = void (*)(void (*)(int));
#ifdef _WIN32
        auto set_creation_hook = reinterpret_cast<CreationHook>(
            GetProcAddress(dso, "tdlua_json_test_creation_hook"));
#else
        auto set_creation_hook = reinterpret_cast<CreationHook>(
            dlsym(dso, "tdlua_json_test_creation_hook"));
#endif
        check(cores && (backend != "json" || set_creation_hook), "construction counters unavailable");
#ifdef _WIN32
        check(FreeLibrary(dso) != 0, "could not release module reference");
#else
        dlclose(dso);
#endif
        // JSON exposes each construction boundary. Native has one equivalent
        // factory fault, armed through its control hook.
        if (backend == "json") {
            for (const CreationStage point : {
                     CreationStage::BeforeClientId,
                     CreationStage::AfterClientId,
                     CreationStage::AfterLedgerRegistration,
                     CreationStage::AfterEngineRegistration,
                     CreationStage::BeforeDispatcherConstruction,
                     CreationStage::AfterDispatcherSetup}) {
                const int stage = value(point);
                const auto before = ledger();
                const auto owners_before = stats().owners;
                construction_stage = stage;
                set_creation_hook(failConstruction);
                lua_pushinteger(L, stage); lua_setglobal(L, "diagnostic_stage");
                run(L, "local ok,err=pcall(function() return td() end); "
                       "print('construction fault stage='..diagnostic_stage..' pcall_ok='.."
                       "tostring(ok)..' error='..tostring(err)); "
                       "assert(not ok and err:find('bad_alloc'))");
                set_creation_hook(nullptr);
                lua_gc(L, LUA_GCCOLLECT, 0);
                lua_gc(L, LUA_GCCOLLECT, 0);
                const auto after = ledger();
                check(stats().owners == owners_before && cores() == 0 && stats().clients == 0 && stats().tombstones == 0 &&
                      after.records == 0 && after.reserved == 0 && after.active_sends == 0 &&
                      after.uncertain == 0 && after.materialized == before.materialized &&
                      after.closes == before.closes, "factory rollback retained core/client or materialized an actor");
                std::cout << "factory stage " << stage << ": zero cores/live records before VM close PASS\n";
            }
        } else {
            const auto before = ledger();
            const auto owners_before = stats().owners;
            check(control(value(ControlAction::FailNextClientCreation)) == 1,
                  "construction fault not armed");
            run(L, "local ok,err=pcall(function() return td() end); "
                   "print('native construction fault pcall_ok='..tostring(ok).."
                   "' error='..tostring(err)); "
                   "assert(not ok and err:find('bad_alloc'))");
            lua_gc(L, LUA_GCCOLLECT, 0);
            lua_gc(L, LUA_GCCOLLECT, 0);
            const auto after = ledger();
            check(stats().owners == owners_before && cores() == 0 && stats().clients == 0 && stats().tombstones == 0 &&
                  after.records == 0 && after.reserved == 0 && after.active_sends == 0 &&
                  after.uncertain == 0 && after.materialized == before.materialized &&
                  after.closes == before.closes,
                  "factory rollback retained core/client or materialized an actor");
            std::cout << "native client creation: zero cores/live records before VM close PASS\n";
        }
        for (const char *request : {"close", "destroy"}) {
            const auto before = ledger();
            run(L, "client=td(); checkAuthorization()");
            check(control(value(ControlAction::MarkTerminalAcks)) == 1,
                  "terminal acknowledgment gate could not be armed");
            check(control(value(ControlAction::FailNextSend)) == 1, "send fault not armed");
            lua_pushstring(L, request); lua_setglobal(L, "shutdown_name");
            run(L, "local ok,err=pcall(function() return client[shutdown_name](client) end); "
                   "print('shutdown fault request='..shutdown_name..' pcall_ok='.."
                   "tostring(ok)..' error='..tostring(err)..' closed='.."
                   "tostring(client:isClosed())); "
                   "assert(not ok and err:find('bad_alloc')); assert(not client:isClosed()); "
                   "checkAuthorization(); "
                   "client=nil; collectgarbage('collect'); collectgarbage('collect')");
            check(control(value(ControlAction::WaitForTerminalAck)) == 1,
                  "terminal mailbox publication was not acknowledged");
            const auto after = ledger();
            check(after.records == before.records && after.reserved == before.reserved &&
                  after.remaining == before.remaining && after.active_sends == 0 && after.uncertain == 0,
                  "rejected synchronous shutdown retained ledger records");
            check(after.closes == before.closes + 1 && after.terminals == before.terminals + 1,
                  "GC did not close the materialized client exactly once");
            std::cout << request << ": synchronous rejection, recovery and one GC close PASS\n";
        }
        // A routing failure must not make the destructor repeat disposal after
        // closeInternal has already forgotten the runtime registration.
        check(control(value(ControlAction::PauseSource)) == 1, "source pause not armed");
        run(L, "client=td(); callbacks=0; failed_handle=client:getAuthorizationState("
               "function() callbacks=callbacks+1 end)");
        check(control(value(ControlAction::WaitForSource)) == 1,
              "receiver did not reach the source gate");
        check(control(value(ControlAction::FailBeforePublish)) == 1 &&
                  control(value(ControlAction::ReleaseSource)) == 1,
              "routing fault not released");
        check(control(value(ControlAction::WaitForCleanup)) == 1 &&
                  stats().state == tdlua::receiver::State::Failed,
              "receiver did not enter Failed cleanup");
        run(L, "client=nil; for i=1,3 do "
               "local ok,err=pcall(collectgarbage,'collect'); "
               "if not ok then assert(tostring(err):find('tdlua: receiver out of memory',1,true)); "
               "print('observed Failed finalizer error: '..tostring(err)) end end; "
               "assert(callbacks==0 and failed_handle:ready()); "
               "local ok,err=pcall(function() failed_handle:wait(0) end); "
               "assert(not ok and err:find('client closed')); "
               "failed_handle=nil; collectgarbage('collect'); collectgarbage('collect')");
        const auto detached = stats();
        std::cout << "Failed GC counters: cores=" << cores()
                  << " clients=" << detached.clients << " tombstones=" << detached.tombstones
                  << " owners=" << detached.owners << '\n';
        check(cores() == 0 && detached.clients == detached.tombstones,
              "Failed GC retained an application client/core");
        std::cout << "Failed GC: callback-free settlement, zero application clients/cores PASS\n";
        lua_close(L); L = nullptr;
        check(control(value(ControlAction::WaitForNoOwners)) == 1,
              "last VM did not join");
        const auto joined = stats();
        const auto cleanup = ledger();
        check(joined.owners == 0 && !joined.joinable && joined.clients == 0 && joined.tombstones == 0,
              "runtime owner/client survived VM close");
        check(cleanup.phase == tdlua::receiver::FailureCleanup::Phase::Quiescent &&
              cleanup.records == 0 && cleanup.active_sends == 0 && cleanup.uncertain == 0,
              "Failed cleanup did not prove quiescence");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (L) lua_close(L);
        return 1;
    }
}
