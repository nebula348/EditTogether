#pragma once

#include "Bytes.hpp"
#include "Protocol.hpp"

#include <unordered_map>

namespace et {

// Authoritative level state: collab ID -> object save string, plus the level header.
// Keeps an order-independent hash up to date so checksums are O(1).
class ObjectStore {
public:
    std::unordered_map<uint64_t, std::string> objects;
    std::string header;

    uint64_t hash() const { return m_hash ^ fnv1a(header); }
    bool has(uint64_t id) const { return objects.contains(id); }
    std::string const* get(uint64_t id) const {
        auto it = objects.find(id);
        return it == objects.end() ? nullptr : &it->second;
    }

    void set(uint64_t id, std::string data) {
        auto [it, inserted] = objects.try_emplace(id);
        if (!inserted) m_hash ^= entryHash(id, it->second);
        it->second = std::move(data);
        m_hash ^= entryHash(id, it->second);
    }
    void erase(uint64_t id) {
        auto it = objects.find(id);
        if (it == objects.end()) return;
        m_hash ^= entryHash(id, it->second);
        objects.erase(it);
    }
    void apply(Op const& op) {
        switch (op.kind) {
            case OpKind::Create:
            case OpKind::Modify: set(op.id, op.data); break;
            case OpKind::Delete: erase(op.id); break;
            case OpKind::Header: header = op.data; break;
        }
    }
    void clear() {
        objects.clear();
        header.clear();
        m_hash = 0;
    }

private:
    static uint64_t entryHash(uint64_t id, std::string const& s) {
        uint64_t h = fnv1a(s, 1469598103934665603ull ^ (id * 0x9E3779B97F4A7C15ull));
        return h * 0xff51afd7ed558ccdull;
    }
    uint64_t m_hash = 0;
};

} // namespace et
