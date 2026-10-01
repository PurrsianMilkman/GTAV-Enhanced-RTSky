// RTSky - game state sampled on the script fiber
#include "GameData.h"
#include "ScriptHookV.h"

#include <windows.h>

#include <atomic>

namespace rtsky::game {
namespace {

// Native hashes (identical in the legacy and gen9/Enhanced native databases)
constexpr uint64_t GET_FRAME_COUNT = 0xFC8202EFC642E6F2ull;
constexpr uint64_t GET_FINAL_RENDERED_CAM_COORD = 0xA200EB1EE790F448ull;
constexpr uint64_t GET_FINAL_RENDERED_CAM_ROT = 0x5B4E4C817FCC2DFBull;
constexpr uint64_t GET_FINAL_RENDERED_CAM_FOV = 0x80EC114669DAEFF4ull;
constexpr uint64_t GET_FINAL_RENDERED_CAM_NEAR_CLIP = 0xD0082607100D7193ull;
constexpr uint64_t GET_FINAL_RENDERED_CAM_FAR_CLIP = 0xDFC8CBC606FDB0FCull;
constexpr uint64_t GET_CLOCK_HOURS = 0x25223CA6B4D20B7Full;
constexpr uint64_t GET_CLOCK_MINUTES = 0x13D2B8ADD79640F2ull;
constexpr uint64_t GET_CLOCK_SECONDS = 0x494E97C2EF27C470ull;
constexpr uint64_t GET_CURR_WEATHER_STATE = 0xF3BBE884A14BB413ull;
constexpr uint64_t GET_RAIN_LEVEL = 0x96695E368AD855F3ull;
constexpr uint64_t GET_SNOW_LEVEL = 0xC5868A966E5BE3AEull;
constexpr uint64_t IS_INTERIOR_SCENE = 0xBC72B5D7A1CBD54Dull;
constexpr uint64_t IS_PAUSE_MENU_ACTIVE = 0xB0034A223497FFCBull;
constexpr uint64_t IS_CUTSCENE_ACTIVE = 0x991251AFC3981F84ull;
constexpr uint64_t GET_IS_LOADING_SCREEN_ACTIVE = 0x10D0A8F259E93EC9ull;

float3 ToFloat3(const natives::NativeVector3& v)
{
    return { v.x, v.y, v.z };
}

bool IsFinite(const float3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

void GameData::Tick()
{
    using namespace natives;

    CameraSample cam;
    cam.serial = ++m_serial;
    cam.gameFrame = static_cast<uint32_t>(Invoke<int32_t>(GET_FRAME_COUNT));
    cam.position = ToFloat3(Invoke<NativeVector3>(GET_FINAL_RENDERED_CAM_COORD));
    cam.rotationDeg = ToFloat3(Invoke<NativeVector3>(GET_FINAL_RENDERED_CAM_ROT, int32_t(2)));
    cam.fovDeg = Invoke<float>(GET_FINAL_RENDERED_CAM_FOV);
    cam.nearClip = Invoke<float>(GET_FINAL_RENDERED_CAM_NEAR_CLIP);
    cam.farClip = Invoke<float>(GET_FINAL_RENDERED_CAM_FAR_CLIP);
    cam.valid = IsFinite(cam.position) && IsFinite(cam.rotationDeg) && cam.fovDeg > 1.0f && cam.fovDeg < 170.0f &&
                cam.nearClip > 0.0f && cam.farClip > cam.nearClip;

    // Publish (seqlock: odd sequence while writing)
    const long long next = m_head + 1;
    Slot& slot = m_ring[next % kRingSize];
    InterlockedIncrement(&slot.sequence);
    std::atomic_thread_fence(std::memory_order_release);
    slot.sample = cam;
    std::atomic_thread_fence(std::memory_order_release);
    InterlockedIncrement(&slot.sequence);
    InterlockedExchange64(&m_head, next);

    EnvironmentSample env;
    const int h = Invoke<int32_t>(GET_CLOCK_HOURS);
    const int m = Invoke<int32_t>(GET_CLOCK_MINUTES);
    const int s = Invoke<int32_t>(GET_CLOCK_SECONDS);
    env.hours = static_cast<float>(h) + static_cast<float>(m) / 60.0f + static_cast<float>(s) / 3600.0f;
    uint32_t w1 = 0, w2 = 0;
    float pct = 0.0f;
    Invoke<void>(GET_CURR_WEATHER_STATE, &w1, &w2, &pct);
    env.weatherFrom = w1;
    env.weatherTo = w2;
    env.weatherBlend = Saturate(pct);
    env.rainLevel = Invoke<float>(GET_RAIN_LEVEL);
    env.snowLevel = Invoke<float>(GET_SNOW_LEVEL);
    env.interior = Invoke<int32_t>(IS_INTERIOR_SCENE) != 0;
    env.pauseMenu = Invoke<int32_t>(IS_PAUSE_MENU_ACTIVE) != 0;
    env.cutscene = Invoke<int32_t>(IS_CUTSCENE_ACTIVE) != 0;
    env.loading = Invoke<int32_t>(GET_IS_LOADING_SCREEN_ACTIVE) != 0;
    env.valid = true;

    InterlockedIncrement(&m_envSequence);
    std::atomic_thread_fence(std::memory_order_release);
    m_env = env;
    std::atomic_thread_fence(std::memory_order_release);
    InterlockedIncrement(&m_envSequence);
}

bool GameData::GetCamera(uint32_t latency, CameraSample* out) const
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const long long head = InterlockedCompareExchange64(const_cast<volatile long long*>(&m_head), 0, 0);
        if (head < static_cast<long long>(latency))
            return false;
        if (latency >= kRingSize - 2)
            return false;
        const Slot& slot = m_ring[(head - latency) % kRingSize];
        const long seq1 = InterlockedCompareExchange(const_cast<volatile long*>(&slot.sequence), 0, 0);
        if (seq1 & 1)
            continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        CameraSample copy = slot.sample;
        std::atomic_thread_fence(std::memory_order_acquire);
        const long seq2 = InterlockedCompareExchange(const_cast<volatile long*>(&slot.sequence), 0, 0);
        if (seq1 != seq2)
            continue;
        *out = copy;
        return copy.valid;
    }
    return false;
}

EnvironmentSample GameData::GetEnvironment() const
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const long seq1 = InterlockedCompareExchange(const_cast<volatile long*>(&m_envSequence), 0, 0);
        if (seq1 & 1)
            continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        EnvironmentSample copy = m_env;
        std::atomic_thread_fence(std::memory_order_acquire);
        const long seq2 = InterlockedCompareExchange(const_cast<volatile long*>(&m_envSequence), 0, 0);
        if (seq1 == seq2)
            return copy;
    }
    return EnvironmentSample{};
}

GameData& Game()
{
    static GameData instance;
    return instance;
}

} // namespace rtsky::game
