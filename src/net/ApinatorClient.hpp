#pragma once

#include "Relay.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace et::net {

// Apinator client: WebSocket over TLS on its own thread. Channel signatures come from the auth service in
// server/, which holds the app secret. It reconnects and resubscribes on its own.
class ApinatorClient final : public Relay {
public:
    struct Config {
        std::string authUrl; // base URL of the relay auth service (server/ in this repository)
    };
    explicit ApinatorClient(Config cfg);
    ~ApinatorClient() override;

    void subscribe(std::string channel, std::string authFields) override;
    void unsubscribe(std::string channel) override;
    void publish(std::string channel, std::string event, std::string data) override;
    void poll(std::vector<RelayEvent>& out) override;
    bool connected() const override;
    double pingMs() const override;
    std::string status() const override;

    // For tests: cuts the connection like a network failure would.
    void simulateDisconnect();
    uint64_t messagesIn() const;
    uint64_t messagesOut() const;

    struct Shared; // network thread state

private:
    std::shared_ptr<Shared> m_s;
};

// Asks the auth service for a new room. Blocks, so don't call it from the main thread.
struct RoomGrant {
    std::string code;
    std::string hostToken;
    std::string error;
};
RoomGrant requestRoom(std::string const& authUrl);

} // namespace et::net
