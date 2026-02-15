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

    {
        "ThirdPersonCameraBindToHead",
        OptionType::Bool,
        { u8"Camera / Third Person", u8"相机 / 第三人称" },
        { u8"Bind 3P render camera to head", u8"第三人称相机绑定头部" },
        { u8"When third-person rendering is active, render from the player head/eye position instead of the engine shoulder camera. This helps prevent seeing your own head while moving.",
          u8"第三人称渲染时，使用玩家头部/眼睛位置作为相机，而不是引擎提供的肩部相机。可避免移动时看到自己头部。" },
        { u8"Disabled for death/observer and view-entity cutscenes.",
          u8"死亡/观察者与视角实体（过场镜头）时不会启用。" },
        0.0f, 0.0f,
        "false"
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
        "MoveDirectionFromController",
        OptionType::Bool,
        { u8"Input / Turning", u8"输入 / 转向" },
        { u8"Controller determines the direction of movement", u8"手柄决定前进方向" },
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
        "MouseModeEnabled",
        OptionType::Bool,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Enable Mouse Mode", u8"启用键鼠模式" },
        { u8"Enables desktop-style mouse aiming while staying in VR rendering.",
          u8"启用桌面式键鼠瞄准，但仍保持VR渲染。" },
        { u8"Mouse X turns your body; mouse Y aims (and optionally tilts view).",
          u8"鼠标X控制转身；鼠标Y控制俯仰（可选同时带动视角）。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "MouseModeYawSensitivity",
        OptionType::Float,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse Yaw Sensitivity", u8"鼠标水平灵敏度" },
        { u8"How much mouse X rotates yaw per count.",
          u8"鼠标水平每个计数导致的转向幅度。" },
        { u8"Negative values invert direction.",
          u8"负值可反向。" },
        -0.2f, 0.2f,
        "0.022"
    },
    {
        "MouseModePitchSensitivity",
        OptionType::Float,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse Pitch Sensitivity", u8"鼠标垂直灵敏度" },
        { u8"How much mouse Y changes pitch per count.",
          u8"鼠标垂直每个计数导致的俯仰变化。" },
        { u8"Negative values invert direction.",
          u8"负值可反向。" },
        -0.2f, 0.2f,
        "0.022"
    },
    {
        "MouseModePitchAffectsView",
        OptionType::Bool,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse Pitch Tilts View", u8"鼠标俯仰带动视角" },
        { u8"When enabled, mouse Y also tilts the rendered view up/down (adds a pitch offset on top of HMD tracking).",
          u8"开启后，鼠标Y也会带动渲染视角上下俯仰（在头显追踪基础上叠加俯仰偏移）。" },
        { u8"Useful if aiming at high/low targets is difficult without moving your head. May increase motion sickness.",
          u8"适合不想抬头/低头也能全方向瞄准的场景，但可能更容易晕。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "MouseModeTurnSmoothing",
        OptionType::Float,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Yaw Smoothing (seconds)", u8"水平平滑 (秒)" },
        { u8"Smooths mouse-driven yaw (turning) to avoid 'stepping' when CreateMove rate < VR refresh.",
          u8"对鼠标驱动的水平转向做平滑，避免 CreateMove 频率低于 VR 刷新率时出现台阶感。" },
        { u8"0 disables smoothing. Typical: 0.03~0.08.",
          u8"0 关闭平滑。常用：0.03~0.08。" },
        0.0f, 0.25f,
        "0.05"
    },
    {
        "MouseModePitchSmoothing",
        OptionType::Float,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Pitch Smoothing (seconds)", u8"垂直平滑 (秒)" },
        { u8"Smooths mouse-driven pitch (aim up/down) to avoid stutter when CreateMove rate < VR refresh.",
          u8"对鼠标驱动的垂直瞄准做平滑，避免 CreateMove 频率低于 VR 刷新率时出现卡顿/台阶。" },
        { u8"0 disables smoothing. Typical: 0.03~0.08.",
          u8"0 关闭平滑。常用：0.03~0.08。" },
        0.0f, 0.25f,
        "0.05"
    },
    {
        "MouseModeViewmodelAnchorOffset",
        OptionType::Vec3,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse Mode Viewmodel Anchor Offset (m)", u8"鼠标模式 Viewmodel 锚点偏移 (米)" },
        { u8"HMD-local offset for the fixed viewmodel/aim origin (meters).",
          u8"基于 HMD 的固定 viewmodel/瞄准起点偏移（米制）。" },
        { u8"X=forward, Y=right, Z=up. Tune so the gun sits where you want.",
          u8"X=前方，Y=右方，Z=上方。调到枪在你想要的位置。" },
        -1.0f, 1.0f,
        "0.22,0.00,-0.18"
    },
    {
        "MouseModeAimConvergeDistance",
        OptionType::Float,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse Mode Convergence Distance (units)", u8"鼠标模式汇聚距离 (单位)" },
        { u8"Scheme B: viewmodel ray is steered to intersect the HMD-center ray at this distance (Source units).",
          u8"方案B：让 viewmodel 发出的瞄准射线在该距离与视线中心射线相交（Source 单位）。" },
        { u8"Helps keep the aim line near screen center even if you move the anchor. 2048~4096 is common.",
          u8"即使调整锚点，也能让瞄准线远处回到视野中心。常用 2048~4096。" },
        0.0f, 8192.0f,
        "2048"
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
        "ControllerHudCut",
        OptionType::Bool,
        { u8"HUD (Controller)", u8"HUD（手柄）" },
        { u8"HUD clipping", u8"HUD裁切" },
        { u8"cutting the HUD at the bottom-left and bottom-right corners to the controllers.",
          u8"将左下角和右下角的hud裁切到手柄。" },
        "false"
     },
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
        "false"
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
        { u8"Inventory Gesture Activation Range", u8"道具锚点抓取的有效范围" },
        { u8"Distance from the inventory anchor within which grabbing is allowed (meters).",
          u8"手部靠近道具栏多近时，允许触发物品抓取（米）。" },
        { u8"Grab is triggered by pressing the grip when the hand is inside this range.",
          u8"手进入范围后按下握键即可抓取道具。" },
        0.1f, 0.5f,
        "0.16"
    },
    {
        "InventoryChestOffset",
        OptionType::Vec3,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Chest Anchor Offset (x,y,z)", u8"医疗包锚点偏移 (x,y,z)" },
        { u8"Offset from the chest reference point to the medkit anchor (meters).",
          u8"从胸口基准点到医疗包锚点的米制偏移。" },
        { u8"Default position is directly in front of the head, centered.",
          u8"默认位于头顶正前方中央位置。" },
        -0.6f, 0.6f,
        "0.20,0.0,-0.20"
    },
    {
        "InventoryBackOffset",
        OptionType::Vec3,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Weapon Switch Anchor Offset (x,y,z)", u8"主/副武器切换锚点偏移 (x,y,z)" },
        { u8"Offset from the chest reference point to the weapon switch anchor (meters).",
          u8"从胸口基准点到武器切换锚点的米制偏移。" },
        { u8"By default, this anchor is still located at the chest.",
          u8"默认仍位于胸口位置。" },
        -0.6f, 0.6f,
        "-0.25,0.0,-0.10"
    },
    {
        "InventoryLeftWaistOffset",
        OptionType::Vec3,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Left Waist Anchor Offset (x,y,z)", u8"投掷物锚点偏移 (x,y,z)" },
        { u8"Offset from the chest reference point to the throwable item anchor (meters).",
          u8"从胸口基准点到投掷物锚点的米制偏移。" },
        { u8"Default position is in front of the head, to the left.",
          u8"默认位于头顶左前方区域。" },
        -0.6f, 0.6f,
        "0.05,-0.25,-0.50"
    },
    {
        "InventoryRightWaistOffset",
        OptionType::Vec3,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Right Waist Anchor Offset (x,y,z)", u8"药片/兴奋剂锚点偏移 (x,y,z)" },
        { u8"Offset from the chest reference point to the pills/adrenaline anchor (meters).",
          u8"从胸口基准点到药片/兴奋剂锚点的米制偏移。" },
        { u8"Default position is in front of the head, to the right.",
          u8"默认位于头顶右前方区域。" },
        -0.6f, 0.6f,
        "0.05,0.25,-0.50"
    },
    {
        "ShowInventoryAnchors",
        OptionType::Bool,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Show Inventory Grab Zones",
          u8"显示道具栏抓取区域（不熟悉位置时建议开启）" },
        { u8"Draws visible grab zones for inventory anchors.",
          u8"在世界中绘制可视化的道具栏抓取区域。" },
        { u8"Recommended for learning anchor positions; can be disabled once familiar.",
          u8"熟悉各锚点位置后可关闭以减少视觉干扰。" },
        0.0f, 0.0f,
        "false"
    },
       // Optics (Scope / Rear Mirror)
    {
        "ScopeEnabled",
        OptionType::Bool,
        { u8"Optics", u8"光学" },
        { u8"Enable Gun Scope", u8"启用枪械瞄准镜" },
        { u8"Renders a gun-mounted scope view to an overlay.",
          u8"将枪械瞄准镜画面渲染到一个覆盖层上。" },
        { u8"If disabled, scope rendering and overlay are skipped.",
          u8"关闭后将不再渲染瞄准镜及其覆盖层。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "ScopeRTTSize",
        OptionType::Int,
        { u8"Optics", u8"光学" },
        { u8"Scope Render Texture Size", u8"瞄准镜渲染分辨率" },
        { u8"Render target size (pixels) for the scope view.",
          u8"瞄准镜画面渲染目标尺寸（像素）。" },
        { u8"Higher is sharper but costs more GPU time.",
          u8"越高越清晰，但GPU开销更大。" },
        128.f, 1024.f,
        "512"
    },
    {
        "ScopeFov",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Scope Field of View", u8"瞄准镜视野(FOV)" },
        { u8"Camera FOV used when rendering through the scope (degrees).",
          u8"瞄准镜渲染相机使用的视野角（度）。" },
        { u8"Smaller FOV = higher magnification.",
          u8"FOV 越小 = 倍率越高。" },
        5.0f, 45.0f,
        "30"
    },
    {
        "ScopeMagnification",
        OptionType::String,
        { u8"Optics", u8"光学" },
        { u8"Scope Magnification Steps (FOV list)", u8"瞄准镜倍率档位（FOV列表）" },
        { u8"Comma-separated list of scope FOV values used as toggle steps.SteamVR button binding to toggle Scope Magnification.",
          u8"用逗号分隔的一组瞄准镜FOV，用于切换倍率档位。Steamvr按键绑定选择Toggle Scope Magnification使用" },
        { u8"Example: 20,15,10,5. Values are clamped to 5~45.",
          u8"示例：20,15,10,5。数值会被限制在 5~45。" },
        0.0f, 0.0f,
        "20,15,10,5"
    },
    {
        "ScopeAimSensitivityScale",
        OptionType::String,
        { u8"Scope", u8"瞄准镜" },
        { u8"Scoped Aim Sensitivity Scale", u8"开镜灵敏度缩放" },
        { u8"Scales controller aim delta when scope is active (ADS / zoom sensitivity).",
          u8"瞄准镜触发时按比例降低手柄瞄准灵敏度（类似开镜灵敏度）。" },
        { u8"Accepts 0~1 or 0~100; supports comma list matching ScopeMagnification.",
          u8"支持 0~1 或 0~100；也支持逗号列表，对应 ScopeMagnification 的档位顺序。" },
        0.0f, 0.0f,
        "100,100,100,100"
     },
    {
        "ScopeZNear",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Scope Camera Near Plane", u8"瞄准镜近裁剪面" },
        { u8"Near clip distance for the scope camera.",
          u8"瞄准镜相机的近裁剪距离。" },
        { u8"Increase if the scope camera clips through nearby geometry.",
          u8"如果近处几何体被裁剪/穿模，可适当调大。" },
        0.1f, 6.0f,
        "2"
    },
    {
        "ScopeOverlayWidthMeters",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Scope Overlay Width", u8"瞄准镜覆盖层宽度" },
        { u8"Physical width of the scope overlay quad (meters).",
          u8"瞄准镜覆盖层平面的物理宽度（米）。" },
        { u8"Adjust to match your scope model size.",
          u8"调到与瞄准镜模型大小一致即可。" },
        0.01f, 0.3f,
        "0.15"
    },
    {
        "ScopeOverlayXOffset",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Scope Overlay X Offset", u8"瞄准镜覆盖层X偏移" },
        { u8"Overlay offset along controller forward axis (meters).",
          u8"覆盖层沿控制器前方轴的偏移（米）。" },
        { u8"Positive moves it forward.",
          u8"正数更靠前。" },
        -0.1f, 0.1f,
        "0.02"
    },
    {
        "ScopeOverlayYOffset",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Scope Overlay Y Offset", u8"瞄准镜覆盖层Y偏移" },
        { u8"Overlay offset along controller right axis (meters).",
          u8"覆盖层沿控制器右侧轴的偏移（米）。" },
        { u8"Positive moves it to the right.",
          u8"正数更靠右。" },
        -0.1f, 0.1f,
        "0.04"
    },
    {
        "ScopeOverlayZOffset",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Scope Overlay Z Offset", u8"瞄准镜覆盖层Z偏移" },
        { u8"Overlay offset along controller up axis (meters).",
          u8"覆盖层沿控制器上方轴的偏移（米）。" },
        { u8"Positive moves it up.",
          u8"正数更靠上。" },
        -0.1f, 0.1f,
        "-0.06"
    },
    {
        "ScopeOverlayAngleOffset",
        OptionType::Vec3,
        { u8"Optics", u8"光学" },
        { u8"Scope Overlay Angle Offset (pitch,yaw,roll)", u8"瞄准镜覆盖层角度偏移 (俯仰,偏航,翻滚)" },
        { u8"Additional rotation for the scope overlay quad (degrees).",
          u8"瞄准镜覆盖层平面的额外旋转（度）。" },
        { u8"Use this to make the scope face your eye correctly.",
          u8"用于让瞄准镜平面正确朝向眼睛。" },
        -180.f, 180.f,
        "-65,-5,-5"
    },
    {
        "ScopeRequireLookThrough",
        OptionType::Bool,
        { u8"Optics", u8"光学" },
        { u8"Require Looking Through Scope", u8"需要贴近瞄准镜才显示" },
        { u8"Only shows the scope overlay when your eye is close and aligned.",
          u8"仅在眼睛靠近并对准时才显示瞄准镜覆盖层。" },
        { u8"Disabling makes the overlay always visible (useful for testing).",
          u8"关闭后覆盖层总显示（便于调试）。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "ScopeLookThroughDistanceMeters",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Look-Through Distance", u8"贴近距离" },
        { u8"Max eye-to-scope distance to count as looking through (meters).",
          u8"眼睛到瞄准镜的最大距离（米），小于该值才算在看镜。" },
        { u8"Larger is more forgiving.",
          u8"越大越宽松。" },
        0.01f, 2.0f,
        "0.5"
    },
    {
        "ScopeLookThroughAngleDeg",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Look-Through Angle", u8"贴近角度" },
        { u8"Max angle off the scope axis to count as looking through (degrees).",
          u8"视线偏离瞄准镜轴线的最大角度（度）。" },
        { u8"Smaller is stricter; larger is more forgiving.",
          u8"越小越严格，越大越宽松。" },
        1.0f, 89.0f,
        "60"
    },
    {
        "ScopeOverlayAlwaysVisible",
        OptionType::Bool,
        { u8"Optics", u8"光学" },
        { u8"Scope Overlay Always Visible", u8"瞄准镜覆盖层始终可见" },
        { u8"Keeps the scope overlay visible even when not looking through.",
          u8"即使没有贴近瞄准镜，也保持覆盖层可见。" },
        { u8"Useful for aligning/previewing the overlay.",
          u8"用于对齐/预览覆盖层位置。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "ScopeOverlayIdleAlpha",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Scope Idle Alpha", u8"瞄准镜闲置透明度" },
        { u8"Overlay alpha when not looking through (0~1).",
          u8"未贴近时覆盖层透明度（0~1）。" },
        { u8"0 = invisible, 1 = fully opaque.",
          u8"0=完全透明，1=完全不透明。" },
        0.0f, 1.0f,
        "0.5"
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
