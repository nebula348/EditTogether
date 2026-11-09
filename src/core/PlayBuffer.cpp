#include "PlayBuffer.hpp"

#include <algorithm>
#include <cmath>

namespace et {

namespace {
double senderSeconds(msg::PlayState const& s) {
    return static_cast<double>(s.time) / 1000.0;
}
} // namespace

float PlayBuffer::angleDelta(float a, float b) {
    return static_cast<float>(std::remainder(static_cast<double>(b) - a, 360.0));
}

bool PlayBuffer::discontinuous(msg::PlayState const& a, msg::PlayState const& b) {
    if (a.life != b.life) return true; // died and respawned in between
    double dt = std::max(0.0, senderSeconds(b) - senderSeconds(a));
    double dist = std::hypot(static_cast<double>(b.x) - a.x, static_cast<double>(b.y) - a.y);
    // GD's fastest real motion stays far below this; anything above is a teleport.
    return dist > 60.0 + 1500.0 * dt;
}

void PlayBuffer::push(msg::PlayState const& s, double localNow) {
    m_lastReceived = localNow;
    // Clock offset: the smallest (receive - send) seen recently is the one with the least network delay.
    m_offsets.push_back({localNow, localNow - senderSeconds(s)});
    while (!m_offsets.empty() && localNow - m_offsets.front().at > kOffsetWindow) m_offsets.pop_front();
    m_offset = m_offsets.front().value;
    for (auto const& o : m_offsets) m_offset = std::min(m_offset, o.value);

    if (!m_samples.empty() && s.time <= m_samples.back().time) return; // late or duplicate
    m_samples.push_back(s);
    while (m_samples.size() > 2 && senderSeconds(m_samples.back()) - senderSeconds(m_samples.front()) > kKeep)
        m_samples.pop_front();
}

std::optional<msg::PlayState> PlayBuffer::at(double localNow) const {
    if (m_samples.empty() || localNow - m_lastReceived > kStale) return std::nullopt;
    double render = localNow - m_offset - kDelay; // in sender time
    if (render <= senderSeconds(m_samples.front())) return m_samples.front();
    if (render >= senderSeconds(m_samples.back())) return m_samples.back(); // hold the last real state
    auto next = std::upper_bound(m_samples.begin(), m_samples.end(), render,
        [](double t, msg::PlayState const& s) { return t < senderSeconds(s); });
    auto const& b = *next;
    auto const& a = *(next - 1);
    if (discontinuous(a, b)) return a; // jump exactly when the sample says so
    double span = senderSeconds(b) - senderSeconds(a);
    float f = span > 0 ? static_cast<float>((render - senderSeconds(a)) / span) : 1.f;
    msg::PlayState out = a; // mode, icon, flags change discretely
    out.x = a.x + (b.x - a.x) * f;
    out.y = a.y + (b.y - a.y) * f;
    out.rotation = a.rotation + angleDelta(a.rotation, b.rotation) * f;
    return out;
}

} // namespace et
