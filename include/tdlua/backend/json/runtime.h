#pragma once
#include "tdlua/common/failure_cleanup.h"
#include "tdlua/common/receiver.h"
#include <nlohmann/json.hpp>

struct lua_State;

class JsonRuntime {
  public:
    using Engine = tdlua::receiver::Runtime<nlohmann::json>;
    using Box = Engine::Box;
    static JsonRuntime &instance();
    std::uint64_t attachVm();
    void detachVm(std::uint64_t owner);
    std::int32_t createClient(std::uint64_t owner);
    void send(std::int32_t client, const nlohmann::json &request);
    nlohmann::json execute(const nlohmann::json &request);
    nlohmann::json receive(std::int32_t client, double timeout);
    bool retireReserved(std::int32_t client);
    void rejectShutdown(std::int32_t client);
    bool shutdownPending(std::int32_t client) const { return lifecycle_->shutdownPending(client); }
    void forget(std::int32_t client, bool timed_out = false);
#ifdef TDLUA_TESTING
    bool waitForNoOwners()
    {
        return engine_.waitForNoOwners(std::chrono::seconds(3));
    }
    void replaceWithFake(std::int32_t client);
    std::size_t testClientRegistrySize()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return clients_.size();
    }
#endif
    tdlua::receiver::FailureCleanup::Stats cleanupStats() const
    {
        return lifecycle_->stats();
    }
    tdlua::receiver::RuntimeStats stats() const
    {
        return engine_.stats();
    }

  private:
    JsonRuntime();
    std::shared_ptr<tdlua::receiver::FailureCleanup> lifecycle_;
    std::shared_ptr<tdlua::receiver::Source<nlohmann::json>> source_;
    Engine engine_;
    std::mutex mutex_;
    std::map<int, std::shared_ptr<Box>> clients_;
};

// VM owner registration is created before any other TDLua finalizable object.
void tdlua_json_initialize_vm(lua_State *L);
void tdlua_json_retain_vm(lua_State *L, int client);
std::uint64_t tdlua_json_vm_owner(lua_State *L);

#ifdef TDLUA_TESTING
void tdlua_json_test_creation_stage(int stage);
#endif
