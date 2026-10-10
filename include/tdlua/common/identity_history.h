// Copyright (c) 2026 Giuseppe Marino
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <map>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
namespace tdlua { namespace receiver {
// Exact membership, including gaps owned by other users. Allocate only at
// registration. Retirement never inserts a history node in an OOM path.
class IdentityHistory {
public:
    bool contains(int id) const {
        if (id<=0) return false;
        const auto value=static_cast<std::uint32_t>(id);
        auto it=blocks_.find(value/64);
        return it!=blocks_.end() && (it->second & (std::uint64_t(1)<<(value%64)));
    }
    void insert(int id) {
        if (id<=0 || contains(id)) throw std::logic_error("receiver client ID invalid/reused");
        const auto value=static_cast<std::uint32_t>(id);
        blocks_[value/64] |= std::uint64_t(1)<<(value%64);
        ++identities_;
    }
    std::size_t blocks() const { return blocks_.size(); }
    std::size_t identities() const { return identities_; }
private:
    std::map<std::uint32_t,std::uint64_t> blocks_;
    std::size_t identities_=0;
};
}}
