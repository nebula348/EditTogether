#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace et::net::json {

// Minimal JSON reader for flat objects. Nested values are skipped and kept as raw text.
std::string escape(std::string_view s);

struct Value {
    bool isString = false;
    std::string text; // unescaped for strings, raw text otherwise (numbers, true/false/null, {...}, [...])
};
using Object = std::map<std::string, Value, std::less<>>;

// Returns nullopt for anything that is not a well formed JSON object.
std::optional<Object> parseObject(std::string_view s);
// Value of `key` when it is a string, else empty.
std::string str(Object const& o, std::string_view key);
double num(Object const& o, std::string_view key, double fallback = 0);

} // namespace et::net::json
