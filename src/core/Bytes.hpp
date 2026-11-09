#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace et {

// Little-endian binary writer used for all network messages.
struct ByteWriter {
    std::string buf;

    void u8(uint8_t v) { buf.push_back(static_cast<char>(v)); }
    void u16(uint16_t v) { put(v, 2); }
    void u32(uint32_t v) { put(v, 4); }
    void u64(uint64_t v) { put(v, 8); }
    void str(std::string_view s) {
        u32(static_cast<uint32_t>(s.size()));
        buf.append(s);
    }

private:
    void put(uint64_t v, int n) {
        for (int i = 0; i < n; ++i) buf.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
};

// Bounds-checked reader. Any out-of-range read sets ok=false and returns zero/empty.
struct ByteReader {
    std::string_view d;
    size_t p = 0;
    bool ok = true;

    explicit ByteReader(std::string_view data) : d(data) {}

    uint8_t u8() { return static_cast<uint8_t>(get(1)); }
    uint16_t u16() { return static_cast<uint16_t>(get(2)); }
    uint32_t u32() { return static_cast<uint32_t>(get(4)); }
    uint64_t u64() { return get(8); }
    std::string str(size_t maxLen) {
        uint32_t n = u32();
        if (!ok || n > maxLen || d.size() - p < n) {
            ok = false;
            return {};
        }
        std::string s(d.substr(p, n));
        p += n;
        return s;
    }
    // Checks the count against the bytes left, so a bogus count can't make us allocate a lot.
    uint32_t count(size_t minElemBytes, uint32_t maxCount) {
        uint32_t n = u32();
        if (!ok || n > maxCount || (minElemBytes && (d.size() - p) / minElemBytes < n)) {
            ok = false;
            return 0;
        }
        return n;
    }
    bool done() const { return ok && p == d.size(); }

private:
    uint64_t get(int n) {
        if (!ok || d.size() - p < static_cast<size_t>(n)) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(d[p + i])) << (8 * i);
        p += n;
        return v;
    }
};

inline uint64_t fnv1a(std::string_view s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace et
