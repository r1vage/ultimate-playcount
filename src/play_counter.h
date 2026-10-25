#pragma once
// Counts how long the current track was actually listened to. No foobar2000 dependency.
// Feed it playback time ticks and seeks; it never sees tag edits or info refreshes, which is the point:
// those must not restart the count.

#include <algorithm>
#include <cmath>
#include <limits>

namespace upc {

// When a play counts: after a percentage of the track, after some seconds of listening, or on reaching the end.
enum class count_mode { percent = 0, seconds = 1, track_end = 2 };

// Seconds of listening a track needs; infinity for track_end (counted on reaching the end instead).
// Tracks without a known length (streams) need 60 s in every mode.
inline double threshold_for(count_mode mode, double value, double length) {
    if (length <= 0) return 60;
    switch (mode) {
    case count_mode::percent: return length * std::clamp(value, 1.0, 100.0) / 100;
    case count_mode::seconds: return (std::min)(value > 0 ? value : 60, length); // whole track if shorter
    default: return (std::numeric_limits<double>::infinity)();
    }
}

class play_counter {
public:
    // Ticks normally arrive once per second. Used for the first tick (no clock reference yet), the end-of-track
    // credit and re-announce detection.
    static constexpr double max_step = 3.0;
    // Tick timing jitter allowed when capping a step by the real time that passed.
    static constexpr double clock_slack = 0.5;

    // `clock`: the steady clock now, if known (else the first tick is capped at max_step).
    void start(double threshold, double length, double position = 0, double clock = -1) {
        m_active = true;
        m_logged = false;
        m_threshold = threshold;
        m_length = length;
        m_pos = position;
        m_listened = 0;
        m_clock = clock;
        m_last_step = 0;
    }

    void stop() { m_active = false; }

    // `clock`: a steady clock in seconds. Returns true once, on the tick that reaches the threshold.
    // Forward steps count as listening, capped by the real time since the previous tick: when foobar2000 is
    // busy for a while (tags being written, a slow panel) its time updates bunch up and the next one jumps
    // several seconds, which is still music heard. Seeks are reported separately (on_seek), so they never count.
    bool on_time(double position, double clock) {
        if (!m_active) return false;
        const double step = position - m_pos;
        const double cap = m_clock < 0 ? max_step : clock - m_clock + clock_slack;
        m_pos = position;
        m_clock = clock;
        m_last_step = step;
        if (step > 0) m_listened += (std::min)(step, cap);
        return check();
    }

    // Position change of the last tick: more than max_step means foobar2000 was busy (or a seek went unreported).
    double last_step() const { return m_last_step; }

    void on_seek(double position) { m_pos = position; }

    // A "new track" for the same file that picks up where we are (not near the start) is the core
    // re-announcing the playing track after a refresh, not a replay: keep counting.
    bool continues_at(double position) const {
        return m_active && position > max_step && position - m_pos >= -max_step && position - m_pos <= max_step;
    }

    // The two decisions the playback hook makes, kept here so they're tested:
    // a "new track" for the same file that continues where we are is a re-announce: keep counting.
    bool reannounced(bool same_file, double position) const { return same_file && continues_at(position); }
    // A stop that starts another track is left to the new_track that follows (it may be a re-announce).
    static bool stop_ends_track(bool starting_another) { return !starting_another; }

    // Call when the track ends (EOF or switching tracks). If playback got to the end, credits the last
    // partial second, which the final tick misses. Returns true if that logs the play: the threshold is
    // reached, or (track_end mode, infinite threshold) playback reached the end.
    bool finish() {
        if (!m_active) return false;
        const bool at_end = m_length > 0 && m_length - m_pos <= max_step;
        if (at_end && m_pos < m_length) {
            m_listened += m_length - m_pos;
            m_pos = m_length;
        }
        if (at_end && std::isinf(m_threshold) && !m_logged) return m_logged = true;
        return check();
    }

    // After finish(): the track was left before its end without counting as played, after at least a second of
    // listening. A track that played to its end is no skip, even if seeking made it miss the threshold.
    static constexpr double min_skip_seconds = 1.0;
    bool skipped() const {
        if (!m_active || m_logged) return false;
        const bool at_end = m_length > 0 && m_length - m_pos <= max_step;
        return !at_end && m_listened >= min_skip_seconds;
    }

    bool active() const { return m_active; }
    bool logged() const { return m_logged; }
    double listened() const { return m_listened; }
    double threshold() const { return m_threshold; }

private:
    bool check() {
        // Epsilon: a sum of fractional ticks may land a hair below an equal threshold.
        if (m_logged || m_listened + 1e-6 < m_threshold) return false;
        m_logged = true;
        return true;
    }

    bool m_active = false;
    bool m_logged = false;
    double m_threshold = 0;
    double m_length = 0;
    double m_pos = 0;
    double m_listened = 0;
    double m_clock = -1; // steady clock at the previous tick; -1 = none yet
    double m_last_step = 0;
};

} // namespace upc
