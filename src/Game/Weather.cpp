// RTSky - weather -> sky parameters
//
// Starting values derived from weather.xml (per-type "Sun" / "Cloud" / "Rain" / "Snow" / "Fog"
// factors) and the noon light_dir_mult of each w_*.xml timecycle (RAIN and THUNDER have no direct
// sun at all). They only shape the sky model; the composite works with ratios, so moderate errors
// here change the angular weighting of the sky, not the overall brightness of the game.
#include "Weather.h"

#include "../Common/Math.h"

namespace rtsky::game {
namespace {

struct WeatherEntry
{
    uint32_t hash;
    WeatherParams params;
};

// hash = joaat(lower-case name)
constexpr WeatherEntry kWeathers[] = {
    { 0x97AA0A79u, { 0.00f, 0.60f, 1.00f, 0.0f } }, // EXTRASUNNY
    { 0x36A83D84u, { 0.10f, 0.60f, 0.90f, 0.0f } }, // CLEAR
    { 0xA4CA1326u, { 0.50f, 0.50f, 0.50f, 0.0f } }, // NEUTRAL
    { 0x10DCF4B5u, { 0.20f, 0.55f, 0.75f, 0.0f } }, // SMOG
    { 0xAE737644u, { 0.40f, 0.45f, 0.55f, 0.2f } }, // FOGGY
    { 0x30FDAF5Cu, { 0.45f, 0.55f, 0.65f, 0.0f } }, // CLOUDS
    { 0xBB898D2Du, { 0.80f, 0.45f, 0.30f, 0.0f } }, // OVERCAST
    { 0x6DB1A50Du, { 0.60f, 0.45f, 0.45f, 0.5f } }, // CLEARING
    { 0x54A69840u, { 0.95f, 0.30f, 0.05f, 1.0f } }, // RAIN
    { 0xB677829Fu, { 1.00f, 0.20f, 0.00f, 1.0f } }, // THUNDER
    { 0xEFB6EFF6u, { 0.85f, 0.40f, 0.20f, 0.3f } }, // SNOW
    { 0x23FB812Bu, { 0.35f, 0.55f, 0.70f, 0.1f } }, // SNOWLIGHT
    { 0x27EA2814u, { 1.00f, 0.25f, 0.05f, 0.3f } }, // BLIZZARD
    { 0xAAC9C895u, { 0.30f, 0.55f, 0.80f, 0.1f } }, // XMAS
    { 0xC91A3202u, { 0.90f, 0.30f, 0.10f, 0.8f } }, // HALLOWEEN
    { 0x16FDB812u, { 0.95f, 0.30f, 0.05f, 1.0f } }, // RAIN_HALLOWEEN
    { 0x7FCCC459u, { 0.70f, 0.40f, 0.40f, 0.3f } }, // SNOW_HALLOWEEN
};

constexpr WeatherParams kUnknown = { 0.30f, 0.50f, 0.70f, 0.0f };

WeatherParams Lookup(uint32_t hash)
{
    for (const WeatherEntry& e : kWeathers)
    {
        if (e.hash == hash)
            return e.params;
    }
    return kUnknown;
}

} // namespace

WeatherParams EvaluateWeather(uint32_t from, uint32_t to, float blend, float rainLevel, float snowLevel)
{
    const WeatherParams a = Lookup(from);
    const WeatherParams b = Lookup(to);
    const float t = Saturate(blend);
    WeatherParams r;
    r.cloudiness = Lerp(a.cloudiness, b.cloudiness, t);
    r.overcastTransmission = Lerp(a.overcastTransmission, b.overcastTransmission, t);
    r.directFactor = Lerp(a.directFactor, b.directFactor, t);
    r.wetness = Lerp(a.wetness, b.wetness, t);

    // Scripted rain / snow without a matching weather type still darkens the sky.
    const float precipitation = Saturate(std::max(rainLevel, snowLevel));
    r.cloudiness = std::max(r.cloudiness, 0.9f * precipitation);
    r.directFactor = std::min(r.directFactor, 1.0f - 0.95f * precipitation);
    return r;
}

} // namespace rtsky::game
