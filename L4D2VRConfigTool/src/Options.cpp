#include "Options.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <cctype>
#include <cstdlib>

// Search box text (typed in main.cpp)
char g_OptionSearch[128] = "";

static const char* L(const L10nText& t)
{
    return (g_UseChinese && t.zh && *t.zh) ? t.zh : t.en;
}

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

    const char* groupEn = opt.group.en ? opt.group.en : "";
    const char* groupZh = opt.group.zh ? opt.group.zh : "";
    const char* titleEn = opt.title.en ? opt.title.en : "";
    const char* titleZh = opt.title.zh ? opt.title.zh : "";
    const char* descEn = opt.desc.en ? opt.desc.en : "";
    const char* descZh = opt.desc.zh ? opt.desc.zh : "";
    const char* tipEn = opt.tip.en ? opt.tip.en : "";
    const char* tipZh = opt.tip.zh ? opt.tip.zh : "";

    return ContainsI(opt.key, g_OptionSearch) ||
        ContainsI(groupEn, g_OptionSearch) || ContainsI(groupZh, g_OptionSearch) ||
        ContainsI(titleEn, g_OptionSearch) || ContainsI(titleZh, g_OptionSearch) ||
        ContainsI(descEn, g_OptionSearch) || ContainsI(descZh, g_OptionSearch) ||
        ContainsI(tipEn, g_OptionSearch) || ContainsI(tipZh, g_OptionSearch);
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
        const char* desc = L(opt.desc);
        const char* tip = L(opt.tip);
        if (desc && *desc) { ImGui::Separator(); ImGui::TextWrapped("%s", desc); }
        if (tip && *tip) { ImGui::Separator(); ImGui::TextWrapped("%s", tip); }
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
        { u8"Multiplayer / Server Compatibility", u8"多人 / 服务器兼容" },
        { u8"Non-VR Server Compatibility Mode", u8"非VR服务器兼容模式" },
        { u8"When playing on a non-VR server, convert VR movement and interaction into forms that are more acceptable to standard servers.",
          u8"在非VR服务器上游玩时，将VR移动和交互转换为更适合传统服务器的形式。" },
        { u8"Recommended for multiplayer. Usually unnecessary for single-player or VR-enabled servers.",
          u8"多人游戏推荐启用。单人或支持VR的服务器通常不需要。" },
    },

    // Aim Line
    {
        "AimLineEnabled",
        OptionType::Bool,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Enable Aim Line", u8"启用瞄准线" },
        { u8"Whether to render the VR aiming line.",
          u8"是否渲染VR瞄准线。" },
        { u8"Disabling slightly reduces rendering and logic overhead.",
          u8"关闭可略微减少渲染和逻辑开销。" }
    },
    {
        "AimLineFrameDurationMultiplier",
        OptionType::Float,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Aim Line Persistence", u8"瞄准线保留时间" },
        { u8"Controls how long the aim line lingers (trail effect). Higher values keep historical frames longer.",
          u8"控制瞄准线停留时间（拖尾效果）。值越高，历史帧保留越久。" },
        { u8"Recommended 1.0 ~ 1.5. Too high may look cluttered.",
          u8"推荐 1.0 ~ 1.5，过高可能显得杂乱。" },
        0.1f, 5.0f
    },
    {
        "AimLineMaxHz",
        OptionType::Int,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Aim Line Update Rate Limit", u8"瞄准线刷新率上限" },
        { u8"Limits the maximum aim-line updates per second to control trace and CPU usage.",
          u8"限制瞄准线每秒更新次数，以控制追踪和CPU开销。" },
        { u8"Recommended 60~120. Lower to 45/30 on weaker machines.",
          u8"推荐 60~120。性能不足时可降到 45/30。" },
        10.f, 240.f
    },
    {
        "AimLineThickness",
        OptionType::Float,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Aim Line Thickness", u8"瞄准线粗细" },
        { u8"Affects visual thickness only; does not affect hit detection.",
          u8"仅影响视觉粗细，不影响判定。" },
        { u8"Recommended 1.0 ~ 2.0",
          u8"推荐 1.0 ~ 2.0" },
        0.1f, 5.0f
    },
    {
        "AimLineColor",
        OptionType::Color,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Aim Line Color", u8"瞄准线颜色" },
        { u8"Sets the aim line color (RGBA). Supports values in 0~1 or 0~255.",
          u8"设置瞄准线颜色（RGBA）。支持 0~1 或 0~255。" },
        { u8"Example: 1,1,1,1 or 255,255,255,255",
          u8"示例：1,1,1,1 或 255,255,255,255" }
    },

    // Head / View
    {
        "HeadSmoothing",
        OptionType::Float,
        { u8"View / Head", u8"视角 / 头部" },
        { u8"Head Tracking Smoothing", u8"头部追踪平滑" },
        { u8"Applies smoothing to head tracking to reduce jitter at the cost of slight latency.",
          u8"为头部追踪添加平滑以减少抖动，但会略微增加延迟。" },
        { u8"Motion-sickness-prone players may increase this slightly.",
          u8"易晕玩家可适当提高该值。" },
        0.0f, 1.0f
    },
    {
        "VRScale",
        OptionType::Float,
        { u8"View / Head", u8"视角 / 头部" },
        { u8"VR World Scale", u8"VR 世界缩放" },
        { u8"Adjusts overall world scale (distance and sense of size).",
          u8"调整整体世界尺度（距离感与尺寸感）。" },
        { u8"Default is 1.0. Large changes may affect distance perception.",
          u8"默认 1.0，过大调整会影响距离感知。" },
        0.5f, 2.0f
    },

    // Turning / Input
    {
        "TurnSpeed",
        OptionType::Float,
        { u8"Turning / Input", u8"转向 / 输入" },
        { u8"Turn Speed", u8"平滑转向速度" },
        { u8"Controls smooth turning speed.",
          u8"控制平滑转向速度。" },
        { u8"Too fast may cause motion sickness; too slow makes turning awkward.",
          u8"过快可能引起眩晕，过慢会影响操作。" },
        0.1f, 10.0f
    },
    {
        "ControllerSmoothing",
        OptionType::Float,
        { u8"Turning / Input", u8"转向 / 输入" },
        { u8"Controller Input Smoothing", u8"手柄输入平滑" },
        { u8"Smooths controller input to reduce hand jitter.",
          u8"平滑手柄输入以减少抖动。" },
        { u8"Excessive values may introduce input latency.",
          u8"过高会增加输入延迟。" },
        0.0f, 1.0f
    },

    // HUD
    {
        "ControllerHudSize",
        OptionType::Float,
        { u8"HUD / Controller HUD", u8"HUD / 手柄HUD" },
        { u8"Controller HUD Size", u8"手柄HUD大小" },
        { u8"Adjusts the size of the HUD attached to the controller.",
          u8"调整附着在手柄上的HUD尺寸。" },
        { u8"", u8"" },
        0.1f, 3.0f
    },
    {
        "ControllerHudXOffset",
        OptionType::Float,
        { u8"HUD / Controller HUD", u8"HUD / 手柄HUD" },
        { u8"Controller HUD X Offset", u8"手柄HUD X偏移" },
        { u8"Fine-tunes the left/right position of the controller HUD.",
          u8"微调手柄HUD的左右位置。" },
        { u8"", u8"" },
        -50.f, 50.f
    },
    {
        "ControllerHudYOffset",
        OptionType::Float,
        { u8"HUD / Controller HUD", u8"HUD / 手柄HUD" },
        { u8"Controller HUD Y Offset", u8"手柄HUD Y偏移" },
        { u8"Fine-tunes the up/down position of the controller HUD.",
          u8"微调手柄HUD的上下位置。" },
        { u8"", u8"" },
        -50.f, 50.f
    },
    {
        "ControllerHudZOffset",
        OptionType::Float,
        { u8"HUD / Controller HUD", u8"HUD / 手柄HUD" },
        { u8"Controller HUD Z Offset", u8"手柄HUD Z偏移" },
        { u8"Fine-tunes the forward/backward distance of the controller HUD.",
          u8"微调手柄HUD的前后距离。" },
        { u8"", u8"" },
        -50.f, 50.f
    },

    // Special Infected Assist
    {
        "SpecialInfectedPreWarningAutoAimEnabled",
        OptionType::Bool,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Enable Special Infected Warning / Assist", u8"启用特感预警 / 辅助" },
        { u8"Enables special infected pre-warning and related assist logic.",
          u8"启用特感预警及相关辅助逻辑。" },
        { u8"Note: This is an assist feature. Use responsibly.",
          u8"注意：此为辅助功能，请合理使用。" }
    },
    {
        "SpecialInfectedPreWarningDistance",
        OptionType::Float,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Special Infected Warning Distance", u8"特感预警距离" },
        { u8"Triggers a warning when special infected enter this distance.",
          u8"当特感进入该距离时触发预警。" },
        { u8"Recommended 400 ~ 600",
          u8"推荐 400 ~ 600" },
        0.f, 3000.f
    },
    {
        "SpecialInfectedTraceMaxHz",
        OptionType::Int,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Special Infected Detection Rate Limit", u8"特感检测频率上限" },
        { u8"Limits how often special infected detection traces/logic run per second to reduce CPU cost.",
          u8"限制特感检测/逻辑每秒运行次数以降低CPU开销。" },
        { u8"Recommended 60~120. Lower if performance issues occur.",
          u8"推荐 60~120。如有性能问题可再降低。" },
        10.f, 240.f
    },

    // Throw Arc
    {
        "ThrowArcMaxHz",
        OptionType::Int,
        { u8"Throw Prediction", u8"投掷预测" },
        { u8"Throw Trajectory Update Rate", u8"投掷轨迹刷新率" },
        { u8"Limits how frequently the throw trajectory is updated.",
          u8"限制投掷轨迹更新频率。" },
        { u8"Lowering this can reduce performance cost.",
          u8"降低该值可减少性能开销。" },
        10.f, 240.f
    },
    {
        "ThrowArcLandingOffset",
        OptionType::Float,
        { u8"Throw Prediction", u8"投掷预测" },
        { u8"Throw Landing Offset", u8"投掷落点补偿" },
        { u8"Applies positional compensation to the predicted landing point to account for network or engine error.",
          u8"为预测落点添加位置补偿，以应对网络或引擎误差。" },
        { u8"", u8"" },
        -50.f, 50.f
    },

    // Viewmodel
    {
        "ViewmodelAdjustEnabled",
        OptionType::Bool,
        { u8"View Model", u8"武器视模" },
        { u8"Enable View Model Adjustment", u8"启用视模调整" },
        { u8"Allows manual adjustment and saving of weapon/view models.",
          u8"允许手动调整并保存武器/视模位置。" },
        { u8"Typically used to fine-tune weapon grip positioning.",
          u8"通常用于微调武器握持位置。" }
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
        const char* group = L(opt.group);

        if (!currentGroup || std::strcmp(currentGroup, group) != 0)
        {
            currentGroup = group;
            ImGui::SeparatorText(currentGroup);
        }

        ImGui::PushID(opt.key);

        // Render control
        switch (opt.type)
        {
        case OptionType::Bool:
        {
            bool v = ParseBool(GetStr(key), false);
            if (ImGui::Checkbox(L(opt.title), &v))
                g_Values[key] = v ? "true" : "false";
            DrawHelp(opt);
            break;
        }
        case OptionType::Float:
        {
            float defV = (opt.min != 0.f || opt.max != 0.f) ? (opt.min + opt.max) * 0.5f : 0.f;
            float v = GetFloat(key, defV);
            if (ImGui::SliderFloat(L(opt.title), &v, opt.min, opt.max, "%.3f"))
                g_Values[key] = std::to_string(v);
            DrawHelp(opt);
            break;
        }
        case OptionType::Int:
        {
            int defV = 0;
            int v = GetInt(key, defV);
            if (ImGui::SliderInt(L(opt.title), &v, (int)opt.min, (int)opt.max))
                g_Values[key] = std::to_string(v);
            DrawHelp(opt);
            break;
        }
        case OptionType::Color:
        {
            ImVec4 c = GetColor(key, ImVec4(1, 1, 1, 1));
            if (ImGui::ColorEdit4(L(opt.title), (float*)&c))
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
