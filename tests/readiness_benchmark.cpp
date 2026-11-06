// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
//
// E1.4B real-TDLib benchmark (opt-in, manual). It measures the worker and the
// readiness notification path with the JSON backend and no Telegram login.
// No production timeout is changed: the 1 s experiment is selected through
// the TDLUA_TEST_WORKER_TIMEOUT_MS test environment variable.
#include "tdlua/backend/json/client.h"
#include "tdlua/backend/json/runtime.h"
#include "tdlua/binding/lua_binding.h"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

extern "C" void tdlua_json_test_inject(int client, int value, int terminal);

double user_seconds(const rusage &usage)
{
    return static_cast<double>(usage.ru_utime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec) / 1e6;
}

double sys_seconds(const rusage &usage)
{
    return static_cast<double>(usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_stime.tv_usec) / 1e6;
}

double percentile(std::vector<double> samples, double fraction)
{
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    std::size_t index = static_cast<std::size_t>(
        fraction * static_cast<double>(samples.size() - 1));
    if (index >= samples.size()) index = samples.size() - 1;
    return samples[index];
}

nlohmann::json probe_update(int value)
{
    return {{"@type", "updateOption"},
            {"name", "tdlua_probe"},
            {"value", {{"@type", "optionValueString"},
                       {"value", std::to_string(value)}}}};
}

} // namespace

int main(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "watch";
    const double seconds = argc > 2 ? std::atof(argv[2]) : 2.0;
    const int runs = argc > 3 ? std::atoi(argv[3]) : 3;
    const int latency_samples = argc > 4 ? std::atoi(argv[4]) : 1000;
    const bool watch = mode == "watch";

    lua_State *L = luaL_newstate();
    if (!L) return 1;
    luaL_openlibs(L);
    if (luaopen_tdlua(L) != 1) {
        std::fprintf(stderr, "cannot open tdlua\n");
        return 1;
    }
    lua_setglobal(L, "tdlua");

    const auto init_started = Clock::now();
    lua_getglobal(L, "tdlua");
    if (lua_pcall(L, 0, 1, 0) != 0) {
        std::fprintf(stderr, "client: %s\n", lua_tostring(L, -1));
        return 1;
    }
    auto *client = static_cast<TDLua *>(
        tdlua_binding::get_client_at(L, -1));
    if (!client) {
        std::fprintf(stderr, "client handle missing\n");
        return 1;
    }
    lua_setglobal(L, "client");
    const int client_id = client->clientIdForTesting();

    // A request that TDLib can answer without setTdlibParameters: it returns a
    // deterministic error. Measure the round trip.
    const auto send_started = Clock::now();
    lua_getglobal(L, "client");
    lua_getfield(L, -1, "send");
    lua_pushvalue(L, -2);
    lua_newtable(L);
    lua_pushliteral(L, "getMe");
    lua_setfield(L, -2, "@type");
    if (lua_pcall(L, 2, 0, 0) != 0) {
        std::fprintf(stderr, "send: %s\n", lua_tostring(L, -1));
        return 1;
    }
    const nlohmann::json response = client->receiveBackend(2.0);
    const double getme_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - send_started)
            .count();
    const double init_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - init_started)
            .count();
    if (response.is_null() || response.empty())
        std::fprintf(stderr, "warning: no getMe response\n");

    tdlua::receiver::ReadinessView<nlohmann::json> view = client->readiness();

    std::printf("mode,run,seconds,user_s,sys_s,wall_s,init_ms,getme_ms,"
                "receive_calls,watch_receive_calls,empty_receive_calls,"
                "published,watch_demands,consumed,latency_samples,"
                "p50_us,p95_us,p99_us\n");

    for (int run = 1; run <= runs; ++run) {
        const auto before_stats = JsonRuntime::instance().stats();
        rusage before_usage{};
        getrusage(RUSAGE_SELF, &before_usage);
        int consumed = 0;

        if (watch) {
            if (!view.enableWatch(true)) {
                std::fprintf(stderr, "cannot enable watch\n");
                return 1;
            }
        }

        const auto wall_started = Clock::now();
        const auto deadline = wall_started +
            std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(seconds));
        while (Clock::now() < deadline) {
            if (watch) {
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - Clock::now());
                const int wait_ms = remaining.count() > 0
                    ? static_cast<int>(remaining.count())
                    : 0;
                const int ready = view.wait(wait_ms);
                if (ready == 1) {
                    view.clearWake();
                    while (view.hasWork()) {
                        nlohmann::json update =
                            client->receiveBackend(0.0);
                        if (update.is_null() || update.empty()) break;
                        ++consumed;
                    }
                }
            } else {
                nlohmann::json update = client->receiveBackend(0.05);
                if (!update.is_null() && !update.empty()) ++consumed;
            }
        }
        const double wall = std::chrono::duration<double>(
            Clock::now() - wall_started).count();
        rusage after_usage{};
        getrusage(RUSAGE_SELF, &after_usage);
        const auto after_stats = JsonRuntime::instance().stats();
        if (watch) view.disableWatch();

        std::printf("%s,%d,%.3f,%.6f,%.6f,%.6f,%.3f,%.3f,"
                    "%llu,%llu,%llu,%llu,%llu,%d,0,0,0,0\n",
                    mode.c_str(), run, seconds,
                    user_seconds(after_usage) - user_seconds(before_usage),
                    sys_seconds(after_usage) - sys_seconds(before_usage),
                    wall, init_ms, getme_ms,
                    (unsigned long long)(after_stats.receive_calls -
                                         before_stats.receive_calls),
                    (unsigned long long)(after_stats.watch_receive_calls -
                                         before_stats.watch_receive_calls),
                    (unsigned long long)(after_stats.empty_receive_calls -
                                         before_stats.empty_receive_calls),
                    (unsigned long long)(after_stats.published -
                                         before_stats.published),
                    (unsigned long long)after_stats.watch_demands,
                    consumed);
    }

    if (watch && view.enableWatch(true)) {
        std::vector<double> latency;
        latency.reserve(static_cast<std::size_t>(latency_samples));
        for (int sample = 0; sample < latency_samples; ++sample) {
            const auto started = Clock::now();
            tdlua_json_test_inject(client_id, sample, 0);
            const int ready = view.wait(2000);
            const double micros = std::chrono::duration<double, std::micro>(
                Clock::now() - started).count();
            view.clearWake();
            while (view.hasWork()) {
                nlohmann::json update = client->receiveBackend(0.0);
                if (update.is_null() || update.empty()) break;
            }
            if (ready == 1) latency.push_back(micros);
        }
        view.disableWatch();
        const double p50 = percentile(latency, 0.50);
        const double p95 = percentile(latency, 0.95);
        const double p99 = percentile(latency, 0.99);
        std::printf("latency,%zu,0,0,0,0,0,0,0,0,0,0,0,0,%zu,"
                    "%.1f,%.1f,%.1f\n",
                    latency.size(), latency.size(), p50, p95, p99);
    }

    tdlua_json_test_inject(client_id, 0, 1);
    client->receiveBackend(0.2);
    view.disableWatch();
    lua_close(L);
    return 0;
}
