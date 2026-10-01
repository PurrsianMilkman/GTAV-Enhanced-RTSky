// RTSky - camera / TLAS-space calibration from the GPU probe (ProbeCS)
//
// The probe traces primary rays through the game's TLAS under 8 hypotheses (camera latency 0..3 x
// TLAS in world space or camera-relative) and counts how many agree with the depth buffer. This
// class turns those counts into a decision with hysteresis. Latency only becomes observable while
// the camera moves, so latency scores are updated only for frames flagged "informative".
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace rtsky::render {

class Calibration
{
public:
    static constexpr int kLatencies = 4;
    static constexpr int kSpaces = 2; // 0 = world, 1 = camera-relative

    // results[h] = matches, results[8+h] = valid rays, h = latency * 2 + space
    void Submit(const uint32_t* results, bool informative);

    int Latency() const;      // best latency (0..3)
    int TlasSpace() const;    // 0 world, 1 camera-relative
    float Confidence() const; // score of the chosen hypothesis [0,1]
    bool HasData() const;
    std::string Describe() const;

private:
    mutable SRWLOCK m_lock = SRWLOCK_INIT;
    float m_score[kLatencies][kSpaces] = {};
    int m_samples[kLatencies][kSpaces] = {};
    int m_latency = 1;
    int m_space = 0;
    int m_switchVotes = 0;
    uint64_t m_submissions = 0;
};

} // namespace rtsky::render
