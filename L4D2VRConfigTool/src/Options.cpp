#include "Options.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <cctype>
#include <cstdlib>
#include <cfloat>
#include <algorithm>

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

static std::string GetDefaultStr(const Option& opt)
{
    return (opt.defaultValue && *opt.defaultValue) ? std::string(opt.defaultValue) : std::string();
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

static float GetDefaultFloat(const Option& opt)
{
    float def = 0.f;
    if (TryParseFloat(GetDefaultStr(opt), def))
        return def;
    if (opt.min != 0.f || opt.max != 0.f)
        return (opt.min + opt.max) * 0.5f;
    return 0.f;
}

static int GetDefaultInt(const Option& opt)
{
    int def = 0;
    if (TryParseInt(GetDefaultStr(opt), def))
        return def;
    if (opt.min != 0.f || opt.max != 0.f)
        return static_cast<int>((opt.min + opt.max) * 0.5f);
    return 0;
}

static bool GetDefaultBool(const Option& opt)
{
    return ParseBool(GetDefaultStr(opt), false);
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

static ImVec4 ParseColorString(const std::string& s, const ImVec4& defVal)
{
    if (s.empty()) return defVal;
    float v[4] = { defVal.x, defVal.y, defVal.z, defVal.w };

    // Normalize all separators to spaces
    std::string copy = s;
    for (auto& ch : copy) if (ch == ',' || ch == '\t') ch = ' ';

    int got = std::sscanf(copy.c_str(), "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]);
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

static ImVec4 GetColor(const Option& opt, const ImVec4& fallback)
{
    ImVec4 def = ParseColorString(GetDefaultStr(opt), fallback);
    return ParseColorString(GetStr(opt.key), def);
}

static void SetColor(const std::string& key, const ImVec4& c)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f", c.x, c.y, c.z, c.w);
    g_Values[key] = buf;
}

struct Vec3
{
    float x;
    float y;
    float z;
};

static bool TryParseVec3(const std::string& s, Vec3& out)
{
    if (s.empty()) return false;
    Vec3 copy = out;
    std::string normalized = s;
    for (auto& ch : normalized) if (ch == ',' || ch == '\t') ch = ' ';

    float v[3] = { copy.x, copy.y, copy.z };
    int got = std::sscanf(normalized.c_str(), "%f %f %f", &v[0], &v[1], &v[2]);
    if (got < 3) return false;
    out.x = v[0];
    out.y = v[1];
    out.z = v[2];
    return true;
}

static Vec3 GetVec3Default(const Option& opt, const Vec3& fallback)
{
    Vec3 def = fallback;
    TryParseVec3(GetDefaultStr(opt), def);
    return def;
}

static Vec3 GetVec3(const Option& opt, const Vec3& defVal)
{
    Vec3 v = defVal;
    TryParseVec3(GetStr(opt.key), v);
    return v;
}

static void SetVec3(const std::string& key, const Vec3& v)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f", v.x, v.y, v.z);
    g_Values[key] = buf;
}

static void DrawHelp(const Option& opt)
{
    // Show detailed help when hovering the control (does not consume layout space)
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        // Keep tooltip width stable between different options to avoid size flicker.
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f), ImVec2(480.0f, FLT_MAX));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 38.0f);
        ImGui::TextDisabled("Key: %s", opt.key);
        const char* desc = L(opt.desc);
        const char* tip = L(opt.tip);
        if (desc && *desc) { ImGui::Separator(); ImGui::TextWrapped("%s", desc); }
        if (tip && *tip) { ImGui::Separator(); ImGui::TextWrapped("%s", tip); }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ============================================================
// Option table (based on vr.cpp / vr.h)
// ============================================================

Option g_Options[] =
{
    // View / Scale
    {
        "VRScale",
        OptionType::Float,
        { u8"View / Scale", u8"视角 / 尺度" },
        { u8"World Scale", u8"世界缩放" },
        { u8"Adjusts overall world scale (distance and size perception).",
          u8"调整整体世界尺度（距离与大小感知）。" },
        { u8"Keep close to real-world meter scale. 43.2 covers most play spaces.",
          u8"尽量保持与真实世界接近。43.2一般最合适。" },
        30.0f, 55.0f,
        "43.2"
    },
    {
        "IPDScale",
        OptionType::Float,
        { u8"View / Scale", u8"视角 / 尺度" },
        { u8"IPD Scale", u8"瞳距缩放" },
        { u8"Multiplies headset IPD to fine-tune stereo separation.",
          u8"按比例调整头显瞳距以微调立体分离度。" },
        { u8"Use for small comfort tweaks only.",
          u8"仅用于小幅舒适度微调。" },
        0.8f, 1.2f,
        "1.0"
    },
    // Input / Turning
    {
        "LeftHanded",
        OptionType::Bool,
        { u8"Input / Turning", u8"输入 / 转向" },
        { u8"Left-Handed Mode", u8"左手持武" },
        { u8"Swaps dominant hand interactions for left-handed players.",
          u8"为左手玩家切换主要交互手。" },
        { u8"Toggle if you primarily aim with the left controller.",
          u8"如果主要用左手瞄准，请开启。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "TurnSpeed",
        OptionType::Float,
        { u8"Input / Turning", u8"输入 / 转向" },
        { u8"Smooth Turn Speed", u8"平滑转向速度" },
        { u8"Controls smooth turning speed.",
          u8"控制平滑转向的转速。" },
        { u8"0.2~0.6 is comfortable for most people.",
          u8"多数人适合 0.2~0.6。" },
        0.1f, 1.0f,
        "0.3"
    },
    {
        "SnapTurning",
        OptionType::Bool,
        { u8"Input / Turning", u8"输入 / 转向" },
        { u8"Use Snap Turning", u8"启用分段转向" },
        { u8"Turns in fixed increments instead of smooth rotation.",
          u8"使用固定角度分段转向，替代连续旋转。" },
        { u8"Preferable for motion-sickness-prone players.",
          u8"容易晕动症的玩家建议开启。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "SnapTurnAngle",
        OptionType::Float,
        { u8"Input / Turning", u8"输入 / 转向" },
        { u8"Snap Turn Angle", u8"分段转向角度" },
        { u8"Degrees turned per snap when snap turning is enabled.",
          u8"分段转向时每次旋转的角度。" },
        { u8"30°~60° is common. Higher = fewer snaps, lower = finer control.",
          u8"30°~60° 比较常见。角度越大，次数越少；越小越精细。" },
        15.0f, 120.0f,
        "45.0"
    },
    {
        "ControllerSmoothing",
        OptionType::Float,
        { u8"Input / Turning", u8"输入 / 转向" },
        { u8"Controller Input Smoothing", u8"手柄输入平滑" },
        { u8"Smooths controller input to reduce jitter.",
          u8"平滑处理手柄输入以减少抖动。" },
        { u8"Keep under 0.5 to avoid noticeable latency.",
          u8"建议低于 0.5 以避免明显延迟。" },
        0.0f, 0.8f,
        "0.0"
    },
    {
        "ForceNonVRServerMovement",
        OptionType::Bool,
        { u8"Multiplayer / Server", u8"多人 / 服务器" },
        { u8"Non-VR Server Compatibility Mode", u8"非VR服务器兼容模式" },
        { u8"Converts VR movement/interaction to be more acceptable to standard servers.",
          u8"将VR移动与交互转换为更符合传统服务器的形式。" },
        { u8"Recommended for public multiplayer servers.",
          u8"非自己建房多必须开启。" },
        0.0f, 0.0f,
        "false"
    },

    // HUD (Main)
    {
        "HudDistance",
        OptionType::Float,
        { u8"HUD (Main)", u8"HUD（主界面）" },
        { u8"HUD Distance", u8"HUD 距离" },
        { u8"Distance from the head to the main HUD plane.",
          u8"头部到主HUD平面的距离。" },
        { u8"Closer feels larger; farther reduces eye strain.",
          u8"越近越大，越远越护眼。" },
        0.5f, 3.0f,
        "1.3"
    },
    {
        "HudSize",
        OptionType::Float,
        { u8"HUD (Main)", u8"HUD（主界面）" },
        { u8"HUD Size", u8"HUD 尺寸" },
        { u8"Overall scale of the main HUD.",
          u8"主HUD整体缩放。" },
        { u8"1.2~2.0 fits most users.",
          u8"1.2~2.0 适合大多数人。" },
        0.8f, 3.0f,
        "1.8"
    },
    {
        "HudAlwaysVisible",
        OptionType::Bool,
        { u8"HUD (Main)", u8"HUD（主界面）" },
        { u8"HUD Always Visible", u8"HUD 总是可见" },
        { u8"Keep HUD visible even when not toggled by gameplay.",
          u8"即使未被游戏逻辑显示也始终显示HUD。" },
        { u8"Disable if you prefer a minimal view.",
          u8"若想极简视野可关闭。" },
        0.0f, 0.0f,
        "true"
    },

    // HUD (Controller)
    {
        "ControllerHudSize",
        OptionType::Float,
        { u8"HUD (Controller)", u8"HUD（手柄）" },
        { u8"Controller HUD Size", u8"手柄HUD大小" },
        { u8"Size of the HUD attached to the controller.",
          u8"附着在手柄上的HUD尺寸。" },
        { u8"0.08~0.2 keeps it readable without blocking view.",
          u8"0.08~0.2 既可读又不挡视线。" },
        0.05f, 0.5f,
        "0.1"
    },
    {
        "ControllerHudXOffset",
        OptionType::Float,
        { u8"HUD (Controller)", u8"HUD（手柄）" },
        { u8"Controller HUD X Offset", u8"手柄HUD X偏移" },
        { u8"Left/right offset of controller HUD (meters).",
          u8"手柄HUD左右偏移（米）。" },
        { u8"", u8"" },
        -0.5f, 0.5f,
        "0.1"
    },
    {
        "ControllerHudYOffset",
        OptionType::Float,
        { u8"HUD (Controller)", u8"HUD（手柄）" },
        { u8"Controller HUD Y Offset", u8"手柄HUD Y偏移" },
        { u8"Up/down offset of controller HUD (meters).",
          u8"手柄HUD上下偏移（米）。" },
        { u8"", u8"" },
        -0.5f, 0.5f,
        "0.1"
    },
    {
        "ControllerHudZOffset",
        OptionType::Float,
        { u8"HUD (Controller)", u8"HUD（手柄）" },
        { u8"Controller HUD Z Offset", u8"手柄HUD Z偏移" },
        { u8"Forward/back offset of controller HUD (meters).",
          u8"手柄HUD前后偏移（米）。" },
        { u8"", u8"" },
        -1.0f, 0.5f,
        "-0.3"
    },
    {
        "ControllerHudRotation",
        OptionType::Float,
        { u8"HUD (Controller)", u8"HUD（手柄）" },
        { u8"Controller HUD Rotation", u8"手柄HUD 旋转角" },
        { u8"Rotates the controller HUD around the grip.",
          u8"围绕握把旋转手柄HUD。" },
        { u8"Negative tilts inward for right-hand HUD.",
          u8"负值向内倾斜（右手HUD）。" },
        -180.0f, 180.0f,
        "-60"
    },

    // Hands / Debug
    {
        "HideArms",
        OptionType::Bool,
        { u8"Hands / Debug", u8"手部 / 调试" },
        { u8"Hide Arms", u8"隐藏手臂" },
        { u8"Hides in-game arm models while keeping hands/controllers.",
          u8"隐藏游戏中的手臂模型，仅保留手/手柄。" },
        { u8"Useful when arm animations conflict with your pose.",
          u8"当手臂动画与姿态不一致时可开启。" },
        0.0f, 0.0f,
        "false"
    },
    // Interaction / Combos
    {
        "RequireSecondaryAttackForItemSwitch",
        OptionType::Bool,
        { u8"Interaction / Combos", u8"交互 / 组合键" },
        { u8"Require Alt-Fire for Item Switch", u8"切换物品需副攻键" },
        { u8"Prevents accidental item switches unless secondary attack is held.",
          u8"需要按住副攻击键才切换物品，避免误触。" },
        { u8"", u8"" },
        0.0f, 0.0f,
        "true"
    },
    {
        "VoiceRecordCombo",
        OptionType::String,
        { u8"Interaction / Combos", u8"交互 / 组合键" },
        { u8"Voice Chat Combo", u8"语音聊天组合键" },
        { u8"VR action combination that triggers voice chat (format: Action+Action).",
          u8"触发语音聊天的VR动作组合（格式：动作+动作）。" },
        { u8"Set to \"false\" to disable.",
          u8"设为 \"false\" 可禁用。" },
        0.0f, 0.0f,
        "Crouch+Reload"
    },
    {
        "QuickTurnCombo",
        OptionType::String,
        { u8"Interaction / Combos", u8"交互 / 组合键" },
        { u8"Quick Turn Combo", u8"快速转身组合键" },
        { u8"Action combo that triggers a quick 180° turn.",
          u8"触发快速180°转身的动作组合。" },
        { u8"Use VR action names joined with +.",
          u8"使用VR动作名并用 + 连接。" },
        0.0f, 0.0f,
        "SecondaryAttack+Crouch"
    },
    {
        "ViewmodelAdjustEnabled",
        OptionType::Bool,
        { u8"Interaction / Combos", u8"交互 / 组合键" },
        { u8"Enable Viewmodel Adjustment", u8"启用视模调整" },
        { u8"Allows saving manual weapon/viewmodel offsets in VR.",
          u8"允许在VR中保存武器/视模的手动偏移。" },
        { u8"", u8"" },
        0.0f, 0.0f,
        "true"
    },
    {
        "ViewmodelAdjustCombo",
        OptionType::String,
        { u8"Interaction / Combos", u8"交互 / 组合键" },
        { u8"Viewmodel Adjust Combo", u8"视模调整组合键" },
        { u8"Action combo to toggle viewmodel adjustment mode.",
          u8"用于切换视模调整模式的组合动作。" },
        { u8"Set to \"false\" to disable if you never edit offsets.",
          u8"若不需要可设为 \"false\" 禁用。" },
        0.0f, 0.0f,
        "Reload+SecondaryAttack"
    },

    // Aim Assist
    {
        "AimLineEnabled",
        OptionType::Bool,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Enable Aim Line", u8"启用瞄准线" },
        { u8"Renders the VR aiming line.",
          u8"渲染VR瞄准线。" },
        { u8"Disable to reduce visual clutter and cost.",
          u8"关闭可减少视觉杂乱和开销。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "MeleeAimLineEnabled",
        OptionType::Bool,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Enable Melee Aim Line", u8"启用近战瞄准线" },
        { u8"Shows the aim line when wielding melee weapons.",
          u8"在使用近战武器时仍显示瞄准线。" },
        { u8"", u8"" },
        0.0f, 0.0f,
        "true"
    },
    {
        "AimLineThickness",
        OptionType::Float,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Aim Line Thickness", u8"瞄准线粗细" },
        { u8"Affects visual thickness only; does not affect hit detection.",
          u8"仅影响视觉粗细，不影响判定。" },
        { u8"0.15~0.5 is usually readable.",
          u8"0.15~0.5 通常足够清晰。" },
        0.05f, 1.0f,
        "0.3"
    },
    {
        "AimLineColor",
        OptionType::Color,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Aim Line Color", u8"瞄准线颜色" },
        { u8"RGBA color for the aim line (supports 0~1 or 0~255).",
          u8"瞄准线的RGBA颜色（支持 0~1 或 0~255）。" },
        { u8"", u8"" },
        0.0f, 0.0f,
        "255,0,0,180"
    },
    {
        "AimLineMaxHz",
        OptionType::Int,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Aim Line Update Rate Limit", u8"瞄准线刷新率上限" },
        { u8"Limits aim-line updates per second to control CPU cost.",
          u8"限制瞄准线每秒更新次数以控制CPU开销。" },
        { u8"Match your headset FPS or halve it for performance.",
          u8"可与头显帧率一致，或减半以省资源。" },
        30.f, 180.f,
        "90"
    },
    // Scope
    {
        "ScopeAimSensitivityScale",
        OptionType::String,
        { u8"Scope", u8"瞄准镜" },
        { u8"Scoped Aim Sensitivity Scale", u8"开镜灵敏度缩放" },
        { u8"Scales controller aim while scope is active (ADS / zoom sensitivity).",
          u8"瞄准镜触发时按比例降低手柄瞄准灵敏度（类似开镜灵敏度）。" },
        { u8"Accepts 0~1 or 0~100; comma list matches ScopeMagnification order.",
          u8"支持 0~1 或 0~100；也支持逗号列表，对应 ScopeMagnification 的档位顺序。" },
        0.0f, 0.0f,
        "100"
    },

    // Motion Gestures
    {
        "MotionGestureSwingThreshold",
        OptionType::Float,
        { u8"Motion Gestures", u8"动作手势" },
        { u8"Swing Gesture Threshold", u8"挥动判定阈值" },
        { u8"Velocity needed to detect a swing gesture.",
          u8"判定挥动手势所需的速度阈值。" },
        { u8"Increase to reduce false swings.",
          u8"提高可减少误判。" },
        0.5f, 4.0f,
        "2.1"
    },
    {
        "MotionGestureDownSwingThreshold",
        OptionType::Float,
        { u8"Motion Gestures", u8"动作手势" },
        { u8"Down Swing Threshold", u8"下劈判定阈值" },
        { u8"Velocity threshold for downward swing recognition.",
          u8"判定下劈动作的速度阈值。" },
        { u8"", u8"" },
        0.5f, 4.0f,
        "2.0"
    },
    {
        "MotionGestureJumpThreshold",
        OptionType::Float,
        { u8"Motion Gestures", u8"动作手势" },
        { u8"Jump Gesture Threshold", u8"跳跃手势阈值" },
        { u8"Vertical velocity required to trigger jump gesture.",
          u8"触发跳跃手势所需的垂直速度。" },
        { u8"", u8"" },
        0.5f, 4.0f,
        "2.0"
    },
    {
        "MotionGestureCooldown",
        OptionType::Float,
        { u8"Motion Gestures", u8"动作手势" },
        { u8"Gesture Cooldown", u8"手势冷却" },
        { u8"Minimum seconds between repeated gesture activations.",
          u8"重复手势触发之间的最小间隔（秒）。" },
        { u8"", u8"" },
        0.0f, 2.0f,
        "0.8"
    },
    {
        "MotionGestureHoldDuration",
        OptionType::Float,
        { u8"Motion Gestures", u8"动作手势" },
        { u8"Gesture Hold Duration", u8"手势按住时长" },
        { u8"Seconds a gesture must be held to count as intentional.",
          u8"手势需保持的秒数，超过才判定为有效。" },
        { u8"", u8"" },
        0.0f, 1.0f,
        "0.2"
    },
    // Inventory Anchors
    {
        "InventoryGestureRange",
        OptionType::Float,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Inventory Gesture Range", u8"物品手势触发距离" },
        { u8"How far the hand can move from the body to trigger inventory gestures (meters).",
          u8"手部离身体多远触发物品手势（米）。" },
        { u8"0.15~0.35 works for most arm lengths.",
          u8"0.15~0.35 适合大多数身材。" },
        0.1f, 0.5f,
        "0.16"
    },
    {
        "InventoryChestOffset",
        OptionType::Vec3,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Chest Anchor Offset (x,y,z)", u8"胸前锚点偏移 (x,y,z)" },
        { u8"Offset from headset to chest anchor in meters.",
          u8"从头部到胸前锚点的米制偏移。" },
        { u8"Adjust per body size. Positive X = forward.",
          u8"根据体型调整。X 正为前方。" },
        -0.6f, 0.6f,
        "0.20,0.0,-0.20"
    },
    {
        "InventoryBackOffset",
        OptionType::Vec3,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Back Anchor Offset (x,y,z)", u8"背部锚点偏移 (x,y,z)" },
        { u8"Offset from headset to back anchor in meters.",
          u8"从头部到背部锚点的米制偏移。" },
        { u8"", u8"" },
        -0.6f, 0.6f,
        "-0.25,0.0,-0.10"
    },
    {
        "InventoryLeftWaistOffset",
        OptionType::Vec3,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Left Waist Anchor Offset (x,y,z)", u8"左腰锚点偏移 (x,y,z)" },
        { u8"Offset from headset to left waist anchor in meters.",
          u8"从头部到左腰锚点的米制偏移。" },
        { u8"", u8"" },
        -0.6f, 0.6f,
        "0.05,-0.25,-0.50"
    },
    {
        "InventoryRightWaistOffset",
        OptionType::Vec3,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Right Waist Anchor Offset (x,y,z)", u8"右腰锚点偏移 (x,y,z)" },
        { u8"Offset from headset to right waist anchor in meters.",
          u8"从头部到右腰锚点的米制偏移。" },
        { u8"", u8"" },
        -0.6f, 0.6f,
        "0.05,0.25,-0.50"
    },
    {
        "ShowInventoryAnchors",
        OptionType::Bool,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Draw the item-switching grab zone.", u8"绘制道具切换区域" },
        "false"
   },

    // Custom Actions
    {
        "CustomAction1Command",
        OptionType::String,
        { u8"Custom Actions", u8"自定义动作" },
        { u8"Custom Action 1 Command", u8"自定义动作1指令" },
        { u8"Console command",
          u8"可填控制台指令" },
        { u8"Mapped to VR custom action slot 1.",
          u8"对应VR自定义动作槽1。" },
        0.0f, 0.0f,
        "thirdpersonshoulder"
    },
    {
        "CustomAction2Command",
        OptionType::String,
        { u8"Custom Actions", u8"自定义动作" },
        { u8"Custom Action 2 Command", u8"自定义动作2指令" },
        { u8"", u8"" },
        { u8"", u8"" },
        0.0f, 0.0f,
        "say !buy"
    },
    {
        "CustomAction3Command",
        OptionType::String,
        { u8"Custom Actions", u8"自定义动作" },
        { u8"Custom Action 3 Command", u8"自定义动作3指令" },
        { u8"", u8"" },
        { u8"", u8"" },
        0.0f, 0.0f,
        ""
    },
    {
        "CustomAction4Command",
        OptionType::String,
        { u8"Custom Actions", u8"自定义动作" },
        { u8"Custom Action 4 Command", u8"自定义动作4指令" },
        { u8"", u8"" },
        { u8"", u8"" },
        0.0f, 0.0f,
        ""
    },
    {
        "CustomAction5Command",
        OptionType::String,
        { u8"Custom Actions", u8"自定义动作" },
        { u8"Custom Action 5 Command", u8"自定义动作5指令" },
        { u8"", u8"" },
        { u8"", u8"" },
        0.0f, 0.0f,
        ""
    },

    // Special Infected Assist
    {
        "SpecialInfectedPreWarningDistance",
        OptionType::Float,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Pre-Warning Distance", u8"特感预警距离" },
        { u8"Triggers warning/auto-aim logic when within this range (units).",
          u8"特感进入此距离时触发预警/辅助（游戏单位）。" },
        { u8"Extend for more reaction time; shorten to reduce assists.",
          u8"想要更长反应时间可调大，想少辅助可调小。" },
        300.0f, 1200.0f,
        "850.0"
    },
    {
        "SpecialInfectedPreWarningAutoAimEnabled",
        OptionType::Bool,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Enable Pre-Warning Auto Aim", u8"启用预警自动瞄准" },
        { u8"Allows the pre-warning system to gently steer aim toward threats.",
          u8"允许预警系统轻微引导瞄准至威胁目标。" },
        { u8"Disable for pure warnings only.",
          u8"若只想要预警，可关闭。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "SpecialInfectedAutoAimLerp",
        OptionType::Float,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Auto Aim Lerp", u8"自动瞄准插值" },
        { u8"How quickly auto aim blends toward the target (0 = off).",
          u8"自动瞄准向目标插值的速度（0 表示关闭）。" },
        { u8"Kept within the in-game clamp of 0~0.3.",
          u8"与游戏内限制一致（0~0.3）。" },
        0.0f, 0.3f,
        "0.3"
    },
    {
        "SpecialInfectedAutoAimCooldown",
        OptionType::Float,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Auto Aim Cooldown", u8"自动瞄准冷却" },
        { u8"Minimum seconds between auto-aim assists.",
          u8"两次自动瞄准辅助之间的最小秒数。" },
        { u8"Game enforces at least 0.5s.",
          u8"游戏最少强制 0.5 秒。" },
        0.5f, 2.0f,
        "0.5"
    },
    {
        "SpecialInfectedPreWarningAimAngle",
        OptionType::Float,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Aim Assist Angle", u8"瞄准辅助角度" },
        { u8"Maximum angular offset allowed for auto-aim to engage.",
          u8"自动瞄准可介入的最大角度偏移。" },
        { u8"Clamped by game to 0~10 degrees.",
          u8"游戏会限制在 0~10 度。" },
        0.0f, 10.0f,
        "5.0"
    },
    {
        "SpecialInfectedPreWarningAimSnapDistance",
        OptionType::Float,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Aim Snap Distance", u8"瞄准锁定距离" },
        { u8"Distance at which aim lock starts (units).",
          u8"开始瞄准锁定的距离（游戏单位）。" },
        { u8"Kept within game clamp 0~20.",
          u8"遵循游戏 0~20 的限制。" },
        0.0f, 20.0f,
        "18.0"
    },
    {
        "SpecialInfectedPreWarningAimReleaseDistance",
        OptionType::Float,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Aim Release Distance", u8"瞄准释放距离" },
        { u8"Distance where auto aim stops (>= snap distance).",
          u8"自动瞄准停止的距离（需 ≥ 锁定距离）。" },
        { u8"Game clamps to 0~30.",
          u8"游戏限制 0~30。" },
        0.0f, 30.0f,
        "28.0"
    },
    {
        "SpecialInfectedPreWarningAimOffsetBoomer",
        OptionType::Vec3,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Boomer Aim Offset (x,y,z)", u8"胖子瞄准偏移 (x,y,z)" },
        { u8"Offset applied when auto-aiming at a Boomer (units).",
          u8"自动瞄准胖子时的偏移量（游戏单位）。" },
        { u8"", u8"" },
        -60.0f, 60.0f,
        "5,0,40"
    },
    {
        "SpecialInfectedPreWarningAimOffsetSmoker",
        OptionType::Vec3,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Smoker Aim Offset (x,y,z)", u8"烟鬼瞄准偏移 (x,y,z)" },
        { u8"", u8"" },
        { u8"", u8"" },
        -60.0f, 60.0f,
        "3,0,42"
    },
    {
        "SpecialInfectedPreWarningAimOffsetHunter",
        OptionType::Vec3,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Hunter Aim Offset (x,y,z)", u8"猎人瞄准偏移 (x,y,z)" },
        { u8"", u8"" },
        { u8"", u8"" },
        -60.0f, 60.0f,
        "5,0,20"
    },
    {
        "SpecialInfectedPreWarningAimOffsetSpitter",
        OptionType::Vec3,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Spitter Aim Offset (x,y,z)", u8"口水瞄准偏移 (x,y,z)" },
        { u8"", u8"" },
        { u8"", u8"" },
        -60.0f, 60.0f,
        "3,0,42"
    },
    {
        "SpecialInfectedPreWarningAimOffsetJockey",
        OptionType::Vec3,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Jockey Aim Offset (x,y,z)", u8"猴子瞄准偏移 (x,y,z)" },
        { u8"", u8"" },
        { u8"", u8"" },
        -60.0f, 60.0f,
        "10,0,20"
    },
    {
        "SpecialInfectedPreWarningAimOffsetCharger",
        OptionType::Vec3,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Charger Aim Offset (x,y,z)", u8"牛子瞄准偏移 (x,y,z)" },
        { u8"", u8"" },
        { u8"", u8"" },
        -60.0f, 60.0f,
        "3,0,48"
    },
    {
        "SpecialInfectedPreWarningAimOffsetTank",
        OptionType::Vec3,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Tank Aim Offset (x,y,z)", u8"坦克瞄准偏移 (x,y,z)" },
        { u8"", u8"" },
        { u8"", u8"" },
        -60.0f, 60.0f,
        "5,0,45"
    },
    {
        "SpecialInfectedPreWarningAimOffsetWitch",
        OptionType::Vec3,
        { u8"Special Infected Assist", u8"特感辅助" },
        { u8"Witch Aim Offset (x,y,z)", u8"妹子瞄准偏移 (x,y,z)" },
        { u8"", u8"" },
        { u8"", u8"" },
        -60.0f, 60.0f,
        "5,0,25"
    }
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
            bool v = ParseBool(GetStr(key), GetDefaultBool(opt));
            if (ImGui::Checkbox(L(opt.title), &v))
                g_Values[key] = v ? "true" : "false";
            DrawHelp(opt);
            break;
        }
        case OptionType::Float:
        {
            float v = GetFloat(key, GetDefaultFloat(opt));
            v = std::clamp(v, opt.min, opt.max);
            if (ImGui::SliderFloat(L(opt.title), &v, opt.min, opt.max, "%.3f"))
                g_Values[key] = std::to_string(v);
            DrawHelp(opt);
            break;
        }
        case OptionType::Int:
        {
            int v = GetInt(key, GetDefaultInt(opt));
            v = std::clamp(v, (int)opt.min, (int)opt.max);
            if (ImGui::SliderInt(L(opt.title), &v, (int)opt.min, (int)opt.max))
                g_Values[key] = std::to_string(v);
            DrawHelp(opt);
            break;
        }
        case OptionType::Color:
        {
            ImVec4 c = GetColor(opt, ImVec4(1, 1, 1, 1));
            if (ImGui::ColorEdit4(L(opt.title), (float*)&c))
                SetColor(key, c);
            DrawHelp(opt);
            break;
        }
        case OptionType::String:
        {
            std::string value = GetStr(key);
            if (value.empty())
                value = GetDefaultStr(opt);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", value.c_str());
            if (ImGui::InputText(L(opt.title), buf, IM_ARRAYSIZE(buf)))
                g_Values[key] = buf;
            DrawHelp(opt);
            break;
        }
        case OptionType::Vec3:
        {
            Vec3 v = GetVec3(opt, GetVec3Default(opt, { 0.f, 0.f, 0.f }));
            v.x = std::clamp(v.x, opt.min, opt.max);
            v.y = std::clamp(v.y, opt.min, opt.max);
            v.z = std::clamp(v.z, opt.min, opt.max);
            float arr[3] = { v.x, v.y, v.z };
            if (ImGui::SliderFloat3(L(opt.title), arr, opt.min, opt.max, "%.3f"))
            {
                Vec3 newVal{ arr[0], arr[1], arr[2] };
                SetVec3(key, newVal);
            }
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
