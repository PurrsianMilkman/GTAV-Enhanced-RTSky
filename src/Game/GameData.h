// RTSky - game state sampled on the ScriptHookV script fiber (the only place natives may be called)
// and read by the render hooks through lock-free snapshots.
#pragma once

#include "../Common/Math.h"

#include <cstdint>

namespace rtsky::game {

struct CameraSample
{
    uint64_t serial = 0;      // increases with every script tick
    uint32_t gameFrame = 0;   // GET_FRAME_COUNT
    float3 position;          // GET_FINAL_RENDERED_CAM_COORD
    float3 rotationDeg;       // GET_FINAL_RENDERED_CAM_ROT(2): x pitch, y roll, z yaw
    float fovDeg = 50.0f;     // GET_FINAL_RENDERED_CAM_FOV (vertical)
    float nearClip = 0.15f;
    float farClip = 10000.0f;
    bool valid = false;
};

struct EnvironmentSample
{
    float hours = 12.0f;           // fractional clock hours [0, 24)
    uint32_t weatherFrom = 0;      // joaat hash
    uint32_t weatherTo = 0;
    float weatherBlend = 0.0f;     // 0 -> weatherFrom, 1 -> weatherTo
    float rainLevel = 0.0f;
    float snowLevel = 0.0f;
    bool interior = false;
    bool pauseMenu = false;
    bool cutscene = false;
    bool loading = false;
    bool valid = false;
};

class GameData
{
public:
    static constexpr uint32_t kRingSize = 16;

    // Script fiber only.
    void Tick();

    // Render side. latency 0 = newest sample, 1 = the one before, ...
    bool GetCamera(uint32_t latency, CameraSample* out) const;
    EnvironmentSample GetEnvironment() const;

private:
    // Seqlock-protected ring of camera samples
    struct Slot
    {
        volatile long sequence = 0; // odd while being written
        CameraSample sample;
    };
    Slot m_ring[kRingSize];
    volatile long long m_head = -1; // index of the newest slot (monotonic counter)

    volatile long m_envSequence = 0;
    EnvironmentSample m_env;
    uint64_t m_serial = 0;
};

GameData& Game();

} // namespace rtsky::game
