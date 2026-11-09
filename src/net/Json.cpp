#include "Json.hpp"

#include <cstdlib>

namespace et::net::json {

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static char const* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

namespace {

struct Parser {
    std::string_view s;
    size_t p = 0;
    int depth = 0;

    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    }
    bool eat(char c) {
        ws();
        if (p < s.size() && s[p] == c) { ++p; return true; }
        return false;
    }

    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 63));
            out += static_cast<char>(0x80 | (cp & 63));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 63));
            out += static_cast<char>(0x80 | ((cp >> 6) & 63));
            out += static_cast<char>(0x80 | (cp & 63));
        }
    }

    bool hex4(uint32_t& v) {
        if (p + 4 > s.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (!eat('"')) return false;
        while (p < s.size()) {
            char c = s[p++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (p >= s.size()) return false;
            switch (char e = s[p++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) { // surrogate pair
                        uint32_t lo;
                        if (p + 2 > s.size() || s[p] != '\\' || s[p + 1] != 'u') return false;
                        p += 2;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: (void)e; return false;
            }
        }
        return false;
    }

    // Skips one value, returning its raw text.
    bool skip(std::string& raw) {
        ws();
        size_t start = p;
        if (p >= s.size() || ++depth > 16) return false;
        bool ok = false;
        char c = s[p];
        if (c == '"') {
            std::string tmp;
            ok = string(tmp);
        } else if (c == '{' || c == '[') {
            char close = c == '{' ? '}' : ']';
            ++p;
            ok = true;
            if (!eat(close)) {
                for (;;) {
                    std::string tmp;
                    if (c == '{') {
                        if (!string(tmp) || !eat(':')) { ok = false; break; }
                    }
                    if (!skip(tmp)) { ok = false; break; }
                    if (eat(',')) continue;
                    ok = eat(close);
                    break;
                }
            }
        } else {
            while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']' && s[p] != ' ' && s[p] != '\n' &&
                   s[p] != '\r' && s[p] != '\t')
                ++p;
            ok = p > start;
        }
        --depth;
        if (ok) raw.assign(s.substr(start, p - start));
        return ok;
    }
};

} // namespace

std::optional<Object> parseObject(std::string_view s) {
    Parser ps{s};
    if (!ps.eat('{')) return std::nullopt;
    Object out;
    if (ps.eat('}')) {
        ps.ws();
        return ps.p == s.size() ? std::optional<Object>(out) : std::nullopt;
    }
    for (;;) {
        std::string key;
        if (!ps.string(key) || !ps.eat(':')) return std::nullopt;
        ps.ws();
        Value v;
        if (ps.p < s.size() && s[ps.p] == '"') {
            v.isString = true;
            if (!ps.string(v.text)) return std::nullopt;
        } else if (!ps.skip(v.text)) {
            return std::nullopt;
        }
        out[std::move(key)] = std::move(v);
        if (ps.eat(',')) continue;
        if (!ps.eat('}')) return std::nullopt;
        break;
    }
    ps.ws();
    if (ps.p != s.size()) return std::nullopt;
    return out;
}

std::string str(Object const& o, std::string_view key) {
    auto it = o.find(key);
    return it != o.end() && it->second.isString ? it->second.text : std::string();
}

double num(Object const& o, std::string_view key, double fallback) {
    auto it = o.find(key);
    if (it == o.end() || it->second.isString) return fallback;
    char* end = nullptr;
    double v = std::strtod(it->second.text.c_str(), &end);
    return end == it->second.text.c_str() ? fallback : v;
}

} // namespace et::net::json
