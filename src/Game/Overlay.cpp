// RTSky - on-screen text through the game's UI natives
#include "Overlay.h"

#include "ScriptHookV.h"

#include <windows.h>

#include <algorithm>

namespace rtsky::game::overlay {
namespace {

using natives::Invoke;

constexpr uint64_t SET_TEXT_FONT = 0x66E0276CC5F6B9DAull;
constexpr uint64_t SET_TEXT_SCALE = 0x07C837F9A01C34C9ull;
constexpr uint64_t SET_TEXT_COLOUR = 0xBE6B23FFA53FB442ull;
constexpr uint64_t SET_TEXT_OUTLINE = 0x2513DFB0FB8400FEull;
constexpr uint64_t BEGIN_TEXT_COMMAND_DISPLAY_TEXT = 0x25FBB336DF1804CBull;
constexpr uint64_t ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME = 0x6C188BE134E074AAull;
constexpr uint64_t END_TEXT_COMMAND_DISPLAY_TEXT = 0xCD015E5BB0D96A57ull;
constexpr uint64_t DRAW_RECT = 0x3A618A217E5154F0ull;

constexpr float kLeft = 0.006f;
constexpr float kTop = 0.006f;
constexpr float kLineHeight = 0.021f;
constexpr float kScale = 0.28f;
constexpr ULONGLONG kToastMs = 2500;

SRWLOCK g_toastLock = SRWLOCK_INIT;
std::string* g_toast = new std::string(); // never destroyed (see Renderer singletons)
ULONGLONG g_toastUntil = 0;

void Text(float x, float y, const std::string& text, int r, int g, int b)
{
    // A single text component holds at most 99 characters.
    const std::string clipped = text.size() > 99 ? text.substr(0, 99) : text;
    Invoke<void>(SET_TEXT_FONT, int32_t(0));
    Invoke<void>(SET_TEXT_SCALE, 0.0f, kScale);
    Invoke<void>(SET_TEXT_COLOUR, int32_t(r), int32_t(g), int32_t(b), int32_t(255));
    Invoke<void>(SET_TEXT_OUTLINE);
    Invoke<void>(BEGIN_TEXT_COMMAND_DISPLAY_TEXT, "STRING");
    Invoke<void>(ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME, clipped.c_str());
    Invoke<void>(END_TEXT_COMMAND_DISPLAY_TEXT, x, y, int32_t(0));
}

void Box(float top, size_t lines, float width)
{
    const float h = kLineHeight * static_cast<float>(lines) + 0.008f;
    Invoke<void>(DRAW_RECT, kLeft + width * 0.5f - 0.003f, top + h * 0.5f - 0.002f, width, h, int32_t(0), int32_t(0), int32_t(0),
                 int32_t(150), int32_t(0));
}

} // namespace

void Toast(const std::string& text)
{
    AcquireSRWLockExclusive(&g_toastLock);
    *g_toast = text;
    g_toastUntil = GetTickCount64() + kToastMs;
    ReleaseSRWLockExclusive(&g_toastLock);
}

void Draw(const std::vector<std::string>& lines)
{
    if (!SHV().IsLoaded())
        return;
    std::string toast;
    AcquireSRWLockShared(&g_toastLock);
    if (GetTickCount64() < g_toastUntil)
        toast = *g_toast;
    ReleaseSRWLockShared(&g_toastLock);

    float y = kTop;
    if (!lines.empty())
    {
        size_t longest = 0;
        for (const std::string& l : lines)
            longest = std::max(longest, l.size());
        Box(y, lines.size(), std::min(0.006f + 0.0052f * static_cast<float>(longest), 0.62f));
        for (size_t i = 0; i < lines.size(); ++i)
        {
            // First line (the verdict) in yellow, the rest in white.
            const bool head = i == 0;
            Text(kLeft, y, lines[i], 255, head ? 220 : 255, head ? 90 : 255);
            y += kLineHeight;
        }
        y += 0.006f;
    }
    if (!toast.empty())
    {
        Box(y, 1, std::min(0.006f + 0.0052f * static_cast<float>(toast.size()), 0.62f));
        Text(kLeft, y, toast, 120, 230, 255);
    }
}

} // namespace rtsky::game::overlay
