#include "Options.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <cctype>
#include <cstdlib>

// Search box text (typed in main.cpp)
char g_OptionSearch[128] = "";

// ----------------------------
// Small helper: ASCII case-insensitive substring search
// (Chinese text is not case-converted and will still match)
// ----------------------------
static bool ContainsI(const char* haystack, const char* needle)
{
    if (!haystack || !*haystack || !needle || !*needle) return false;

    auto lower_ascii = [](unsigned char c) -> unsigned char {
        if (c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A' + 'a');
        return c;
        };

    for (const char* h = haystack; *h; ++h)
    {
        const char* h2 = h;
        const char* n2 = needle;
        while (*h2 && *n2 && lower_ascii((unsigned char)*h2) == lower_ascii((unsigned char)*n2))
        {
            ++h2; ++n2;
        }
        if (*n2 == '\0') return true;
    }
    return false;
}

static bool OptionMatchesFilter(const Option& opt)
{
    if (g_OptionSearch[0] == '\0')
        return true;

    return ContainsI(opt.key, g_OptionSearch) ||
        ContainsI(opt.group, g_OptionSearch) ||
        ContainsI(opt.title, g_OptionSearch) ||
        ContainsI(opt.desc, g_OptionSearch) ||
        ContainsI(opt.tip, g_OptionSearch);
}

static std::string GetStr(const std::string& key)
{
    auto it = g_Values.find(key);
    return (it != g_Values.end()) ? it->second : std::string();
}

static bool ParseBool(const std::string& s, bool defVal)
{
    if (s.empty()) return defVal;
    std::string t = s;
    for (auto& ch : t) ch = (char)std::tolower((unsigned char)ch);
    if (t == "1" || t == "true" || t == "yes" || t == "on" || t == "enable" || t == "enabled") return true;
    if (t == "0" || t == "false" || t == "no" || t == "off" || t == "disable" || t == "disabled") return false;
    return defVal;
}

static bool TryParseFloat(const std::string& s, float& out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtof(s.c_str(), &end);
    return end && end != s.c_str();
}

static bool TryParseInt(const std::string& s, int& out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (!(end && end != s.c_str())) return false;
    out = (int)v;
    return true;
}

static float GetFloat(const std::string& key, float defVal)
{
    float v = defVal;
    std::string s = GetStr(key);
    if (TryParseFloat(s, v)) return v;
    return defVal;
}

static int GetInt(const std::string& key, int defVal)
{
    int v = defVal;
    std::string s = GetStr(key);
    if (TryParseInt(s, v)) return v;
    return defVal;
}

static ImVec4 GetColor(const std::string& key, const ImVec4& defVal)
{
    // Supported formats:
    // 1) "r,g,b,a" (0~1 or 0~255)
    // 2) "r g b a"
    std::string s = GetStr(key);
    if (s.empty()) return defVal;

    float v[4] = { defVal.x, defVal.y, defVal.z, defVal.w };

    // Normalize all separators to spaces
    for (auto& ch : s) if (ch == ',' || ch == '\t') ch = ' ';

    int got = std::sscanf(s.c_str(), "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]);
    if (got < 3) return defVal;
    if (got == 3) v[3] = defVal.w;

    // If it looks like 0~255, normalize to 0~1
    if (v[0] > 1.f || v[1] > 1.f || v[2] > 1.f || v[3] > 1.f)
    {
        v[0] /= 255.f; v[1] /= 255.f; v[2] /= 255.f; v[3] /= 255.f;
    }

    // Clamp
    for (int i = 0; i < 4; ++i)
    {
        if (v[i] < 0.f) v[i] = 0.f;
        if (v[i] > 1.f) v[i] = 1.f;
    }
    return ImVec4(v[0], v[1], v[2], v[3]);
}

static void SetColor(const std::string& key, const ImVec4& c)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f", c.x, c.y, c.z, c.w);
    g_Values[key] = buf;
}

static void DrawHelp(const Option& opt)
{
    // Show detailed help when hovering the control (does not consume layout space)
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::BeginTooltip();
        ImGui::TextDisabled("Key: %s", opt.key);
        if (opt.desc && *opt.desc) { ImGui::Separator(); ImGui::TextWrapped("%s", opt.desc); }
        if (opt.tip && *opt.tip) { ImGui::Separator(); ImGui::TextWrapped("%s", opt.tip); }
        ImGui::EndTooltip();
    }
}

// ============================================================
// Option table (based on vr.cpp / vr.h)
// ============================================================

Option g_Options[] =
{
    // Multiplayer / Server
    {
        "ForceNonVRServerMovement",
        OptionType::Bool,
        u8"Multiplayer / Server Compatibility",
        u8"Non-VR Server Compatibility Mode",
        u8"When playing on a non-VR server, convert VR movement and interaction into forms that are more acceptable to standard servers.",
        u8"Recommended for multiplayer. Usually unnecessary for single-player or VR-enabled servers.",
    },

    // Aim Line
    {
        "AimLineEnabled",
        OptionType::Bool,
        u8"Aim Assist",
        u8"Enable Aim Line",
        u8"Whether to render the VR aiming line.",
        u8"Disabling slightly reduces rendering and logic overhead."
    },
    {
        "AimLineFrameDurationMultiplier",
        OptionType::Float,
        u8"Aim Assist",
        u8"Aim Line Persistence",
        u8"Controls how long the aim line lingers (trail effect). Higher values keep historical frames longer.",
        u8"Recommended 1.0 ~ 1.5. Too high may look cluttered.",
        0.1f, 5.0f
    },
    {
        "AimLineMaxHz",
        OptionType::Int,
        u8"Aim Assist",
        u8"Aim Line Update Rate Limit",
        u8"Limits the maximum aim-line updates per second to control trace and CPU usage.",
        u8"Recommended 60~120. Lower to 45/30 on weaker machines.",
        10.f, 240.f
    },
    {
        "AimLineThickness",
        OptionType::Float,
        u8"Aim Assist",
        u8"Aim Line Thickness",
        u8"Affects visual thickness only; does not affect hit detection.",
        u8"Recommended 1.0 ~ 2.0",
        0.1f, 5.0f
    },
    {
        "AimLineColor",
        OptionType::Color,
        u8"Aim Assist",
        u8"Aim Line Color",
        u8"Sets the aim line color (RGBA). Supports values in 0~1 or 0~255.",
        u8"Example: 1,1,1,1 or 255,255,255,255"
    },

    // Head / View
    {
        "HeadSmoothing",
        OptionType::Float,
        u8"View / Head",
        u8"Head Tracking Smoothing",
        u8"Applies smoothing to head tracking to reduce jitter at the cost of slight latency.",
        u8"Motion-sickness-prone players may increase this slightly.",
        0.0f, 1.0f
    },
    {
        "VRScale",
        OptionType::Float,
        u8"View / Head",
        u8"VR World Scale",
        u8"Adjusts overall world scale (distance and sense of size).",
        u8"Default is 1.0. Large changes may affect distance perception.",
        0.5f, 2.0f
    },

    // Turning / Input
    {
        "TurnSpeed",
        OptionType::Float,
        u8"Turning / Input",
        u8"Turn Speed",
        u8"Controls smooth turning speed.",
        u8"Too fast may cause motion sickness; too slow makes turning awkward.",
        0.1f, 10.0f
    },
    {
        "ControllerSmoothing",
        OptionType::Float,
        u8"Turning / Input",
        u8"Controller Input Smoothing",
        u8"Smooths controller input to reduce hand jitter.",
        u8"Excessive values may introduce input latency.",
        0.0f, 1.0f
    },

    // HUD
    {
        "ControllerHudSize",
        OptionType::Float,
        u8"HUD / Controller HUD",
        u8"Controller HUD Size",
        u8"Adjusts the size of the HUD attached to the controller.",
        u8"",
        0.1f, 3.0f
    },
    {
        "ControllerHudXOffset",
        OptionType::Float,
        u8"HUD / Controller HUD",
        u8"Controller HUD X Offset",
        u8"Fine-tunes the left/right position of the controller HUD.",
        u8"",
        -50.f, 50.f
    },
    {
        "ControllerHudYOffset",
        OptionType::Float,
        u8"HUD / Controller HUD",
        u8"Controller HUD Y Offset",
        u8"Fine-tunes the up/down position of the controller HUD.",
        u8"",
        -50.f, 50.f
    },
    {
        "ControllerHudZOffset",
        OptionType::Float,
        u8"HUD / Controller HUD",
        u8"Controller HUD Z Offset",
        u8"Fine-tunes the forward/backward distance of the controller HUD.",
        u8"",
        -50.f, 50.f
    },

    // Special Infected Assist
    {
        "SpecialInfectedPreWarningAutoAimEnabled",
        OptionType::Bool,
        u8"Special Infected Assist",
        u8"Enable Special Infected Warning / Assist",
        u8"Enables special infected pre-warning and related assist logic.",
        u8"Note: This is an assist feature. Use responsibly."
    },
    {
        "SpecialInfectedPreWarningDistance",
        OptionType::Float,
        u8"Special Infected Assist",
        u8"Special Infected Warning Distance",
        u8"Triggers a warning when special infected enter this distance.",
        u8"Recommended 400 ~ 600",
        0.f, 3000.f
    },
    {
        "SpecialInfectedTraceMaxHz",
        OptionType::Int,
        u8"Special Infected Assist",
        u8"Special Infected Detection Rate Limit",
        u8"Limits how often special infected detection traces/logic run per second to reduce CPU cost.",
        u8"Recommended 60~120. Lower if performance issues occur.",
        10.f, 240.f
    },

    // Throw Arc
    {
        "ThrowArcMaxHz",
        OptionType::Int,
        u8"Throw Prediction",
        u8"Throw Trajectory Update Rate",
        u8"Limits how frequently the throw trajectory is updated.",
        u8"Lowering this can reduce performance cost.",
        10.f, 240.f
    },
    {
        "ThrowArcLandingOffset",
        OptionType::Float,
        u8"Throw Prediction",
        u8"Throw Landing Offset",
        u8"Applies positional compensation to the predicted landing point to account for network or engine error.",
        u8"",
        -50.f, 50.f
    },

    // Viewmodel
    {
        "ViewmodelAdjustEnabled",
        OptionType::Bool,
        u8"View Model",
        u8"Enable View Model Adjustment",
        u8"Allows manual adjustment and saving of weapon/view models.",
        u8"Typically used to fine-tune weapon grip positioning."
    },
};

const int g_OptionCount = (int)(sizeof(g_Options) / sizeof(g_Options[0]));

// ============================================================
// UI renderer
// ============================================================

void DrawOptionsUI()
{
    const char* currentGroup = nullptr;

    for (int i = 0; i < g_OptionCount; ++i)
    {
        const Option& opt = g_Options[i];
        if (!OptionMatchesFilter(opt))
            continue;

        std::string key = opt.key;

        // Group header
        if (!currentGroup || std::strcmp(currentGroup, opt.group) != 0)
        {
            currentGroup = opt.group;
            ImGui::SeparatorText(currentGroup);
        }

        ImGui::PushID(opt.key);

        // Render control
        switch (opt.type)
        {
        case OptionType::Bool:
        {
            bool v = ParseBool(GetStr(key), false);
            if (ImGui::Checkbox(opt.title, &v))
                g_Values[key] = v ? "true" : "false";
            DrawHelp(opt);
            break;
        }
        case OptionType::Float:
        {
            float defV = (opt.min != 0.f || opt.max != 0.f) ? (opt.min + opt.max) * 0.5f : 0.f;
            float v = GetFloat(key, defV);
            if (ImGui::SliderFloat(opt.title, &v, opt.min, opt.max, "%.3f"))
                g_Values[key] = std::to_string(v);
            DrawHelp(opt);
            break;
        }
        case OptionType::Int:
        {
            int defV = 0;
            int v = GetInt(key, defV);
            if (ImGui::SliderInt(opt.title, &v, (int)opt.min, (int)opt.max))
                g_Values[key] = std::to_string(v);
            DrawHelp(opt);
            break;
        }
        case OptionType::Color:
        {
            ImVec4 c = GetColor(key, ImVec4(1, 1, 1, 1));
            if (ImGui::ColorEdit4(opt.title, (float*)&c))
                SetColor(key, c);
            DrawHelp(opt);
            break;
        }
        }

        // Display the key below the control in gray text for easy screenshots/support
        ImGui::TextDisabled("%s", opt.key);
        ImGui::Spacing();

        ImGui::PopID();
    }
}
