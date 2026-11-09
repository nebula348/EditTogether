#pragma once

#include "Link.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>

namespace et::net {

// One piece of a frame sent through a message relay.
struct Chunk {
    enum Flags : uint8_t { End = 1, Compressed = 2 };
    uint32_t epoch = 0; // identifies the logical connection (picked by the side that opened it)
    uint32_t seq = 0;   // per direction, starts at 0
    uint8_t flags = 0;  // End: last chunk of a frame; Compressed: the frame is zlib-compressed
    std::string data;
};
std::string encodeChunk(Chunk const& c);
std::optional<Chunk> decodeChunk(std::string_view bytes);

// zlib helpers (frame payloads). decompress refuses outputs above maxSize.
std::string compressFrame(std::string_view data);
std::optional<std::string> decompressFrame(std::string_view data, size_t maxSize);

// Link over a message relay: frames are compressed, split into chunks and numbered. Sending is rate limited.
class ChunkLink final : public Link {
public:
    static constexpr size_t kChunkSize = 8000;
    static constexpr double kReorderTimeout = 20.0; // a missing chunk for this long = broken link

    using SendFn = std::function<void(std::string bytes)>;
    using ClockFn = std::function<double()>;

    ChunkLink(uint32_t epoch, std::string label, SendFn send, ClockFn clock, double bytesPerSecond);

    // Called by the router for each chunk addressed to this link.
    void deliver(Chunk c);
    void fail(std::string why);

    bool update(std::vector<std::string>& frames) override;
    void send(std::string_view payload) override;
    void setMaxFrame(uint32_t n) override { m_maxFrame = n; }
    bool outEmpty() const override { return m_queue.empty(); }
    std::string const& error() const override { return m_error; }
    std::string describe() const override { return m_label; }
    uint64_t bytesIn() const override { return m_bytesIn; }
    uint64_t bytesOut() const override { return m_bytesOut; }
    bool viaRelay() const override { return true; }
    uint32_t epoch() const { return m_epoch; }

private:
    uint32_t m_epoch;
    std::string m_label;
    SendFn m_sendFn;
    ClockFn m_clock;
    double m_rate;
    double m_budget = 0;
    double m_lastRefill = 0;
    uint32_t m_sendSeq = 0;
    uint32_t m_recvSeq = 0;
    std::vector<std::string> m_queue; // encoded chunks waiting for budget
    size_t m_queueHead = 0;
    std::map<uint32_t, Chunk> m_pending; // out-of-order chunks
    double m_gapSince = -1;
    std::string m_assembling;
    bool m_assemblingCompressed = false;
    uint32_t m_maxFrame = 4096;
    bool m_dead = false;
    std::string m_error;
    uint64_t m_bytesIn = 0, m_bytesOut = 0;
};

} // namespace et::net
