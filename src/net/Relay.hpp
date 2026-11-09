#pragma once

#include <string>
#include <vector>

namespace et::net {

struct RelayEvent {
    enum class Kind { Connected, Disconnected, Subscribed, SubscribeFailed, Message };
    Kind kind = Kind::Message;
    std::string channel; // Subscribed, SubscribeFailed, Message
    std::string event;   // Message: the client event name, e.g. "client-c"
    std::string data;    // Message: the event's data string; Disconnected / SubscribeFailed: the reason
};

// Publish/subscribe relay with private channels. Apinator in the mod, an in-memory fake in the tests.
// Called from the main thread; nothing here blocks.
class Relay {
public:
    virtual ~Relay() = default;
    // authFields: extra members (JSON, without braces) sent to the auth service to prove the right to subscribe.
    virtual void subscribe(std::string channel, std::string authFields) = 0;
    virtual void unsubscribe(std::string channel) = 0;
    // Client events are delivered to the other subscribers of the channel, never echoed back.
    virtual void publish(std::string channel, std::string event, std::string data) = 0;
    virtual void poll(std::vector<RelayEvent>& out) = 0;
    virtual bool connected() const = 0;
    virtual double pingMs() const { return -1; }
    virtual std::string status() const { return connected() ? "Connected" : "Connecting"; }
};

} // namespace et::net
