#pragma once

#include "Protocol.hpp"

#include <deque>
#include <optional>

namespace et {

// Jitter buffer for one remote player's position stream.
// Samples carry the sender's clock; we draw kDelay behind the newest estimate of "now" on the sender and
// interpolate linearly between the two real samples around that instant. No smoothing, no extrapolation:
// between samples the path is a straight line, at samples it is exactly what GD reported.
class PlayBuffer {
public:
    static constexpr double kDelay = 0.100;      // seconds behind real time; covers ~3 samples at 30 Hz
    static constexpr double kKeep = 2.0;         // seconds of history kept
    static constexpr double kStale = 5.0;        // no data for this long: stop drawing the player
    static constexpr double kOffsetWindow = 2.0; // clock offset = minimum seen over this window

    // localNow: receiver's monotonic clock, seconds.
    void push(msg::PlayState const& s, double localNow);
    // The state to draw at localNow, or nullopt if nothing (recent) to draw.
    std::optional<msg::PlayState> at(double localNow) const;

    // True if moving from a to b is a jump (respawn, teleport portal), not continuous motion.
    static bool discontinuous(msg::PlayState const& a, msg::PlayState const& b);
    // Shortest signed difference b - a in degrees, in [-180, 180], for any input range.
    static float angleDelta(float a, float b);

private:
    struct Offset {
        double at;
        double value;
    };
    std::deque<msg::PlayState> m_samples; // ascending sender time
    std::deque<Offset> m_offsets;
    double m_offset = 0;
    double m_lastReceived = -1e9;
};

} // namespace et
