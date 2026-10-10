#pragma once
#include "tdlua/common/failure_cleanup.h"
#include <nlohmann/json.hpp>
#include <limits>

namespace tdlua { namespace receiver {
// Discard payload without constructing a DOM. Lexer buffers may still allocate;
// this reduces OOM pressure, it does not promise allocation-free TDLib receive.
class JsonCleanupMetadata final : public nlohmann::json_sax<nlohmann::json> {
public:
    using Json = nlohmann::json;
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(Json::number_integer_t value) override {
        if (depth_==1 && key_==Key::Client && value>0 && value<=std::numeric_limits<int>::max())
            client_=static_cast<int>(value);
        return true;
    }
    bool number_unsigned(Json::number_unsigned_t value) override {
        if (depth_==1 && key_==Key::Client && value>0 && value<=static_cast<unsigned>(std::numeric_limits<int>::max()))
            client_=static_cast<int>(value);
        return true;
    }
    bool number_float(Json::number_float_t, const Json::string_t &) override { return true; }
    bool string(Json::string_t &value) override {
        if (key_==Key::Type && depth_==1) authorization_update_=value=="updateAuthorizationState";
        if (key_==Key::Type && auth_depth_!=0 && depth_==auth_depth_)
            authorization_closed_=value=="authorizationStateClosed";
        return true;
    }
    bool binary(Json::binary_t &) override { return true; }
    bool start_object(std::size_t) override {
        if (depth_==1 && key_==Key::Authorization) auth_depth_=2;
        ++depth_; key_=Key::None; return true;
    }
    bool key(Json::string_t &value) override {
        key_=value=="@client_id" ? Key::Client : value=="@type" ? Key::Type :
             value=="authorization_state" ? Key::Authorization : Key::None;
        return true;
    }
    bool end_object() override {
        if (depth_==auth_depth_) auth_depth_=0;
        --depth_; key_=Key::None; return true;
    }
    bool start_array(std::size_t) override { ++depth_; key_=Key::None; return true; }
    bool end_array() override { --depth_; key_=Key::None; return true; }
    bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override { return false; }
    static FailureCleanup::Event read(const char *payload) {
        JsonCleanupMetadata metadata;
        if (!Json::sax_parse(payload,&metadata) || metadata.client_==0)
            throw std::runtime_error("invalid TDLib cleanup metadata");
        FailureCleanup::Event event;
        event.item=true; event.client=metadata.client_;
        event.terminal=metadata.authorization_update_ && metadata.authorization_closed_;
        return event;
    }
private:
    enum class Key { None, Client, Type, Authorization };
    Key key_=Key::None;
    int depth_=0, auth_depth_=0, client_=0;
    bool authorization_update_=false, authorization_closed_=false;
};
}}
