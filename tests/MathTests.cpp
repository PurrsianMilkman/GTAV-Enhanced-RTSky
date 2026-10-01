// RTSky - host-side tests for the platform independent math (camera basis, sun orbit, weather).
// Build natively, e.g.:  g++ -std=c++20 -I src tests/MathTests.cpp src/Game/SunModel.cpp src/Game/Weather.cpp
#include "Common/Math.h"
#include "Game/SunModel.h"
#include "Game/Weather.h"

#include <cmath>
#include <cstdio>

using namespace rtsky;

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "  ok  " : "FAILED", what);
    if (!ok)
        ++g_failures;
}

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool Near(const float3& a, const float3& b, float eps = 1e-3f) { return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps); }

static float ElevationDeg(const float3& d) { return std::asin(d.z) / kDegToRad; }

int main()
{
    std::printf("Camera basis (GET_FINAL_RENDERED_CAM_ROT order 2)\n");
    {
        CameraBasis b = CameraBasisFromRotation({ 0, 0, 0 });
        Check(Near(b.forward, { 0, 1, 0 }) && Near(b.right, { 1, 0, 0 }) && Near(b.up, { 0, 0, 1 }), "identity looks north (+Y), right is east, up is +Z");
        b = CameraBasisFromRotation({ 0, 0, 90 });
        Check(Near(b.forward, { -1, 0, 0 }), "heading 90 looks west");
        b = CameraBasisFromRotation({ 30, 0, 0 });
        Check(Near(b.forward, { 0, std::cos(30 * kDegToRad), std::sin(30 * kDegToRad) }), "positive pitch looks up");
        b = CameraBasisFromRotation({ 17, 11, -63 });
        Check(Near(Dot(b.forward, b.right), 0) && Near(Dot(b.forward, b.up), 0) && Near(Dot(b.right, b.up), 0), "basis is orthogonal");
        Check(Near(Cross(b.right, b.forward), b.up), "basis is right-handed (right x forward = up)");
        // Modders' forward formula: (-sin(yaw)|cos(pitch)|, cos(yaw)|cos(pitch)|, sin(pitch))
        const float p = 17 * kDegToRad, y = -63 * kDegToRad;
        Check(Near(b.forward, { -std::sin(y) * std::cos(p), std::cos(y) * std::cos(p), std::sin(p) }), "matches the ScriptHookV forward vector");
    }

    std::printf("Sun orbit (time.xml / CodeWalker)\n");
    {
        game::SunModelParams params;
        auto sun = [&](float h) { return game::ComputeCelestialDirections(h, params).sun; };
        Check(Near(sun(6.0f), { 1, 0, 0 }, 1e-3f), "sunrise due east at 06:00");
        Check(Near(sun(20.0f), { -1, 0, 0 }, 1e-3f), "sunset due west at 20:00");
        const float3 noon = sun(13.0f);
        Check(Near(noon.x, 0.0f) && noon.y < 0.0f && Near(ElevationDeg(noon), 58.0f, 0.1f), "13:00 due south at 58 deg elevation");
        Check(sun(12.0f).z > 0.8f && sun(2.0f).z < 0.0f, "up at noon, below the horizon at 02:00");
        float maxElevation = -90.0f;
        for (float h = 0.0f; h < 24.0f; h += 0.25f)
            maxElevation = std::fmax(maxElevation, ElevationDeg(sun(h)));
        Check(Near(maxElevation, 58.0f, 0.2f), "highest elevation is 58 deg");
        const float3 moon = game::ComputeCelestialDirections(1.0f, params).moon;
        Check(moon.z > 0.5f, "moon is up at 01:00");
        Check(Near(Length(sun(9.37f)), 1.0f), "directions are unit length");
    }

    std::printf("Weather\n");
    {
        const uint32_t extrasunny = 0x97AA0A79u, thunder = 0xB677829Fu, rain = 0x54A69840u;
        game::WeatherParams a = game::EvaluateWeather(extrasunny, extrasunny, 0.0f, 0.0f, 0.0f);
        Check(Near(a.cloudiness, 0.0f) && Near(a.directFactor, 1.0f), "EXTRASUNNY is clear with full sun");
        game::WeatherParams b = game::EvaluateWeather(thunder, thunder, 0.0f, 0.0f, 0.0f);
        Check(Near(b.cloudiness, 1.0f) && Near(b.directFactor, 0.0f), "THUNDER is overcast without direct sun");
        game::WeatherParams c = game::EvaluateWeather(extrasunny, rain, 0.5f, 0.0f, 0.0f);
        Check(Near(c.cloudiness, 0.475f), "transition blends the two weathers");
        game::WeatherParams d = game::EvaluateWeather(extrasunny, extrasunny, 0.0f, 1.0f, 0.0f);
        Check(d.cloudiness >= 0.9f - 1e-4f && d.directFactor <= 0.05f + 1e-4f, "scripted rain darkens a clear weather");
    }

    std::printf(g_failures == 0 ? "\nall tests passed\n" : "\n%d test(s) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
