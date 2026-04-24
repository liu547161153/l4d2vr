#include "Options.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <cctype>
#include <cstdlib>
#include <cfloat>
#include <algorithm>
#include <sstream>
#include <vector>

// Search box text (typed in main.cpp)
char g_OptionSearch[128] = "";

static const char* L(const L10nText& t)
{
    return (g_UseChinese && t.zh && *t.zh) ? t.zh : t.en;
}

static const char* kAllowedDefaultsText = R"(VRScale=43.2
IPDScale=1.0
TurnSpeed=0.3
SnapTurning=false
SnapTurnAngle=45.0
LeftHanded=false
AntiAliasing=0

ThirdPersonCameraFollowHmd=true
ThirdPersonVRCameraOffset=38

AutoMatQueueMode=false
WriteOnlyPerformanceTweaksEnabled=false


LeftWristHudEnabled=false
LeftWristHudWidthMeters=0.1
LeftWristHudXOffset=0.01
LeftWristHudYOffset=0.01
LeftWristHudZOffset=-0.0
LeftWristHudAngleOffset=-75,0,0
RightAmmoHudEnabled=false
RightAmmoHudWidthMeters=0.8
RightAmmoHudXOffset=-0.07
RightAmmoHudYOffset=0.03
RightAmmoHudZOffset=-0.09
RightAmmoHudAngleOffset=-75,0,0

ForceNonVRServerMovement=false
MoveDirectionFromController=false

cmd=exec cam.cfg
HudDistance=1.3
HudSize=1.3
HudAlwaysVisible=false
ControllerSmoothing=0.0
HideArms=false
ViewmodelDisableMoveBob=false

RequireSecondaryAttackForItemSwitch=false
VoiceRecordCombo=Crouch+Reload
QuickTurnCombo=SecondaryAttack+Crouch
ViewmodelAdjustEnabled=false
ViewmodelAdjustCombo=Reload+SecondaryAttack

AimLineOnlyWhenLaserSight=false
BlockFireOnFriendlyAimEnabled=false

D3DAimLineOverlayEnabled=false
D3DAimLineOverlayWidthPixels=2.0
D3DAimLineOverlayOutlinePixels=1.0
D3DAimLineOverlayEndpointPixels=1.5
D3DAimLineOverlayColor=255,0,0,100
D3DAimLineOverlayOutlineColor=255,0,0,1

MotionGestureSwingThreshold=2
MotionGesturePushThreshold=1.5
MotionGestureDownSwingThreshold=2.0
MotionGestureJumpThreshold=2.0
MotionGestureCooldown=0.8
MotionGestureHoldDuration=0.2


InventoryGestureRange=0.16
InventoryBodyOriginOffset=-0.1,0.0,-0.28
InventoryChestOffset=0.45,0.0,0.5
InventoryBackOffset=0.12,0.0,0.1
InventoryLeftWaistOffset=0.45,-0.28,0.25
InventoryRightWaistOffset=0.45,0.28,0.25
 
ShowInventoryAnchors=true
InventoryAnchorColor=0,255,255,255
InventoryHudMarkerDistance=0.45
InventoryHudMarkerUpOffset=-0.10
InventoryHudMarkerSeparation=0.14

InventoryQuickSwitchEnabled=true
InventoryQuickSwitchOffset=0.05,0.2,0.2  
InventoryQuickSwitchZoneRadius=0.15    

CustomAction1Command=thirdpersonshoulder
CustomAction2Command=
CustomAction3Command=
CustomAction4Command=
CustomAction5Command=

ScopeEnabled=false
ScopeRTTSize=512
ScopeFov=20
ScopeMagnification=20,15,5,3
ScopeAimSensitivityScale=60,40,35,15
ScopeZNear=2
ScopeCameraOffset=12,0,3
ScopeCameraAngleOffset=0,0,0
ScopeOverlayWidthMeters=0.3
ScopeOverlayXOffset=0.02
ScopeOverlayYOffset=0.04
ScopeOverlayZOffset=-0.06
ScopeOverlayAngleOffset=-45,-5,-5
ScopeRequireLookThrough=true
ScopeLookThroughDistanceMeters=0.5
ScopeLookThroughAngleDeg=60
ScopeOverlayAlwaysVisible=false
ScopeOverlayIdleAlpha=0.5
ScopeStabilizationEnabled=true
ScopeStabilizationMinCutoff=0.5
ScopeStabilizationBeta=0.5
ScopeStabilizationDCutoff=1.0


MouseModeEnabled=false
MouseModeYawSensitivity=0.01
MouseModePitchSensitivity=0.01
MouseModeViewmodelAnchorOffset=0.42,0.0,-0.28
MouseModeAimConvergeDistance=2048
MouseModeTurnSmoothing=0.03
MouseModePitchSmoothing=0.03
MouseModePitchAffectsView=true
MouseModeAimFromHmd=true
MouseModeScopedViewmodelAnchorOffset=0.35,0.0,-0.13
MouseModeHmdAimSensitivity=1
MouseModeScopeOverlayOffset=0,-0.02,-0.3
MouseModeScopeOverlayAngleOffset=0,0,0
MouseModeScopeSensitivityScale=50,25,15,5
MouseModeScopeToggleKey=key:q
MouseModeScopeMagnificationKey=key:z

AutoRepeatSemiAutoFire=false
AutoRepeatSemiAutoFireHz=20.0
AutoRepeatSprayPushEnabled=false
AutoRepeatSprayPushDelayTicks=0
AutoRepeatSprayPushHoldTicks=1
AutoFastMelee=false
AutoFastMeleeShoveEcho=true
AutoFastMeleeUseWeaponSwitch=true
AutoFastMeleePushWaitTicks=2
AutoFastMeleePostWaitTicks=29

HitSoundEnabled=false
KillSoundEnabled=false
HitSoundSpec=game:vrmod/hit.mp3
KillSoundNormalSpec=game:vrmod/kill.mp3
KillSoundHeadshotSpec=game:vrmod/headshot.mp3
HitSoundVolume=1.2
KillSoundVolume=1.8
HeadshotSoundVolume=1.3

HitIndicatorEnabled=false
KillIndicatorEnabled=false
KillIndicatorMaterialBaseSpec=overlays/2965700751


ShadowTweaksEnabled=false
EffectiveAttackRangeAutoFireEnabled=false
AutoFlashlightEnabled=true
)";

static const std::unordered_map<std::string, std::string>& GetAllowedDefaultOverrides()
{
    static const std::unordered_map<std::string, std::string> overrides = []()
    {
        std::unordered_map<std::string, std::string> map;
        std::istringstream stream(kAllowedDefaultsText);
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;

            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            map.emplace(line.substr(0, eq), line.substr(eq + 1));
        }
        return map;
    }();

    return overrides;
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
    if (opt.key)
    {
        const auto& overrides = GetAllowedDefaultOverrides();
        const auto it = overrides.find(opt.key);
        if (it != overrides.end())
            return it->second;
    }

    return (opt.defaultValue && *opt.defaultValue) ? std::string(opt.defaultValue) : std::string();
}

bool IsAllowedOptionKey(const std::string& key)
{
    return GetAllowedDefaultOverrides().find(key) != GetAllowedDefaultOverrides().end();
}

std::string GetOptionDefaultValue(const Option& opt)
{
    return GetDefaultStr(opt);
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

static bool GetBool(const std::string& key, bool defVal)
{
    return ParseBool(GetStr(key), defVal);
}

static bool StartsWith(const char* value, const char* prefix)
{
    if (!value || !prefix) return false;
    const size_t len = std::strlen(prefix);
    return std::strncmp(value, prefix, len) == 0;
}

static bool IsEnabled(const char* key, bool defVal = false)
{
    return GetBool(key, defVal);
}

static bool GetHitKillIndicatorsEnabled()
{
    return GetBool("KillIndicatorEnabled", false) || GetBool("HitIndicatorEnabled", false);
}

static void SetHitKillIndicatorsEnabled(bool enabled)
{
    const char* value = enabled ? "true" : "false";
    g_Values["KillIndicatorEnabled"] = value;
    g_Values["HitIndicatorEnabled"] = value;
}

static bool IsOptionVisible(const Option& opt)
{
    const char* key = opt.key;

    if (std::strcmp(key, "HitIndicatorEnabled") == 0)
        return false;

    if (std::strcmp(key, "AimLineOnlyWhenLaserSight") == 0)
        return IsEnabled("D3DAimLineOverlayEnabled");

    // Key groups hidden behind their main feature toggles.
    if (StartsWith(key, "MouseMode") && std::strcmp(key, "MouseModeEnabled") != 0)
        return IsEnabled("MouseModeEnabled");

    if (StartsWith(key, "LeftWristHud") && std::strcmp(key, "LeftWristHudEnabled") != 0)
        return IsEnabled("LeftWristHudEnabled");

    if (StartsWith(key, "RightAmmoHud") && std::strcmp(key, "RightAmmoHudEnabled") != 0)
        return IsEnabled("RightAmmoHudEnabled");

    if (StartsWith(key, "InventoryQuickSwitch") && std::strcmp(key, "InventoryQuickSwitchEnabled") != 0)
        return IsEnabled("InventoryQuickSwitchEnabled");

    if (StartsWith(key, "D3DAimLineOverlay") && std::strcmp(key, "D3DAimLineOverlayEnabled") != 0)
        return IsEnabled("D3DAimLineOverlayEnabled");

    if (StartsWith(key, "Scope") && std::strcmp(key, "ScopeEnabled") != 0)
    {
        if (!IsEnabled("ScopeEnabled"))
            return false;

        // Look-through tuning options only matter when look-through gating is enabled.
        if ((std::strcmp(key, "ScopeLookThroughDistanceMeters") == 0 || std::strcmp(key, "ScopeLookThroughAngleDeg") == 0) &&
            !IsEnabled("ScopeRequireLookThrough"))
            return false;

        if (StartsWith(key, "ScopeStabilization") && std::strcmp(key, "ScopeStabilizationEnabled") != 0)
            return IsEnabled("ScopeStabilizationEnabled", true);
    }

    // Individual dependency rules.
    if (std::strcmp(key, "SnapTurnAngle") == 0)
        return IsEnabled("SnapTurning");

    if (StartsWith(key, "Roomscale1To1") && std::strcmp(key, "Roomscale1To1Movement") != 0)
        return IsEnabled("Roomscale1To1Movement");

    if (std::strcmp(key, "ViewmodelAdjustCombo") == 0)
        return IsEnabled("ViewmodelAdjustEnabled");

    // Queued-render tuning options are only meaningful when AutoMatQueueMode is enabled.
    if (StartsWith(key, "QueuedRender"))
        return IsEnabled("AutoMatQueueMode");

    if (std::strcmp(key, "r_shadows") == 0 ||
        std::strcmp(key, "ShadowEntityTweaksEnabled") == 0 ||
        std::strcmp(key, "r_shadowrendertotexture") == 0 ||
        std::strcmp(key, "r_flashlightdepthtexture") == 0 ||
        std::strcmp(key, "r_flashlightdepthres") == 0 ||
        std::strcmp(key, "r_shadow_half_update_rate") == 0 ||
        std::strcmp(key, "r_shadowmaxrendered") == 0 ||
        std::strcmp(key, "cl_max_shadow_renderable_dist") == 0 ||
        std::strcmp(key, "r_FlashlightDetailProps") == 0 ||
        std::strcmp(key, "z_mob_simple_shadows") == 0 ||
        std::strcmp(key, "r_shadowfromworldlights") == 0 ||
        std::strcmp(key, "r_flashlightmodels") == 0 ||
        std::strcmp(key, "r_shadows_on_renderables_enable") == 0 ||
        std::strcmp(key, "r_flashlightrendermodels") == 0 ||
        std::strcmp(key, "cl_player_shadow_dist") == 0 ||
        std::strcmp(key, "z_infected_shadows") == 0 ||
        std::strcmp(key, "nb_shadow_blobby_dist") == 0 ||
        std::strcmp(key, "nb_shadow_cull_dist") == 0 ||
        std::strcmp(key, "r_flashlightinfectedshadows") == 0 ||
        std::strcmp(key, "ShadowControlDisableShadows") == 0 ||
        std::strcmp(key, "ShadowControlMaxDist") == 0 ||
        std::strcmp(key, "ShadowControlLocalLightShadows") == 0 ||
        std::strcmp(key, "ProjectedTextureEnableShadows") == 0 ||
        std::strcmp(key, "ProjectedTextureShadowQuality") == 0)
    {
        if (!IsEnabled("ShadowTweaksEnabled"))
            return false;

        if (std::strcmp(key, "r_flashlightdepthres") == 0)
            return IsEnabled("r_flashlightdepthtexture", true);

        if ((std::strcmp(key, "ShadowControlDisableShadows") == 0 ||
             std::strcmp(key, "ShadowControlMaxDist") == 0 ||
             std::strcmp(key, "ShadowControlLocalLightShadows") == 0 ||
             std::strcmp(key, "ProjectedTextureEnableShadows") == 0 ||
             std::strcmp(key, "ProjectedTextureShadowQuality") == 0) &&
            !IsEnabled("ShadowEntityTweaksEnabled"))
        {
            return false;
        }

        if (std::strcmp(key, "ProjectedTextureShadowQuality") == 0)
            return IsEnabled("ProjectedTextureEnableShadows", true);
    }

    if (std::strcmp(key, "AutoRepeatSemiAutoFireHz") == 0 ||
        std::strcmp(key, "AutoRepeatSprayPushEnabled") == 0)
        return IsEnabled("AutoRepeatSemiAutoFire");

    if (std::strcmp(key, "AutoRepeatSprayPushDelayTicks") == 0 ||
        std::strcmp(key, "AutoRepeatSprayPushHoldTicks") == 0)
    {
        return IsEnabled("AutoRepeatSemiAutoFire") && IsEnabled("AutoRepeatSprayPushEnabled");
    }

    if (StartsWith(key, "AutoFastMelee") && std::strcmp(key, "AutoFastMelee") != 0)
        return IsEnabled("AutoFastMelee");

    if (std::strcmp(key, "HitSoundSpec") == 0)
        return IsEnabled("HitSoundEnabled");

    if (std::strcmp(key, "HitSoundVolume") == 0)
        return IsEnabled("HitSoundEnabled") || IsEnabled("KillSoundEnabled");

    if (std::strcmp(key, "KillSoundNormalSpec") == 0 ||
        std::strcmp(key, "KillSoundHeadshotSpec") == 0 ||
        std::strcmp(key, "KillSoundVolume") == 0 ||
        std::strcmp(key, "HeadshotSoundVolume") == 0)
    {
        return IsEnabled("KillSoundEnabled");
    }

    if (std::strcmp(key, "FeedbackSoundSpatialBlend") == 0 ||
        std::strcmp(key, "FeedbackSoundSpatialRange") == 0)
        return IsEnabled("HitSoundEnabled") || IsEnabled("KillSoundEnabled");

    if (StartsWith(key, "KillIndicator") && std::strcmp(key, "KillIndicatorEnabled") != 0)
        return GetHitKillIndicatorsEnabled();

    if (std::strcmp(key, "EffectiveAttackRangeAutoFireEnabled") == 0 ||
        std::strcmp(key, "AimLineThickness") == 0 ||
        std::strcmp(key, "AimLineColor") == 0 ||
        std::strcmp(key, "AimLineMaxHz") == 0)
        return IsEnabled("AimLineEnabled");

    return true;
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
    const int r = std::clamp(static_cast<int>(c.x * 255.0f + 0.5f), 0, 255);
    const int g = std::clamp(static_cast<int>(c.y * 255.0f + 0.5f), 0, 255);
    const int b = std::clamp(static_cast<int>(c.z * 255.0f + 0.5f), 0, 255);
    const int a = std::clamp(static_cast<int>(c.w * 255.0f + 0.5f), 0, 255);
    std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d", r, g, b, a);
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
        ImGui::PushTextWrapPos(0.0f);
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
        "true"
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
        "MouseModeAimFromHmd",
        OptionType::Bool,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Aim From HMD", u8"从头显瞄准" },
        { u8"When enabled, mouse-mode aiming is driven by the HMD center ray instead of the fixed viewmodel anchor.",
          u8"开启后，键鼠模式的瞄准将改为使用头显中心射线，而不是固定的 viewmodel 锚点。" },
        { u8"Recommended if you want to keep holding the gun/stocks naturally and use the headset to fine-aim.",
          u8"适合希望保持握枪姿势不变、用头显做微调瞄准的场景。" },
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
        "MouseModeScopeSensitivityScale",
        OptionType::String,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse Mode Scoped Sensitivity Scale", u8"键鼠模式开镜灵敏度缩放" },
        { u8"Scales aim sensitivity while the mouse-mode scope overlay is active.",
          u8"键鼠模式的瞄准镜覆盖层开启时，对瞄准灵敏度进行缩放。" },
        { u8"Accepts 0~1 or 0~100; supports comma list matching magnification steps. Example: 50,25,15,5.",
          u8"支持 0~1 或 0~100；也支持逗号列表，对应倍率档位顺序。示例：50,25,15,5。" },
        0.0f, 0.0f,
        "50,25,15,5"
    },
    {
        "MouseModeScopeToggleKey",
        OptionType::String,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse Mode Scope Toggle Key", u8"键鼠模式开镜切换按键" },
        { u8"Keyboard key used to toggle the mouse-mode scope overlay on/off.",
          u8"用于开/关键鼠模式瞄准镜覆盖层的键盘按键。" },
        { u8"Format: key:<name> (e.g., key:6, key:f9). Leave empty to disable.",
          u8"格式：key:<按键名>（如 key:6、key:f9）。留空表示禁用。" },
        0.0f, 0.0f,
        "key:6"
    },
    {
        "MouseModeScopeMagnificationKey",
        OptionType::String,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse Mode Scope Magnification Key", u8"键鼠模式倍率切换按键" },
        { u8"Keyboard key used to cycle magnification steps while mouse-mode scope is active.",
          u8"键鼠模式开镜状态下，用于切换倍率档位的键盘按键。" },
        { u8"Format: key:<name> (e.g., key:7, key:f10). Leave empty to disable.",
          u8"格式：key:<按键名>（如 key:7、key:f10）。留空表示禁用。" },
        0.0f, 0.0f,
        "key:7"
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

    // Performance / Rendering
    {
        "AutoMatQueueMode",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Multi-core rendering", u8"多核渲染" },
        { u8"Turns on multi-core rendering for the mod. Do not enable the in-game multi-core rendering option.",
          u8"用于开启工具内的多核渲染功能，不要去开启游戏里的多核渲染选项。" },
        { u8"May cause ghosting. Not recommended for standing play.",
          u8"开启后可能出现重影，不建议站姿游玩时使用。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "ShadowTweaksEnabled",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Enable Shadow Optimization Controls", u8"启用阴影优化控制" },
        { u8"Simplify shadow rendering to improve performance.",
          u8"简化阴影显示，提高性能。" },
        { u8"",
          u8"" },
        0.0f, 0.0f,
        "false"
    },
    {
        "WriteOnlyPerformanceTweaksEnabled",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Enable Model Render Distance Tweaks", u8"启用模型渲染距离优化" },
        { u8"",
          u8"" },
        { u8"Useful if some models stay visible too far away and you want a more aggressive distance setup to save performance.",
          u8"如果有些模型在较远距离仍然参与渲染，想用更激进的距离设置来节省性能，可以开启。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "r_shadows",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Enable Dynamic Shadows", u8"启用动态阴影" },
        { u8"Master switch for dynamic shadows.",
          u8"动态阴影总开关。" },
        { u8"Disable only if you want the biggest frametime savings.",
          u8"只有在追求最大帧时间收益时才建议关闭。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "r_shadowrendertotexture",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Render Shadows To Texture", u8"阴影渲染到纹理" },
        { u8"Controls render-to-texture shadow maps used by the client shadow manager.",
          u8"控制客户端阴影管理器使用的阴影贴图渲染路径。" },
        { u8"Keeping this on preserves higher quality shadows, but it costs GPU time.",
          u8"保持开启能保留较高质量阴影，但会增加 GPU 开销。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "r_flashlightdepthtexture",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Flashlight Depth Shadows", u8"手电深度阴影" },
        { u8"Enables depth-texture shadows for flashlights and projected lights.",
          u8"启用手电和投射光使用的深度纹理阴影。" },
        { u8"Turning this off is a strong performance cut when flashlight shadows are expensive.",
          u8"当手电阴影很吃性能时，关闭它能明显减负。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "r_flashlightdepthres",
        OptionType::Int,
        { u8"Performance", u8"性能" },
        { u8"Flashlight Shadow Resolution", u8"手电阴影分辨率" },
        { u8"Resolution of the flashlight shadow depth texture.",
          u8"手电阴影深度纹理的分辨率。" },
        { u8"Lower values reduce GPU cost and VRAM use. 512 is a good balance in VR.",
          u8"越低越省 GPU 和显存。VR 下 512 通常是不错的平衡点。" },
        64.0f, 2048.0f,
        "256"
    },
    {
        "r_shadow_half_update_rate",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Half-Rate Shadow Updates", u8"阴影半速更新" },
        { u8"Updates client shadows at half the frame rate.",
          u8"让客户端阴影按半帧率更新。" },
        { u8"Usually one of the safest ways to shave shadow frametime.",
          u8"通常是最安全、最划算的阴影减负方式之一。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "r_shadowmaxrendered",
        OptionType::Int,
        { u8"Performance", u8"性能" },
        { u8"Max Rendered Shadows", u8"每帧最大阴影数量" },
        { u8"Caps how many shadows the client renders.",
          u8"限制客户端实际渲染的阴影数量。" },
        { u8"Lowering this can smooth frametime spikes in busy scenes.",
          u8"在复杂场景里调低它可以减少帧时间尖峰。" },
        0.0f, 32.0f,
        "0"
    },
    {
        "cl_max_shadow_renderable_dist",
        OptionType::Float,
        { u8"Performance", u8"性能" },
        { u8"Shadow Render Distance", u8"阴影渲染距离" },
        { u8"Maximum distance from the camera for rendering shadow casters/receivers.",
          u8"相机周围参与阴影渲染的最大距离。" },
        { u8"Lower values help most on large outdoor maps.",
          u8"在大场景或室外地图里，降低它往往最有效。" },
        0.0f, 8192.0f,
        "0"
    },
    {
        "r_FlashlightDetailProps",
        OptionType::Int,
        { u8"Performance", u8"性能" },
        { u8"Flashlight Detail Props", u8"手电影响细节物件" },
        { u8"Controls whether flashlight lighting/shadow passes affect detail props.",
          u8"控制手电光照/阴影是否作用到细节物件。" },
        { u8"0 = off, 1 = single-pass, 2 = multi-pass. 0 is the cheapest.",
          u8"0=关闭，1=单通道，2=多通道。0 最省性能。" },
        0.0f, 2.0f,
        "0"
    },
    {
        "z_mob_simple_shadows",
        OptionType::Int,
        { u8"Performance", u8"性能" },
        { u8"Infected Shadow Quality", u8"尸群阴影质量" },
        { u8"Controls common infected shadow quality: 0 = full, 1 = blob, 2 = off.",
          u8"控制普通感染者阴影质量：0=完整，1=团块，2=关闭。" },
        { u8"1 usually keeps readability while cutting a lot of shadow work.",
          u8"通常设为 1 就能保住可读性，同时明显减负。" },
        0.0f, 2.0f,
        "2"
    },
    {
        "r_shadowfromworldlights",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"World-Light Shadows", u8"世界光源阴影" },
        { u8"Allows dynamic shadows to be cast from world lights.",
          u8"允许世界光源投射动态阴影。" },
        { u8"Disable this for a noticeable GPU win if you mostly care about direct readability.",
          u8"如果更看重帧时间而不是环境层次，关掉它通常会有明显收益。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "r_flashlightmodels",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Flashlight On Models", u8"手电影响模型" },
        { u8"Allows flashlight shadowing/lighting work on renderable models.",
          u8"允许手电对可渲染模型进行光照/阴影处理。" },
        { u8"Disable this if model-heavy scenes are your bottleneck.",
          u8"如果卡在大量模型场景，关闭它会更划算。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "r_shadows_on_renderables_enable",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Shadows On Renderables", u8"阴影投到可渲染物" },
        { u8"Controls whether RTT shadows are allowed to cast onto other renderables.",
          u8"控制 RTT 阴影是否允许投射到其他可渲染物体上。" },
        { u8"Turn this off if character and prop shadows are still showing up on models or the ground.",
          u8"如果角色或道具阴影还会投到模型或地面上，先关这个。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "r_flashlightrendermodels",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Flashlight Render Models", u8"手电渲染模型" },
        { u8"Engine-side switch for flashlight rendering on models.",
          u8"控制手电是否在模型上走渲染路径的引擎侧开关。" },
        { u8"If r_flashlightmodels is not enough, this is usually the next one to cut.",
          u8"如果只关 r_flashlightmodels 还不够，通常下一步就砍这个。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "cl_player_shadow_dist",
        OptionType::Float,
        { u8"Performance", u8"性能" },
        { u8"Player Shadow Distance", u8"玩家阴影距离" },
        { u8"Maximum distance for player shadow rendering.",
          u8"玩家阴影的最大渲染距离。" },
        { u8"Set this to 0 if you want the nearby player shadow to disappear completely.",
          u8"想把近处玩家阴影彻底压掉，就把它设成 0。" },
        0.0f, 8192.0f,
        "0"
    },
    {
        "z_infected_shadows",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Infected Dynamic Shadows", u8"感染者动态阴影" },
        { u8"Controls dynamic shadows on infected.",
          u8"控制感染者的动态阴影。" },
        { u8"If commons and specials are still casting shadows, disable this next.",
          u8"如果普通/特殊感染者还在投阴影，下一步就关它。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "nb_shadow_blobby_dist",
        OptionType::Float,
        { u8"Performance", u8"性能" },
        { u8"NPC Blob Shadow Distance", u8"NPC 团块阴影距离" },
        { u8"Distance budget for blob-style shadows used by NPCs.",
          u8"NPC 团块式阴影的距离预算。" },
        { u8"0 is the strongest cut if simple blob shadows are still visible.",
          u8"如果简化团块阴影还看得见，设 0 是最狠的一刀。" },
        0.0f, 8192.0f,
        "0"
    },
    {
        "nb_shadow_cull_dist",
        OptionType::Float,
        { u8"Performance", u8"性能" },
        { u8"NPC Shadow Cull Distance", u8"NPC 阴影裁剪距离" },
        { u8"Cull distance for NPC shadows.",
          u8"NPC 阴影的裁剪距离。" },
        { u8"Lowering this trims map-wide NPC shadow work without touching lighting itself.",
          u8"调低它可以砍掉更远处的 NPC 阴影开销，而不直接动光照本身。" },
        0.0f, 8192.0f,
        "0"
    },
    {
        "r_flashlightinfectedshadows",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Flashlight Infected Shadows", u8"手电感染者阴影" },
        { u8"Controls flashlight shadowing on infected.",
          u8"控制手电对感染者产生的阴影。" },
        { u8"Useful when flashlight scenes still spike because infected are receiving/casting shadow work.",
          u8"如果开手电时感染者相关阴影仍然拖帧，这一项通常很有用。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "ShadowEntityTweaksEnabled",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Enable Entity Shadow Overrides", u8"启用实体阴影覆盖" },
        { u8"Overrides ShadowControl / EnvProjectedTexture netvars discovered from client.dll.",
          u8"覆盖从 client.dll 里找到的 ShadowControl / EnvProjectedTexture 网络字段。" },
        { u8"Use this when you want map-level shadow entities trimmed in addition to cvars.",
          u8"如果你想连地图里的阴影实体也一起压缩，就开启它。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "ShadowControlDisableShadows",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"ShadowControl Disable Shadows", u8"ShadowControl 禁用阴影" },
        { u8"Writes m_bDisableShadows on CShadowControl entities.",
          u8"直接写入 CShadowControl 实体上的 m_bDisableShadows。" },
        { u8"Strongest visual cut for maps that rely on ShadowControl.",
          u8"对依赖 ShadowControl 的地图来说，这是最狠的一刀。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "ShadowControlMaxDist",
        OptionType::Float,
        { u8"Performance", u8"性能" },
        { u8"ShadowControl Max Distance", u8"ShadowControl 最大距离" },
        { u8"Writes m_flShadowMaxDist on CShadowControl entities.",
          u8"直接写入 CShadowControl 实体上的 m_flShadowMaxDist。" },
        { u8"Lowering this usually saves frametime with less damage than fully disabling shadows.",
          u8"相比直接关掉阴影，先砍这个通常更容易保住观感。" },
        0.0f, 8192.0f,
        "1200"
    },
    {
        "ShadowControlLocalLightShadows",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"ShadowControl Local Lights", u8"ShadowControl 局部光阴影" },
        { u8"Writes m_bEnableLocalLightShadows on CShadowControl entities.",
          u8"直接写入 CShadowControl 实体上的 m_bEnableLocalLightShadows。" },
        { u8"Disable this first if point/spot lights are the expensive part.",
          u8"如果贵在点光/聚光阴影，先关这个最合适。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "ProjectedTextureEnableShadows",
        OptionType::Bool,
        { u8"Performance", u8"性能" },
        { u8"Projected Texture Shadows", u8"投射纹理阴影" },
        { u8"Writes m_bEnableShadows on CEnvProjectedTexture entities.",
          u8"直接写入 CEnvProjectedTexture 实体上的 m_bEnableShadows。" },
        { u8"Useful for trimming flashlight-like projected shadow casters without touching every other light path.",
          u8"适合单独压掉类似手电这类投射光阴影，而不动其他光照路径。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "ProjectedTextureShadowQuality",
        OptionType::Int,
        { u8"Performance", u8"性能" },
        { u8"Projected Texture Quality", u8"投射纹理阴影质量" },
        { u8"Writes m_nShadowQuality on CEnvProjectedTexture entities.",
          u8"直接写入 CEnvProjectedTexture 实体上的 m_nShadowQuality。" },
        { u8"Lower values are cheaper. Start at 0 or 1 and compare frametime.",
          u8"值越低越省。建议先从 0 或 1 开始比对帧时间。" },
        0.0f, 3.0f,
        "0"
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


    // HUD (Hand)
    {
        "LeftWristHudEnabled",
        OptionType::Bool,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Enable Status HUD (Off-hand)", u8"启用状态HUD（副手）" },
        { u8"Shows a small wrist-style HUD on the off-hand controller using a SteamVR overlay.",
          u8"在副手手柄上用SteamVR覆盖层显示一个腕表式小HUD。" },
        { u8"Displays HP and quick item status (throwable/med/pills or adrenaline).",
          u8"显示生命值与关键物品状态（投掷物/医疗槽/药片或肾上腺素）。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "LeftWristHudAlpha",
        OptionType::Float,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Status HUD Opacity Multiplier", u8"状态HUD透明度倍率" },
        { u8"Extra background opacity multiplier for the Status HUD (0..1).",
          u8"状态HUD的额外背景透明度倍率（0..1）。" },
        { u8"",
          u8"" },
        0.0f, 1.0f,
        "0.35"
    },
    {
        "LeftWristHudWidthMeters",
        OptionType::Float,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Status HUD Width (meters)", u8"状态HUD宽度（米）" },
        { u8"Physical width of the Status HUD overlay quad (meters).",
          u8"状态HUD覆盖层平面的物理宽度（米）。" },
        { u8"Bigger = easier to read, but can feel intrusive.",
          u8"越大越容易看清，但也更显眼。" },
        0.01f, 0.40f,
        "0.1"
    },
    {
        "LeftWristHudShowTeammates",
        OptionType::Bool,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Show Teammate Mini HP Bars", u8"显示队友小血条" },
        { u8"Shows up to 3 teammate HP bars on the Status HUD.",
          u8"在状态HUD上显示最多3名队友的小血条。" },
        { u8"Includes temp HP and a third-strike frame.",
          u8"包含临时血，并在黑白时加边框提示。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "LeftWristHudXOffset",
        OptionType::Float,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Status HUD X Offset", u8"状态HUD X偏移" },
        { u8"Overlay translation in controller local space (meters).",
          u8"覆盖层在手柄本地坐标系中的平移（米）。" },
        { u8"Uses the same axis convention as other overlay offsets (ScopeOverlay*).",
          u8"与其他覆盖层偏移（ScopeOverlay*）使用相同坐标约定。" },
        -0.25f, 0.25f,
        "0.01"
    },
    {
        "LeftWristHudYOffset",
        OptionType::Float,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Status HUD Y Offset", u8"状态HUD Y偏移" },
        { u8"Overlay translation in controller local space (meters).",
          u8"覆盖层在手柄本地坐标系中的平移（米）。" },
        { u8"Uses the same axis convention as other overlay offsets (ScopeOverlay*).",
          u8"与其他覆盖层偏移（ScopeOverlay*）使用相同坐标约定。" },
        -0.25f, 0.25f,
        "0.01"
    },
    {
        "LeftWristHudZOffset",
        OptionType::Float,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Status HUD Z Offset", u8"状态HUD Z偏移" },
        { u8"Overlay translation in controller local space (meters).",
          u8"覆盖层在手柄本地坐标系中的平移（米）。" },
        { u8"Uses the same axis convention as other overlay offsets (ScopeOverlay*).",
          u8"与其他覆盖层偏移（ScopeOverlay*）使用相同坐标约定。" },
        -0.25f, 0.25f,
        "0.0"
    },
    {
        "LeftWristHudAngleOffset",
        OptionType::Vec3,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Status HUD Angle Offset (pitch,yaw,roll)", u8"状态HUD角度偏移 (俯仰,偏航,翻滚)" },
        { u8"Additional rotation for the Status HUD overlay (degrees).",
          u8"状态HUD覆盖层的额外旋转（度）。" },
        { u8"Adjust so it faces your eyes naturally.",
          u8"调到看起来像贴在手腕上、自然朝向眼睛即可。" },
        -180.f, 180.f,
        "-75,0,0"
    },

    {
        "RightAmmoHudEnabled",
        OptionType::Bool,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Enable Ammo HUD (Gun-hand)", u8"启用弹药HUD（持枪手）" },
        { u8"Shows a compact ammo HUD on the gun-hand controller using a SteamVR overlay.",
          u8"在持枪手手柄上用SteamVR覆盖层显示一个科技感弹药框。" },
        { u8"Displays clip/reserve and upgraded ammo when available.",
          u8"显示弹匣/备弹，并在有特殊子弹时显示剩余量。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "RightAmmoHudAlpha",
        OptionType::Float,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Ammo HUD Opacity Multiplier", u8"弹药HUD透明度倍率" },
        { u8"Extra background opacity multiplier for the ammo HUD (0..1).",
          u8"弹药HUD的额外背景透明度倍率（0..1）。" },
        { u8"",
          u8"" },
        0.0f, 1.0f,
        "0.35"
     },
    {
        "RightAmmoHudWidthMeters",
        OptionType::Float,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Ammo HUD Width (meters)", u8"弹药HUD宽度（米）" },
        { u8"Physical width of the ammo HUD overlay quad (meters).",
          u8"弹药HUD覆盖层平面的物理宽度（米）。" },
        { u8"Increase if numbers are too small.",
          u8"如果数字太小就调大。" },
        0.01f, 0.50f,
        "0.1"
    },
    {
        "RightAmmoHudXOffset",
        OptionType::Float,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Ammo HUD X Offset", u8"弹药HUD X偏移" },
        { u8"Overlay translation in controller local space (meters).",
          u8"覆盖层在手柄本地坐标系中的平移（米）。" },
        { u8"Uses the same axis convention as other overlay offsets (ScopeOverlay*).",
          u8"与其他覆盖层偏移（ScopeOverlay*）使用相同坐标约定。" },
        -0.25f, 0.25f,
        "-0.07"
    },
    {
        "RightAmmoHudYOffset",
        OptionType::Float,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Ammo HUD Y Offset", u8"弹药HUD Y偏移" },
        { u8"Overlay translation in controller local space (meters).",
          u8"覆盖层在手柄本地坐标系中的平移（米）。" },
        { u8"Uses the same axis convention as other overlay offsets (ScopeOverlay*).",
          u8"与其他覆盖层偏移（ScopeOverlay*）使用相同坐标约定。" },
        -0.25f, 0.25f,
        "0.03"
    },
    {
        "RightAmmoHudZOffset",
        OptionType::Float,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Ammo HUD Z Offset", u8"弹药HUD Z偏移" },
        { u8"Overlay translation in controller local space (meters).",
          u8"覆盖层在手柄本地坐标系中的平移（米）。" },
        { u8"Uses the same axis convention as other overlay offsets (ScopeOverlay*).",
          u8"与其他覆盖层偏移（ScopeOverlay*）使用相同坐标约定。" },
        -0.25f, 0.25f,
        "-0.09"
    },
    {
        "RightAmmoHudAngleOffset",
        OptionType::Vec3,
        { u8"HUD (Hand)", u8"HUD（手柄）" },
        { u8"Ammo HUD Angle Offset (pitch,yaw,roll)", u8"弹药HUD角度偏移 (俯仰,偏航,翻滚)" },
        { u8"Additional rotation for the ammo HUD overlay (degrees).",
          u8"弹药HUD覆盖层的额外旋转（度）。" },
        { u8"Adjust so it sits like a weapon-side panel.",
          u8"调到像贴在武器旁边的小屏幕即可。" },
        -180.f, 180.f,
        "-75,0,0"
    },

    // Hands / Debug
    {
        "HideArms",
        OptionType::Bool,
        { u8"Hands / Debug", u8"手部 / 调试" },
        { u8"Hide Arms", u8"隐藏手臂" },
        { u8"Hides in-game arm models while keeping weapons.",
          u8"隐藏游戏中的手臂模型，仅保留武器。" },
        { u8"",
          u8"" },
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
        { u8"Enable Weapon Position Adjustment", u8"启用武器位置调整" },
        { u8"Allows saving manual weapon model offsets in VR.",
          u8"允许在VR中保存武器模型的位置偏移。" },
        { u8"", u8"" },
        0.0f, 0.0f,
        "true"
    },
    {
        "ViewmodelAdjustCombo",
        OptionType::String,
        { u8"Interaction / Combos", u8"交互 / 组合键" },
        { u8"Weapon Position Adjust Combo", u8"武器位置调整组合键" },
        { u8"Action combo to toggle weapon position adjustment mode.",
          u8"用于切换武器位置调整模式的组合动作。" },
        { u8"Set to \"false\" to disable if you never edit offsets.",
          u8"若不需要可设为 \"false\" 禁用。" },
        0.0f, 0.0f,
        "Reload+SecondaryAttack"
    },
    {
        "ViewmodelDisableMoveBob",
        OptionType::Bool,
        { u8"Interaction / Combos", u8"交互 / 组合键" },
        { u8"Disable Weapon Move Bob", u8"禁用武器移动晃动" },
        { u8"Disables movement bob/sway on the first-person weapon model.",
          u8"禁用第一人称武器模型随移动产生的晃动/摆动。" },
        { u8"When enabled, the weapon model appears more stable while moving.",
          u8"开启后，移动时武器模型会更稳定。" },
        0.0f, 0.0f,
        "false"
    },
    // Weapons / Fire
    {
        "AutoRepeatSemiAutoFire",
        OptionType::Bool,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Hold-to-Fire for Semi-Auto", u8"单发枪长按连发" },
        { u8"Converts a held primary-fire input into pulses so semi-auto / single-shot guns keep firing while you hold the trigger.",
          u8"把持续按住的主开火输入变成“点射脉冲”，让半自动/单发武器在按住扳机时也能连续射击。" },
        { u8"Does not affect full-auto weapons. Use AutoRepeatSemiAutoFireHz to adjust click rate.",
          u8"不影响全自动武器。可用 AutoRepeatSemiAutoFireHz 调整“连点”频率。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "AutoRepeatSemiAutoFireHz",
        OptionType::Float,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Auto-Repeat Rate (Hz)", u8"连点频率 (Hz)" },
        { u8"How many fire pulses per second to generate while holding the trigger.",
          u8"按住扳机时每秒生成多少次开火脉冲。" },
        { u8"Higher can feel snappier, but the weapon's real fire rate still limits shots.",
          u8"调高会更“跟手”，但实际射速仍受武器本身限制。" },
        1.0f, 12.0f,
        "12.0"
    },
    {
        "AutoRepeatSprayPushEnabled",
        OptionType::Bool,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Auto Spray-Push", u8"自动 Spray-Push" },
        { u8"While hold-to-fire is active, automatically applies the extra spray/push input assist used by pump/chrome shotguns and AWP/scout.",
          u8"在长按连发激活时，自动附带 pump/chrome 霰弹枪和 AWP/scout 使用的 spray/push 输入辅助。" },
        { u8"Only matters when Hold-to-Fire for Semi-Auto is enabled.",
          u8"仅在“单发枪长按连发”开启时才有意义。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "AutoFastMelee",
        OptionType::Bool,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Auto Fast Melee", u8"自动快速近战" },
        { u8"Turns melee into fixed one-by-one client-side pulses instead of relying on a raw continuous hold.",
          u8"把近战输入改成固定“一下一下”的客户端脉冲，而不是直接依赖原始连续长按。" },
        { u8"Useful when long melee holds feel sticky. If a server still jams swings, release and press again instead of mashing.",
          u8"适合近战长按容易发黏时使用。如果某些服里仍会卡刀，建议松开后再按，不要一直闷按。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "HitSoundEnabled",
        OptionType::Bool,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Hit Sound Feedback", u8"命中音效反馈" },
        { u8"Plays a short cue immediately when your shot trace hits an infected target.",
          u8"当本地射击轨迹命中感染者目标时，立刻播放短促命中提示音。" },
        { u8"Uses the same local hit detection as the projected hit icon.",
          u8"和命中位置图标使用同一套本地命中检测。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "HitSoundSpec",
        OptionType::String,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Hit Sound", u8"命中音效" },
        { u8"Sound spec for non-lethal hits. Supports alias:, file:, game:, gamesound:, cmd:, or a direct audio file path.",
          u8"普通命中音效配置。支持 alias:、file:、game:、gamesound:、cmd:，也支持直接填写音频文件路径。" },
        { u8"Direct file paths may be absolute, or relative to the VR folder. Example: hit.mp3 resolves to VR\\hit.mp3",
          u8"文件路径可用绝对路径，也可相对 VR 目录。示例：hit.mp3 会解析到 VR\\hit.mp3" },
        0.0f, 0.0f,
        "hit.mp3"
    },
    {
        "HitSoundVolume",
        OptionType::Float,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Feedback Sound Volume", u8"反馈音量" },
        { u8"Unified volume multiplier for hit, kill, and headshot sounds. Adjusting this slider updates all three together.",
          u8"统一控制命中、击杀、爆头三种提示音的音量倍率。调整这个滑杆会同时写入三个配置项。" },
        { u8"1.0 keeps source loudness unchanged. This slider is clamped to 0.5~2.0 in the config tool.",
          u8"1.0 表示保持素材原始响度。配置工具里这个滑杆的范围限制为 0.5~2.0。" },
        0.5f, 2.0f,
        "1.0"
    },
    {
        "KillSoundEnabled",
        OptionType::Bool,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Kill Sound Feedback", u8"击杀音效反馈" },
        { u8"Plays a short sound when your local kill counter increases.",
          u8"当本地击杀计数增加时播放短促提示音。" },
        { u8"Headshots can use a separate sound via KillSoundHeadshotSpec.",
          u8"爆头可通过 KillSoundHeadshotSpec 使用另一种音效。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "KillSoundNormalSpec",
        OptionType::String,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Normal Kill Sound", u8"普通击杀音效" },
        { u8"Sound spec for normal kills. Supports alias:, file:, game:, gamesound:, cmd:, or a direct audio file path.",
          u8"普通击杀音效配置。支持 alias:、file:、game:、gamesound:、cmd:，也支持直接填写音频文件路径。" },
        { u8"Direct file paths may be absolute, or relative to the VR folder.",
          u8"文件路径可用绝对路径，也可相对 VR 目录。" },
        0.0f, 0.0f,
        "kill.mp3"
    },
    {
        "KillSoundHeadshotSpec",
        OptionType::String,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Headshot Kill Sound", u8"爆头击杀音效" },
        { u8"Sound spec used when the confirmed kill followed a recent head hit.",
          u8"当确认击杀来自最近一次头部命中时使用的音效配置。" },
        { u8"Direct file paths may be absolute, or relative to the VR folder. Example: headshot.mp3 resolves to VR\\headshot.mp3",
          u8"文件路径可用绝对路径，也可相对 VR 目录。示例：headshot.mp3 会解析到 VR\\headshot.mp3" },
        0.0f, 0.0f,
        "headshot.mp3"
    },
    {
        "KillSoundVolume",
        OptionType::Float,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Kill Sound Volume", u8"击杀音量" },
        { u8"Volume multiplier for normal kill sounds.",
          u8"普通击杀音效的音量倍率。" },
        { u8"Useful when the kill cue should sit lower than the hit cue or headshot cue.",
          u8"适合在你想让普通击杀弱于命中或爆头提示时调整。" },
        0.0f, 2.0f,
        "0.95"
    },
    {
        "HeadshotSoundVolume",
        OptionType::Float,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Headshot Volume", u8"爆头音量" },
        { u8"Volume multiplier used when a confirmed kill is matched to a recent head hit.",
          u8"当确认击杀匹配到最近一次头部命中时使用的音量倍率。" },
        { u8"Set above 1.0 if you want the headshot cue to pop harder than the normal kill cue.",
          u8"如果想让爆头提示更“炸”，可以设到 1.0 以上。" },
        0.0f, 2.0f,
        "1.10"
    },
    {
        "FeedbackSoundSpatialBlend",
        OptionType::Float,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Feedback Spatial Blend", u8"反馈空间感强度" },
        { u8"Blends the custom audio between centered playback and stereo panning / distance falloff from the hit point.",
          u8"控制自定义音频从“居中播放”过渡到“按命中点做左右声像和距离衰减”的强度。" },
        { u8"0 disables spatial feel. 1 makes the hit location drive the sound as strongly as possible.",
          u8"0 表示关闭空间感。1 表示尽量让命中位置主导声音方向与远近感。" },
        0.0f, 1.0f,
        "0.85"
    },
    {
        "FeedbackSoundSpatialRange",
        OptionType::Float,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Feedback Spatial Range", u8"反馈空间作用距离" },
        { u8"Distance in Source units over which hit / kill sounds fade with depth.",
          u8"命中 / 击杀音效按距离衰减时使用的作用范围，单位是 Source 世界单位。" },
        { u8"Higher keeps distant hits louder. Lower makes nearby impacts sound more intimate.",
          u8"调高会让远处命中更响，调低会让近处反馈更贴脸、远处更轻。" },
        64.0f, 8192.0f,
        "1400"
    },
    {
        "KillIndicatorEnabled",
        OptionType::Bool,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Hit / Kill Icons", u8"命中 / 击杀图标" },
        { u8"Projects animated hit and kill icons onto the HUD at the traced impact / confirmed kill position.",
          u8"把动态命中和击杀图标投影到 HUD 上，并跟随命中点 / 确认击杀位置显示。" },
        { u8"Uses Source materials, so animated VMT/VTF feedback packs can play directly.",
          u8"直接使用 Source 材质，带动画的 VMT/VTF 反馈素材可以直接播放。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "KillIndicatorMaterialBaseSpec",
        OptionType::String,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Kill Icon Material Base", u8"击杀图标材质目录" },
        { u8"Base material path or folder. The mod will use /hit, /kill, and /headshot under it.",
          u8"材质基础路径或目录。模组会自动使用其中的 /hit、/kill 和 /headshot 材质。" },
        { u8"Supports engine material paths like overlays/2965700751 or an absolute folder path ending in materials\\...",
          u8"支持 overlays/2965700751 这类材质路径，也支持以 materials\\... 结尾的绝对目录路径。" },
        0.0f, 0.0f,
        "overlays/2965700751"
    },
    {
        "KillIndicatorLifetimeSeconds",
        OptionType::Float,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Kill Icon Duration", u8"击杀图标持续时间" },
        { u8"How long each kill icon stays visible.",
          u8"每个击杀图标在屏幕上保留多久。" },
        { u8"Longer values are easier to notice but can feel busy during hordes.",
          u8"时间越长越容易注意到，但尸潮时会更占视野。" },
        0.10f, 3.0f,
        "0.85"
    },
    {
        "KillIndicatorSizePixels",
        OptionType::Float,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Kill Icon Size", u8"击杀图标大小" },
        { u8"Base on-screen size of the projected kill icon.",
          u8"投影后的击杀图标基础屏幕尺寸。" },
        { u8"Headshots get a slightly larger pulse automatically.",
          u8"爆头会自动带一点更大的脉冲放大效果。" },
        16.0f, 512.0f,
        "180"
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
        "BlockFireOnFriendlyAimEnabled",
        OptionType::Bool,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Friendly-fire Aim Guard ", u8"禁止向队友开火" },
        { u8"Suppresses firing when your aim line is on a teammate.",
         u8"当瞄准线指向队友时抑制开火。" },
        { u8"This is the startup default; you can still toggle it at runtime via SteamVR binding.",
        u8"这是启动时的默认值；运行中仍可用 SteamVR 绑定开关切换。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "EffectiveAttackRangeAutoFireEnabled",
        OptionType::Bool,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Effective-range Auto Fire", u8"有效距离自动开火" },
        { u8"When enabled, the mod fires automatically while the aim line is in effective attack range.",
          u8"开启后，当瞄准线处于有效攻击距离时自动开火。" },
        { u8"Witches are excluded. You can toggle this at runtime via SteamVR binding.",
          u8"女巫会被排除。运行中也可用 SteamVR 绑定开关切换。" },
        0.0f, 0.0f,
        "false"
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
        { u8"RGBA color for the aim line, stored as 0~255 integers.",
          u8"瞄准线的 RGBA 颜色，按 0~255 整数保存。" },
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

    // Inventory Quick Switch (HL:Alyx style)
    {
        "InventoryQuickSwitchEnabled",
        OptionType::Bool,
        { u8"Inventory / Quick Switch", u8"物品栏 / 快速切换" },
        { u8"Enable Inventory Quick Switch",
          u8"启用快速道具切换（类似半条命Alyx）" },
        { u8"Disables the legacy body-anchored inventory switching, and enables a quick-switch that spawns 4 zones around the RIGHT hand when a SteamVR bind is pressed.",
          u8"开启后会禁用原有的身体锚点道具切换，并启用右手快速切换：按下SteamVR绑定后，以右手柄为原点生成4个区域，手柄碰到区域即可切换道具。" },
        { u8"Bind the SteamVR action 'InventoryQuickSwitch' to a button (recommended: hold-to-show).",
          u8"请在SteamVR里将动作 'InventoryQuickSwitch' 绑定到任意按键（建议按住触发）。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "InventoryQuickSwitchOffset",
        OptionType::Vec3,
        { u8"Inventory / Quick Switch", u8"物品栏 / 快速切换" },
        { u8"Quick Switch Zone Offset (x,y,z)",
          u8"快速切换区域偏移 (x,y,z)" },
        { u8"Offset vector in meters (forward,right,up). x = common forward bias for all zones, y = left/right distance, z = up/down distance.",
          u8"米制偏移向量（前、右、上）。x为所有区域共同的前向偏移；y为左右距离；z为上下距离。" },
        { u8"Zones are computed from the origin set at right-hand position when the quick-switch button is pressed.",
          u8"按下快速切换按键时会把右手柄位置作为原点，四个区域基于此原点计算。" },
        -0.6f, 0.6f,
        "0.06,0.12,0.12"
    },
    {
        "InventoryQuickSwitchZoneRadius",
        OptionType::Float,
        { u8"Inventory / Quick Switch", u8"物品栏 / 快速切换" },
        { u8"Quick Switch Zone Radius", u8"快速切换区域半径" },
        { u8"Selection radius for each quick-switch zone (meters).",
          u8"每个快速切换区域的判定半径（米）。" },
        { u8"Increase if selection feels too strict; decrease to reduce accidental picks.",
          u8"感觉不好选就调大；误触太多就调小。" },
        0.03f, 0.30f,
        "0.10"
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
    },
    {
        "AntiAliasing",
        OptionType::Int,
        { u8"Performance", u8"性能" },
        { u8"Anti-Aliasing Level", u8"抗锯齿级别" },
        { u8"Controls the anti-aliasing level used by the VR rendering path.",
          u8"控制 VR 渲染路径使用的抗锯齿级别。" },
        { u8"0 disables anti-aliasing. Raise it only if your GPU has headroom.",
          u8"0 表示关闭抗锯齿。只有在显卡余量充足时再提高。" },
        0.0f, 8.0f,
        "0"
    },
    {
        "ThirdPersonVRCameraOffset",
        OptionType::Float,
        { u8"Camera / Third Person", u8"相机 / 第三人称" },
        { u8"Third-Person Camera Offset", u8"第三人称相机偏移" },
        { u8"Distance offset applied to the third-person VR camera.",
          u8"应用到第三人称 VR 相机的距离偏移。" },
        { u8"Positive values usually move the camera farther back.",
          u8"正值通常会把相机拉得更靠后。" },
        -200.0f, 200.0f,
        "38"
    },
    {
        "D3DAimLineOverlayEnabled",
        OptionType::Bool,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Enable Aim-Line", u8"启用瞄准线" },
        { u8"Draws a screen-space aim line overlay using the D3D layer.",
          u8"使用 D3D 层绘制屏幕空间的瞄准线覆盖层。" },
        { u8"Useful if you want a flat overlay instead of only world-space helpers.",
          u8"如果你想使用平面覆盖层，而不只依赖世界空间辅助线，可以开启。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "AimLineOnlyWhenLaserSight",
        OptionType::Bool,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"Show Aim Line Only With Laser Sight", u8"仅在激光瞄准开启时显示瞄准线" },
        { u8"When enabled, the VR aim line is hidden unless your firearm has the in-game laser sight upgrade active.",
          u8"开启后，除非枪械已激活游戏内的激光瞄准升级，否则隐藏VR瞄准线。" },
        { u8"Throwable trajectory arcs are not affected.",
          u8"投掷物抛物线不受影响。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "D3DAimLineOverlayWidthPixels",
        OptionType::Float,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"D3D Aim-Line Width", u8"D3D 瞄准线宽度" },
        { u8"Pixel width of the D3D aim-line overlay.",
          u8"D3D 瞄准线覆盖层的像素宽度。" },
        { u8"Increase only if the line is too thin to see clearly.",
          u8"只有在瞄准线太细时再调大。" },
        0.0f, 20.0f,
        "2.0"
    },
    {
        "D3DAimLineOverlayOutlinePixels",
        OptionType::Float,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"D3D Aim-Line Outline Width", u8"D3D 瞄准线描边宽度" },
        { u8"Pixel width of the outline around the D3D aim-line.",
          u8"D3D 瞄准线外描边的像素宽度。" },
        { u8"Use a small outline to keep the line visible on bright scenes.",
          u8"亮场景下可用少量描边来提高可见性。" },
        0.0f, 20.0f,
        "1.0"
    },
    {
        "D3DAimLineOverlayEndpointPixels",
        OptionType::Float,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"D3D Aim-Line Endpoint Size", u8"D3D 瞄准线端点大小" },
        { u8"Pixel size of the endpoint marker on the D3D aim-line overlay.",
          u8"D3D 瞄准线覆盖层端点标记的像素大小。" },
        { u8"Raise it if you want the hit point marker to stand out more.",
          u8"如果希望落点标记更醒目，可以调大。" },
        0.0f, 20.0f,
        "1.5"
    },
    {
        "D3DAimLineOverlayColor",
        OptionType::Color,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"D3D Aim-Line Color", u8"D3D 瞄准线颜色" },
        { u8"RGBA color for the D3D aim-line overlay.",
          u8"D3D 瞄准线覆盖层使用的 RGBA 颜色。" },
        { u8"Use a bright color with some alpha for readability.",
          u8"建议使用高亮颜色并保留一点透明度。" },
        0.0f, 0.0f,
        "255,0,0,100"
    },
    {
        "D3DAimLineOverlayOutlineColor",
        OptionType::Color,
        { u8"Aim Assist", u8"辅助瞄准" },
        { u8"D3D Aim-Line Outline Color", u8"D3D 瞄准线描边颜色" },
        { u8"RGBA color for the D3D aim-line outline.",
          u8"D3D 瞄准线描边使用的 RGBA 颜色。" },
        { u8"Keep it subtle so the main line remains the focus.",
          u8"建议颜色低调一些，避免抢主线的视觉焦点。" },
        0.0f, 0.0f,
        "255,0,0,1"
    },
    {
        "MotionGesturePushThreshold",
        OptionType::Float,
        { u8"Motion Gestures", u8"动作手势" },
        { u8"Push Gesture Threshold", u8"推手势阈值" },
        { u8"Velocity threshold required to recognize a push gesture.",
          u8"识别推手势所需的速度阈值。" },
        { u8"Increase it if forward hand motions trigger too easily.",
          u8"如果向前挥手过于容易误触，就把它调高。" },
        0.0f, 10.0f,
        "1.5"
    },
    {
        "InventoryBodyOriginOffset",
        OptionType::Vec3,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Body Origin Offset (x,y,z)", u8"身体原点偏移 (x,y,z)" },
        { u8"Offsets the body-relative origin used to place inventory anchors.",
          u8"调整用于放置物品栏锚点的身体相对原点。" },
        { u8"Use this if all anchor positions feel consistently shifted.",
          u8"如果所有锚点整体都偏了，用这个统一修正。" },
        -2.0f, 2.0f,
        "-0.1,0.0,-0.28"
    },
    {
        "InventoryAnchorColor",
        OptionType::Color,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Inventory Anchor Color", u8"物品栏锚点颜色" },
        { u8"RGBA color used when drawing visible inventory anchors.",
          u8"显示物品栏锚点时使用的 RGBA 颜色。" },
        { u8"Only matters when inventory anchors are visible.",
          u8"仅在显示锚点时生效。" },
        0.0f, 0.0f,
        "0,255,255,255"
    },
    {
        "InventoryHudMarkerDistance",
        OptionType::Float,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Inventory HUD Marker Distance", u8"物品栏 HUD 标记距离" },
        { u8"Distance used when placing inventory HUD markers relative to the player.",
          u8"相对玩家放置物品栏 HUD 标记时使用的距离。" },
        { u8"Increase it if markers feel too close to your face.",
          u8"如果标记离脸太近，可以调大。" },
        -2.0f, 2.0f,
        "0.45"
    },
    {
        "InventoryHudMarkerUpOffset",
        OptionType::Float,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Inventory HUD Marker Up Offset", u8"物品栏 HUD 标记上移偏移" },
        { u8"Vertical offset applied to inventory HUD markers.",
          u8"应用到物品栏 HUD 标记的垂直偏移。" },
        { u8"Negative values move the markers lower.",
          u8"负值会把标记往下移。" },
        -2.0f, 2.0f,
        "-0.10"
    },
    {
        "InventoryHudMarkerSeparation",
        OptionType::Float,
        { u8"Inventory / Anchors", u8"物品栏 / 锚点" },
        { u8"Inventory HUD Marker Separation", u8"物品栏 HUD 标记间距" },
        { u8"Controls spacing between inventory HUD markers.",
          u8"控制物品栏 HUD 标记之间的间距。" },
        { u8"Raise it if markers overlap each other.",
          u8"如果标记互相重叠，就把它调大。" },
        0.0f, 2.0f,
        "0.14"
    },
    {
        "ScopeCameraOffset",
        OptionType::Vec3,
        { u8"Optics", u8"光学" },
        { u8"Scope Camera Offset (x,y,z)", u8"瞄准镜相机偏移 (x,y,z)" },
        { u8"Offsets the rendered scope camera relative to the weapon or controller.",
          u8"调整瞄准镜渲染相机相对武器或控制器的位置。" },
        { u8"Use this when the rendered scope view feels misaligned.",
          u8"如果瞄准镜画面位置不对，可以用它校正。" },
        -180.0f, 180.0f,
        "12,0,3"
    },
    {
        "ScopeCameraAngleOffset",
        OptionType::Vec3,
        { u8"Optics", u8"光学" },
        { u8"Scope Camera Angle Offset (pitch,yaw,roll)", u8"瞄准镜相机角度偏移 (俯仰,偏航,翻滚)" },
        { u8"Additional rotation applied to the rendered scope camera.",
          u8"额外应用到瞄准镜渲染相机的旋转。" },
        { u8"Useful when the scope picture is tilted or not centered.",
          u8"当镜内画面歪斜或不居中时很有用。" },
        -180.0f, 180.0f,
        "0,0,0"
    },
    {
        "ScopeStabilizationEnabled",
        OptionType::Bool,
        { u8"Optics", u8"光学" },
        { u8"Enable Scope Stabilization", u8"启用瞄准镜稳定" },
        { u8"Applies smoothing to scoped aiming to reduce visible jitter.",
          u8"对开镜瞄准应用平滑处理，减少可见抖动。" },
        { u8"Useful for high magnification scopes where tiny hand motion is amplified.",
          u8"高倍率瞄准镜下微小手抖会被放大，这个选项尤其有用。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "ScopeStabilizationMinCutoff",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Scope Stabilization Min Cutoff", u8"瞄准镜稳定最小截止频率" },
        { u8"Base cutoff used by the scope stabilization filter.",
          u8"瞄准镜稳定滤波器使用的基础截止频率。" },
        { u8"Lower values smooth more but respond slower.",
          u8"数值越低越平滑，但响应也越慢。" },
        0.0f, 10.0f,
        "0.5"
    },
    {
        "ScopeStabilizationBeta",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Scope Stabilization Beta", u8"瞄准镜稳定 Beta" },
        { u8"Dynamic responsiveness factor for the scope stabilization filter.",
          u8"瞄准镜稳定滤波器的动态响应系数。" },
        { u8"Higher values track fast motion more aggressively.",
          u8"数值越高，快速动作的跟随会更积极。" },
        0.0f, 10.0f,
        "0.5"
    },
    {
        "ScopeStabilizationDCutoff",
        OptionType::Float,
        { u8"Optics", u8"光学" },
        { u8"Scope Stabilization D Cutoff", u8"瞄准镜稳定导数截止频率" },
        { u8"Derivative cutoff used by the scope stabilization filter.",
          u8"瞄准镜稳定滤波器使用的导数截止频率。" },
        { u8"Adjust only if you need to fine-tune filter behavior.",
          u8"只有在需要细调滤波行为时再改它。" },
        0.0f, 10.0f,
        "1.0"
    },
    {
        "MouseModeScopedViewmodelAnchorOffset",
        OptionType::Vec3,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse-Mode Scoped Anchor Offset (x,y,z)", u8"键鼠模式开镜锚点偏移 (x,y,z)" },
        { u8"Viewmodel anchor offset used while scoped in mouse mode.",
          u8"键鼠模式开镜时使用的武器模型锚点偏移。" },
        { u8"Use this to line up scoped weapons separately from hip-fire.",
          u8"用于把开镜时的武器位置与腰射状态分开校正。" },
        -2.0f, 2.0f,
        "0.35,0.0,-0.13"
    },
    {
        "MouseModeHmdAimSensitivity",
        OptionType::Float,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse-Mode HMD Aim Sensitivity", u8"键鼠模式头显瞄准灵敏度" },
        { u8"Sensitivity multiplier applied when mouse-mode aiming is driven from the HMD.",
          u8"键鼠模式由头显驱动瞄准时使用的灵敏度倍率。" },
        { u8"Raise it only if headset-driven aiming feels too sluggish.",
          u8"只有在头显瞄准明显偏慢时再提高。" },
        0.0f, 10.0f,
        "1"
    },
    {
        "MouseModeScopeOverlayOffset",
        OptionType::Vec3,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse-Mode Scope Overlay Offset (x,y,z)", u8"键鼠模式瞄准镜覆盖层偏移 (x,y,z)" },
        { u8"Offset for the scope overlay when using mouse mode.",
          u8"键鼠模式下瞄准镜覆盖层的位置偏移。" },
        { u8"Use this to align the overlay with scoped weapons in mouse mode.",
          u8"用于把键鼠模式下的瞄准镜覆盖层和武器对齐。" },
        -2.0f, 2.0f,
        "0,-0.02,-0.3"
    },
    {
        "MouseModeScopeOverlayAngleOffset",
        OptionType::Vec3,
        { u8"Input / Mouse Mode", u8"输入 / 键鼠模式" },
        { u8"Mouse-Mode Scope Overlay Angle Offset", u8"键鼠模式瞄准镜覆盖层角度偏移" },
        { u8"Angular offset for the scope overlay when using mouse mode.",
          u8"键鼠模式下瞄准镜覆盖层的角度偏移。" },
        { u8"Adjust this if the scope overlay appears rotated incorrectly.",
          u8"如果瞄准镜覆盖层角度不对，可以用它校正。" },
        -180.0f, 180.0f,
        "0,0,0"
    },
    {
        "AutoRepeatSprayPushDelayTicks",
        OptionType::Int,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Spray-Push Delay Ticks", u8"Spray-Push 延迟 Tick" },
        { u8"Delay before the automatic spray-push assist is applied.",
          u8"自动 spray-push 辅助触发前等待的 Tick 数。" },
        { u8"Increase it if the assist happens too early for your weapon timing.",
          u8"如果辅助触发得太早，可以把它调大。" },
        0.0f, 120.0f,
        "0"
    },
    {
        "AutoRepeatSprayPushHoldTicks",
        OptionType::Int,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Spray-Push Hold Ticks", u8"Spray-Push 保持 Tick" },
        { u8"How long the automatic spray-push assist stays active once triggered.",
          u8"自动 spray-push 辅助触发后保持生效的 Tick 数。" },
        { u8"Use a low value unless your weapon timing needs more hold time.",
          u8"除非武器节奏确实需要，否则建议保持较低数值。" },
        0.0f, 120.0f,
        "1"
    },
    {
        "AutoFastMeleeShoveEcho",
        OptionType::Bool,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Auto Fast-Melee Shove Echo", u8"自动快近战推击回显" },
        { u8"Adds an extra shove-style echo step during the auto fast-melee routine.",
          u8"在自动快近战流程中额外加入一次类似推击的回显步骤。" },
        { u8"Useful only if your current timing benefits from that extra shove signal.",
          u8"只有在你的当前节奏确实需要额外推击信号时才建议开启。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "AutoFastMeleeUseWeaponSwitch",
        OptionType::Bool,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Auto Fast-Melee Uses Weapon Switch", u8"自动快近战使用切枪步骤" },
        { u8"Allows the auto fast-melee routine to include a weapon-switch step.",
          u8"允许自动快近战流程里包含一次切枪步骤。" },
        { u8"Disable it only if weapon switching breaks your preferred timing.",
          u8"只有在切枪会打乱你的节奏时才建议关闭。" },
        0.0f, 0.0f,
        "true"
    },
    {
        "AutoFastMeleePushWaitTicks",
        OptionType::Int,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Auto Fast-Melee Push Wait", u8"自动快近战推击等待" },
        { u8"Ticks to wait after the push step during auto fast-melee.",
          u8"自动快近战流程中推击步骤之后等待的 Tick 数。" },
        { u8"Raise it if your combo is getting cut off too early.",
          u8"如果连段太早被打断，可以调大。" },
        0.0f, 120.0f,
        "2"
    },
    {
        "AutoFastMeleePostWaitTicks",
        OptionType::Int,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Auto Fast-Melee Post Wait", u8"自动快近战收尾等待" },
        { u8"Ticks to wait before the auto fast-melee routine resets.",
          u8"自动快近战流程重置前等待的 Tick 数。" },
        { u8"Increase it if the macro restarts before the animation has settled.",
          u8"如果宏在动作还没收完时就重新开始，可以调大。" },
        0.0f, 120.0f,
        "29"
    },
    {
        "HitIndicatorEnabled",
        OptionType::Bool,
        { u8"Weapons / Fire", u8"武器 / 开火" },
        { u8"Hit Indicator", u8"命中指示器" },
        { u8"Shows a visual hit indicator when your shot connects.",
          u8"当你的射击命中时显示视觉命中提示。" },
        { u8"Useful if you want feedback without relying only on sound.",
          u8"如果你不想只依赖音效反馈，可以开启它。" },
        0.0f, 0.0f,
        "false"
    },
    {
        "AutoFlashlightEnabled",
        OptionType::Bool,
        { u8"General", u8"通用" },
        { u8"Automatic Flashlight", u8"自动手电筒" },
        { u8"Automatically manages flashlight usage while playing.",
          u8"在游玩过程中自动管理手电筒的开关。" },
        { u8"Useful on dark maps if you do not want to toggle the flashlight manually.",
          u8"在黑暗地图里如果不想手动开关手电，可以开启。" },
        0.0f, 0.0f,
        "true"
    }
};

const int g_OptionCount = (int)(sizeof(g_Options) / sizeof(g_Options[0]));

// ============================================================
// UI renderer
// ============================================================

void DrawOptionsUI()
{
    auto renderOption = [](const Option& opt)
    {
        std::string key = opt.key;
        ImGui::PushID(opt.key);

        switch (opt.type)
        {
        case OptionType::Bool:
        {
            bool v = (key == "KillIndicatorEnabled")
                ? GetHitKillIndicatorsEnabled()
                : ParseBool(GetStr(key), GetDefaultBool(opt));
            if (ImGui::Checkbox(L(opt.title), &v))
            {
                if (key == "KillIndicatorEnabled")
                    SetHitKillIndicatorsEnabled(v);
                else
                    g_Values[key] = v ? "true" : "false";
            }
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
            const ImGuiColorEditFlags colorFlags =
                ImGuiColorEditFlags_DisplayRGB |
                ImGuiColorEditFlags_InputRGB |
                ImGuiColorEditFlags_Uint8;
            if (ImGui::ColorEdit4(L(opt.title), (float*)&c, colorFlags))
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

        ImGui::Spacing();
        ImGui::PopID();
    };

    std::vector<std::string> groupOrder;
    groupOrder.reserve(g_OptionCount);

    for (int i = 0; i < g_OptionCount; ++i)
    {
        const Option& opt = g_Options[i];
        if (!IsAllowedOptionKey(opt.key) || !IsOptionVisible(opt) || !OptionMatchesFilter(opt))
            continue;

        const std::string group = L(opt.group);
        if (std::find(groupOrder.begin(), groupOrder.end(), group) == groupOrder.end())
            groupOrder.push_back(group);
    }

    for (const std::string& group : groupOrder)
    {
        if (!group.empty())
            ImGui::SeparatorText(group.c_str());

        for (int i = 0; i < g_OptionCount; ++i)
        {
            const Option& opt = g_Options[i];
            if (!IsAllowedOptionKey(opt.key) || !IsOptionVisible(opt) || !OptionMatchesFilter(opt))
                continue;

            if (group != L(opt.group))
                continue;

            renderOption(opt);
        }
    }
}
