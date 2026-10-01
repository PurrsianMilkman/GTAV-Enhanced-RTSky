// RTSky - configuration loading (minimal INI parser, no dependencies)
#include "Config.h"
#include "Log.h"
#include "Math.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace rtsky {
namespace {

SRWLOCK g_configLock = SRWLOCK_INIT;
Config g_config;
std::wstring g_configPath;

std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

using IniMap = std::map<std::string, std::string>; // "section.key" (lower case) -> value

bool ParseIni(const std::wstring& path, IniMap& out)
{
    FILE* f = _wfopen(path.c_str(), L"r");
    if (f == nullptr)
        return false;

    std::string section;
    char line[1024];
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        std::string s = Trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#')
            continue;
        if (s.front() == '[')
        {
            size_t close = s.find(']');
            section = Lower(Trim(s.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = Lower(Trim(s.substr(0, eq)));
        std::string value = s.substr(eq + 1);
        // Strip trailing comments
        size_t comment = value.find_first_of(";#");
        if (comment != std::string::npos)
            value = value.substr(0, comment);
        out[section + "." + key] = Trim(value);
    }
    fclose(f);
    return true;
}

class Reader
{
public:
    explicit Reader(const IniMap& map) : m_map(map) {}

    void Get(const char* key, bool& v) const
    {
        if (const std::string* s = Find(key))
        {
            std::string l = Lower(*s);
            v = (l == "1" || l == "true" || l == "yes" || l == "on");
        }
    }
    void Get(const char* key, int& v) const
    {
        if (const std::string* s = Find(key))
            v = static_cast<int>(std::strtol(s->c_str(), nullptr, 0));
    }
    void Get(const char* key, uint32_t& v) const
    {
        if (const std::string* s = Find(key))
            v = static_cast<uint32_t>(std::strtoul(s->c_str(), nullptr, 0));
    }
    void Get(const char* key, float& v) const
    {
        if (const std::string* s = Find(key))
            v = std::strtof(s->c_str(), nullptr);
    }
    // "auto" -> autoValue, otherwise integer
    void GetAutoInt(const char* key, int& v, int autoValue) const
    {
        if (const std::string* s = Find(key))
            v = Lower(*s) == "auto" ? autoValue : static_cast<int>(std::strtol(s->c_str(), nullptr, 0));
    }
    const std::string* Find(const char* key) const
    {
        auto it = m_map.find(key);
        return it == m_map.end() ? nullptr : &it->second;
    }

private:
    const IniMap& m_map;
};

} // namespace

bool Config::Load(const std::wstring& path)
{
    IniMap ini;
    if (!ParseIni(path, ini))
        return false;
    Reader r(ini);

    r.Get("general.enabled", enabled);
    r.Get("general.loglevel", logLevel);

    r.Get("hotkeys.toggle", keyToggle);
    r.Get("hotkeys.debugview", keyDebugView);
    r.Get("hotkeys.reload", keyReload);
    r.Get("hotkeys.dumpframe", keyDumpFrame);

    if (const std::string* s = r.Find("trace.path"))
        tracePath = Lower(*s) == "inline" ? TracePath::Inline : TracePath::Pipeline;
    r.Get("trace.raysperpixel", raysPerPixel);
    r.Get("trace.maxraydistance", maxRayDistance);
    r.Get("trace.normalbias", normalBias);
    r.Get("trace.distancebias", distanceBias);
    r.Get("trace.tmin", tMin);
    r.Get("trace.nearfieldradius", nearFieldRadius);
    r.Get("trace.instancemask", instanceMask);
    if (const std::string* s = r.Find("trace.foliagemode"))
    {
        std::string l = Lower(*s);
        foliageMode = l == "opaque" ? FoliageMode::Opaque : l == "ignore" ? FoliageMode::Ignore : FoliageMode::Stochastic;
    }
    r.Get("trace.foliageopacity", foliageOpacity);
    r.Get("trace.foliagecells", foliageCells);
    r.Get("trace.sunshadowrays", sunShadowRays);
    r.Get("trace.sunsoftness", sunSoftness);

    r.Get("denoise.maxhistory", maxHistory);
    r.Get("denoise.depthreject", depthReject);
    r.Get("denoise.normalreject", normalReject);
    r.Get("denoise.iterations", denoiseIterations);
    r.Get("denoise.sigmaplane", sigmaPlane);
    r.Get("denoise.normalpower", normalPower);
    r.Get("denoise.sigmaluminance", sigmaLuminance);

    r.Get("composite.strength", strength);
    r.Get("composite.minratio", minRatio);
    r.Get("composite.maxratio", maxRatio);
    r.Get("composite.gameskyocclusion", gameSkyOcclusion);
    r.Get("composite.directscale", directScale);
    r.Get("composite.artificialambient", artificialAmbient);
    r.Get("composite.groundalbedo", groundAlbedo);
    r.Get("composite.nearfadedistance", nearFadeDistance);
    r.Get("composite.fadestart", fadeStart);
    r.Get("composite.fadeend", fadeEnd);
    r.Get("composite.interiorstrength", interiorStrength);
    r.Get("composite.cutscenestrength", cutsceneStrength);
    r.Get("composite.debugview", debugView);

    r.GetAutoInt("camera.latency", cameraLatency, -1);
    if (const std::string* s = r.Find("camera.tlasspace"))
    {
        std::string l = Lower(*s);
        tlasSpace = l == "world" ? TlasSpaceSetting::World : l == "camera" ? TlasSpaceSetting::CameraRelative : TlasSpaceSetting::Auto;
    }
    if (const std::string* s = r.Find("camera.depthmode"))
    {
        std::string l = Lower(*s);
        depthMode = l == "reversed" ? DepthModeSetting::ReversedFinite
                  : l == "reversed_infinite" ? DepthModeSetting::ReversedInfinite
                  : l == "standard" ? DepthModeSetting::Standard
                  : DepthModeSetting::Auto;
    }
    r.Get("camera.fovscale", fovScale);
    r.Get("camera.calibrationprobe", calibrationProbe);
    r.Get("camera.mincalibrationscore", minCalibrationScore);

    r.GetAutoInt("detection.gbufferordinal", gbufferOrdinal, -1);
    r.Get("detection.compositecandidate", compositeCandidate);
    r.GetAutoInt("detection.compositeordinal", compositeOrdinal, -1);
    r.GetAutoInt("detection.tlasselect", tlasSelect, -1);
    r.Get("detection.tlasclone", tlasClone);
    r.Get("detection.stableframes", stableFrames);

    r.Get("sky.sunintensity", sunIntensity);
    r.Get("sky.moonintensity", moonIntensity);
    r.Get("sky.miescale", mieScale);
    r.Get("sky.groundalbedo", atmosphereGroundAlbedo);
    r.Get("sky.sunroll", sunRollDeg);
    r.Get("sky.daystarthour", dayStartHour);
    r.Get("sky.daylengthhours", dayLengthHours);
    r.Get("sky.sunazimuthoffset", sunAzimuthOffsetDeg);
    r.Get("sky.moonroll", moonRollDeg);

    // Sanitise
    raysPerPixel = Clamp(raysPerPixel, 1, 16);
    denoiseIterations = Clamp(denoiseIterations, 0, 5);
    cameraLatency = Clamp(cameraLatency, -1, 3);
    foliageOpacity = Saturate(foliageOpacity);
    foliageCells = Clamp(foliageCells, 1.0f, 64.0f);
    maxHistory = Clamp(maxHistory, 1.0f, 255.0f);
    minRatio = Clamp(minRatio, 0.0f, 1.0f);
    maxRatio = std::max(maxRatio, minRatio);
    stableFrames = Clamp(stableFrames, 1, 60);
    instanceMask &= 0xFFu;
    debugView = Clamp(debugView, 0, 8);
    dayLengthHours = Clamp(dayLengthHours, 1.0f, 23.0f);
    return true;
}

Config ConfigSnapshot()
{
    AcquireSRWLockShared(&g_configLock);
    Config copy = g_config;
    ReleaseSRWLockShared(&g_configLock);
    return copy;
}

void SetConfigPath(const std::wstring& path)
{
    AcquireSRWLockExclusive(&g_configLock);
    g_configPath = path;
    ReleaseSRWLockExclusive(&g_configLock);
}

bool ReloadConfig()
{
    AcquireSRWLockShared(&g_configLock);
    std::wstring path = g_configPath;
    ReleaseSRWLockShared(&g_configLock);

    Config fresh;
    bool ok = fresh.Load(path);
    if (!ok)
        LOG_WARN("RTSky.ini not found or unreadable, using defaults");

    AcquireSRWLockExclusive(&g_configLock);
    g_config = fresh;
    ReleaseSRWLockExclusive(&g_configLock);

    log::SetLevel(static_cast<log::Level>(Clamp(fresh.logLevel, 0, 3)));
    return ok;
}

void SetConfigDebugView(int view)
{
    AcquireSRWLockExclusive(&g_configLock);
    g_config.debugView = view;
    ReleaseSRWLockExclusive(&g_configLock);
}

void SetConfigEnabled(bool enabled)
{
    AcquireSRWLockExclusive(&g_configLock);
    g_config.enabled = enabled;
    ReleaseSRWLockExclusive(&g_configLock);
}

} // namespace rtsky
