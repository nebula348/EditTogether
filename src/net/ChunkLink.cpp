#include "ChunkLink.hpp"

#include "core/Bytes.hpp"

#include <zlib.h>

namespace et::net {

std::string encodeChunk(Chunk const& c) {
    ByteWriter w;
    w.u32(c.epoch);
    w.u32(c.seq);
    w.u8(c.flags);
    w.buf.append(c.data);
    return std::move(w.buf);
}

std::optional<Chunk> decodeChunk(std::string_view bytes) {
    ByteReader r(bytes);
    Chunk c;
    c.epoch = r.u32();
    c.seq = r.u32();
    c.flags = r.u8();
    if (!r.ok || c.flags & ~(Chunk::End | Chunk::Compressed)) return std::nullopt;
    c.data.assign(bytes.substr(9));
    if (c.data.size() > ChunkLink::kChunkSize) return std::nullopt;
    return c;
}

std::string compressFrame(std::string_view data) {
    uLongf cap = compressBound(static_cast<uLong>(data.size()));
    std::string out(4 + cap, '\0');
    uint32_t n = static_cast<uint32_t>(data.size());
    for (int i = 0; i < 4; ++i) out[i] = static_cast<char>((n >> (8 * i)) & 255); // original size first
    if (compress2(reinterpret_cast<Bytef*>(out.data() + 4), &cap, reinterpret_cast<Bytef const*>(data.data()),
                  static_cast<uLong>(data.size()), 6) != Z_OK)
        return {};
    out.resize(4 + cap);
    return out;
}

std::optional<std::string> decompressFrame(std::string_view data, size_t maxSize) {
    if (data.size() < 4) return std::nullopt;
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n |= static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << (8 * i);
    if (n > maxSize) return std::nullopt; // decompression bomb / oversized frame
    std::string out(n, '\0');
    uLongf len = n;
    if (uncompress(reinterpret_cast<Bytef*>(out.data()), &len, reinterpret_cast<Bytef const*>(data.data() + 4),
                   static_cast<uLong>(data.size() - 4)) != Z_OK || len != n)
        return std::nullopt;
    return out;
}

ChunkLink::ChunkLink(uint32_t epoch, std::string label, SendFn send, ClockFn clock, double bytesPerSecond)
  : m_epoch(epoch), m_label(std::move(label)), m_sendFn(std::move(send)), m_clock(std::move(clock)), m_rate(bytesPerSecond) {
    m_lastRefill = m_clock();
    m_budget = m_rate; // allow an initial burst of one second's worth
}

void ChunkLink::fail(std::string why) {
    if (!m_dead) m_error = std::move(why);
    m_dead = true;
}

void ChunkLink::send(std::string_view payload) {
    if (m_dead) return;
    std::string body(payload);
    uint8_t compressed = 0;
    if (payload.size() >= 512) {
        auto z = compressFrame(payload);
        if (!z.empty() && z.size() < payload.size()) {
            body = std::move(z);
            compressed = Chunk::Compressed;
        }
    }
    size_t pos = 0;
    do {
        Chunk c;
        c.epoch = m_epoch;
        c.seq = m_sendSeq++;
        size_t n = std::min(kChunkSize, body.size() - pos);
        c.data = body.substr(pos, n);
        pos += n;
        c.flags = compressed | (pos >= body.size() ? Chunk::End : 0);
        m_queue.push_back(encodeChunk(c));
    } while (pos < body.size());
}

void ChunkLink::deliver(Chunk c) {
    if (m_dead || c.epoch != m_epoch) return;
    if (c.seq < m_recvSeq) return; // duplicate
    if (m_pending.size() > 4096) return fail("Too many out-of-order packets");
    m_bytesIn += c.data.size() + 9;
    m_pending.emplace(c.seq, std::move(c));
}

bool ChunkLink::update(std::vector<std::string>& frames) {
    if (m_dead) return false;
    double now = m_clock();

    // Receive: consume chunks in sequence order.
    for (auto it = m_pending.find(m_recvSeq); it != m_pending.end(); it = m_pending.find(m_recvSeq)) {
        Chunk& c = it->second;
        if (m_assembling.empty()) m_assemblingCompressed = c.flags & Chunk::Compressed;
        m_assembling.append(c.data);
        if (m_assembling.size() > m_maxFrame + 1024) { fail("Packet exceeds size limit"); return false; }
        bool end = c.flags & Chunk::End;
        m_pending.erase(it);
        ++m_recvSeq;
        if (!end) continue;
        if (m_assemblingCompressed) {
            auto plain = decompressFrame(m_assembling, m_maxFrame);
            if (!plain) { fail("Invalid compressed packet"); return false; }
            frames.push_back(std::move(*plain));
        } else {
            if (m_assembling.size() > m_maxFrame) { fail("Packet exceeds size limit"); return false; }
            frames.push_back(std::move(m_assembling));
        }
        m_assembling.clear();
    }
    if (!m_pending.empty()) {
        if (m_gapSince < 0) m_gapSince = now;
        else if (now - m_gapSince > kReorderTimeout) { fail("Lost data on the relay connection"); return false; }
    } else {
        m_gapSince = -1;
    }

    // Send within the bandwidth budget.
    m_budget = std::min(m_rate, m_budget + (now - m_lastRefill) * m_rate);
    m_lastRefill = now;
    while (m_queueHead < m_queue.size() && m_budget >= static_cast<double>(m_queue[m_queueHead].size())) {
        m_budget -= static_cast<double>(m_queue[m_queueHead].size());
        m_bytesOut += m_queue[m_queueHead].size();
        m_sendFn(std::move(m_queue[m_queueHead++]));
    }
    if (m_queueHead == m_queue.size()) {
        m_queue.clear();
        m_queueHead = 0;
    }
    return !m_dead;
}

} // namespace et::net
