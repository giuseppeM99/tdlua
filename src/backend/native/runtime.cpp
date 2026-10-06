#include "tdlua/backend/native/runtime.h"

#include <chrono>
#include <utility>

NativeResponse::NativeResponse()
    : client_id(0), request_id(0), object(nullptr), dispatched(false)
{
}

NativeResponse::NativeResponse(NativeResponse &&other) noexcept
    : client_id(other.client_id), request_id(other.request_id),
      object(std::move(other.object)), dispatched(other.dispatched)
{
}

NativeResponse &NativeResponse::operator=(NativeResponse &&other) noexcept
{
    if (this != &other) {
        client_id = other.client_id;
        request_id = other.request_id;
        object = std::move(other.object);
        dispatched = other.dispatched;
    }
    return *this;
}

NativeRuntime &NativeRuntime::instance()
{
    // TDLib's ClientManager uses TDLib's process-wide allocator during its
    // destructor. Keep the manager alive until process termination so that
    // static destruction order cannot tear down TDLib before the manager.
    static NativeRuntime *runtime = new NativeRuntime();
    return *runtime;
}

NativeRuntime::NativeRuntime()
    : manager_(), receive_mutex_(), pending_(), active_clients_()
{
}

td::ClientManager::ClientId NativeRuntime::create_client()
{
    const td::ClientManager::ClientId client_id = manager_.create_client_id();
    std::lock_guard<std::mutex> lock(receive_mutex_);
    active_clients_.insert(client_id);
    return client_id;
}

void NativeRuntime::send(td::ClientManager::ClientId client_id,
                         td::ClientManager::RequestId request_id,
                         td::td_api::object_ptr<td::td_api::Function> request)
{
    manager_.send(client_id, request_id, std::move(request));
}

NativeResponse NativeRuntime::receive(td::ClientManager::ClientId client_id,
                                      double timeout)
{
    std::lock_guard<std::mutex> lock(receive_mutex_);
    if (active_clients_.find(client_id) == active_clients_.end()) {
        return NativeResponse();
    }
    const std::map<td::ClientManager::ClientId, std::queue<NativeResponse> >::iterator queued =
        pending_.find(client_id);
    if (queued != pending_.end() && !queued->second.empty()) {
        NativeResponse result(std::move(queued->second.front()));
        queued->second.pop();
        return result;
    }

    const auto started = std::chrono::steady_clock::now();
    while (true) {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        const double remaining = timeout - elapsed;
        if (remaining <= 0.0) {
            return NativeResponse();
        }

        td::ClientManager::Response response = manager_.receive(remaining);
        if (!response.object) {
            return NativeResponse();
        }

        NativeResponse result;
        result.client_id = response.client_id;
        result.request_id = response.request_id;
        result.object = std::move(response.object);
        if (result.client_id == client_id) {
            return result;
        }
        if (active_clients_.find(result.client_id) != active_clients_.end()) {
            pending_[result.client_id].push(std::move(result));
        }
    }
}

td::td_api::object_ptr<td::td_api::Object> NativeRuntime::execute(
    td::td_api::object_ptr<td::td_api::Function> request)
{
    return td::ClientManager::execute(std::move(request));
}

void NativeRuntime::forget(td::ClientManager::ClientId client_id)
{
    std::lock_guard<std::mutex> lock(receive_mutex_);
    active_clients_.erase(client_id);
    pending_.erase(client_id);
}
