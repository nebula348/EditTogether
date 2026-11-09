#pragma once

#include "Tls.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace et::net {

struct Url {
    bool tls = true;
    std::string host;
    uint16_t port = 443;
    std::string base; // path prefix without trailing slash
};
std::optional<Url> parseUrl(std::string const& text); // http:// or https://

struct HttpResponse {
    int status = 0; // 0 = transport failure, see error
    std::string body;
    std::string error;
};

// Small HTTP/1.1 client for the auth service. Keeps one connection open and blocks, so use it from the network thread.
class HttpClient {
public:
    explicit HttpClient(Url url) : m_url(std::move(url)) {}
    HttpResponse get(std::string const& path) { return request("GET", path, {}); }
    HttpResponse post(std::string const& path, std::string const& json) { return request("POST", path, json); }
    void close() { m_stream.close(); }

    static constexpr double kTimeout = 10.0;

private:
    HttpResponse request(char const* method, std::string const& path, std::string const& body);
    HttpResponse attempt(char const* method, std::string const& path, std::string const& body, bool& reused);

    Url m_url;
    Stream m_stream;
};

// Decodes a complete chunked body. Returns false if it is malformed or cut short.
bool decodeChunked(std::string_view in, std::string& out, size_t* consumed = nullptr);

} // namespace et::net
