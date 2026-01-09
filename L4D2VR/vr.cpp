
#include "vr.h"
#include <Windows.h>
#include "sdk.h"
#include "game.h"
#include "hooks.h"
#include "trace.h"
#include "sdk/ivdebugoverlay.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>
#include <d3d9_vr.h>

namespace
{
    // Returns true if the call should be skipped because we ran it too recently.
    inline bool ShouldThrottle(std::chrono::steady_clock::time_point& last, float maxHz)
    {
        if (maxHz <= 0.0f)
            return false;

        const float minInterval = 1.0f / std::max(1.0f, maxHz);
        const auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count() != 0)
        {
            const float elapsed = std::chrono::duration<float>(now - last).count();
            if (elapsed < minInterval)
                return true;
        }
        last = now;
        return false;
    }

    inline float MinIntervalSeconds(float maxHz)
    {
        if (maxHz <= 0.0f)
            return 0.0f;
        return 1.0f / std::max(1.0f, maxHz);
    }


    // Normalize Source-style view angles:
    // - Bring pitch/yaw into [-180, 180] first (avoid -30 becoming 330 then clamped to 89).
    // - Then clamp pitch to [-89, 89].
    inline void NormalizeAndClampViewAngles(QAngle& a)
    {
        while (a.x > 180.f) a.x -= 360.f;
        while (a.x < -180.f) a.x += 360.f;
        while (a.y > 180.f) a.y -= 360.f;
        while (a.y < -180.f) a.y += 360.f;
        a.z = 0.f;
        if (a.x > 89.f) a.x = 89.f;
        if (a.x < -89.f) a.x = -89.f;
    }
    inline bool IsFirearmWeaponId(C_WeaponCSBase::WeaponID id)
    {
        switch (id)
        {
        case C_WeaponCSBase::WeaponID::PISTOL:
        case C_WeaponCSBase::WeaponID::MAGNUM:
        case C_WeaponCSBase::WeaponID::UZI:
        case C_WeaponCSBase::WeaponID::MAC10:
        case C_WeaponCSBase::WeaponID::MP5:

        case C_WeaponCSBase::WeaponID::PUMPSHOTGUN:
        case C_WeaponCSBase::WeaponID::SHOTGUN_CHROME:
        case C_WeaponCSBase::WeaponID::AUTOSHOTGUN:
        case C_WeaponCSBase::WeaponID::SPAS:

        case C_WeaponCSBase::WeaponID::M16A1:
        case C_WeaponCSBase::WeaponID::AK47:
        case C_WeaponCSBase::WeaponID::SCAR:
        case C_WeaponCSBase::WeaponID::SG552:

        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE:
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
        case C_WeaponCSBase::WeaponID::AWP:
        case C_WeaponCSBase::WeaponID::SCOUT:

        case C_WeaponCSBase::WeaponID::GRENADE_LAUNCHER:
        case C_WeaponCSBase::WeaponID::M60:
        case C_WeaponCSBase::WeaponID::MACHINEGUN:
            return true;

        default:
            return false;
        }
    }
}

// ----------------------------
// One Euro filter helpers (for scope stabilization)
// ----------------------------
constexpr float kPi = 3.14159265358979323846f;

inline float OneEuroAlpha(float cutoffHz, float dt)
{
    cutoffHz = std::max(0.0001f, cutoffHz);
    dt = std::max(0.000001f, dt);
    const float tau = 1.0f / (2.0f * kPi * cutoffHz);
    return 1.0f / (1.0f + tau / dt);
}

inline float AngleDeltaDeg(float a, float b)
{
    float d = a - b;
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    return d;
}

inline Vector OneEuroFilterVec3(
    const Vector& x,
    Vector& xHat,
    Vector& dxHat,
    bool& initialized,
    float dt,
    float minCutoff,
    float beta,
    float dCutoff)
{
    if (!initialized)
    {
        initialized = true;
        xHat = x;
        dxHat = { 0.0f, 0.0f, 0.0f };
        return xHat;
    }

    const Vector dx = (x - xHat) * (1.0f / std::max(0.000001f, dt));
    const float aD = OneEuroAlpha(dCutoff, dt);
    dxHat = dxHat + (dx - dxHat) * aD;

    const float speed = VectorLength(dxHat);
    const float cutoff = std::max(0.0001f, minCutoff + beta * speed);
    const float a = OneEuroAlpha(cutoff, dt);
    xHat = xHat + (x - xHat) * a;
    return xHat;
}

inline QAngle OneEuroFilterAngles(
    const QAngle& x,
    QAngle& xHat,
    QAngle& dxHat,
    bool& initialized,
    float dt,
    float minCutoff,
    float beta,
    float dCutoff)
{
    if (!initialized)
    {
        initialized = true;
        xHat = x;
        dxHat = { 0.0f, 0.0f, 0.0f };
        return xHat;
    }

    const float invDt = 1.0f / std::max(0.000001f, dt);
    const QAngle dx = {
        AngleDeltaDeg(x.x, xHat.x) * invDt,
        AngleDeltaDeg(x.y, xHat.y) * invDt,
        AngleDeltaDeg(x.z, xHat.z) * invDt
    };

    const float aD = OneEuroAlpha(dCutoff, dt);
    dxHat.x = dxHat.x + (dx.x - dxHat.x) * aD;
    dxHat.y = dxHat.y + (dx.y - dxHat.y) * aD;
    dxHat.z = dxHat.z + (dx.z - dxHat.z) * aD;

    const float speed = std::sqrt(dxHat.x * dxHat.x + dxHat.y * dxHat.y + dxHat.z * dxHat.z);
    const float cutoff = std::max(0.0001f, minCutoff + beta * speed);
    const float a = OneEuroAlpha(cutoff, dt);

    xHat.x = xHat.x + AngleDeltaDeg(x.x, xHat.x) * a;
    xHat.y = xHat.y + AngleDeltaDeg(x.y, xHat.y) * a;
    xHat.z = xHat.z + AngleDeltaDeg(x.z, xHat.z) * a;

    // Keep angles in a sane range.
    while (xHat.x > 180.f) xHat.x -= 360.f;
    while (xHat.x < -180.f) xHat.x += 360.f;
    while (xHat.y > 180.f) xHat.y -= 360.f;
    while (xHat.y < -180.f) xHat.y += 360.f;
    while (xHat.z > 180.f) xHat.z -= 360.f;
    while (xHat.z < -180.f) xHat.z += 360.f;
    return xHat;
}

VR::VR(Game* game)
{
    m_Game = game;

    char errorString[MAX_STR_LEN];

    vr::HmdError error = vr::VRInitError_None;
    m_System = vr::VR_Init(&error, vr::VRApplication_Scene);

    if (error != vr::VRInitError_None)
    {
        snprintf(errorString, MAX_STR_LEN, "VR_Init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        Game::errorMsg(errorString);
        return;
    }

    m_Compositor = vr::VRCompositor();
    if (!m_Compositor)
    {
        Game::errorMsg("Compositor initialization failed.");
        return;
    }

    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    m_ViewmodelAdjustmentSavePath = std::string(currentDir) + "\\viewmodel_adjustments.txt";
    LoadViewmodelAdjustments();

    ConfigureExplicitTiming();

    m_Input = vr::VRInput();
    m_System = vr::OpenVRInternal_ModuleContext().VRSystem();

    m_System->GetRecommendedRenderTargetSize(&m_RenderWidth, &m_RenderHeight);
    m_AntiAliasing = 0;

    float l_left = 0.0f, l_right = 0.0f, l_top = 0.0f, l_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Left, &l_left, &l_right, &l_top, &l_bottom);

    float r_left = 0.0f, r_right = 0.0f, r_top = 0.0f, r_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Right, &r_left, &r_right, &r_top, &r_bottom);

    float tanHalfFov[2];

    tanHalfFov[0] = std::max({ -l_left, l_right, -r_left, r_right });
    tanHalfFov[1] = std::max({ -l_top, l_bottom, -r_top, r_bottom });
    // For some headsets, the driver provided texture size doesn't match the geometric aspect ratio of the lenses.
    // In this case, we need to adjust the vertical tangent while still rendering to the recommended RT size.
    m_TextureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFov[0];
    m_TextureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFov[0];
    m_TextureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFov[1];
    m_TextureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFov[1];

    m_TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFov[0];
    m_TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFov[0];
    m_TextureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFov[1];
    m_TextureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFov[1];

    m_Aspect = tanHalfFov[0] / tanHalfFov[1];
    m_Fov = 2.0f * atan(tanHalfFov[0]) * 360 / (3.14159265358979323846 * 2);

    InstallApplicationManifest("manifest.vrmanifest");
    SetActionManifest("action_manifest.json");

    std::thread configParser(&VR::WaitForConfigUpdate, this);
    configParser.detach();

    while (!g_D3DVR9)
        Sleep(10);

    g_D3DVR9->GetBackBufferData(&m_VKBackBuffer);
    m_Overlay = vr::VROverlay();
    m_Overlay->CreateOverlay("MenuOverlayKey", "MenuOverlay", &m_MainMenuHandle);
    m_Overlay->CreateOverlay("HUDOverlayTopKey", "HUDOverlayTop", &m_HUDTopHandle);

    const char* bottomOverlayKeys[4] = { "HUDOverlayBottom1", "HUDOverlayBottom2", "HUDOverlayBottom3", "HUDOverlayBottom4" };
    for (int i = 0; i < 4; ++i)
    {
        m_Overlay->CreateOverlay(bottomOverlayKeys[i], bottomOverlayKeys[i], &m_HUDBottomHandles[i]);
    }

    // Gun-mounted scope lens overlay (render-to-texture)
    m_Overlay->CreateOverlay("ScopeOverlayKey", "ScopeOverlay", &m_ScopeHandle);
    m_Overlay->CreateOverlay("RearMirrorOverlayKey", "RearMirrorOverlay", &m_RearMirrorHandle);

    m_Overlay->SetOverlayInputMethod(m_MainMenuHandle, vr::VROverlayInputMethod_Mouse);
    m_Overlay->SetOverlayInputMethod(m_HUDTopHandle, vr::VROverlayInputMethod_Mouse);
    for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
    {
        m_Overlay->SetOverlayInputMethod(overlay, vr::VROverlayInputMethod_Mouse);
    }

    // Scope overlay is purely visual
    m_Overlay->SetOverlayInputMethod(m_ScopeHandle, vr::VROverlayInputMethod_None);
    m_Overlay->SetOverlayInputMethod(m_RearMirrorHandle, vr::VROverlayInputMethod_None);
    m_Overlay->SetOverlayFlag(m_ScopeHandle, vr::VROverlayFlags_IgnoreTextureAlpha, true);
    m_Overlay->SetOverlayFlag(m_RearMirrorHandle, vr::VROverlayFlags_IgnoreTextureAlpha, true);


    m_Overlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    m_Overlay->SetOverlayFlag(m_HUDTopHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
    {
        m_Overlay->SetOverlayFlag(overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    }

    int windowWidth, windowHeight;
    m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

    const vr::HmdVector2_t mouseScaleHUD = { windowWidth, windowHeight };
    m_Overlay->SetOverlayMouseScale(m_HUDTopHandle, &mouseScaleHUD);
    for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
    {
        m_Overlay->SetOverlayMouseScale(overlay, &mouseScaleHUD);
    }

    const vr::HmdVector2_t mouseScaleMenu = { m_RenderWidth, m_RenderHeight };
    m_Overlay->SetOverlayMouseScale(m_MainMenuHandle, &mouseScaleMenu);

    UpdatePosesAndActions();
    FinishFrame();

    m_IsInitialized = true;
    m_IsVREnabled = true;
}

void VR::ConfigureExplicitTiming()
{
    if (!m_Compositor)
        return;

    m_Compositor->SetExplicitTimingMode(
        vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);

    m_CompositorExplicitTiming = true;
}

int VR::SetActionManifest(const char* fileName)
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char path[MAX_STR_LEN];
    sprintf_s(path, MAX_STR_LEN, "%s\\VR\\SteamVRActionManifest\\%s", currentDir, fileName);

    if (m_Input->SetActionManifestPath(path) != vr::VRInputError_None)
    {
        Game::errorMsg("SetActionManifestPath failed");
    }

    m_Input->GetActionHandle("/actions/main/in/ActivateVR", &m_ActionActivateVR);
    m_Input->GetActionHandle("/actions/main/in/Jump", &m_ActionJump);
    m_Input->GetActionHandle("/actions/main/in/PrimaryAttack", &m_ActionPrimaryAttack);
    m_Input->GetActionHandle("/actions/main/in/Reload", &m_ActionReload);
    m_Input->GetActionHandle("/actions/main/in/Use", &m_ActionUse);
    m_Input->GetActionHandle("/actions/main/in/Walk", &m_ActionWalk);
    m_Input->GetActionHandle("/actions/main/in/Turn", &m_ActionTurn);
    m_Input->GetActionHandle("/actions/main/in/SecondaryAttack", &m_ActionSecondaryAttack);
    m_Input->GetActionHandle("/actions/main/in/NextItem", &m_ActionNextItem);
    m_Input->GetActionHandle("/actions/main/in/PrevItem", &m_ActionPrevItem);
    m_Input->GetActionHandle("/actions/main/in/ResetPosition", &m_ActionResetPosition);
    m_Input->GetActionHandle("/actions/main/in/Crouch", &m_ActionCrouch);
    m_Input->GetActionHandle("/actions/main/in/Flashlight", &m_ActionFlashlight);
    m_Input->GetActionHandle("/actions/main/in/SpecialInfectedAutoAimToggle", &m_ActionSpecialInfectedAutoAimToggle);
    m_Input->GetActionHandle("/actions/main/in/MenuSelect", &m_MenuSelect);
    m_Input->GetActionHandle("/actions/main/in/MenuBack", &m_MenuBack);
    m_Input->GetActionHandle("/actions/main/in/MenuUp", &m_MenuUp);
    m_Input->GetActionHandle("/actions/main/in/MenuDown", &m_MenuDown);
    m_Input->GetActionHandle("/actions/main/in/MenuLeft", &m_MenuLeft);
    m_Input->GetActionHandle("/actions/main/in/MenuRight", &m_MenuRight);
    m_Input->GetActionHandle("/actions/main/in/Spray", &m_Spray);
    m_Input->GetActionHandle("/actions/main/in/Scoreboard", &m_Scoreboard);
    m_Input->GetActionHandle("/actions/main/in/ToggleHUD", &m_ToggleHUD);
    m_Input->GetActionHandle("/actions/main/in/Pause", &m_Pause);
    m_Input->GetActionHandle("/actions/main/in/NonVRServerMovementAngleToggle", &m_NonVRServerMovementAngleToggle);
    m_Input->GetActionHandle("/actions/main/in/ScopeMagnificationToggle", &m_ActionScopeMagnificationToggle);
    m_Input->GetActionHandle("/actions/main/in/CustomAction1", &m_CustomAction1);
    m_Input->GetActionHandle("/actions/main/in/CustomAction2", &m_CustomAction2);
    m_Input->GetActionHandle("/actions/main/in/CustomAction3", &m_CustomAction3);
    m_Input->GetActionHandle("/actions/main/in/CustomAction4", &m_CustomAction4);
    m_Input->GetActionHandle("/actions/main/in/CustomAction5", &m_CustomAction5);

    m_Input->GetActionSetHandle("/actions/main", &m_ActionSet);
    m_ActiveActionSet = {};
    m_ActiveActionSet.ulActionSet = m_ActionSet;

    return 0;
}

void VR::InstallApplicationManifest(const char* fileName)
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char path[MAX_STR_LEN];
    sprintf_s(path, MAX_STR_LEN, "%s\\VR\\%s", currentDir, fileName);

    vr::VRApplications()->AddApplicationManifest(path);
}


void VR::Update()
{
    if (!m_IsInitialized || !m_Game->m_Initialized)
        return;

    if (m_IsVREnabled && g_D3DVR9)
    {
        // Prevents crashing at menu
        if (!m_Game->m_EngineClient->IsInGame())
        {
            IMatRenderContext* rndrContext = m_Game->m_MaterialSystem->GetRenderContext();
            if (!rndrContext)
            {
                HandleMissingRenderContext("VR::Update");
                return;
            }
            rndrContext->SetRenderTarget(NULL);
            m_Game->m_CachedArmsModel = false;
            m_CreatedVRTextures = false; // Have to recreate textures otherwise some workshop maps won't render
        }
    }

    SubmitVRTextures();

    bool posesValid = UpdatePosesAndActions();

    if (!posesValid)
    {
        // Continue using the last known poses so smoothing and aim helpers stay active.
    }

    UpdateTracking();


    if (m_Game->m_VguiSurface->IsCursorVisible())
        ProcessMenuInput();
    else
        ProcessInput();
}

bool VR::GetWalkAxis(float& x, float& y) {
    vr::InputAnalogActionData_t d;
    if (GetAnalogActionData(m_ActionWalk, d)) {  // m_ActionWalk        д     ʹ  
        x = d.x; y = d.y;
        return true;
    }
    x = y = 0.0f;
    return false;
}

void VR::CreateVRTextures()
{
    int windowWidth, windowHeight;
    m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

    m_Game->m_MaterialSystem->isGameRunning = false;
    m_Game->m_MaterialSystem->BeginRenderTargetAllocation();
    m_Game->m_MaterialSystem->isGameRunning = true;

    m_CreatingTextureID = Texture_LeftEye;
    m_LeftEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("leftEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SEPARATE, TEXTUREFLAGS_NOMIP);
    m_CreatingTextureID = Texture_RightEye;
    m_RightEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("rightEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SEPARATE, TEXTUREFLAGS_NOMIP);
    m_CreatingTextureID = Texture_HUD;
    m_HUDTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("vrHUD", windowWidth, windowHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);

    // Square RTT for gun-mounted scope lens
    m_CreatingTextureID = Texture_Scope;
    m_ScopeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx(
        "vrScope",
        static_cast<int>(m_ScopeRTTSize),
        static_cast<int>(m_ScopeRTTSize),
        RT_SIZE_NO_CHANGE,
        m_Game->m_MaterialSystem->GetBackBufferFormat(),
        MATERIAL_RT_DEPTH_SEPARATE,
        TEXTUREFLAGS_NOMIP);

    // Square RTT for off-hand rear mirror
    m_CreatingTextureID = Texture_RearMirror;
    m_RearMirrorTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx(
        "vrRearMirror",
        static_cast<int>(m_RearMirrorRTTSize),
        static_cast<int>(m_RearMirrorRTTSize),
        RT_SIZE_NO_CHANGE,
        m_Game->m_MaterialSystem->GetBackBufferFormat(),
        MATERIAL_RT_DEPTH_SEPARATE,
        TEXTUREFLAGS_NOMIP);

    m_CreatingTextureID = Texture_Blank;
    m_BlankTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("blankTexture", 512, 512, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);

    m_CreatingTextureID = Texture_None;

    m_Game->m_MaterialSystem->EndRenderTargetAllocation();

    m_CreatedVRTextures = true;
}

void VR::SubmitVRTextures()
{
    if (!m_Compositor)
        return;

    bool successfulSubmit = false;
    bool timingDataSubmitted = false;

    auto ensureTimingData = [&]()
        {
            if (!m_CompositorExplicitTiming || timingDataSubmitted)
                return;

            vr::EVRCompositorError timingError = m_Compositor->SubmitExplicitTimingData();
            if (timingError != vr::VRCompositorError_None)
            {
                LogCompositorError("SubmitExplicitTimingData", timingError);
            }

            timingDataSubmitted = true;
        };

    auto submitEye = [&](vr::EVREye eye, vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds)
        {
            ensureTimingData();

            vr::EVRCompositorError submitError = m_Compositor->Submit(eye, texture, bounds, vr::Submit_Default);
            if (submitError != vr::VRCompositorError_None)
            {
                LogCompositorError("Submit", submitError);
                return false;
            }

            successfulSubmit = true;
            return true;
        };

    auto hideHudOverlays = [&]()
        {
            vr::VROverlay()->HideOverlay(m_HUDTopHandle);
            for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
                vr::VROverlay()->HideOverlay(overlay);
        };

    const vr::VRTextureBounds_t topBounds{ 0.0f, 0.0f, 1.0f, 0.5f };
    const std::array<vr::VRTextureBounds_t, 4> bottomBounds{
        vr::VRTextureBounds_t{ 0.0f, 0.5f, 0.25f, 1.0f },
        vr::VRTextureBounds_t{ 0.25f, 0.5f, 0.5f, 1.0f },
        vr::VRTextureBounds_t{ 0.5f, 0.5f, 0.75f, 1.0f },
        vr::VRTextureBounds_t{ 0.75f, 0.5f, 1.0f, 1.0f }
    };

    auto applyHudTexture = [&](vr::VROverlayHandle_t overlay, const vr::VRTextureBounds_t& bounds)
        {
            vr::VROverlay()->SetOverlayTextureBounds(overlay, &bounds);
            vr::VROverlay()->SetOverlayTexture(overlay, &m_VKHUD.m_VRTexture);
        };

    auto applyScopeTexture = [&](vr::VROverlayHandle_t overlay)
        {
            static const vr::VRTextureBounds_t full{ 0.0f, 0.0f, 1.0f, 1.0f };
            vr::VROverlay()->SetOverlayTextureBounds(overlay, &full);
            vr::VROverlay()->SetOverlayTexture(overlay, &m_VKScope.m_VRTexture);
        };
    auto applyRearMirrorTexture = [&](vr::VROverlayHandle_t overlay)
        {
            static const vr::VRTextureBounds_t full{ 0.0f, 0.0f, 1.0f, 1.0f };
            vr::VROverlay()->SetOverlayTextureBounds(overlay, &full);
            vr::VROverlay()->SetOverlayTexture(overlay, &m_VKRearMirror.m_VRTexture);
        };

    //     ֡û       ݣ    ߲˵ /Overlay ·  
    if (!m_RenderedNewFrame)
    {
        if (!m_BlankTexture)
            CreateVRTextures();

        if (!vr::VROverlay()->IsOverlayVisible(m_MainMenuHandle))
            RepositionOverlays();

        vr::VROverlay()->SetOverlayTexture(m_MainMenuHandle, &m_VKBackBuffer.m_VRTexture);
        vr::VROverlay()->ShowOverlay(m_MainMenuHandle);
        hideHudOverlays();
        vr::VROverlay()->HideOverlay(m_ScopeHandle);
        vr::VROverlay()->HideOverlay(m_RearMirrorHandle);

        if (!m_Game->m_EngineClient->IsInGame())
        {
            submitEye(vr::Eye_Left, &m_VKBlankTexture.m_VRTexture, nullptr);
            submitEye(vr::Eye_Right, &m_VKBlankTexture.m_VRTexture, nullptr);
        }

        if (successfulSubmit && m_CompositorExplicitTiming)
        {
            m_CompositorNeedsHandoff = true;
            FinishFrame();
        }

        return;
    }


    vr::VROverlay()->HideOverlay(m_MainMenuHandle);
    applyHudTexture(m_HUDTopHandle, topBounds);
    for (int i = 0; i < 4; ++i)
    {
        applyHudTexture(m_HUDBottomHandles[i], bottomBounds[i]);
    }
    if (m_Game->m_VguiSurface->IsCursorVisible())
    {
        vr::VROverlay()->ShowOverlay(m_HUDTopHandle);
        for (size_t i = 0; i < m_HUDBottomHandles.size(); ++i)
        {
            if (i == 0 && m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand) == vr::k_unTrackedDeviceIndexInvalid)
                continue;
            if (i == 3 && m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand) == vr::k_unTrackedDeviceIndexInvalid)
                continue;

            vr::VROverlay()->ShowOverlay(m_HUDBottomHandles[i]);
        }
    }

    // Scope overlay independent of HUD cursor mode
    if (m_ScopeTexture && m_ScopeEnabled)
    {
        applyScopeTexture(m_ScopeHandle);
        const float alpha = IsScopeActive() ? 1.0f : std::clamp(m_ScopeOverlayIdleAlpha, 0.0f, 1.0f);
        vr::VROverlay()->SetOverlayAlpha(m_ScopeHandle, alpha);

        vr::TrackedDeviceIndex_t leftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        vr::TrackedDeviceIndex_t rightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        if (m_LeftHanded)
            std::swap(leftControllerIndex, rightControllerIndex);
        const vr::TrackedDeviceIndex_t gunControllerIndex = rightControllerIndex;

        if (ShouldRenderScope() && gunControllerIndex != vr::k_unTrackedDeviceIndexInvalid)
            vr::VROverlay()->ShowOverlay(m_ScopeHandle);
        else
            vr::VROverlay()->HideOverlay(m_ScopeHandle);
    }
    else
    {
        vr::VROverlay()->HideOverlay(m_ScopeHandle);
    }

    if (m_RearMirrorTexture && m_RearMirrorEnabled)
    {
        applyRearMirrorTexture(m_RearMirrorHandle);
        vr::VROverlay()->SetOverlayAlpha(m_RearMirrorHandle, std::clamp(m_RearMirrorAlpha, 0.0f, 1.0f));

        vr::TrackedDeviceIndex_t leftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        vr::TrackedDeviceIndex_t rightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        if (m_LeftHanded)
            std::swap(leftControllerIndex, rightControllerIndex);
        const vr::TrackedDeviceIndex_t offHandControllerIndex = leftControllerIndex;

        if (ShouldRenderRearMirror() && offHandControllerIndex != vr::k_unTrackedDeviceIndexInvalid)
            vr::VROverlay()->ShowOverlay(m_RearMirrorHandle);
        else
            vr::VROverlay()->HideOverlay(m_RearMirrorHandle);
    }
    else
    {
        vr::VROverlay()->HideOverlay(m_RearMirrorHandle);
    }

    submitEye(vr::Eye_Left, &m_VKLeftEye.m_VRTexture, &(m_TextureBounds)[0]);
    submitEye(vr::Eye_Right, &m_VKRightEye.m_VRTexture, &(m_TextureBounds)[1]);

    if (successfulSubmit && m_CompositorExplicitTiming)
    {
        m_CompositorNeedsHandoff = true;
        FinishFrame();
    }


    m_RenderedNewFrame = false;
}

void VR::LogCompositorError(const char* action, vr::EVRCompositorError error)
{
    if (error == vr::VRCompositorError_None || !action)
        return;

    constexpr auto kLogCooldown = std::chrono::seconds(5);
    const auto now = std::chrono::steady_clock::now();

    if (now - m_LastCompositorErrorLog < kLogCooldown)
        return;

    Game::logMsg("[VR] %s failed with VRCompositorError %d", action, static_cast<int>(error));
    m_LastCompositorErrorLog = now;
}


void VR::GetPoseData(vr::TrackedDevicePose_t& poseRaw, TrackedDevicePoseData& poseOut)
{
    if (poseRaw.bPoseIsValid)
    {
        vr::HmdMatrix34_t mat = poseRaw.mDeviceToAbsoluteTracking;
        Vector pos;
        Vector vel;
        QAngle ang;
        QAngle angvel;
        pos.x = -mat.m[2][3];
        pos.y = -mat.m[0][3];
        pos.z = mat.m[1][3];
        ang.x = asin(mat.m[1][2]) * (180.0 / 3.141592654);
        ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0 / 3.141592654);
        ang.z = atan2f(-mat.m[1][0], mat.m[1][1]) * (180.0 / 3.141592654);
        vel.x = -poseRaw.vVelocity.v[2];
        vel.y = -poseRaw.vVelocity.v[0];
        vel.z = poseRaw.vVelocity.v[1];
        angvel.x = -poseRaw.vAngularVelocity.v[2] * (180.0 / 3.141592654);
        angvel.y = -poseRaw.vAngularVelocity.v[0] * (180.0 / 3.141592654);
        angvel.z = poseRaw.vAngularVelocity.v[1] * (180.0 / 3.141592654);

        poseOut.TrackedDevicePos = pos;
        poseOut.TrackedDeviceVel = vel;
        poseOut.TrackedDeviceAng = ang;
        poseOut.TrackedDeviceAngVel = angvel;
    }
}

void VR::RepositionOverlays(bool attachToControllers)
{
    vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;
    Vector hmdPosition = { hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] };
    Vector hmdForward = { -hmdMat.m[0][2], 0, -hmdMat.m[2][2] };

    int windowWidth, windowHeight;
    m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

    vr::HmdMatrix34_t menuTransform =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 1.0f
    };

    vr::ETrackingUniverseOrigin trackingOrigin = vr::VRCompositor()->GetTrackingSpace();

    // Reposition main menu overlay
    float renderWidth = m_VKBackBuffer.m_VulkanData.m_nWidth;
    float renderHeight = m_VKBackBuffer.m_VulkanData.m_nHeight;

    float widthRatio = windowWidth / renderWidth;
    float heightRatio = windowHeight / renderHeight;
    menuTransform.m[0][0] *= widthRatio;
    menuTransform.m[1][1] *= heightRatio;

    hmdForward[1] = 0;
    VectorNormalize(hmdForward);

    Vector menuDistance = hmdForward * 3;
    Vector menuNewPos = menuDistance + hmdPosition;

    menuTransform.m[0][3] = menuNewPos.x;
    menuTransform.m[1][3] = menuNewPos.y - 0.25;
    menuTransform.m[2][3] = menuNewPos.z;

    float xScale = menuTransform.m[0][0];
    float hmdRotationDegrees = atan2f(hmdMat.m[0][2], hmdMat.m[2][2]);

    menuTransform.m[0][0] *= cos(hmdRotationDegrees);
    menuTransform.m[0][2] = sin(hmdRotationDegrees);
    menuTransform.m[2][0] = -sin(hmdRotationDegrees) * xScale;
    menuTransform.m[2][2] *= cos(hmdRotationDegrees);

    vr::VROverlay()->SetOverlayTransformAbsolute(m_MainMenuHandle, trackingOrigin, &menuTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_MainMenuHandle, 1.5 * (1.0 / heightRatio));

    auto buildFacingTransform = [&](const Vector& position)
        {
            vr::HmdMatrix34_t transform =
            {
                1.0f, 0.0f, 0.0f, position.x,
                0.0f, 1.0f, 0.0f, position.y,
                0.0f, 0.0f, 1.0f, position.z
            };

            float cosYaw = cos(hmdRotationDegrees);
            float sinYaw = sin(hmdRotationDegrees);
            transform.m[0][0] *= cosYaw;
            transform.m[0][2] = sinYaw;
            transform.m[2][0] = -sinYaw;
            transform.m[2][2] *= cosYaw;

            return transform;
        };

    // Reposition HUD overlays
    Vector hudDistance = hmdForward * (m_HudDistance + m_FixedHudDistanceOffset);
    Vector hudNewPos = hudDistance + hmdPosition;
    hudNewPos.y -= 0.25f;
    hudNewPos.y += m_FixedHudYOffset;

    float hudAspect = static_cast<float>(windowHeight) / static_cast<float>(windowWidth);
    float hudHalfStackOffset = (m_HudSize * hudAspect) * 0.25f;

    Vector hudCenterPos = hudNewPos;
    Vector hudTopPos = hudCenterPos;
    hudTopPos.y += hudHalfStackOffset;

    vr::HmdMatrix34_t hudTopTransform = buildFacingTransform(hudTopPos);

    vr::VROverlay()->SetOverlayTransformAbsolute(m_HUDTopHandle, trackingOrigin, &hudTopTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_HUDTopHandle, m_HudSize);

    Vector hudRight = { cos(hmdRotationDegrees), 0.0f, -sin(hmdRotationDegrees) };
    float segmentWidth = m_HudSize / 4.0f;

    for (size_t i = 0; i < m_HUDBottomHandles.size(); ++i)
    {
        vr::VROverlayHandle_t overlay = m_HUDBottomHandles[i];

        // Bottom 1 & 4 attach to controllers, 2-3 stay fixed in front
        if (attachToControllers && (i == 0 || i == 3))
        {
            vr::ETrackedControllerRole controllerRole = (i == 0) ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
            vr::TrackedDeviceIndex_t controllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(controllerRole);

            if (controllerIndex != vr::k_unTrackedDeviceIndexInvalid)
            {
                // m_ControllerHudRotation is in degrees; allow any magnitude (e.g., 15, 90, 360+) for easier tuning.
                const float controllerHudRotationRad = m_ControllerHudRotation * (3.14159265358979323846f / 180.0f);
                const float cosRotation = cosf(controllerHudRotationRad);
                const float sinRotation = sinf(controllerHudRotationRad);

                const float controllerHudXOffset = (i == 0) ? -m_ControllerHudXOffset : m_ControllerHudXOffset;

                vr::HmdMatrix34_t relativeTransform =
                {
                    1.0f, 0.0f, 0.0f, controllerHudXOffset,
                    0.0f, cosRotation, -sinRotation, m_ControllerHudYOffset - hudHalfStackOffset,
                    0.0f, sinRotation,  cosRotation, m_ControllerHudZOffset
                };

                vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(overlay, controllerIndex, &relativeTransform);
                vr::VROverlay()->SetOverlayWidthInMeters(overlay, m_ControllerHudSize);
            }
            else
            {
                vr::VROverlay()->HideOverlay(overlay);
            }
        }
        else
        {
            const float segmentIndexOffset = static_cast<float>(i) - 1.5f;
            Vector offset = hudRight * (segmentIndexOffset * segmentWidth);
            Vector segmentPos = hudCenterPos + offset;
            segmentPos.y -= hudHalfStackOffset;
            vr::HmdMatrix34_t segmentTransform = buildFacingTransform(segmentPos);
            vr::VROverlay()->SetOverlayTransformAbsolute(overlay, trackingOrigin, &segmentTransform);
            vr::VROverlay()->SetOverlayWidthInMeters(overlay, segmentWidth);
        }
    }

    // Reposition scope overlay relative to the gun-hand controller.
    // Note: gun hand follows the same left-handed swap logic used in GetPoses().
    {
        vr::TrackedDeviceIndex_t leftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        vr::TrackedDeviceIndex_t rightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        if (m_LeftHanded)
            std::swap(leftControllerIndex, rightControllerIndex);

        const vr::TrackedDeviceIndex_t gunControllerIndex = rightControllerIndex;
        if (gunControllerIndex != vr::k_unTrackedDeviceIndexInvalid)
        {
            const float deg2rad = 3.14159265358979323846f / 180.0f;

            auto mul33 = [](const float a[3][3], const float b[3][3], float out[3][3])
                {
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
                };

            const float pitch = m_ScopeOverlayAngleOffset.x * deg2rad;
            const float yaw = m_ScopeOverlayAngleOffset.y * deg2rad;
            const float roll = m_ScopeOverlayAngleOffset.z * deg2rad;

            const float cp = cosf(pitch), sp = sinf(pitch);
            const float cy = cosf(yaw), sy = sinf(yaw);
            const float cr = cosf(roll), sr = sinf(roll);

            const float Rx[3][3] = {
                {1.0f, 0.0f, 0.0f},
                {0.0f, cp,   -sp},
                {0.0f, sp,   cp}
            };
            const float Ry[3][3] = {
                {cy,   0.0f, sy},
                {0.0f, 1.0f, 0.0f},
                {-sy,  0.0f, cy}
            };
            const float Rz[3][3] = {
                {cr,   -sr,  0.0f},
                {sr,   cr,   0.0f},
                {0.0f, 0.0f, 1.0f}
            };

            float RyRx[3][3];
            float R[3][3];
            mul33(Ry, Rx, RyRx);
            mul33(Rz, RyRx, R);

            vr::HmdMatrix34_t scopeRelative = {
                R[0][0], R[0][1], R[0][2], m_ScopeOverlayXOffset,
                R[1][0], R[1][1], R[1][2], m_ScopeOverlayYOffset,
                R[2][0], R[2][1], R[2][2], m_ScopeOverlayZOffset
            };

            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_ScopeHandle, gunControllerIndex, &scopeRelative);
            vr::VROverlay()->SetOverlayWidthInMeters(m_ScopeHandle, std::max(0.01f, m_ScopeOverlayWidthMeters));
        }
    }

    // Reposition rear mirror overlay relative to the off-hand controller.
    {
        vr::TrackedDeviceIndex_t leftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
        vr::TrackedDeviceIndex_t rightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        if (m_LeftHanded)
            std::swap(leftControllerIndex, rightControllerIndex);

        const vr::TrackedDeviceIndex_t offHandControllerIndex = leftControllerIndex;
        if (offHandControllerIndex != vr::k_unTrackedDeviceIndexInvalid)
        {
            const float deg2rad = 3.14159265358979323846f / 180.0f;

            auto mul33 = [](const float a[3][3], const float b[3][3], float out[3][3])
                {
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
                };

            const float pitch = m_RearMirrorOverlayAngleOffset.x * deg2rad;
            const float yaw = m_RearMirrorOverlayAngleOffset.y * deg2rad;
            const float roll = m_RearMirrorOverlayAngleOffset.z * deg2rad;

            const float cp = cosf(pitch), sp = sinf(pitch);
            const float cy = cosf(yaw), sy = sinf(yaw);
            const float cr = cosf(roll), sr = sinf(roll);

            const float Rx[3][3] = {
                {1.0f, 0.0f, 0.0f},
                {0.0f, cp,   -sp},
                {0.0f, sp,   cp}
            };
            const float Ry[3][3] = {
                {cy,   0.0f, sy},
                {0.0f, 1.0f, 0.0f},
                {-sy,  0.0f, cy}
            };
            const float Rz[3][3] = {
                {cr,   -sr,  0.0f},
                {sr,   cr,   0.0f},
                {0.0f, 0.0f, 1.0f}
            };

            float RyRx[3][3];
            float R[3][3];
            mul33(Ry, Rx, RyRx);
            mul33(Rz, RyRx, R);

            vr::HmdMatrix34_t mirrorRelative = {
                R[0][0], R[0][1], R[0][2], m_RearMirrorOverlayXOffset,
                R[1][0], R[1][1], R[1][2], m_RearMirrorOverlayYOffset,
                R[2][0], R[2][1], R[2][2], m_RearMirrorOverlayZOffset
            };

            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_RearMirrorHandle, offHandControllerIndex, &mirrorRelative);
            vr::VROverlay()->SetOverlayWidthInMeters(m_RearMirrorHandle, std::max(0.01f, m_RearMirrorOverlayWidthMeters));
        }
    }
}

void VR::GetPoses()
{
    vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];

    vr::TrackedDeviceIndex_t leftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    vr::TrackedDeviceIndex_t rightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);

    if (m_LeftHanded)
        std::swap(leftControllerIndex, rightControllerIndex);

    vr::TrackedDevicePose_t leftControllerPose = m_Poses[leftControllerIndex];
    vr::TrackedDevicePose_t rightControllerPose = m_Poses[rightControllerIndex];

    GetPoseData(hmdPose, m_HmdPose);
    GetPoseData(leftControllerPose, m_LeftControllerPose);
    GetPoseData(rightControllerPose, m_RightControllerPose);
}

bool VR::UpdatePosesAndActions()
{
    if (!m_Compositor)
        return false;

    vr::EVRCompositorError result = m_Compositor->WaitGetPoses(m_Poses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
    bool posesValid = (result == vr::VRCompositorError_None);

    if (!posesValid && m_CompositorExplicitTiming)
        m_CompositorNeedsHandoff = false;

    m_Input->UpdateActionState(&m_ActiveActionSet, sizeof(vr::VRActiveActionSet_t), 1);
    return posesValid;
}

void VR::GetViewParameters()
{
    vr::HmdMatrix34_t eyeToHeadLeft = m_System->GetEyeToHeadTransform(vr::Eye_Left);
    vr::HmdMatrix34_t eyeToHeadRight = m_System->GetEyeToHeadTransform(vr::Eye_Right);
    m_EyeToHeadTransformPosLeft.x = eyeToHeadLeft.m[0][3];
    m_EyeToHeadTransformPosLeft.y = eyeToHeadLeft.m[1][3];
    m_EyeToHeadTransformPosLeft.z = eyeToHeadLeft.m[2][3];

    m_EyeToHeadTransformPosRight.x = eyeToHeadRight.m[0][3];
    m_EyeToHeadTransformPosRight.y = eyeToHeadRight.m[1][3];
    m_EyeToHeadTransformPosRight.z = eyeToHeadRight.m[2][3];
}

bool VR::PressedDigitalAction(vr::VRActionHandle_t& actionHandle, bool checkIfActionChanged)
{
    vr::InputDigitalActionData_t digitalActionData{};

    if (!GetDigitalActionData(actionHandle, digitalActionData))
        return false;

    if (checkIfActionChanged)
        return digitalActionData.bState && digitalActionData.bChanged;

    return digitalActionData.bState;
}

bool VR::GetDigitalActionData(vr::VRActionHandle_t& actionHandle, vr::InputDigitalActionData_t& digitalDataOut)
{
    vr::EVRInputError result = m_Input->GetDigitalActionData(actionHandle, &digitalDataOut, sizeof(digitalDataOut), vr::k_ulInvalidInputValueHandle);

    return result == vr::VRInputError_None;
}

bool VR::GetAnalogActionData(vr::VRActionHandle_t& actionHandle, vr::InputAnalogActionData_t& analogDataOut)
{
    vr::EVRInputError result = m_Input->GetAnalogActionData(actionHandle, &analogDataOut, sizeof(analogDataOut), vr::k_ulInvalidInputValueHandle);

    if (result == vr::VRInputError_None)
        return true;

    return false;
}

void VR::ProcessMenuInput()
{
    const bool inGame = m_Game->m_EngineClient->IsInGame();
    vr::VROverlayHandle_t currentOverlay = inGame ? m_HUDTopHandle : m_MainMenuHandle;

    const auto controllerHoveringOverlay = [&](vr::VROverlayHandle_t overlay)
        {
            return CheckOverlayIntersectionForController(overlay, vr::TrackedControllerRole_LeftHand) ||
                CheckOverlayIntersectionForController(overlay, vr::TrackedControllerRole_RightHand);
        };

    vr::VROverlayHandle_t hoveredOverlay = vr::k_ulOverlayHandleInvalid;

    if (inGame)
    {
        if (controllerHoveringOverlay(m_HUDTopHandle))
        {
            hoveredOverlay = m_HUDTopHandle;
        }
        else
        {
            for (vr::VROverlayHandle_t overlay : m_HUDBottomHandles)
            {
                if (controllerHoveringOverlay(overlay))
                {
                    hoveredOverlay = overlay;
                    break;
                }
            }
        }
    }
    else if (controllerHoveringOverlay(m_MainMenuHandle))
    {
        hoveredOverlay = m_MainMenuHandle;
    }

    const bool isHoveringOverlay = hoveredOverlay != vr::k_ulOverlayHandleInvalid;

    if (isHoveringOverlay)
        currentOverlay = hoveredOverlay;

    const bool isHudOverlay = inGame && (currentOverlay == m_HUDTopHandle ||
        std::find(m_HUDBottomHandles.begin(), m_HUDBottomHandles.end(), currentOverlay) != m_HUDBottomHandles.end());

    // Overlays can't process action inputs if the laser is active, so
    // only activate laser if a controller is pointing at the overlay
    if (isHoveringOverlay)
    {
        vr::VROverlay()->SetOverlayFlag(currentOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);

        int windowWidth, windowHeight;
        m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

        vr::VREvent_t vrEvent;
        while (vr::VROverlay()->PollNextOverlayEvent(currentOverlay, &vrEvent, sizeof(vrEvent)))
        {
            switch (vrEvent.eventType)
            {
            case vr::VREvent_MouseMove:
            {
                float laserX = vrEvent.data.mouse.x;
                float laserY = vrEvent.data.mouse.y;

                if (isHudOverlay)
                {
                    laserY = -laserY + windowHeight;
                }
                else // main menu (uses render sized texture)
                {
                    laserX = (laserX / m_RenderWidth) * windowWidth;
                    laserY = ((-laserY + m_RenderHeight) / m_RenderHeight) * windowHeight;
                }

                m_Game->m_VguiInput->SetCursorPos(laserX, laserY);
                break;
            }

            case vr::VREvent_MouseButtonDown:
                // Don't allow holding down the mouse down in the pause menu. The resume button can be clicked before
                // the MouseButtonUp event is polled, which causes issues with the overlay.
                if (currentOverlay == m_MainMenuHandle)
                    m_Game->m_VguiInput->InternalMousePressed(ButtonCode_t::MOUSE_LEFT);
                break;

            case vr::VREvent_MouseButtonUp:
                if (isHudOverlay)
                    m_Game->m_VguiInput->InternalMousePressed(ButtonCode_t::MOUSE_LEFT);
                m_Game->m_VguiInput->InternalMouseReleased(ButtonCode_t::MOUSE_LEFT);
                break;

            case vr::VREvent_ScrollDiscrete:
                m_Game->m_VguiInput->InternalMouseWheeled((int)vrEvent.data.scroll.ydelta);
                break;
            }
        }
    }
    else
    {
        vr::VROverlay()->SetOverlayFlag(currentOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);

        if (PressedDigitalAction(m_MenuSelect, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_SPACE);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_SPACE);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_SPACE);
        }
        if (PressedDigitalAction(m_MenuBack, true) || PressedDigitalAction(m_Pause, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_ESCAPE);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_ESCAPE);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_ESCAPE);
        }
        if (PressedDigitalAction(m_MenuUp, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_UP);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_UP);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_UP);
        }
        if (PressedDigitalAction(m_MenuDown, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_DOWN);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_DOWN);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_DOWN);
        }
        if (PressedDigitalAction(m_MenuLeft, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_LEFT);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_LEFT);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_LEFT);
        }
        if (PressedDigitalAction(m_MenuRight, true))
        {
            m_Game->m_VguiInput->InternalKeyCodeTyped(ButtonCode_t::KEY_RIGHT);
            m_Game->m_VguiInput->InternalKeyCodePressed(ButtonCode_t::KEY_RIGHT);
            m_Game->m_VguiInput->InternalKeyCodeReleased(ButtonCode_t::KEY_RIGHT);
        }
    }
}

void VR::ProcessInput()
{
    if (!m_IsVREnabled)
        return;

    vr::VROverlay()->SetOverlayFlag(m_HUDTopHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
    for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
        vr::VROverlay()->SetOverlayFlag(overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);

    typedef std::chrono::duration<float, std::milli> duration;
    auto currentTime = std::chrono::steady_clock::now();
    if (m_PrevFrameTime.time_since_epoch().count() == 0)
    {
        m_PrevFrameTime = currentTime;
    }
    duration elapsed = currentTime - m_PrevFrameTime;
    float deltaTime = elapsed.count();
    m_PrevFrameTime = currentTime;
    m_LastFrameDuration = std::clamp(deltaTime * 0.001f, 0.0f, 0.25f);

    UpdateSpecialInfectedWarningAction();

    if (m_SuppressPlayerInput)
    {
        m_PrimaryAttackDown = false;
        return;
    }

    vr::InputAnalogActionData_t analogActionData;

    if (GetAnalogActionData(m_ActionTurn, analogActionData))
    {
        if (m_SnapTurning)
        {
            if (!m_PressedTurn && analogActionData.x > 0.5)
            {
                m_RotationOffset -= m_SnapTurnAngle;
                m_PressedTurn = true;
            }
            else if (!m_PressedTurn && analogActionData.x < -0.5)
            {
                m_RotationOffset += m_SnapTurnAngle;
                m_PressedTurn = true;
            }
            else if (analogActionData.x < 0.3 && analogActionData.x > -0.3)
                m_PressedTurn = false;
        }
        // Smooth turning
        else
        {
            float deadzone = 0.2;
            // smoother turning
            float xNormalized = (abs(analogActionData.x) - deadzone) / (1 - deadzone);
            if (analogActionData.x > deadzone)
            {
                m_RotationOffset -= m_TurnSpeed * deltaTime * xNormalized;
            }
            if (analogActionData.x < -deadzone)
            {
                m_RotationOffset += m_TurnSpeed * deltaTime * xNormalized;
            }
        }

        // Wrap from 0 to 360
        m_RotationOffset -= 360 * std::floor(m_RotationOffset / 360);
    }

    // TODO: Instead of ClientCmding, override Usercmd in CreateMove
#if 0
    if (GetAnalogActionData(m_ActionWalk, analogActionData))
    {
        bool pushingStickX = true;
        bool pushingStickY = true;
        if (analogActionData.y > 0.5)
        {
            m_Game->ClientCmd_Unrestricted("-back");
            m_Game->ClientCmd_Unrestricted("+forward");
        }
        else if (analogActionData.y < -0.5)
        {
            m_Game->ClientCmd_Unrestricted("-forward");
            m_Game->ClientCmd_Unrestricted("+back");
        }
        else
        {
            m_Game->ClientCmd_Unrestricted("-back");
            m_Game->ClientCmd_Unrestricted("-forward");
            pushingStickY = false;
        }

        if (analogActionData.x > 0.5)
        {
            m_Game->ClientCmd_Unrestricted("-moveleft");
            m_Game->ClientCmd_Unrestricted("+moveright");
        }
        else if (analogActionData.x < -0.5)
        {
            m_Game->ClientCmd_Unrestricted("-moveright");
            m_Game->ClientCmd_Unrestricted("+moveleft");
        }
        else
        {
            m_Game->ClientCmd_Unrestricted("-moveright");
            m_Game->ClientCmd_Unrestricted("-moveleft");
            pushingStickX = false;
        }

        m_PushingThumbstick = pushingStickX || pushingStickY;
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-forward");
        m_Game->ClientCmd_Unrestricted("-back");
        m_Game->ClientCmd_Unrestricted("-moveleft");
        m_Game->ClientCmd_Unrestricted("-moveright");
    }
#else
    // Movement via console commands disabled; handled in Hooks::dCreateMove via CUserCmd.
#endif

    const bool jumpGestureActive = currentTime < m_JumpGestureHoldUntil;
    if (PressedDigitalAction(m_ActionJump) || jumpGestureActive)
    {
        m_Game->ClientCmd_Unrestricted("+jump");
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-jump");
    }

    if (PressedDigitalAction(m_ActionUse))
    {
        m_Game->ClientCmd_Unrestricted("+use");
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-use");
    }

    auto getActionState = [&](vr::VRActionHandle_t* handle, vr::InputDigitalActionData_t& data, bool& isDown, bool& justPressed)
        {
            isDown = false;
            justPressed = false;
            if (!handle)
                return false;

            const bool valid = GetDigitalActionData(*handle, data);
            if (!valid)
                return false;

            isDown = data.bState;
            justPressed = data.bState && data.bChanged;
            return true;
        };

    auto getComboStates = [&](const ActionCombo& combo,
        vr::InputDigitalActionData_t& primaryData,
        vr::InputDigitalActionData_t& secondaryData,
        bool& primaryDown,
        bool& secondaryDown,
        bool& primaryJustPressed,
        bool& secondaryJustPressed)
        {
            const bool primaryValid = getActionState(combo.primary, primaryData, primaryDown, primaryJustPressed);
            const bool secondaryValid = getActionState(combo.secondary, secondaryData, secondaryDown, secondaryJustPressed);
            return primaryValid && secondaryValid;
        };

    vr::InputDigitalActionData_t primaryAttackActionData{};
    bool primaryAttackDown = false;
    bool primaryAttackJustPressed = false;
    getActionState(&m_ActionPrimaryAttack, primaryAttackActionData, primaryAttackDown, primaryAttackJustPressed);
    m_PrimaryAttackDown = primaryAttackDown;

    vr::InputDigitalActionData_t crouchActionData{};
    bool crouchButtonDown = false;
    bool crouchJustPressed = false;
    bool crouchDataValid = getActionState(&m_ActionCrouch, crouchActionData, crouchButtonDown, crouchJustPressed);

    vr::InputDigitalActionData_t resetActionData{};
    [[maybe_unused]] bool resetButtonDown = false;
    bool resetJustPressed = false;
    bool resetDataValid = getActionState(&m_ActionResetPosition, resetActionData, resetButtonDown, resetJustPressed);

    vr::InputDigitalActionData_t reloadActionData{};
    bool reloadButtonDown = false;
    bool reloadJustPressed = false;
    bool reloadDataValid = getActionState(&m_ActionReload, reloadActionData, reloadButtonDown, reloadJustPressed);

    vr::InputDigitalActionData_t secondaryAttackActionData{};
    bool secondaryAttackActive = false;
    [[maybe_unused]] bool secondaryAttackJustPressed = false;
    bool secondaryAttackDataValid = getActionState(&m_ActionSecondaryAttack, secondaryAttackActionData, secondaryAttackActive, secondaryAttackJustPressed);

    const bool gestureSecondaryAttackActive = currentTime < m_SecondaryAttackGestureHoldUntil;
    const bool gestureReloadActive = currentTime < m_ReloadGestureHoldUntil;

    vr::InputDigitalActionData_t flashlightActionData{};
    bool flashlightButtonDown = false;
    bool flashlightJustPressed = false;
    bool flashlightDataValid = getActionState(&m_ActionFlashlight, flashlightActionData, flashlightButtonDown, flashlightJustPressed);

    vr::InputDigitalActionData_t autoAimToggleActionData{};
    [[maybe_unused]] bool autoAimToggleDown = false;
    bool autoAimToggleJustPressed = false;
    [[maybe_unused]] bool autoAimToggleDataValid = getActionState(&m_ActionSpecialInfectedAutoAimToggle, autoAimToggleActionData, autoAimToggleDown, autoAimToggleJustPressed);

    vr::InputDigitalActionData_t nonVrServerMovementToggleActionData{};
    [[maybe_unused]] bool nonVrServerMovementToggleDown = false;
    bool nonVrServerMovementToggleJustPressed = false;
    [[maybe_unused]] bool nonVrServerMovementToggleDataValid = getActionState(&m_NonVRServerMovementAngleToggle, nonVrServerMovementToggleActionData, nonVrServerMovementToggleDown, nonVrServerMovementToggleJustPressed);

    vr::InputDigitalActionData_t scopeMagnificationToggleActionData{};
    [[maybe_unused]] bool scopeMagnificationToggleDown = false;
    bool scopeMagnificationToggleJustPressed = false;
    [[maybe_unused]] bool scopeMagnificationToggleDataValid = getActionState(&m_ActionScopeMagnificationToggle, scopeMagnificationToggleActionData, scopeMagnificationToggleDown, scopeMagnificationToggleJustPressed);

    C_BasePlayer* localPlayer = nullptr;
    {
        const int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        localPlayer = static_cast<C_BasePlayer*>(m_Game->GetClientEntity(playerIndex));
    }

    auto getWeaponSlot = [](C_WeaponCSBase* weapon) -> int
        {
            if (!weapon)
                return 1;

            switch (weapon->GetWeaponID())
            {
            case C_WeaponCSBase::WeaponID::PISTOL:
            case C_WeaponCSBase::WeaponID::MAGNUM:
            case C_WeaponCSBase::WeaponID::MELEE:
                return 2;

            case C_WeaponCSBase::WeaponID::UZI:
            case C_WeaponCSBase::WeaponID::PUMPSHOTGUN:
            case C_WeaponCSBase::WeaponID::AUTOSHOTGUN:
            case C_WeaponCSBase::WeaponID::M16A1:
            case C_WeaponCSBase::WeaponID::HUNTING_RIFLE:
            case C_WeaponCSBase::WeaponID::MAC10:
            case C_WeaponCSBase::WeaponID::SHOTGUN_CHROME:
            case C_WeaponCSBase::WeaponID::SCAR:
            case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
            case C_WeaponCSBase::WeaponID::SPAS:
            case C_WeaponCSBase::WeaponID::CHAINSAW:
            case C_WeaponCSBase::WeaponID::GRENADE_LAUNCHER:
            case C_WeaponCSBase::WeaponID::AK47:
            case C_WeaponCSBase::WeaponID::MP5:
            case C_WeaponCSBase::WeaponID::SG552:
            case C_WeaponCSBase::WeaponID::AWP:
            case C_WeaponCSBase::WeaponID::SCOUT:
            case C_WeaponCSBase::WeaponID::M60:
                return 1;
            default:
                return 0;
            }
        };

    const float gestureRange = m_InventoryGestureRange * m_VRScale;
    const float chestGestureRange = gestureRange;

    // Inventory anchors should be BODY-relative, not fully HMD-relative.
    // We build a yaw-only body basis (forward/right) from the HMD yaw, and use world up.
    // This keeps anchors stable when you look up/down or roll your head.
    const Vector worldUp(0.f, 0.f, 1.f);

    Vector invForward = m_HmdForward;
    invForward.z = 0.f;
    float invFwdLen = sqrtf(invForward.x * invForward.x + invForward.y * invForward.y + invForward.z * invForward.z);
    if (invFwdLen < 0.001f)
    {
        // Fallback if forward is degenerate for any reason
        invForward = Vector(1.f, 0.f, 0.f);
        invFwdLen = 1.f;
    }
    invForward *= (1.f / invFwdLen);

    // Right = forward x up
    Vector invRight(
        invForward.y * worldUp.z - invForward.z * worldUp.y,
        invForward.z * worldUp.x - invForward.x * worldUp.z,
        invForward.x * worldUp.y - invForward.y * worldUp.x
    );
    float invRightLen = sqrtf(invRight.x * invRight.x + invRight.y * invRight.y + invRight.z * invRight.z);
    if (invRightLen < 0.001f)
    {
        invRight = Vector(0.f, 1.f, 0.f);
        invRightLen = 1.f;
    }
    invRight *= (1.f / invRightLen);

    // Anchor origin: estimate a more stable "body / pelvis" point (relative to the HMD position, but in body space).
    const Vector bodyOrigin = m_HmdPosAbs
        + (invForward * (m_InventoryBodyOriginOffset.x * m_VRScale))
        + (invRight * (m_InventoryBodyOriginOffset.y * m_VRScale))
        + (worldUp * (m_InventoryBodyOriginOffset.z * m_VRScale));

    auto buildAnchor = [&](const Vector& offsets)
        {
            return bodyOrigin
                + (invForward * (offsets.x * m_VRScale))
                + (invRight * (offsets.y * m_VRScale))
                + (worldUp * (offsets.z * m_VRScale));
        };

    const Vector chestAnchor = buildAnchor(m_InventoryChestOffset);
    const Vector backAnchor = buildAnchor(m_InventoryBackOffset);
    const Vector leftWaistAnchor = buildAnchor(m_InventoryLeftWaistOffset);
    const Vector rightWaistAnchor = buildAnchor(m_InventoryRightWaistOffset);

    auto isControllerNear = [&](const Vector& controllerPos, const Vector& anchor, float range)
        {
            return VectorLength(controllerPos - anchor) <= range;
        };

    if (m_DrawInventoryAnchors && m_Game->m_DebugOverlay)
    {
        // Highlight anchors when either hand is inside the region.
        const bool chestActive =
            isControllerNear(m_LeftControllerPosAbs, chestAnchor, chestGestureRange) ||
            isControllerNear(m_RightControllerPosAbs, chestAnchor, chestGestureRange);
        const bool backActive =
            isControllerNear(m_LeftControllerPosAbs, backAnchor, gestureRange) ||
            isControllerNear(m_RightControllerPosAbs, backAnchor, gestureRange);
        const bool leftWaistActive =
            isControllerNear(m_LeftControllerPosAbs, leftWaistAnchor, gestureRange) ||
            isControllerNear(m_RightControllerPosAbs, leftWaistAnchor, gestureRange);
        const bool rightWaistActive =
            isControllerNear(m_LeftControllerPosAbs, rightWaistAnchor, gestureRange) ||
            isControllerNear(m_RightControllerPosAbs, rightWaistAnchor, gestureRange);

        auto drawCircle = [&](const Vector& center, const Vector& axisA, const Vector& axisB, float range, int r, int g, int b)
            {
                const int segments = 24;
                const float twoPi = 6.28318530718f;
                for (int i = 0; i < segments; ++i)
                {
                    const float t0 = (twoPi * i) / segments;
                    const float t1 = (twoPi * (i + 1)) / segments;
                    const Vector dir0 = (axisA * std::cos(t0)) + (axisB * std::sin(t0));
                    const Vector dir1 = (axisA * std::cos(t1)) + (axisB * std::sin(t1));
                    const Vector p0 = center + (dir0 * range);
                    const Vector p1 = center + (dir1 * range);
                    m_Game->m_DebugOverlay->AddLineOverlay(p0, p1, r, g, b, true, m_LastFrameDuration * 2.0f);
                }
            };

        auto drawAnchor = [&](const Vector& anchor, float range, bool active)
            {
                int r = m_InventoryAnchorColorR;
                int g = m_InventoryAnchorColorG;
                int b = m_InventoryAnchorColorB;
                int a = m_InventoryAnchorColorA;
                if (active)
                {
                    r = 255; g = 255; b = 0;
                    a = 220;
                }

                // Center marker box (more visible than lines in bright scenes).
                const float box = std::max(1.0f, range * 0.15f);
                Vector mins(-box, -box, -box);
                Vector maxs(box, box, box);
                QAngle ang(0.f, 0.f, 0.f);
                m_Game->m_DebugOverlay->AddBoxOverlay(anchor, mins, maxs, ang, r, g, b, a, m_LastFrameDuration * 2.0f);

                // Three circles to approximate a sphere (body-space axes).
                drawCircle(anchor, invRight, invForward, range, r, g, b);
                drawCircle(anchor, worldUp, invRight, range, r, g, b);
                drawCircle(anchor, invForward, worldUp, range, r, g, b);
            };

        drawAnchor(chestAnchor, chestGestureRange, chestActive);
        drawAnchor(backAnchor, gestureRange, backActive);
        drawAnchor(leftWaistAnchor, gestureRange, leftWaistActive);
        drawAnchor(rightWaistAnchor, gestureRange, rightWaistActive);

        // --- Front markers (HUD-like, still world overlays) ---
        // Real anchors can be behind you or at the waist and thus out of view.
        // Draw 4 small markers in front of the player, each connected to the real anchor.
        const float hudDist = m_InventoryHudMarkerDistance * m_VRScale;
        const float hudUp = m_InventoryHudMarkerUpOffset * m_VRScale;
        const float hudSep = m_InventoryHudMarkerSeparation * m_VRScale;

        const Vector hudBase = m_HmdPosAbs + (invForward * hudDist) + (worldUp * hudUp);
        const float hudBox = std::max(1.0f, hudSep * 0.18f);
        Vector hudMins(-hudBox, -hudBox, -hudBox);
        Vector hudMaxs(hudBox, hudBox, hudBox);
        QAngle hudAng(0.f, 0.f, 0.f);

        auto drawHudMarker = [&](const Vector& markerPos, const Vector& anchorPos, bool active)
            {
                int r = m_InventoryAnchorColorR;
                int g = m_InventoryAnchorColorG;
                int b = m_InventoryAnchorColorB;
                int a = 220;
                if (active)
                {
                    r = 255; g = 255; b = 0;
                }

                m_Game->m_DebugOverlay->AddBoxOverlay(markerPos, hudMins, hudMaxs, hudAng, r, g, b, a, m_LastFrameDuration * 2.0f);
                m_Game->m_DebugOverlay->AddLineOverlay(markerPos, anchorPos, r, g, b, true, m_LastFrameDuration * 2.0f);
            };

        // Layout: [LeftWaist][Back][Chest][RightWaist]
        drawHudMarker(hudBase + (invRight * (-1.5f * hudSep)), leftWaistAnchor, leftWaistActive);
        drawHudMarker(hudBase + (invRight * (-0.5f * hudSep)), backAnchor, backActive);
        drawHudMarker(hudBase + (invRight * (0.5f * hudSep)), chestAnchor, chestActive);
        drawHudMarker(hudBase + (invRight * (1.5f * hudSep)), rightWaistAnchor, rightWaistActive);
    }

    bool inventoryGripActiveLeft = false;
    bool inventoryGripActiveRight = false;

    auto togglePrimarySecondary = [&]()
        {
            if (!localPlayer)
                return;

            // Weapon_GetSlot uses 0-based slots (0=slot1/primary, 1=slot2/secondary).
            C_BaseCombatWeapon* primary = reinterpret_cast<C_BaseCombatWeapon*>(localPlayer->Weapon_GetSlot(0));
            C_BaseCombatWeapon* secondary = reinterpret_cast<C_BaseCombatWeapon*>(localPlayer->Weapon_GetSlot(1));
            C_BaseCombatWeapon* active = localPlayer->GetActiveWeapon();

            const bool havePrimary = (primary != nullptr);
            const bool haveSecondary = (secondary != nullptr);
            const bool activeIsPrimary = havePrimary && active && (active == primary);
            const bool activeIsSecondary = haveSecondary && active && (active == secondary);

            const char* targetSlotCmd = nullptr;

            if (activeIsPrimary)
            {
                // Already holding primary: swap to secondary if available.
                if (haveSecondary)
                    targetSlotCmd = "slot2";
                else
                    targetSlotCmd = "slot1";
            }
            else if (activeIsSecondary)
            {
                // Already holding secondary: swap to primary if available.
                if (havePrimary)
                    targetSlotCmd = "slot1";
                else
                    targetSlotCmd = "slot2";
            }
            else
            {
                // Holding something else (items, empty hands, etc.): prefer primary, else secondary.
                if (havePrimary)
                    targetSlotCmd = "slot1";
                else if (haveSecondary)
                    targetSlotCmd = "slot2";
            }

            if (targetSlotCmd)
                m_Game->ClientCmd_Unrestricted(targetSlotCmd);
        };

    auto handleGripInventoryGesture = [&](const Vector& controllerPos, bool gripDown, bool gripJustPressed, bool isRightHand)
        {
            if (!gripDown)
                return;

            const bool nearBack = isControllerNear(controllerPos, backAnchor, gestureRange);
            const bool nearChest = isControllerNear(controllerPos, chestAnchor, chestGestureRange);
            const bool nearLeftWaist = isControllerNear(controllerPos, leftWaistAnchor, gestureRange);
            const bool nearRightWaist = isControllerNear(controllerPos, rightWaistAnchor, gestureRange);

            if (!(nearBack || nearChest || nearLeftWaist || nearRightWaist))
                return;

            if (isRightHand)
                inventoryGripActiveRight = true;
            else
                inventoryGripActiveLeft = true;

            if (!gripJustPressed)
                return;

            if (nearBack)
            {
                togglePrimarySecondary();
                return;
            }

            if (nearLeftWaist)
            {
                m_Game->ClientCmd_Unrestricted("slot3");
                return;
            }

            if (nearChest)
            {
                m_Game->ClientCmd_Unrestricted("slot4");
                return;
            }

            if (nearRightWaist)
            {
                m_Game->ClientCmd_Unrestricted("slot5");
            }
        };

    handleGripInventoryGesture(m_LeftControllerPosAbs, reloadButtonDown, reloadJustPressed, false);
    handleGripInventoryGesture(m_RightControllerPosAbs, crouchButtonDown, crouchJustPressed, true);

    const bool suppressReload = inventoryGripActiveLeft;
    const bool suppressCrouch = inventoryGripActiveRight;

    if (suppressReload)
    {
        reloadButtonDown = false;
        reloadJustPressed = false;
    }

    if (suppressCrouch)
    {
        crouchButtonDown = false;
        crouchJustPressed = false;
    }

    if (!m_SpecialInfectedPreWarningAutoAimConfigEnabled)
    {
        m_SpecialInfectedPreWarningAutoAimEnabled = false;
        m_SpecialInfectedPreWarningTargetEntityIndex = -1;
        m_SpecialInfectedPreWarningTargetIsPlayer = false;
        m_SpecialInfectedPreWarningActive = false;
        m_SpecialInfectedPreWarningInRange = false;
        m_SpecialInfectedPreWarningTargetDistanceSq = std::numeric_limits<float>::max();
        m_SpecialInfectedAutoAimDirection = {};
        m_SpecialInfectedAutoAimCooldownEnd = {};
    }
    else if (autoAimToggleJustPressed)
    {
        m_SpecialInfectedPreWarningAutoAimEnabled = !m_SpecialInfectedPreWarningAutoAimEnabled;
        m_SpecialInfectedPreWarningTargetEntityIndex = -1;
        m_SpecialInfectedPreWarningTargetIsPlayer = false;
        m_SpecialInfectedPreWarningActive = false;
        m_SpecialInfectedPreWarningInRange = false;
        m_SpecialInfectedPreWarningTargetDistanceSq = std::numeric_limits<float>::max();
        m_SpecialInfectedAutoAimDirection = {};
        m_SpecialInfectedAutoAimCooldownEnd = {};
    }

    if (nonVrServerMovementToggleJustPressed)
    {
        m_NonVRServerMovementAngleOverride = !m_NonVRServerMovementAngleOverride;
    }

    if (scopeMagnificationToggleJustPressed)
    {
        CycleScopeMagnification();
    }

    if (primaryAttackDown)
    {
        m_Game->ClientCmd_Unrestricted("+attack");
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-attack");
    }

    vr::InputDigitalActionData_t voicePrimaryData{};
    vr::InputDigitalActionData_t voiceSecondaryData{};
    bool voicePrimaryDown = false;
    bool voiceSecondaryDown = false;
    bool voicePrimaryJustPressed = false;
    bool voiceSecondaryJustPressed = false;
    bool voiceComboValid = getComboStates(m_VoiceRecordCombo, voicePrimaryData, voiceSecondaryData,
        voicePrimaryDown, voiceSecondaryDown, voicePrimaryJustPressed, voiceSecondaryJustPressed);
    const bool voiceComboActive = voiceComboValid && voicePrimaryDown && voiceSecondaryDown;
    const bool voiceComboJustActivated = voiceComboValid && ((voicePrimaryJustPressed && voiceSecondaryDown) || (voiceSecondaryJustPressed && voicePrimaryDown));

    vr::InputDigitalActionData_t quickTurnPrimaryData{};
    vr::InputDigitalActionData_t quickTurnSecondaryData{};
    bool quickTurnPrimaryDown = false;
    bool quickTurnSecondaryDown = false;
    [[maybe_unused]] bool quickTurnPrimaryJustPressed = false;
    [[maybe_unused]] bool quickTurnSecondaryJustPressed = false;
    bool quickTurnComboValid = getComboStates(m_QuickTurnCombo, quickTurnPrimaryData, quickTurnSecondaryData,
        quickTurnPrimaryDown, quickTurnSecondaryDown, quickTurnPrimaryJustPressed, quickTurnSecondaryJustPressed);
    bool quickTurnComboPressed = quickTurnComboValid && quickTurnPrimaryDown && quickTurnSecondaryDown;

    vr::InputDigitalActionData_t viewmodelPrimaryData{};
    vr::InputDigitalActionData_t viewmodelSecondaryData{};
    bool viewmodelPrimaryDown = false;
    bool viewmodelSecondaryDown = false;
    [[maybe_unused]] bool viewmodelPrimaryJustPressed = false;
    [[maybe_unused]] bool viewmodelSecondaryJustPressed = false;
    bool viewmodelComboValid = getComboStates(m_ViewmodelAdjustCombo, viewmodelPrimaryData, viewmodelSecondaryData,
        viewmodelPrimaryDown, viewmodelSecondaryDown, viewmodelPrimaryJustPressed, viewmodelSecondaryJustPressed);
    const bool adjustViewmodelActive = m_ViewmodelAdjustEnabled && viewmodelComboValid && viewmodelPrimaryDown && viewmodelSecondaryDown;

    if (adjustViewmodelActive && !m_AdjustingViewmodel)
    {
        m_AdjustingViewmodel = true;
        m_AdjustStartLeftPos = m_LeftControllerPosAbs;
        m_AdjustStartLeftAng = m_LeftControllerAngAbs;
        m_AdjustStartViewmodelPos = m_ViewmodelPosAdjust;
        m_AdjustStartViewmodelAng = m_ViewmodelAngAdjust;
        m_AdjustStartViewmodelForward = m_ViewmodelForward;
        m_AdjustStartViewmodelRight = m_ViewmodelRight;
        m_AdjustStartViewmodelUp = m_ViewmodelUp;
        m_AdjustingKey = m_CurrentViewmodelKey;
        Game::logMsg("[VR] Viewmodel adjust start for %s (pos %.2f %.2f %.2f, ang %.2f %.2f %.2f)",
            m_AdjustingKey.c_str(), m_ViewmodelPosAdjust.x, m_ViewmodelPosAdjust.y, m_ViewmodelPosAdjust.z,
            m_ViewmodelAngAdjust.x, m_ViewmodelAngAdjust.y, m_ViewmodelAngAdjust.z);
    }
    else if (!adjustViewmodelActive && m_AdjustingViewmodel)
    {
        m_AdjustingViewmodel = false;
        m_AdjustingKey.clear();
        if (m_ViewmodelAdjustmentsDirty)
        {
            SaveViewmodelAdjustments();
        }

        Game::logMsg("[VR] Viewmodel adjust end for %s (pos %.2f %.2f %.2f, ang %.2f %.2f %.2f)",
            m_CurrentViewmodelKey.c_str(), m_ViewmodelPosAdjust.x, m_ViewmodelPosAdjust.y, m_ViewmodelPosAdjust.z,
            m_ViewmodelAngAdjust.x, m_ViewmodelAngAdjust.y, m_ViewmodelAngAdjust.z);
    }

    if (resetJustPressed)
    {
        if (crouchButtonDown)
        {
            SendFunctionKey(VK_F2);
        }
        else
        {
            m_CrouchToggleActive = !m_CrouchToggleActive;
            ResetPosition();
        }
    }

    if (!m_VoiceRecordActive && voiceComboJustActivated)
    {
        m_Game->ClientCmd_Unrestricted("+voicerecord");
        m_VoiceRecordActive = true;
    }

    if (m_VoiceRecordActive && (!voiceComboActive || !voiceComboValid))
    {
        m_Game->ClientCmd_Unrestricted("-voicerecord");
        m_VoiceRecordActive = false;
    }

    reloadButtonDown = (reloadButtonDown || gestureReloadActive) && !suppressReload;
    secondaryAttackActive = secondaryAttackActive || gestureSecondaryAttackActive;

    if (!crouchButtonDown && reloadButtonDown && !adjustViewmodelActive)
    {
        m_Game->ClientCmd_Unrestricted("+reload");
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-reload");
    }

    if (secondaryAttackActive && !adjustViewmodelActive)
    {
        m_Game->ClientCmd_Unrestricted("+attack2");
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-attack2");
    }

    if (quickTurnComboPressed && !m_QuickTurnTriggered)
    {
        m_RotationOffset += 180.0f;
        m_RotationOffset -= 360 * std::floor(m_RotationOffset / 360);
        m_QuickTurnTriggered = true;
    }
    else if (!quickTurnComboPressed)
    {
        m_QuickTurnTriggered = false;
    }

    if (secondaryAttackActive || !m_RequireSecondaryAttackForItemSwitch)
    {
        if (PressedDigitalAction(m_ActionPrevItem, true))
        {
            m_Game->ClientCmd_Unrestricted("invprev");
        }
        else if (PressedDigitalAction(m_ActionNextItem, true))
        {
            m_Game->ClientCmd_Unrestricted("invnext");
        }
    }

    bool crouchActive = (!suppressCrouch) && (crouchButtonDown || m_CrouchToggleActive);
    if (crouchActive)
    {
        m_Game->ClientCmd_Unrestricted("+duck");
    }
    else
    {
        m_Game->ClientCmd_Unrestricted("-duck");
    }

    if (flashlightJustPressed)
    {
        if (crouchButtonDown)
            SendFunctionKey(VK_F1);
        else
            m_Game->ClientCmd_Unrestricted("impulse 100");
    }

    if (PressedDigitalAction(m_Spray, true))
    {
        m_Game->ClientCmd_Unrestricted("impulse 201");
    }

    auto handleCustomAction = [&](vr::VRActionHandle_t& actionHandle, const CustomActionBinding& binding)
        {
            vr::InputDigitalActionData_t actionData{};
            if (!GetDigitalActionData(actionHandle, actionData))
                return;

            if (binding.virtualKey.has_value())
            {
                if (binding.holdVirtualKey)
                {
                    if (!actionData.bChanged)
                        return;

                    if (actionData.bState)
                        SendVirtualKeyDown(*binding.virtualKey);
                    else
                        SendVirtualKeyUp(*binding.virtualKey);
                }
                else if (actionData.bState && actionData.bChanged)
                {
                    SendVirtualKey(*binding.virtualKey);
                }
                return;
            }
            if (binding.usePressReleaseCommands)
            {
                if (!actionData.bChanged)
                    return;

                if (actionData.bState)
                {
                    m_Game->ClientCmd_Unrestricted(binding.command.c_str());
                }
                else if (!binding.releaseCommand.empty())
                {
                    m_Game->ClientCmd_Unrestricted(binding.releaseCommand.c_str());
                }
                return;
            }
            if (actionData.bState && actionData.bChanged && !binding.command.empty())
                m_Game->ClientCmd_Unrestricted(binding.command.c_str());
        };

    handleCustomAction(m_CustomAction1, m_CustomAction1Binding);
    handleCustomAction(m_CustomAction2, m_CustomAction2Binding);
    handleCustomAction(m_CustomAction3, m_CustomAction3Binding);
    handleCustomAction(m_CustomAction4, m_CustomAction4Binding);
    handleCustomAction(m_CustomAction5, m_CustomAction5Binding);

    auto showTopHud = [&]()
        {
            vr::VROverlay()->ShowOverlay(m_HUDTopHandle);
        };

    auto controllerHudTooClose = [&](size_t overlayIndex, vr::TrackedDeviceIndex_t controllerIndex)
        {
            if (!m_ControllerHudCut || controllerIndex == vr::k_unTrackedDeviceIndexInvalid || (overlayIndex != 0 && overlayIndex != 3))
                return false;

            const vr::TrackedDevicePose_t& controllerPose = m_Poses[controllerIndex];
            const vr::TrackedDevicePose_t& hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];

            if (!controllerPose.bPoseIsValid || !hmdPose.bPoseIsValid)
                return false;

            vr::HmdMatrix34_t controllerMat = controllerPose.mDeviceToAbsoluteTracking;
            vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;

            int windowWidth, windowHeight;
            m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);
            const float hudAspect = static_cast<float>(windowHeight) / static_cast<float>(windowWidth);
            const float hudHalfStackOffset = (m_HudSize * hudAspect) * 0.25f;

            const float controllerHudRotationRad = m_ControllerHudRotation * (3.14159265358979323846f / 180.0f);
            const float cosRotation = cosf(controllerHudRotationRad);
            const float sinRotation = sinf(controllerHudRotationRad);
            const float controllerHudXOffset = (overlayIndex == 0) ? -m_ControllerHudXOffset : m_ControllerHudXOffset;

            vr::HmdMatrix34_t relativeTransform =
            {
                1.0f, 0.0f, 0.0f, controllerHudXOffset,
                0.0f, cosRotation, -sinRotation, m_ControllerHudYOffset - hudHalfStackOffset,
                0.0f, sinRotation,  cosRotation, m_ControllerHudZOffset
            };

            auto multiplyTransform = [](const vr::HmdMatrix34_t& parent, const vr::HmdMatrix34_t& child)
                {
                    vr::HmdMatrix34_t result = {};
                    for (int row = 0; row < 3; ++row)
                    {
                        result.m[row][0] = parent.m[row][0] * child.m[0][0] + parent.m[row][1] * child.m[1][0] + parent.m[row][2] * child.m[2][0];
                        result.m[row][1] = parent.m[row][0] * child.m[0][1] + parent.m[row][1] * child.m[1][1] + parent.m[row][2] * child.m[2][1];
                        result.m[row][2] = parent.m[row][0] * child.m[0][2] + parent.m[row][1] * child.m[1][2] + parent.m[row][2] * child.m[2][2];
                        result.m[row][3] = parent.m[row][0] * child.m[0][3] + parent.m[row][1] * child.m[1][3] + parent.m[row][2] * child.m[2][3] + parent.m[row][3];
                    }
                    return result;
                };

            vr::HmdMatrix34_t worldTransform = multiplyTransform(controllerMat, relativeTransform);
            Vector overlayPos = { worldTransform.m[0][3], worldTransform.m[1][3], worldTransform.m[2][3] };
            Vector hmdPos = { hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] };
            Vector controllerPos = { controllerMat.m[0][3], controllerMat.m[1][3], controllerMat.m[2][3] };

            const float overlayDistance = VectorLength(overlayPos - hmdPos);
            const float controllerDistance = VectorLength(controllerPos - hmdPos);

            constexpr float overlayCutoff = 0.35f;
            constexpr float controllerCutoff = 0.25f;

            return overlayDistance < overlayCutoff || controllerDistance < controllerCutoff;
        };

    auto showControllerHud = [&](bool attachToControllers)
        {
            for (size_t i = 0; i < m_HUDBottomHandles.size(); ++i)
            {
                if (attachToControllers && (i == 0 || i == 3))
                {
                    vr::ETrackedControllerRole controllerRole = (i == 0) ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
                    vr::TrackedDeviceIndex_t controllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(controllerRole);

                    if (controllerIndex == vr::k_unTrackedDeviceIndexInvalid)
                        continue;
                    if (controllerHudTooClose(i, controllerIndex))
                    {
                        vr::VROverlay()->HideOverlay(m_HUDBottomHandles[i]);
                        continue;
                    }
                }

                vr::VROverlay()->ShowOverlay(m_HUDBottomHandles[i]);
            }
        };

    auto hideTopHud = [&]()
        {
            vr::VROverlay()->HideOverlay(m_HUDTopHandle);
        };

    auto hideControllerHud = [&]()
        {
            for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
                vr::VROverlay()->HideOverlay(overlay);
        };

    bool isControllerVertical = m_RightControllerAngAbs.x > 60 || m_RightControllerAngAbs.x < -45;
    bool menuActive = m_Game->m_EngineClient->IsPaused();
    bool cursorVisible = m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible();
    if (cursorVisible)
    {
        m_HudChatVisibleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
    const bool chatRecent = std::chrono::steady_clock::now() < m_HudChatVisibleUntil;
    if (PressedDigitalAction(m_ToggleHUD, true))
    {
        m_HudToggleState = !m_HudToggleState;
    }

    bool wantsTopHud = PressedDigitalAction(m_Scoreboard) || isControllerVertical || m_HudToggleState || cursorVisible || chatRecent;
    bool wantsControllerHud = m_RenderedHud;
    const bool attachControllerHud = m_ControllerHudCut && !menuActive;
    if ((wantsTopHud && m_RenderedHud) || menuActive)
    {
        RepositionOverlays(attachControllerHud);

        if (PressedDigitalAction(m_Scoreboard))
            m_Game->ClientCmd_Unrestricted("+showscores");
        else
            m_Game->ClientCmd_Unrestricted("-showscores");

        showTopHud();
    }
    else
    {
        hideTopHud();
    }

    if (wantsControllerHud)
    {
        showControllerHud(attachControllerHud);
    }
    else
    {
        hideControllerHud();
    }
    m_RenderedHud = false;

    if (PressedDigitalAction(m_Pause, true))
    {
        m_Game->ClientCmd_Unrestricted("gameui_activate");
        RepositionOverlays(false);
        showTopHud();
        showControllerHud(false);
    }
}

void VR::SendVirtualKey(WORD virtualKey)
{
    SendVirtualKeyDown(virtualKey);
    SendVirtualKeyUp(virtualKey);
}

void VR::SendVirtualKeyDown(WORD virtualKey)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;

    SendInput(1, &input, sizeof(INPUT));
}

void VR::SendVirtualKeyUp(WORD virtualKey)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    input.ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(1, &input, sizeof(INPUT));
}

void VR::SendFunctionKey(WORD virtualKey)
{
    SendVirtualKey(virtualKey);
}

VMatrix VR::VMatrixFromHmdMatrix(const vr::HmdMatrix34_t& hmdMat)
{
    // VMatrix has a different implicit coordinate system than HmdMatrix34_t, but this function does not convert between them
    VMatrix vMat(
        hmdMat.m[0][0], hmdMat.m[1][0], hmdMat.m[2][0], 0.0f,
        hmdMat.m[0][1], hmdMat.m[1][1], hmdMat.m[2][1], 0.0f,
        hmdMat.m[0][2], hmdMat.m[1][2], hmdMat.m[2][2], 0.0f,
        hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3], 1.0f
    );

    return vMat;
}

vr::HmdMatrix34_t VR::VMatrixToHmdMatrix(const VMatrix& vMat)
{
    vr::HmdMatrix34_t hmdMat = { 0 };

    hmdMat.m[0][0] = vMat.m[0][0];
    hmdMat.m[1][0] = vMat.m[0][1];
    hmdMat.m[2][0] = vMat.m[0][2];

    hmdMat.m[0][1] = vMat.m[1][0];
    hmdMat.m[1][1] = vMat.m[1][1];
    hmdMat.m[2][1] = vMat.m[1][2];

    hmdMat.m[0][2] = vMat.m[2][0];
    hmdMat.m[1][2] = vMat.m[2][1];
    hmdMat.m[2][2] = vMat.m[2][2];

    hmdMat.m[0][3] = vMat.m[3][0];
    hmdMat.m[1][3] = vMat.m[3][1];
    hmdMat.m[2][3] = vMat.m[3][2];

    return hmdMat;
}

vr::HmdMatrix34_t VR::GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole)
{
    vr::VRInputValueHandle_t inputValue = vr::k_ulInvalidInputValueHandle;

    if (controllerRole == vr::TrackedControllerRole_RightHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/right", &inputValue);
    }
    else if (controllerRole == vr::TrackedControllerRole_LeftHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/left", &inputValue);
    }

    if (inputValue != vr::k_ulInvalidInputValueHandle)
    {
        char buffer[vr::k_unMaxPropertyStringSize];

        m_System->GetStringTrackedDeviceProperty(vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(controllerRole), vr::Prop_RenderModelName_String,
            buffer, vr::k_unMaxPropertyStringSize);

        vr::RenderModel_ControllerMode_State_t controllerState = { 0 };
        vr::RenderModel_ComponentState_t componentState = { 0 };

        if (vr::VRRenderModels()->GetComponentStateForDevicePath(buffer, vr::k_pch_Controller_Component_Tip, inputValue, &controllerState, &componentState))
        {
            return componentState.mTrackingToComponentLocal;
        }
    }

    // Not a hand controller role or tip lookup failed, return identity
    const vr::HmdMatrix34_t identity =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    return identity;
}

bool VR::CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole)
{
    vr::TrackedDeviceIndex_t deviceIndex = m_System->GetTrackedDeviceIndexForControllerRole(controllerRole);

    if (deviceIndex == vr::k_unTrackedDeviceIndexInvalid)
        return false;

    vr::TrackedDevicePose_t& controllerPose = m_Poses[deviceIndex];

    if (!controllerPose.bPoseIsValid)
        return false;

    VMatrix controllerVMatrix = VMatrixFromHmdMatrix(controllerPose.mDeviceToAbsoluteTracking);
    VMatrix tipVMatrix = VMatrixFromHmdMatrix(GetControllerTipMatrix(controllerRole));
    tipVMatrix.MatrixMul(controllerVMatrix, controllerVMatrix);

    vr::VROverlayIntersectionParams_t  params = { 0 };
    vr::VROverlayIntersectionResults_t results = { 0 };

    params.eOrigin = vr::VRCompositor()->GetTrackingSpace();
    params.vSource = { controllerVMatrix.m[3][0],  controllerVMatrix.m[3][1],  controllerVMatrix.m[3][2] };
    params.vDirection = { -controllerVMatrix.m[2][0], -controllerVMatrix.m[2][1], -controllerVMatrix.m[2][2] };

    return m_Overlay->ComputeOverlayIntersection(overlayHandle, &params, &results);
}

QAngle VR::GetRightControllerAbsAngle()
{
    return m_RightControllerAngAbs;
}

Vector VR::GetRightControllerAbsPos()
{
    return m_RightControllerPosAbs;
}

Vector VR::GetRecommendedViewmodelAbsPos()
{
    Vector viewmodelPos = GetRightControllerAbsPos();
    viewmodelPos -= m_ViewmodelForward * m_ViewmodelPosOffset.x;
    viewmodelPos -= m_ViewmodelRight * m_ViewmodelPosOffset.y;
    viewmodelPos -= m_ViewmodelUp * m_ViewmodelPosOffset.z;

    return viewmodelPos;
}

QAngle VR::GetRecommendedViewmodelAbsAngle()
{
    QAngle result{};

    QAngle::VectorAngles(m_ViewmodelForward, m_ViewmodelUp, result);

    return result;
}

void VR::HandleMissingRenderContext(const char* location)
{
    const char* ctx = location ? location : "unknown";
    LOG("[VR] Missing IMatRenderContext in %s. Disabling VR rendering for this frame.", ctx);
    m_CreatedVRTextures = false;
    m_RenderedNewFrame = false;
    m_RenderedHud = false;
}

void VR::FinishFrame()
{
    if (!m_Compositor || !m_CompositorExplicitTiming)
        return;

    if (!m_CompositorNeedsHandoff)
        return;

    m_Compositor->PostPresentHandoff();
    m_CompositorNeedsHandoff = false;
}

void VR::UpdateTracking()
{
    GetPoses();

    int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
    if (!localPlayer) {
        m_ScopeWeaponIsFirearm = false;
        return;
    }
        

    m_Game->m_IsMeleeWeaponActive = localPlayer->IsMeleeWeaponActive();

    // Scope: only render/show when holding a firearm
    m_ScopeWeaponIsFirearm = false;
    if (C_BaseCombatWeapon* active = localPlayer->GetActiveWeapon())
    {
        if (C_WeaponCSBase* weapon = (C_WeaponCSBase*)active)
            m_ScopeWeaponIsFirearm = IsFirearmWeaponId(weapon->GetWeaponID());
    }
    RefreshActiveViewmodelAdjustment(localPlayer);

    if (!m_IsThirdPersonCamera)
    {
        Vector eyeOrigin = localPlayer->EyePosition();
        if (!eyeOrigin.IsZero())
        {
            m_SetupOrigin = eyeOrigin;
            if (m_SetupOriginPrev.IsZero())
                m_SetupOriginPrev = eyeOrigin;
        }
    }

    // --- Fix: third-person camera shifts CViewSetup::origin behind the player.
    // In this codebase, controller world positions are anchored off m_CameraAnchor (NOT directly off m_SetupOrigin),
    // and m_CameraAnchor is advanced by (m_SetupOrigin - m_SetupOriginPrev). If third-person origin pollutes
    // m_SetupOrigin even briefly, m_CameraAnchor "learns" the offset and controllers/aimline look glued to the
    // animated player model. So we must rebase BOTH m_SetupOrigin and m_CameraAnchor.
    {
        Vector absOrigin = localPlayer->GetAbsOrigin();

        // Desired anchor: follow player XY. Keep current Z from the view setup (eye height/crouch),
        // because Z is used by height logic later.
        Vector desired = m_SetupOrigin;
        if (desired.IsZero())
            desired = absOrigin + Vector(0, 0, 64);
        desired.x = absOrigin.x;
        desired.y = absOrigin.y;

        Vector delta = desired - m_SetupOrigin;
        delta.z = 0.0f; // only rebase planar drift from third-person camera

        // If we haven't initialized anchors yet, initialize cleanly (don't "add huge delta" to zero state).
        if (m_SetupOrigin.IsZero() || m_SetupOriginPrev.IsZero() || m_CameraAnchor.IsZero())
        {
            m_SetupOrigin = desired;
            m_SetupOriginPrev = desired;
            if (m_CameraAnchor.IsZero())
                m_CameraAnchor = desired;
        }
        else if (VectorLength(delta) > 1.0f)
        {
            // Rebase camera anchor and previous setup origin so we DON'T get a one-frame spike
            // in (m_SetupOrigin - m_SetupOriginPrev), and controllers immediately stop sticking to the model.
            m_CameraAnchor += delta;
            m_SetupOriginPrev += delta;
            m_SetupOrigin = desired;
        }
    }

    // HMD tracking
    QAngle hmdAngLocal = m_HmdPose.TrackedDeviceAng;
    Vector hmdPosLocal = m_HmdPose.TrackedDevicePos;

    Vector deltaPosition = hmdPosLocal - m_HmdPosLocalPrev;
    Vector hmdPosCorrected = m_HmdPosCorrectedPrev + deltaPosition;

    VectorPivotXY(hmdPosCorrected, m_HmdPosCorrectedPrev, m_RotationOffset);

    hmdAngLocal.y += m_RotationOffset;
    // Wrap angle from -180 to 180
    hmdAngLocal.y -= 360 * std::floor((hmdAngLocal.y + 180) / 360);

    auto wrapAngle = [](float angle)
        {
            return angle - 360.0f * std::floor((angle + 180.0f) / 360.0f);
        };

    const float headSmoothingStrength = std::clamp(m_HeadSmoothing, 0.0f, 0.99f);
    if (!m_HeadSmoothingInitialized)
    {
        m_HmdPosSmoothed = hmdPosCorrected;
        m_HmdAngSmoothed = hmdAngLocal;
        m_HeadSmoothingInitialized = true;
    }

    if (headSmoothingStrength > 0.0f)
    {
        const float lerpFactor = 1.0f - headSmoothingStrength;
        auto smoothVector = [&](const Vector& target, Vector& current)
            {
                current.x += (target.x - current.x) * lerpFactor;
                current.y += (target.y - current.y) * lerpFactor;
                current.z += (target.z - current.z) * lerpFactor;
            };

        auto smoothAngleComponent = [&](float target, float& current)
            {
                float diff = target - current;
                diff -= 360.0f * std::floor((diff + 180.0f) / 360.0f);
                current += diff * lerpFactor;
            };

        smoothVector(hmdPosCorrected, m_HmdPosSmoothed);
        smoothAngleComponent(hmdAngLocal.x, m_HmdAngSmoothed.x);
        smoothAngleComponent(hmdAngLocal.y, m_HmdAngSmoothed.y);
        smoothAngleComponent(hmdAngLocal.z, m_HmdAngSmoothed.z);
    }
    else
    {
        m_HmdPosSmoothed = hmdPosCorrected;
        m_HmdAngSmoothed = hmdAngLocal;
    }

    m_HmdAngSmoothed.x = wrapAngle(m_HmdAngSmoothed.x);
    m_HmdAngSmoothed.y = wrapAngle(m_HmdAngSmoothed.y);
    m_HmdAngSmoothed.z = wrapAngle(m_HmdAngSmoothed.z);

    Vector hmdPosSmoothed = m_HmdPosSmoothed;
    QAngle hmdAngSmoothed = m_HmdAngSmoothed;

    m_HmdPosCorrectedPrev = hmdPosCorrected;
    m_HmdPosLocalPrev = hmdPosLocal;

    QAngle::AngleVectors(hmdAngSmoothed, &m_HmdForward, &m_HmdRight, &m_HmdUp);

    m_HmdPosLocalInWorld = hmdPosSmoothed * m_VRScale;

    // Roomscale setup
    Vector cameraMovingDirection = m_SetupOrigin - m_SetupOriginPrev;
    Vector cameraToPlayer = m_HmdPosAbsPrev - m_SetupOriginPrev;
    cameraMovingDirection.z = 0;
    cameraToPlayer.z = 0;
    float cameraFollowing = DotProduct(cameraMovingDirection, cameraToPlayer);
    float cameraDistance = VectorLength(cameraToPlayer);

    if (localPlayer->m_hGroundEntity != -1 && localPlayer->m_vecVelocity.IsZero())
        m_RoomscaleActive = true;

    // TODO: Get roomscale to work while using thumbstick
    if ((cameraFollowing < 0 && cameraDistance > 1) || (m_PushingThumbstick))
        m_RoomscaleActive = false;

    if (!m_RoomscaleActive)
        m_CameraAnchor += m_SetupOrigin - m_SetupOriginPrev;

    m_CameraAnchor.z = m_SetupOrigin.z + m_HeightOffset;

    m_HmdPosAbs = m_CameraAnchor - Vector(0, 0, 64) + m_HmdPosLocalInWorld;

    // Check if camera is clipping inside wall
    CGameTrace trace;
    Ray_t ray;
    CTraceFilterSkipNPCsAndPlayers tracefilter((IHandleEntity*)localPlayer, 0);

    Vector extendedHmdPos = m_HmdPosAbs - m_SetupOrigin;
    VectorNormalize(extendedHmdPos);
    extendedHmdPos = m_HmdPosAbs + (extendedHmdPos * 10);
    ray.Init(m_SetupOrigin, extendedHmdPos);

    m_Game->m_EngineTrace->TraceRay(ray, STANDARD_TRACE_MASK, &tracefilter, &trace);
    if (trace.fraction < 1 && trace.fraction > 0)
    {
        Vector distanceInsideWall = trace.endpos - extendedHmdPos;
        m_CameraAnchor += distanceInsideWall;
        m_HmdPosAbs = m_CameraAnchor - Vector(0, 0, 64) + m_HmdPosLocalInWorld;
    }

    // Reset camera if it somehow gets too far
    m_SetupOriginToHMD = m_HmdPosAbs - m_SetupOrigin;
    if (VectorLength(m_SetupOriginToHMD) > 150)
        ResetPosition();

    m_HmdAngAbs = hmdAngSmoothed;

    m_HmdPosAbsPrev = m_HmdPosAbs;
    m_SetupOriginPrev = m_SetupOrigin;

    GetViewParameters();
    m_Ipd = m_EyeToHeadTransformPosRight.x * 2;
    m_EyeZ = m_EyeToHeadTransformPosRight.z;

    // Hand tracking
    Vector leftControllerPosLocal = m_LeftControllerPose.TrackedDevicePos;
    QAngle leftControllerAngLocal = m_LeftControllerPose.TrackedDeviceAng;

    Vector rightControllerPosLocal = m_RightControllerPose.TrackedDevicePos;
    QAngle rightControllerAngLocal = m_RightControllerPose.TrackedDeviceAng;

    float controllerSmoothingStrength = std::clamp(m_ControllerSmoothing, 0.0f, 0.99f);

    if (!m_ControllerSmoothingInitialized)
    {
        m_LeftControllerPosSmoothed = leftControllerPosLocal;
        m_LeftControllerAngSmoothed = leftControllerAngLocal;
        m_RightControllerPosSmoothed = rightControllerPosLocal;
        m_RightControllerAngSmoothed = rightControllerAngLocal;
        m_ControllerSmoothingInitialized = true;
    }

    if (controllerSmoothingStrength > 0.0f)
    {
        float lerpFactor = 1.0f - controllerSmoothingStrength;

        auto smoothVector = [&](const Vector& target, Vector& current)
            {
                current.x += (target.x - current.x) * lerpFactor;
                current.y += (target.y - current.y) * lerpFactor;
                current.z += (target.z - current.z) * lerpFactor;
            };

        auto smoothAngleComponent = [&](float target, float& current)
            {
                float diff = target - current;
                diff -= 360.0f * std::floor((diff + 180.0f) / 360.0f);
                current += diff * lerpFactor;
            };

        smoothVector(leftControllerPosLocal, m_LeftControllerPosSmoothed);
        smoothVector(rightControllerPosLocal, m_RightControllerPosSmoothed);

        smoothAngleComponent(leftControllerAngLocal.x, m_LeftControllerAngSmoothed.x);
        smoothAngleComponent(leftControllerAngLocal.y, m_LeftControllerAngSmoothed.y);
        smoothAngleComponent(leftControllerAngLocal.z, m_LeftControllerAngSmoothed.z);

        smoothAngleComponent(rightControllerAngLocal.x, m_RightControllerAngSmoothed.x);
        smoothAngleComponent(rightControllerAngLocal.y, m_RightControllerAngSmoothed.y);
        smoothAngleComponent(rightControllerAngLocal.z, m_RightControllerAngSmoothed.z);
    }
    else
    {
        m_LeftControllerPosSmoothed = leftControllerPosLocal;
        m_LeftControllerAngSmoothed = leftControllerAngLocal;
        m_RightControllerPosSmoothed = rightControllerPosLocal;
        m_RightControllerAngSmoothed = rightControllerAngLocal;
    }

    auto wrapAngles = [&](QAngle& ang)
        {
            ang.x = wrapAngle(ang.x);
            ang.y = wrapAngle(ang.y);
            ang.z = wrapAngle(ang.z);
        };

    wrapAngles(m_LeftControllerAngSmoothed);
    wrapAngles(m_RightControllerAngSmoothed);

    Vector rightControllerPosSmoothed = m_RightControllerPosSmoothed;
    Vector leftControllerPosSmoothed = m_LeftControllerPosSmoothed;
    QAngle leftControllerAngSmoothed = m_LeftControllerAngSmoothed;
    QAngle rightControllerAngSmoothed = m_RightControllerAngSmoothed;

    Vector hmdToController = rightControllerPosSmoothed - hmdPosLocal;
    Vector rightControllerPosCorrected = hmdPosSmoothed + hmdToController;
    Vector leftHmdToController = leftControllerPosSmoothed - hmdPosLocal;
    Vector leftControllerPosCorrected = hmdPosSmoothed + leftHmdToController;

    // When using stick turning, pivot the controllers around the HMD
    VectorPivotXY(rightControllerPosCorrected, hmdPosSmoothed, m_RotationOffset);
    VectorPivotXY(leftControllerPosCorrected, hmdPosSmoothed, m_RotationOffset);

    Vector rightControllerPosLocalInWorld = rightControllerPosCorrected * m_VRScale;
    Vector leftControllerPosLocalInWorld = leftControllerPosCorrected * m_VRScale;

    m_RightControllerPosAbs = m_CameraAnchor - Vector(0, 0, 64) + rightControllerPosLocalInWorld;
    m_LeftControllerPosAbs = m_CameraAnchor - Vector(0, 0, 64) + leftControllerPosLocalInWorld;

    rightControllerAngSmoothed.y += m_RotationOffset;
    // Wrap angle from -180 to 180
    rightControllerAngSmoothed.y -= 360 * std::floor((rightControllerAngSmoothed.y + 180) / 360);

    QAngle::AngleVectors(leftControllerAngSmoothed, &m_LeftControllerForward, &m_LeftControllerRight, &m_LeftControllerUp);
    QAngle::AngleVectors(rightControllerAngSmoothed, &m_RightControllerForward, &m_RightControllerRight, &m_RightControllerUp);

    // Adjust controller angle 45 degrees downward
    m_LeftControllerForward = VectorRotate(m_LeftControllerForward, m_LeftControllerRight, -45.0);
    m_LeftControllerUp = VectorRotate(m_LeftControllerUp, m_LeftControllerRight, -45.0);

    m_RightControllerForward = VectorRotate(m_RightControllerForward, m_RightControllerRight, -45.0);
    m_RightControllerUp = VectorRotate(m_RightControllerUp, m_RightControllerRight, -45.0);

    m_RightControllerForwardUnforced = m_RightControllerForward;
    if (!m_RightControllerForwardUnforced.IsZero())
        m_LastUnforcedAimDirection = m_RightControllerForwardUnforced;

    const bool shouldForceAim = m_SpecialInfectedPreWarningActive;
    const Vector forcedTarget = m_SpecialInfectedPreWarningTarget;

    if (shouldForceAim)
    {
        Vector toTarget = forcedTarget - m_RightControllerPosAbs;
        if (!toTarget.IsZero())
        {
            VectorNormalize(toTarget);
            const float lerpFactor = m_SpecialInfectedDebug
                ? std::max(0.0f, m_SpecialInfectedAutoAimLerp)
                : std::clamp(m_SpecialInfectedAutoAimLerp, 0.0f, 0.4f);
            if (m_SpecialInfectedAutoAimDirection.IsZero())
                m_SpecialInfectedAutoAimDirection = toTarget;
            else
                m_SpecialInfectedAutoAimDirection += (toTarget - m_SpecialInfectedAutoAimDirection) * lerpFactor;

            if (!m_SpecialInfectedAutoAimDirection.IsZero())
            {
                VectorNormalize(m_SpecialInfectedAutoAimDirection);
                QAngle forcedAngles;
                QAngle::VectorAngles(m_SpecialInfectedAutoAimDirection, m_HmdUp, forcedAngles);
                QAngle::AngleVectors(forcedAngles, &m_RightControllerForward, &m_RightControllerRight, &m_RightControllerUp);
            }
        }
    }
    else
    {
        m_SpecialInfectedAutoAimDirection = m_RightControllerForward;
    }

    // Update scope camera pose + look-through activation
    if (m_ScopeEnabled && m_ScopeWeaponIsFirearm)
    {
        // Raw scope camera pose from controller (used for activation tests).
        const Vector scopePosRaw = m_RightControllerPosAbs
            + m_RightControllerForward * m_ScopeCameraOffset.x
            + m_RightControllerRight * m_ScopeCameraOffset.y
            + m_RightControllerUp * m_ScopeCameraOffset.z;

        QAngle scopeAngRaw;
        QAngle::VectorAngles(m_RightControllerForward, m_RightControllerUp, scopeAngRaw);
        scopeAngRaw.x += m_ScopeCameraAngleOffset.x;
        scopeAngRaw.y += m_ScopeCameraAngleOffset.y;
        scopeAngRaw.z += m_ScopeCameraAngleOffset.z;
        scopeAngRaw.x = wrapAngle(scopeAngRaw.x);
        scopeAngRaw.y = wrapAngle(scopeAngRaw.y);
        scopeAngRaw.z = wrapAngle(scopeAngRaw.z);

        // Default: render pose == raw pose.
        m_ScopeCameraPosAbs = scopePosRaw;
        m_ScopeCameraAngAbs = scopeAngRaw;

        if (m_ScopeRequireLookThrough)
        {
            const float maxDist = std::max(0.0f, m_ScopeLookThroughDistanceMeters) * m_VRScale;
            Vector toScope = scopePosRaw - m_HmdPosAbs;
            const float dist = VectorLength(toScope);
            if (dist > 0.0f && dist <= maxDist)
            {
                toScope /= dist;
                Vector scopeForward;
                QAngle::AngleVectors(scopeAngRaw, &scopeForward, nullptr, nullptr);
                if (!scopeForward.IsZero()) VectorNormalize(scopeForward);

                const float maxAngleRad = std::clamp(m_ScopeLookThroughAngleDeg, 0.0f, 89.0f) * (3.14159265358979323846f / 180.0f);
                const float minDot = cosf(maxAngleRad);

                const float inFrontDot = DotProduct(m_HmdForward, toScope);
                const float alignDot = DotProduct(m_HmdForward, scopeForward);
                m_ScopeActive = (inFrontDot > 0.2f) && (alignDot >= minDot);
            }
            else
            {
                m_ScopeActive = false;
            }
        }
        else
        {
            m_ScopeActive = true;
        }

        // Visual stabilization: smooth the scope RTT camera pose ONLY when scoped-in.
        // This does NOT affect shooting direction (bullets still use controller aim).
        if (m_ScopeStabilizationEnabled && m_ScopeActive)
        {
            const auto now = std::chrono::steady_clock::now();
            float dt = 1.0f / 90.0f;
            if (m_ScopeStabilizationInit && m_ScopeStabilizationLastTime.time_since_epoch().count() != 0)
                dt = std::chrono::duration<float>(now - m_ScopeStabilizationLastTime).count();

            // Clamp dt to avoid spikes (alt-tab, loading, etc.)
            dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 20.0f);

            // Initialize filter state on first scoped-in frame.
            if (!m_ScopeStabilizationInit)
            {
                m_ScopeStabilizationInit = true;
                m_ScopeStabPos = scopePosRaw;
                m_ScopeStabPosDeriv = { 0.0f, 0.0f, 0.0f };
                m_ScopeStabAng = scopeAngRaw;
                m_ScopeStabAngDeriv = { 0.0f, 0.0f, 0.0f };
            }
            m_ScopeStabilizationLastTime = now;

            // Slightly increase smoothing at very low FOV (high magnification).
            const float fovScale = std::clamp(m_ScopeFov / 20.0f, 0.35f, 1.25f);
            const float minCutoff = std::max(0.0001f, m_ScopeStabilizationMinCutoff * fovScale);

            OneEuroFilterVec3(scopePosRaw, m_ScopeStabPos, m_ScopeStabPosDeriv, m_ScopeStabilizationInit,
                dt, minCutoff, m_ScopeStabilizationBeta, m_ScopeStabilizationDCutoff);

            OneEuroFilterAngles(scopeAngRaw, m_ScopeStabAng, m_ScopeStabAngDeriv, m_ScopeStabilizationInit,
                dt, minCutoff, m_ScopeStabilizationBeta, m_ScopeStabilizationDCutoff);

            m_ScopeCameraPosAbs = m_ScopeStabPos;
            m_ScopeCameraAngAbs = m_ScopeStabAng;
        }
        else
        {
            m_ScopeStabilizationInit = false;
            m_ScopeStabilizationLastTime = {};
        }
    }
    else
    {
        m_ScopeActive = false;
        m_ScopeStabilizationInit = false;
        m_ScopeStabilizationLastTime = {};
    }

    UpdateScopeAimLineState();

    if (m_RearMirrorEnabled)
    {
        m_RearMirrorCameraPosAbs =
            m_HmdPosAbs
            + m_HmdForward * m_RearMirrorCameraOffset.x
            + m_HmdRight * m_RearMirrorCameraOffset.y
            + m_HmdUp * m_RearMirrorCameraOffset.z;

        QAngle mirrorAng = m_HmdAngAbs;
        mirrorAng.y += 180.0f;
        mirrorAng += m_RearMirrorCameraAngleOffset;
        NormalizeAndClampViewAngles(mirrorAng);
        m_RearMirrorCameraAngAbs = mirrorAng;
    }

    // Non-VR servers only understand cmd->viewangles. When ForceNonVRServerMovement is enabled,
    // solve an eye-based aim hit point so rendered aim line and real hit point stay consistent.
    UpdateNonVRAimSolution(localPlayer);
    UpdateAimingLaser(localPlayer);

    // controller angles
    QAngle::VectorAngles(m_LeftControllerForward, m_LeftControllerUp, m_LeftControllerAngAbs);
    QAngle::VectorAngles(m_RightControllerForward, m_RightControllerUp, m_RightControllerAngAbs);

    if (m_AdjustingViewmodel)
    {
        auto wrapDelta = [](float angle)
            {
                angle -= 360.0f * std::floor((angle + 180.0f) / 360.0f);
                return angle;
            };

        if (m_AdjustingKey != m_CurrentViewmodelKey)
        {
            m_AdjustStartLeftPos = m_LeftControllerPosAbs;
            m_AdjustStartLeftAng = m_LeftControllerAngAbs;
            m_AdjustStartViewmodelPos = m_ViewmodelPosAdjust;
            m_AdjustStartViewmodelAng = m_ViewmodelAngAdjust;
            m_AdjustStartViewmodelForward = m_ViewmodelForward;
            m_AdjustStartViewmodelRight = m_ViewmodelRight;
            m_AdjustStartViewmodelUp = m_ViewmodelUp;
            m_AdjustingKey = m_CurrentViewmodelKey;
        }

        Vector deltaPos = m_LeftControllerPosAbs - m_AdjustStartLeftPos;
        Vector viewmodelDelta =
        {
            -DotProduct(deltaPos, m_AdjustStartViewmodelForward),
            -DotProduct(deltaPos, m_AdjustStartViewmodelRight),
            -DotProduct(deltaPos, m_AdjustStartViewmodelUp)
        };
        QAngle deltaAng =
        {
            wrapDelta(m_LeftControllerAngAbs.x - m_AdjustStartLeftAng.x),
            wrapDelta(m_LeftControllerAngAbs.y - m_AdjustStartLeftAng.y),
            wrapDelta(m_LeftControllerAngAbs.z - m_AdjustStartLeftAng.z)
        };

        m_ViewmodelPosAdjust = m_AdjustStartViewmodelPos + viewmodelDelta;
        m_ViewmodelAngAdjust =
        {
            m_AdjustStartViewmodelAng.x + deltaAng.x,
            m_AdjustStartViewmodelAng.y + deltaAng.y,
            m_AdjustStartViewmodelAng.z + deltaAng.z
        };
        m_ViewmodelAdjustments[m_CurrentViewmodelKey] = { m_ViewmodelPosAdjust, m_ViewmodelAngAdjust };
        m_ViewmodelAdjustmentsDirty = true;
    }

    PositionAngle viewmodelOffset = localPlayer->GetViewmodelOffset();

    m_ViewmodelPosOffset = viewmodelOffset.position + m_ViewmodelPosAdjust;
    m_ViewmodelAngOffset = viewmodelOffset.angle + m_ViewmodelAngAdjust;

    m_ViewmodelForward = m_RightControllerForward;
    m_ViewmodelUp = m_RightControllerUp;
    m_ViewmodelRight = m_RightControllerRight;

    // Viewmodel yaw offset
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelUp, m_ViewmodelAngOffset.y);
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelUp, m_ViewmodelAngOffset.y);

    // Viewmodel pitch offset
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelRight, m_ViewmodelAngOffset.x);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelRight, m_ViewmodelAngOffset.x);

    // Viewmodel roll offset
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelForward, m_ViewmodelAngOffset.z);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelForward, m_ViewmodelAngOffset.z);

    UpdateMotionGestures(localPlayer);
}

void VR::UpdateMotionGestures(C_BasePlayer* localPlayer)
{
    const auto now = std::chrono::steady_clock::now();
    const float deltaSeconds = m_LastGestureUpdateTime.time_since_epoch().count() == 0
        ? 0.0f
        : std::chrono::duration<float>(now - m_LastGestureUpdateTime).count();
    m_LastGestureUpdateTime = now;

    if (!m_MotionGestureInitialized)
    {
        m_PrevLeftControllerLocalPos = m_LeftControllerPose.TrackedDevicePos;
        m_PrevRightControllerLocalPos = m_RightControllerPose.TrackedDevicePos;
        m_PrevHmdLocalPos = m_HmdPose.TrackedDevicePos;
        m_MotionGestureInitialized = true;
        return;
    }

    if (deltaSeconds <= 0.0f)
    {
        m_PrevLeftControllerLocalPos = m_LeftControllerPose.TrackedDevicePos;
        m_PrevRightControllerLocalPos = m_RightControllerPose.TrackedDevicePos;
        m_PrevHmdLocalPos = m_HmdPose.TrackedDevicePos;
        return;
    }

    const Vector leftDelta = m_LeftControllerPose.TrackedDevicePos - m_PrevLeftControllerLocalPos;
    const Vector rightDelta = m_RightControllerPose.TrackedDevicePos - m_PrevRightControllerLocalPos;
    const Vector hmdDelta = m_HmdPose.TrackedDevicePos - m_PrevHmdLocalPos;

    const Vector leftForwardHorizontal{ m_LeftControllerForward.x, m_LeftControllerForward.y, 0.0f };
    const float leftForwardHorizontalLength = VectorLength(leftForwardHorizontal);
    const Vector leftForwardHorizontalNorm = leftForwardHorizontalLength > 0.0f
        ? leftForwardHorizontal / leftForwardHorizontalLength
        : Vector(0.0f, 0.0f, 0.0f);

    const float leftOutwardHorizontalSpeed = std::max(0.0f, DotProduct(leftDelta, leftForwardHorizontalNorm)) / deltaSeconds;
    const float leftHorizontalSpeed = VectorLength(Vector(leftDelta.x, leftDelta.y, 0.0f)) / deltaSeconds;
    const float leftOutwardSpeed = leftForwardHorizontalLength > 0.01f ? leftOutwardHorizontalSpeed : leftHorizontalSpeed;
    const float rightDownSpeed = (-rightDelta.z) / deltaSeconds;
    const float hmdVerticalSpeed = hmdDelta.z / deltaSeconds;

    auto startHold = [&](std::chrono::steady_clock::time_point& holdUntil)
        {
            holdUntil = now + std::chrono::duration_cast<std::chrono::steady_clock::time_point::duration>(
                std::chrono::duration<float>(m_MotionGestureHoldDuration));
        };

    auto startCooldown = [&](std::chrono::steady_clock::time_point& cooldownEnd)
        {
            cooldownEnd = now + std::chrono::duration_cast<std::chrono::steady_clock::time_point::duration>(
                std::chrono::duration<float>(m_MotionGestureCooldown));
        };

    if (leftOutwardSpeed >= m_MotionGestureSwingThreshold && now >= m_SecondaryGestureCooldownEnd)
    {
        startHold(m_SecondaryAttackGestureHoldUntil);
        startCooldown(m_SecondaryGestureCooldownEnd);
    }

    if (rightDownSpeed >= m_MotionGestureDownSwingThreshold && now >= m_ReloadGestureCooldownEnd)
    {
        startHold(m_ReloadGestureHoldUntil);
        startCooldown(m_ReloadGestureCooldownEnd);
    }

    const bool onGround = localPlayer && localPlayer->m_hGroundEntity != -1;
    if (onGround && hmdVerticalSpeed >= m_MotionGestureJumpThreshold && now >= m_JumpGestureCooldownEnd)
    {
        startHold(m_JumpGestureHoldUntil);
        startCooldown(m_JumpGestureCooldownEnd);
    }

    m_PrevLeftControllerLocalPos = m_LeftControllerPose.TrackedDevicePos;
    m_PrevRightControllerLocalPos = m_RightControllerPose.TrackedDevicePos;
    m_PrevHmdLocalPos = m_HmdPose.TrackedDevicePos;
}

void VR::UpdateNonVRAimSolution(C_BasePlayer* localPlayer)
{
    // Keep the previous solution alive if we throttle this frame.
    if (!m_ForceNonVRServerMovement)
    {
        m_HasNonVRAimSolution = false;
        return;
    }

    if (!localPlayer || !m_Game || !m_Game->m_EngineTrace)
    {
        m_HasNonVRAimSolution = false;
        return;
    }

    // Throttle expensive trace work. Keep this local/static so we don't depend on
    // optional header fields across branches/patches.
    static std::chrono::steady_clock::time_point s_lastSolve{};
    const float kSolveMaxHz = 90.0f; // 0 disables throttling
    if (ShouldThrottle(s_lastSolve, kSolveMaxHz))
        return;

    // Use the same controller direction/origin rules as UpdateAimingLaser.
    Vector direction = m_RightControllerForward;
    if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
        direction = m_RightControllerForwardUnforced;

    if (direction.IsZero())
        direction = m_LastAimDirection.IsZero() ? m_LastUnforcedAimDirection : m_LastAimDirection;

    if (direction.IsZero())
    {
        m_HasNonVRAimSolution = false;
        return;
    }

    VectorNormalize(direction);

    Vector originBase = m_RightControllerPosAbs;
    Vector camDelta = m_ThirdPersonViewOrigin - m_SetupOrigin;
    if (m_IsThirdPersonCamera && camDelta.LengthSqr() > (5.0f * 5.0f))
        originBase += camDelta;

    Vector origin = originBase + direction * 2.0f;

    const float maxDistance = 8192.0f;
    Vector target = origin + direction * maxDistance;

    // 1) Controller ray -> P
    CGameTrace traceP;
    Ray_t rayP;
    CTraceFilterSkipSelf tracefilter((IHandleEntity*)localPlayer, 0);
    rayP.Init(origin, target);
    m_Game->m_EngineTrace->TraceRay(rayP, STANDARD_TRACE_MASK, &tracefilter, &traceP);

    const Vector P = (traceP.fraction < 1.0f && traceP.fraction > 0.0f) ? traceP.endpos : target;
    m_NonVRAimDesiredPoint = P;

    // 2) Eye ray towards P -> H (what the server will actually hit)
    const Vector eye = localPlayer->EyePosition();
    Vector toP = P - eye;
    if (toP.IsZero())
    {
        m_HasNonVRAimSolution = false;
        return;
    }
    VectorNormalize(toP);

    Vector endEye = eye + toP * maxDistance;
    CGameTrace traceH;
    Ray_t rayH;
    rayH.Init(eye, endEye);
    m_Game->m_EngineTrace->TraceRay(rayH, STANDARD_TRACE_MASK, &tracefilter, &traceH);

    const Vector H = (traceH.fraction < 1.0f && traceH.fraction > 0.0f) ? traceH.endpos : endEye;
    m_NonVRAimHitPoint = H;

    // 3) Eye -> H angles
    Vector toH = H - eye;
    if (toH.IsZero())
    {
        m_HasNonVRAimSolution = false;
        return;
    }
    VectorNormalize(toH);

    QAngle ang;
    QAngle::VectorAngles(toH, ang);
    // IMPORTANT: VectorAngles(forward) returns pitch/yaw in [0,360). Normalize BEFORE clamping.
    NormalizeAndClampViewAngles(ang);

    m_NonVRAimAngles = ang;
    m_HasNonVRAimSolution = true;
}

void VR::UpdateAimingLaser(C_BasePlayer* localPlayer)
{
    UpdateSpecialInfectedWarningState();
    UpdateSpecialInfectedPreWarningState();

    if (!m_Game->m_DebugOverlay)
        return;

    C_WeaponCSBase* activeWeapon = nullptr;
    if (localPlayer)
        activeWeapon = static_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());

    if (!ShouldShowAimLine(activeWeapon))
    {
        m_LastAimDirection = Vector{ 0.0f, 0.0f, 0.0f };
        m_HasAimLine = false;
        m_HasAimConvergePoint = false;
        m_HasThrowArc = false;
        m_LastAimWasThrowable = false;
        return;
    }

    bool isThrowable = IsThrowableWeapon(activeWeapon);
    // First-person: aim line follows the right controller.
    // Third-person: aim line follows the camera/HMD (more intuitive and doesn't drift when you look around).
    Vector direction = m_RightControllerForward;
    if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
        direction = m_RightControllerForwardUnforced;
    if (direction.IsZero())
    {
        if (m_LastAimDirection.IsZero())
        {
            const float duration = std::max(m_AimLinePersistence, m_LastFrameDuration * m_AimLineFrameDurationMultiplier);

            if (m_LastAimWasThrowable && m_HasThrowArc)
            {
                DrawThrowArcFromCache(duration);
                return;
            }

            if (!m_HasAimLine)
                return;

            DrawLineWithThickness(m_AimLineStart, m_AimLineEnd, duration);
            return;
        }

        direction = m_LastAimDirection;
        isThrowable = m_LastAimWasThrowable;
    }
    else
    {
        m_LastAimDirection = direction;
        m_LastAimWasThrowable = isThrowable;
    }
    VectorNormalize(direction);

    // Aim line origin is controller-based. In third-person the rendered camera is offset behind the player,
    // so we translate the controller position into the rendered-camera frame.
    // We key off the actual camera delta (not just the boolean) to avoid cases where 3P detection flickers.
    Vector originBase = m_RightControllerPosAbs;
    Vector camDelta = m_ThirdPersonViewOrigin - m_SetupOrigin;
    if (m_IsThirdPersonCamera && camDelta.LengthSqr() > (5.0f * 5.0f))
        originBase += camDelta;

    Vector origin = originBase + direction * 2.0f;

    if (isThrowable)
    {
        m_HasAimConvergePoint = false;

        Vector pitchSource = direction;
        if (!m_ForceNonVRServerMovement && !m_HmdForward.IsZero())
            pitchSource = m_HmdForward;

        DrawThrowArc(origin, direction, pitchSource);
        return;
    }

    const float maxDistance = 8192.0f;
    Vector target = origin + direction * maxDistance;

    // ForceNonVRServerMovement: keep the rendered aim line consistent with what the
    // server will actually trace (eye -> cmd->viewangles).
    if (m_ForceNonVRServerMovement && m_HasNonVRAimSolution)
    {
        m_AimConvergePoint = m_NonVRAimHitPoint;
        m_HasAimConvergePoint = true;
        target = m_AimConvergePoint;
    }
    else
    {
        // Third-person convergence point P: where the *rendered* aim ray hits.
        // IMPORTANT: We do NOT "correct" P based on what the bullet line can reach.
        if (m_IsThirdPersonCamera && localPlayer && m_Game->m_EngineTrace)
        {
            CGameTrace trace;
            Ray_t ray;
            CTraceFilterSkipSelf tracefilter((IHandleEntity*)localPlayer, 0);

            ray.Init(origin, target);
            m_Game->m_EngineTrace->TraceRay(ray, STANDARD_TRACE_MASK, &tracefilter, &trace);

            m_AimConvergePoint = (trace.fraction < 1.0f && trace.fraction > 0.0f) ? trace.endpos : target;
            m_HasAimConvergePoint = true;
            target = m_AimConvergePoint; // draw to P
        }
        else
        {
            m_HasAimConvergePoint = false;
        }
    }

    m_AimLineStart = origin;
    m_AimLineEnd = target;
    m_HasAimLine = true;
    m_HasThrowArc = false;

    DrawAimLine(origin, target);
}

bool VR::ShouldShowAimLine(C_WeaponCSBase* weapon) const
{
    // While pinned/controlled by SI, the player model/camera can be driven by animations,
// causing the aim line to wildly drift and feel broken. Disable it in those states.
    if (m_PlayerControlledBySI)
        return false;

    if (!m_AimLineEnabled || !weapon)
        return false;

    switch (weapon->GetWeaponID())
    {
    case C_WeaponCSBase::PISTOL:
    case C_WeaponCSBase::MAGNUM:
    case C_WeaponCSBase::UZI:
    case C_WeaponCSBase::PUMPSHOTGUN:
    case C_WeaponCSBase::AUTOSHOTGUN:
    case C_WeaponCSBase::M16A1:
    case C_WeaponCSBase::HUNTING_RIFLE:
    case C_WeaponCSBase::MAC10:
    case C_WeaponCSBase::SHOTGUN_CHROME:
    case C_WeaponCSBase::SCAR:
    case C_WeaponCSBase::SNIPER_MILITARY:
    case C_WeaponCSBase::SPAS:
    case C_WeaponCSBase::AK47:
    case C_WeaponCSBase::MP5:
    case C_WeaponCSBase::SG552:
    case C_WeaponCSBase::AWP:
    case C_WeaponCSBase::SCOUT:
    case C_WeaponCSBase::GRENADE_LAUNCHER:
    case C_WeaponCSBase::M60:
    case C_WeaponCSBase::MOLOTOV:
    case C_WeaponCSBase::PIPE_BOMB:
    case C_WeaponCSBase::VOMITJAR:
    case C_WeaponCSBase::TANK_CLAW:
    case C_WeaponCSBase::HUNTER_CLAW:
    case C_WeaponCSBase::CHARGER_CLAW:
    case C_WeaponCSBase::BOOMER_CLAW:
    case C_WeaponCSBase::SMOKER_CLAW:
    case C_WeaponCSBase::SPITTER_CLAW:
    case C_WeaponCSBase::JOCKEY_CLAW:
    case C_WeaponCSBase::VOMIT:
    case C_WeaponCSBase::SPLAT:
    case C_WeaponCSBase::POUNCE:
    case C_WeaponCSBase::LOUNGE:
    case C_WeaponCSBase::PULL:
    case C_WeaponCSBase::CHOKE:
    case C_WeaponCSBase::ROCK:
    case C_WeaponCSBase::MELEE:
    case C_WeaponCSBase::CHAINSAW:
        return m_MeleeAimLineEnabled;
    default:
        return false;
    }
}

void VR::CycleScopeMagnification()
{
    if (!m_ScopeEnabled || m_ScopeMagnificationOptions.empty())
        return;

    m_ScopeMagnificationIndex = (m_ScopeMagnificationIndex + 1) % m_ScopeMagnificationOptions.size();
    m_ScopeFov = std::clamp(m_ScopeMagnificationOptions[m_ScopeMagnificationIndex], 1.0f, 179.0f);

    Game::logMsg("[VR] Scope magnification set to %.2f", m_ScopeFov);
}

void VR::UpdateScopeAimLineState()
{
    if (m_AimLineConfigEnabled)
    {
        m_ScopeForcingAimLine = false;
        return;
    }

    if (!m_ScopeEnabled)
    {
        if (m_ScopeForcingAimLine)
        {
            m_AimLineEnabled = false;
            m_HasAimLine = false;
            m_ScopeForcingAimLine = false;
        }
        return;
    }

    const bool scopeActive = IsScopeActive();
    if (scopeActive && !m_AimLineEnabled)
    {
        m_AimLineEnabled = true;
        m_ScopeForcingAimLine = true;
    }
    else if (!scopeActive && m_ScopeForcingAimLine)
    {
        m_AimLineEnabled = false;
        m_HasAimLine = false;
        m_ScopeForcingAimLine = false;
    }
}

bool VR::IsThrowableWeapon(C_WeaponCSBase* weapon) const
{
    if (!weapon)
        return false;

    switch (weapon->GetWeaponID())
    {
    case C_WeaponCSBase::MOLOTOV:
    case C_WeaponCSBase::PIPE_BOMB:
    case C_WeaponCSBase::VOMITJAR:
        return true;
    default:
        return false;
    }
}

float VR::CalculateThrowArcDistance(const Vector& pitchSource, bool* clampedToMax) const
{
    Vector direction = pitchSource;
    if (direction.IsZero())
        return m_ThrowArcMinDistance;

    VectorNormalize(direction);

    const float pitchInfluence = direction.z * m_ThrowArcPitchScale;
    const float scaledDistance = m_ThrowArcBaseDistance * (1.0f + pitchInfluence);
    const float maxDistance = std::max(m_ThrowArcMinDistance, m_ThrowArcMaxDistance);
    const float clampedDistance = std::clamp(scaledDistance, m_ThrowArcMinDistance, maxDistance);

    if (clampedToMax)
        *clampedToMax = clampedDistance >= maxDistance;

    return clampedDistance;
}

void VR::DrawAimLine(const Vector& start, const Vector& end)
{
    if (!m_AimLineEnabled)
        return;

    // Throttle expensive overlay geometry. Duration persistence keeps it visible.
    if (ShouldThrottle(m_LastAimLineDrawTime, m_AimLineMaxHz))
        return;

    // Keep the overlay alive for at least two frames so it doesn't disappear when the framerate drops.
    const float duration = std::max(std::max(m_AimLinePersistence, m_LastFrameDuration * m_AimLineFrameDurationMultiplier), MinIntervalSeconds(m_AimLineMaxHz));
    if (!m_Game->m_DebugOverlay || !m_AimLineEnabled)
        return;

    DrawLineWithThickness(start, end, duration);
}

void VR::DrawThrowArc(const Vector& origin, const Vector& forward, const Vector& pitchSource)
{
    if (!m_Game->m_DebugOverlay || !m_AimLineEnabled)
        return;

    Vector direction = forward;
    if (direction.IsZero())
        return;
    VectorNormalize(direction);

    Vector planarForward(direction.x, direction.y, 0.0f);
    if (planarForward.IsZero())
        planarForward = direction;
    VectorNormalize(planarForward);

    Vector distanceSource = pitchSource.IsZero() ? direction : pitchSource;
    VectorNormalize(distanceSource);

    bool clampedToMaxDistance = false;
    const float distance = CalculateThrowArcDistance(distanceSource, &clampedToMaxDistance);
    if (clampedToMaxDistance)
    {
        m_HasThrowArc = false;
        m_HasAimLine = false;
        return;
    }
    const float arcHeight = std::max(distance * m_ThrowArcHeightRatio, m_ThrowArcMinDistance * 0.5f);

    Vector landingPoint = origin + planarForward * distance;
    landingPoint.z += m_ThrowArcLandingOffset;
    Vector apex = origin + planarForward * distance * 0.5f;
    apex.z += arcHeight;

    auto lerp = [](const Vector& a, const Vector& b, float t)
        {
            return a + (b - a) * t;
        };

    const float duration = std::max(m_AimLinePersistence, m_LastFrameDuration * m_AimLineFrameDurationMultiplier);
    for (int i = 0; i <= THROW_ARC_SEGMENTS; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(THROW_ARC_SEGMENTS);
        Vector ab = lerp(origin, apex, t);
        Vector bc = lerp(apex, landingPoint, t);
        m_LastThrowArcPoints[i] = lerp(ab, bc, t);
    }

    m_HasThrowArc = true;
    m_HasAimLine = false;

    DrawThrowArcFromCache(duration);
}

void VR::DrawThrowArcFromCache(float duration)
{
    if (!m_Game->m_DebugOverlay || !m_HasThrowArc)
        return;

    // Throttle throw arc drawing; it's a lot of overlay primitives.
    if (ShouldThrottle(m_LastThrowArcDrawTime, m_ThrowArcMaxHz))
        return;

    for (int i = 0; i < THROW_ARC_SEGMENTS; ++i)
    {
        DrawLineWithThickness(m_LastThrowArcPoints[i], m_LastThrowArcPoints[i + 1], duration);
    }
}

void VR::DrawLineWithThickness(const Vector& start, const Vector& end, float duration)
{
    int colorR = 0;
    int colorG = 0;
    int colorB = 0;
    int colorA = 0;
    GetAimLineColor(colorR, colorG, colorB, colorA);

    m_Game->m_DebugOverlay->AddLineOverlay(start, end, colorR, colorG, colorB, false, duration);

    const float thickness = std::max(m_AimLineThickness, 0.0f);
    if (thickness <= 0.0f)
        return;

    Vector forward = end - start;
    if (forward.IsZero())
        return;

    VectorNormalize(forward);

    Vector referenceUp = m_RightControllerUp;
    if (referenceUp.IsZero())
        referenceUp = Vector(0.0f, 0.0f, 1.0f);

    Vector basis1 = CrossProduct(forward, referenceUp);
    if (basis1.IsZero())
        basis1 = CrossProduct(forward, Vector(0.0f, 1.0f, 0.0f));
    if (basis1.IsZero())
        basis1 = CrossProduct(forward, Vector(1.0f, 0.0f, 0.0f));
    if (basis1.IsZero())
        return;

    VectorNormalize(basis1);
    Vector basis2 = CrossProduct(forward, basis1);
    if (basis2.IsZero())
        return;
    VectorNormalize(basis2);

    const int segments = 16;
    const float radius = thickness * 0.5f;
    const float twoPi = 6.28318530718f;

    for (int i = 0; i < segments; ++i)
    {
        const float angle0 = twoPi * static_cast<float>(i) / static_cast<float>(segments);
        const float angle1 = twoPi * static_cast<float>(i + 1) / static_cast<float>(segments);

        const float cos0 = std::cos(angle0);
        const float sin0 = std::sin(angle0);
        const float cos1 = std::cos(angle1);
        const float sin1 = std::sin(angle1);

        Vector offset0 = (basis1 * cos0 + basis2 * sin0) * radius;
        Vector offset1 = (basis1 * cos1 + basis2 * sin1) * radius;

        Vector start0 = start + offset0;
        Vector start1 = start + offset1;
        Vector end0 = end + offset0;
        Vector end1 = end + offset1;

        m_Game->m_DebugOverlay->AddTriangleOverlay(start0, start1, end1, colorR, colorG, colorB, colorA, false, duration);
        m_Game->m_DebugOverlay->AddTriangleOverlay(start0, end1, end0, colorR, colorG, colorB, colorA, false, duration);
    }
}

VR::SpecialInfectedType VR::GetSpecialInfectedType(const C_BaseEntity* entity) const
{
    if (!entity)
        return SpecialInfectedType::None;
    const auto base = reinterpret_cast<const std::uint8_t*>(entity);
    const int zombieClass = *reinterpret_cast<const int*>(base + kZombieClassOffset);

    switch (zombieClass)
    {
    case 1:
        return SpecialInfectedType::Smoker;
    case 2:
        return SpecialInfectedType::Boomer;
    case 3:
        return SpecialInfectedType::Hunter;
    case 4:
        return SpecialInfectedType::Spitter;
    case 5:
        return SpecialInfectedType::Jockey;
    case 6:
        return SpecialInfectedType::Charger;
    case 258:
        return SpecialInfectedType::Witch;
    case 8:
        return SpecialInfectedType::Tank;
    default:
        return SpecialInfectedType::None;
    }
}

VR::SpecialInfectedType VR::GetSpecialInfectedTypeFromModel(const std::string& modelName) const
{
    std::string lower = modelName;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const std::array<std::pair<const char*, SpecialInfectedType>, 5> specialKeywords =
    {
        // L4D2 defaults
        std::make_pair("tank", SpecialInfectedType::Tank),
        std::make_pair("hulk", SpecialInfectedType::Tank),
        std::make_pair("witch", SpecialInfectedType::Witch),
        // l4d1 variants share the same colors
        std::make_pair("hulk_dlc3", SpecialInfectedType::Tank),
        std::make_pair("hulk_l4d1", SpecialInfectedType::Tank)
    };

    auto it = std::find_if(specialKeywords.begin(), specialKeywords.end(), [&](const auto& entry)
        {
            return lower.find(entry.first) != std::string::npos;
        });

    if (it != specialKeywords.end())
        return it->second;

    return SpecialInfectedType::None;
}

bool VR::IsEntityAlive(const C_BaseEntity* entity) const
{
    if (!entity)
        return false;

    const auto base = reinterpret_cast<const std::uint8_t*>(entity);
    const unsigned char lifeState = *reinterpret_cast<const unsigned char*>(base + kLifeStateOffset);

    return lifeState == 0;
}

void VR::DrawSpecialInfectedArrow(const Vector& origin, SpecialInfectedType type)
{
    if (!m_SpecialInfectedArrowEnabled || m_SpecialInfectedArrowSize <= 0.0f || type == SpecialInfectedType::None)
        return;

    if (!m_Game || !m_Game->m_DebugOverlay)
        return;

    const float baseHeight = m_SpecialInfectedArrowHeight;
    const float wingLength = m_SpecialInfectedArrowSize;
    const Vector base = origin + Vector(0.0f, 0.0f, baseHeight);
    const Vector tip = base + Vector(0.0f, 0.0f, -m_SpecialInfectedArrowSize);
    const float duration = std::max(m_LastFrameDuration, 0.03f);

    auto drawArrowLine = [&](const Vector& start, const Vector& end)
        {
            const float offset = m_SpecialInfectedArrowThickness * 0.5f;

            const size_t typeIndex = static_cast<size_t>(type);
            const RgbColor& arrowColor = typeIndex < m_SpecialInfectedArrowColors.size()
                ? m_SpecialInfectedArrowColors[typeIndex]
                : m_SpecialInfectedArrowDefaultColor;

            if (offset <= 0.0f)
            {
                m_Game->m_DebugOverlay->AddLineOverlay(start, end, arrowColor.r, arrowColor.g, arrowColor.b, true, duration);
                return;
            }

            const std::array<Vector, 5> offsets =
            {
                Vector(0.0f, 0.0f, 0.0f),
                Vector(offset, 0.0f, 0.0f),
                Vector(-offset, 0.0f, 0.0f),
                Vector(0.0f, offset, 0.0f),
                Vector(0.0f, -offset, 0.0f)
            };

            for (const auto& lineOffset : offsets)
            {
                m_Game->m_DebugOverlay->AddLineOverlay(start + lineOffset, end + lineOffset, arrowColor.r, arrowColor.g, arrowColor.b, true, duration);
            }
        };

    drawArrowLine(base, tip);
    drawArrowLine(base + Vector(wingLength, 0.0f, 0.0f), tip);
    drawArrowLine(base + Vector(-wingLength, 0.0f, 0.0f), tip);
    drawArrowLine(base + Vector(0.0f, wingLength, 0.0f), tip);
    drawArrowLine(base + Vector(0.0f, -wingLength, 0.0f), tip);
}

void VR::RefreshSpecialInfectedBlindSpotWarning(const Vector& infectedOrigin)
{
    if (m_SpecialInfectedBlindSpotDistance <= 0.0f)
        return;

    if (!IsSpecialInfectedInBlindSpot(infectedOrigin))
        return;

    m_SpecialInfectedWarningTarget = infectedOrigin;
    m_SpecialInfectedWarningTargetActive = true;

    const bool wasActive = m_SpecialInfectedBlindSpotWarningActive;
    m_SpecialInfectedBlindSpotWarningActive = true;
    m_LastSpecialInfectedWarningTime = std::chrono::steady_clock::now();

    if (!wasActive && m_SpecialInfectedWarningActionEnabled)
        StartSpecialInfectedWarningAction();
}

bool VR::IsSpecialInfectedInBlindSpot(const Vector& infectedOrigin) const
{
    Vector toInfected = infectedOrigin - m_HmdPosAbs;
    toInfected.z = 0.0f;
    if (toInfected.IsZero())
        return false;

    const float maxDistance = m_SpecialInfectedBlindSpotDistance;
    if (maxDistance <= 0.0f)
        return false;

    const float maxDistanceSq = maxDistance * maxDistance;
    return toInfected.LengthSqr() <= maxDistanceSq;
}

bool VR::HasLineOfSightToSpecialInfected(const Vector& infectedOrigin, int entityIndex) const
{
    if (!m_Game || !m_Game->m_EngineTrace || !m_Game->m_EngineClient)
        return true;

    int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
    if (!localPlayer)
        return true;

    // Cache expensive TraceRay calls per-entity to avoid spikes when DrawModelExecute fires many times.
    if (entityIndex > 0 && m_SpecialInfectedTraceMaxHz > 0.0f)
    {
        auto& last = m_LastSpecialInfectedTraceTime[entityIndex];
        const auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count() != 0)
        {
            const float minInterval = 1.0f / std::max(1.0f, m_SpecialInfectedTraceMaxHz);
            const float elapsed = std::chrono::duration<float>(now - last).count();
            if (elapsed < minInterval)
            {
                auto it = m_LastSpecialInfectedTraceResult.find(entityIndex);
                if (it != m_LastSpecialInfectedTraceResult.end())
                    return it->second;
            }
        }
        last = now;
    }

    IHandleEntity* targetEntity = nullptr;
    if (entityIndex > 0)
        targetEntity = (IHandleEntity*)m_Game->GetClientEntity(entityIndex);

    CGameTrace trace;
    Ray_t ray;
    CTraceFilterSkipNPCsAndEntity tracefilter((IHandleEntity*)localPlayer, targetEntity, 0);

    ray.Init(m_RightControllerPosAbs, infectedOrigin);
    m_Game->m_EngineTrace->TraceRay(ray, STANDARD_TRACE_MASK, &tracefilter, &trace);

    const bool hasLos = trace.fraction >= 1.0f;

    if (entityIndex > 0)
        m_LastSpecialInfectedTraceResult[entityIndex] = hasLos;

    return hasLos;
}

void VR::RefreshSpecialInfectedPreWarning(const Vector& infectedOrigin, SpecialInfectedType type, int entityIndex, bool isPlayerClass)
{
    if (m_SpecialInfectedPreWarningDistance <= 0.0f || !m_SpecialInfectedPreWarningAutoAimEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_SpecialInfectedAutoAimCooldown > 0.0f && now < m_SpecialInfectedAutoAimCooldownEnd)
        return;

    Vector aimDirection = m_RightControllerForwardUnforced;
    if (aimDirection.IsZero())
        aimDirection = m_LastUnforcedAimDirection;

    Vector toTargetFromController = infectedOrigin - m_RightControllerPosAbs;
    float aimLineDistance = std::numeric_limits<float>::max();
    if (!aimDirection.IsZero() && !toTargetFromController.IsZero())
    {
        VectorNormalize(aimDirection);
        Vector projection = aimDirection * DotProduct(toTargetFromController, aimDirection);
        Vector closestPoint = m_RightControllerPosAbs + projection;
        aimLineDistance = (infectedOrigin - closestPoint).Length();
    }

    const bool isLockedTarget = m_SpecialInfectedPreWarningTargetEntityIndex != -1
        && entityIndex == m_SpecialInfectedPreWarningTargetEntityIndex;
    if (isLockedTarget && aimLineDistance > m_SpecialInfectedPreWarningAimReleaseDistance)
    {
        m_SpecialInfectedPreWarningActive = false;
        m_SpecialInfectedPreWarningInRange = false;
        m_SpecialInfectedPreWarningTargetEntityIndex = -1;
        m_SpecialInfectedPreWarningTargetIsPlayer = false;
        m_SpecialInfectedPreWarningTargetDistanceSq = std::numeric_limits<float>::max();
        return;
    }

    if (m_SpecialInfectedPreWarningTargetEntityIndex != -1 && entityIndex != m_SpecialInfectedPreWarningTargetEntityIndex)
    {
        if (m_SpecialInfectedPreWarningTargetIsPlayer)
            return;
    }

    Vector toInfected = infectedOrigin - m_HmdPosAbs;
    toInfected.z = 0.0f;
    if (toInfected.IsZero())
        return;

    const float distanceSq = toInfected.LengthSqr();
    const float maxDistanceSq = m_SpecialInfectedPreWarningDistance * m_SpecialInfectedPreWarningDistance;
    const bool inRange = distanceSq <= maxDistanceSq;
    if (inRange)
    {
        const float maxAimAngle = m_SpecialInfectedDebug
            ? std::max(0.0f, m_SpecialInfectedPreWarningAimAngle)
            : std::clamp(m_SpecialInfectedPreWarningAimAngle, 0.0f, 5.0f);
        if (maxAimAngle > 0.0f)
        {
            const bool aimLineClose = aimLineDistance <= m_SpecialInfectedPreWarningAimSnapDistance;
            Vector toTarget = infectedOrigin - m_RightControllerPosAbs;
            if (!aimLineClose)
            {
                if (!aimDirection.IsZero() && !toTarget.IsZero())
                {
                    VectorNormalize(aimDirection);
                    VectorNormalize(toTarget);
                    const float minDot = std::cos(DEG2RAD(maxAimAngle));
                    if (DotProduct(aimDirection, toTarget) < minDot)
                        return;
                }
                else
                {
                    return;
                }
            }
        }

        if (!HasLineOfSightToSpecialInfected(infectedOrigin, entityIndex))
            return;

        const bool isPounceType = type == SpecialInfectedType::Hunter || type == SpecialInfectedType::Jockey;
        if (m_SpecialInfectedWarningActionEnabled && isPounceType && m_SpecialInfectedPreWarningEvadeDistance > 0.0f)
        {
            const float evadeDistanceSq = m_SpecialInfectedPreWarningEvadeDistance * m_SpecialInfectedPreWarningEvadeDistance;
            if (distanceSq <= evadeDistanceSq)
            {
                m_SpecialInfectedWarningTarget = infectedOrigin;
                m_SpecialInfectedWarningTargetActive = true;
                m_LastSpecialInfectedWarningTime = now;
                m_SpecialInfectedBlindSpotWarningActive = true;

                if (!m_SpecialInfectedPreWarningEvadeTriggered)
                {
                    StartSpecialInfectedWarningAction();
                    m_SpecialInfectedPreWarningEvadeTriggered = true;
                }
            }
        }
        const bool isCloser = distanceSq < m_SpecialInfectedPreWarningTargetDistanceSq;
        const bool isCandidate = isLockedTarget || isCloser || distanceSq <= (m_SpecialInfectedPreWarningTargetDistanceSq + 0.01f);
        const float updateInterval = std::max(0.0f, m_SpecialInfectedPreWarningTargetUpdateInterval);
        const auto elapsedUpdate = std::chrono::duration<float>(now - m_LastSpecialInfectedPreWarningTargetUpdateTime).count();
        if (isCandidate && (isCloser || updateInterval <= 0.0f || elapsedUpdate >= updateInterval))
        {
            Vector adjustedTarget = infectedOrigin;
            const size_t typeIndex = static_cast<size_t>(type);
            if (typeIndex < m_SpecialInfectedPreWarningAimOffsets.size())
            {
                const Vector& offset = m_SpecialInfectedPreWarningAimOffsets[typeIndex];
                adjustedTarget += (m_HmdRight * offset.x) + (m_HmdForward * offset.y) + (m_HmdUp * offset.z);
            }

            m_SpecialInfectedPreWarningTarget = adjustedTarget;
            m_LastSpecialInfectedPreWarningTargetUpdateTime = now;
            m_SpecialInfectedPreWarningTargetDistanceSq = distanceSq;
            if (!isLockedTarget && entityIndex > 0)
            {
                m_SpecialInfectedPreWarningTargetEntityIndex = entityIndex;
                m_SpecialInfectedPreWarningTargetIsPlayer = isPlayerClass;
            }
        }

        m_SpecialInfectedPreWarningActive = true;
        m_SpecialInfectedPreWarningInRange = true;
        m_LastSpecialInfectedPreWarningSeenTime = now;
        return;
    }
}

void VR::UpdateSpecialInfectedWarningState()
{
    if (!m_SpecialInfectedBlindSpotWarningActive)
    {
        m_SpecialInfectedWarningTargetActive = false;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsedSeconds = std::chrono::duration<float>(now - m_LastSpecialInfectedWarningTime).count();

    if (elapsedSeconds > m_SpecialInfectedBlindSpotWarningDuration)
    {
        m_SpecialInfectedBlindSpotWarningActive = false;
        m_SpecialInfectedWarningTargetActive = false;
    }
}

void VR::UpdateSpecialInfectedPreWarningState()
{
    const auto now = std::chrono::steady_clock::now();
    const bool wasActive = m_SpecialInfectedPreWarningActive;

    if (!m_SpecialInfectedPreWarningAutoAimEnabled)
    {
        m_SpecialInfectedPreWarningActive = false;
        m_SpecialInfectedPreWarningInRange = false;
        m_SpecialInfectedPreWarningTargetEntityIndex = -1;
        m_SpecialInfectedPreWarningTargetIsPlayer = false;
        m_SpecialInfectedPreWarningTargetDistanceSq = std::numeric_limits<float>::max();
        m_SpecialInfectedAutoAimCooldownEnd = {};
        m_SpecialInfectedPreWarningEvadeTriggered = false;
        return;
    }

    if (m_SpecialInfectedAutoAimCooldown > 0.0f && now < m_SpecialInfectedAutoAimCooldownEnd)
    {
        m_SpecialInfectedPreWarningActive = false;
        m_SpecialInfectedPreWarningInRange = false;
        m_SpecialInfectedPreWarningTargetEntityIndex = -1;
        m_SpecialInfectedPreWarningTargetIsPlayer = false;
        m_SpecialInfectedPreWarningTargetDistanceSq = std::numeric_limits<float>::max();
        m_SpecialInfectedPreWarningEvadeTriggered = false;
        return;
    }

    m_SpecialInfectedPreWarningTargetDistanceSq = std::numeric_limits<float>::max();
    const float seenTimeout = 0.1f;

    if (m_SpecialInfectedPreWarningTargetEntityIndex != -1 && m_SpecialInfectedPreWarningTargetIsPlayer)
    {
        if (!m_Game || !m_Game->m_ClientEntityList)
        {
            m_SpecialInfectedPreWarningTargetEntityIndex = -1;
            m_SpecialInfectedPreWarningTargetIsPlayer = false;
            m_SpecialInfectedPreWarningActive = false;
            m_SpecialInfectedPreWarningInRange = false;
            return;
        }

        const int maxEntityIndex = m_Game->m_ClientEntityList->GetHighestEntityIndex();
        if (m_SpecialInfectedPreWarningTargetEntityIndex <= 0 || m_SpecialInfectedPreWarningTargetEntityIndex > maxEntityIndex)
        {
            m_SpecialInfectedPreWarningTargetEntityIndex = -1;
            m_SpecialInfectedPreWarningTargetIsPlayer = false;
            m_SpecialInfectedPreWarningActive = false;
            m_SpecialInfectedPreWarningInRange = false;
            return;
        }

        C_BaseEntity* entity = m_Game->GetClientEntity(m_SpecialInfectedPreWarningTargetEntityIndex);
        const char* className = entity ? m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(entity)) : nullptr;
        const bool isPlayerClass = className && (std::strcmp(className, "CTerrorPlayer") == 0 || std::strcmp(className, "C_TerrorPlayer") == 0);
        const bool isAlive = entity && isPlayerClass && IsEntityAlive(entity);
        const bool isSpecialInfected = entity && (GetSpecialInfectedType(entity) != SpecialInfectedType::None);
        if (!isAlive || !isSpecialInfected)
        {
            m_SpecialInfectedPreWarningTargetEntityIndex = -1;
            m_SpecialInfectedPreWarningTargetIsPlayer = false;
            m_SpecialInfectedPreWarningActive = false;
            m_SpecialInfectedPreWarningInRange = false;
            return;
        }
    }
    else if (m_SpecialInfectedPreWarningInRange)
    {
        const auto elapsed = std::chrono::duration<float>(now - m_LastSpecialInfectedPreWarningSeenTime).count();
        if (elapsed > seenTimeout)
            m_SpecialInfectedPreWarningInRange = false;
    }

    if (m_SpecialInfectedPreWarningActive && !m_SpecialInfectedPreWarningInRange)
        m_SpecialInfectedPreWarningActive = false;

    if (!m_SpecialInfectedPreWarningActive)
        m_SpecialInfectedPreWarningEvadeTriggered = false;

    if (wasActive && !m_SpecialInfectedPreWarningActive && m_SpecialInfectedAutoAimCooldown > 0.0f)
    {
        m_SpecialInfectedAutoAimCooldownEnd = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(m_SpecialInfectedAutoAimCooldown));
        m_SpecialInfectedPreWarningTargetEntityIndex = -1;
        m_SpecialInfectedPreWarningTargetIsPlayer = false;
        m_SpecialInfectedPreWarningTargetDistanceSq = std::numeric_limits<float>::max();
    }
}

void VR::StartSpecialInfectedWarningAction()
{
    if (!m_SpecialInfectedWarningActionEnabled)
        return;

    if (m_SpecialInfectedWarningActionStep != SpecialInfectedWarningActionStep::None)
        return;

    m_Game->ClientCmd_Unrestricted("-attack");
    m_Game->ClientCmd_Unrestricted("-attack2");
    m_Game->ClientCmd_Unrestricted("-jump");
    m_Game->ClientCmd_Unrestricted("-use");
    m_Game->ClientCmd_Unrestricted("-reload");
    m_Game->ClientCmd_Unrestricted("-back");
    m_Game->ClientCmd_Unrestricted("-forward");
    m_Game->ClientCmd_Unrestricted("-moveleft");
    m_Game->ClientCmd_Unrestricted("-moveright");
    m_Game->ClientCmd_Unrestricted("-voicerecord");

    m_SuppressPlayerInput = true;
    m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::PressSecondaryAttack;
    m_SpecialInfectedWarningNextActionTime = std::chrono::steady_clock::now();
}

void VR::UpdateSpecialInfectedWarningAction()
{
    if (!m_SpecialInfectedWarningActionEnabled)
    {
        ResetSpecialInfectedWarningAction();
        return;
    }

    if (m_SpecialInfectedWarningActionStep == SpecialInfectedWarningActionStep::None)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now < m_SpecialInfectedWarningNextActionTime)
        return;

    const auto secondsToDuration = [](float seconds)
        {
            return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(seconds));
        };

    switch (m_SpecialInfectedWarningActionStep)
    {
    case SpecialInfectedWarningActionStep::PressSecondaryAttack:
        m_Game->ClientCmd_Unrestricted("+attack2");
        m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::ReleaseSecondaryAttack;
        m_SpecialInfectedWarningNextActionTime = now + secondsToDuration(m_SpecialInfectedWarningSecondaryHoldDuration);
        break;
    case SpecialInfectedWarningActionStep::ReleaseSecondaryAttack:
        m_Game->ClientCmd_Unrestricted("-attack2");
        m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::PressJump;
        m_SpecialInfectedWarningNextActionTime = now + secondsToDuration(m_SpecialInfectedWarningPostAttackDelay);
        break;
    case SpecialInfectedWarningActionStep::PressJump:
        m_Game->ClientCmd_Unrestricted("+jump");
        m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::ReleaseJump;
        m_SpecialInfectedWarningNextActionTime = now + secondsToDuration(m_SpecialInfectedWarningJumpHoldDuration);
        break;
    case SpecialInfectedWarningActionStep::ReleaseJump:
        m_Game->ClientCmd_Unrestricted("-jump");
        ResetSpecialInfectedWarningAction();
        break;
    default:
        break;
    }
}

void VR::ResetSpecialInfectedWarningAction()
{
    m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::None;
    m_SpecialInfectedWarningNextActionTime = {};
    m_SuppressPlayerInput = false;
}

void VR::GetAimLineColor(int& r, int& g, int& b, int& a) const
{
    if (m_SpecialInfectedBlindSpotWarningActive)
    {
        r = m_AimLineWarningColorR;
        g = m_AimLineWarningColorG;
        b = m_AimLineWarningColorB;
    }
    else if (m_SpecialInfectedPreWarningActive)
    {
        r = 0;
        g = 255;
        b = 0;
    }
    else
    {
        r = m_AimLineColorR;
        g = m_AimLineColorG;
        b = m_AimLineColorB;
    }

    a = m_AimLineColorA;
}

Vector VR::GetViewAngle()
{
    return Vector(m_HmdAngAbs.x, m_HmdAngAbs.y, m_HmdAngAbs.z);
}

Vector VR::GetViewOriginLeft()
{
    Vector viewOriginLeft;

    viewOriginLeft = m_HmdPosAbs + (m_HmdForward * (-(m_EyeZ * m_VRScale)));
    viewOriginLeft = viewOriginLeft + (m_HmdRight * (-((m_Ipd * m_IpdScale * m_VRScale) / 2)));

    return viewOriginLeft;
}

Vector VR::GetViewOriginRight()
{
    Vector viewOriginRight;

    viewOriginRight = m_HmdPosAbs + (m_HmdForward * (-(m_EyeZ * m_VRScale)));
    viewOriginRight = viewOriginRight + (m_HmdRight * (m_Ipd * m_IpdScale * m_VRScale) / 2);

    return viewOriginRight;
}


void VR::ResetPosition()
{
    m_CameraAnchor += m_SetupOrigin - m_HmdPosAbs;
    m_HeightOffset += m_SetupOrigin.z - m_HmdPosAbs.z;
}

std::string VR::GetMeleeWeaponName(C_WeaponCSBase* weapon) const
{
    if (!weapon)
        return std::string();

    if (!m_Game || !m_Game->m_Offsets)
    {
        Game::logMsg("[VR] Missing offsets while resolving melee weapon name.");
        return std::string();
    }

    typedef CMeleeWeaponInfoStore* (__thiscall* tGetMeleeWepInfo)(void* thisptr);
    static tGetMeleeWepInfo oGetMeleeWepInfo = nullptr;

    if (!oGetMeleeWepInfo)
        oGetMeleeWepInfo = (tGetMeleeWepInfo)(m_Game->m_Offsets->GetMeleeWeaponInfoClient.address);

    if (!oGetMeleeWepInfo)
        return std::string();

    CMeleeWeaponInfoStore* meleeWepInfo = oGetMeleeWepInfo(weapon);
    if (!meleeWepInfo)
        return std::string();

    std::string meleeName(meleeWepInfo->meleeWeaponName);
    std::transform(meleeName.begin(), meleeName.end(), meleeName.begin(), ::tolower);
    return meleeName;
}

std::string VR::WeaponIdToString(int weaponId) const
{
    static const std::vector<std::string> weaponNames =
    {
        "none",
        "pistol",
        "uzi",
        "pumpshotgun",
        "autoshotgun",
        "m16a1",
        "hunting_rifle",
        "mac10",
        "shotgun_chrome",
        "scar",
        "sniper_military",
        "spas",
        "first_aid_kit",
        "molotov",
        "pipe_bomb",
        "pain_pills",
        "gascan",
        "propane_tank",
        "oxygen_tank",
        "melee",
        "chainsaw",
        "grenade_launcher",
        "ammo_pack",
        "adrenaline",
        "defibrillator",
        "vomitjar",
        "ak47",
        "gnome_chompski",
        "cola_bottles",
        "fireworks_box",
        "incendiary_ammo",
        "frag_ammo",
        "magnum",
        "mp5",
        "sg552",
        "awp",
        "scout",
        "m60",
        "tank_claw",
        "hunter_claw",
        "charger_claw",
        "boomer_claw",
        "smoker_claw",
        "spitter_claw",
        "jockey_claw",
        "machinegun",
        "vomit",
        "splat",
        "pounce",
        "lounge",
        "pull",
        "choke",
        "rock",
        "physics",
        "ammo",
        "upgrade_item"
    };

    size_t index = static_cast<size_t>(weaponId);
    if (index < weaponNames.size())
        return weaponNames[index];

    return std::string();
}

std::string VR::NormalizeViewmodelAdjustKey(const std::string& rawKey) const
{
    const std::string weaponPrefix = "weapon:";
    if (rawKey.rfind(weaponPrefix, 0) == 0)
    {
        std::string weaponIdString = rawKey.substr(weaponPrefix.size());
        if (!weaponIdString.empty() &&
            std::all_of(weaponIdString.begin(), weaponIdString.end(), [](unsigned char c) { return std::isdigit(c); }))
        {
            try
            {
                int weaponId = std::stoi(weaponIdString);
                std::string weaponName = WeaponIdToString(weaponId);
                if (!weaponName.empty())
                    return weaponPrefix + weaponName;
            }
            catch (...)
            {
            }
        }
    }

    return rawKey;
}

std::string VR::BuildViewmodelAdjustKey(C_WeaponCSBase* weapon) const
{
    if (!weapon)
        return "weapon:none";

    C_WeaponCSBase::WeaponID weaponId = weapon->GetWeaponID();

    if (weaponId == C_WeaponCSBase::WeaponID::MELEE)
    {
        std::string meleeName = GetMeleeWeaponName(weapon);
        if (!meleeName.empty())
            return "melee:" + meleeName;

        Game::logMsg("[VR] Failed to resolve melee name, using generic key.");
        return "melee:unknown";
    }

    std::string weaponName = WeaponIdToString(static_cast<int>(weaponId));
    if (!weaponName.empty())
        return "weapon:" + weaponName;

    return "weapon:" + std::to_string(static_cast<int>(weaponId));
}

ViewmodelAdjustment& VR::EnsureViewmodelAdjustment(const std::string& key)
{
    auto [it, inserted] = m_ViewmodelAdjustments.emplace(key, m_DefaultViewmodelAdjust);
    return it->second;
}

void VR::RefreshActiveViewmodelAdjustment(C_BasePlayer* localPlayer)
{
    C_WeaponCSBase* activeWeapon = localPlayer ? (C_WeaponCSBase*)localPlayer->GetActiveWeapon() : nullptr;
    std::string adjustKey = BuildViewmodelAdjustKey(activeWeapon);

    m_CurrentViewmodelKey = adjustKey;

    if (m_LastLoggedViewmodelKey != m_CurrentViewmodelKey)
    {
        m_LastLoggedViewmodelKey = m_CurrentViewmodelKey;
    }

    ViewmodelAdjustment& adjustment = EnsureViewmodelAdjustment(adjustKey);
    m_ViewmodelPosAdjust = adjustment.position;
    m_ViewmodelAngAdjust = adjustment.angle;
}

void VR::LoadViewmodelAdjustments()
{
    m_ViewmodelAdjustments.clear();

    if (m_ViewmodelAdjustmentSavePath.empty())
    {
        Game::logMsg("[VR] No viewmodel adjustment save path configured.");
        return;
    }

    std::ifstream adjustmentStream(m_ViewmodelAdjustmentSavePath);
    if (!adjustmentStream)
    {
        Game::logMsg("[VR] Viewmodel adjustment file missing, will create on save: %s", m_ViewmodelAdjustmentSavePath.c_str());
        return;
    }

    auto ltrim = [](std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        };
    auto rtrim = [](std::string& s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        };
    auto trim = [&](std::string& s) { ltrim(s); rtrim(s); };

    auto parseVector3 = [&](const std::string& raw, const Vector& defaults)->Vector
        {
            Vector result = defaults;
            std::stringstream ss(raw);
            std::string token;
            float* components[3] = { &result.x, &result.y, &result.z };
            int index = 0;

            while (std::getline(ss, token, ',') && index < 3)
            {
                trim(token);
                if (!token.empty())
                {
                    try
                    {
                        *components[index] = std::stof(token);
                    }
                    catch (...)
                    {
                    }
                }
                ++index;
            }

            return result;
        };

    std::string line;
    while (std::getline(adjustmentStream, line))
    {
        trim(line);
        if (line.empty())
            continue;

        size_t eq = line.find('=');
        size_t separator = line.find(';', eq == std::string::npos ? 0 : eq + 1);

        if (eq == std::string::npos || separator == std::string::npos || separator <= eq)
            continue;

        std::string key = line.substr(0, eq);
        trim(key);
        if (key.empty())
            continue;

        std::string posStr = line.substr(eq + 1, separator - eq - 1);
        std::string angStr = line.substr(separator + 1);

        std::string normalizedKey = NormalizeViewmodelAdjustKey(key);
        if (normalizedKey != key)
            Game::logMsg("[VR] Normalized viewmodel adjust key '%s' -> '%s'", key.c_str(), normalizedKey.c_str());

        Vector posAdjust = parseVector3(posStr, m_DefaultViewmodelAdjust.position);
        Vector angAdjustVec = parseVector3(angStr, Vector{ m_DefaultViewmodelAdjust.angle.x, m_DefaultViewmodelAdjust.angle.y, m_DefaultViewmodelAdjust.angle.z });

        m_ViewmodelAdjustments[normalizedKey] = { posAdjust, { angAdjustVec.x, angAdjustVec.y, angAdjustVec.z } };
    }

    Game::logMsg("[VR] Loaded %zu viewmodel adjustment entries from %s", m_ViewmodelAdjustments.size(), m_ViewmodelAdjustmentSavePath.c_str());
    m_ViewmodelAdjustmentsDirty = false;
}

void VR::SaveViewmodelAdjustments()
{
    if (m_ViewmodelAdjustmentSavePath.empty())
    {
        Game::logMsg("[VR] Cannot save viewmodel adjustments: missing path.");
        return;
    }

    std::ofstream adjustmentStream(m_ViewmodelAdjustmentSavePath, std::ios::trunc);
    if (!adjustmentStream)
    {
        Game::logMsg("[VR] Failed to open %s for saving viewmodel adjustments", m_ViewmodelAdjustmentSavePath.c_str());
        return;
    }

    for (const auto& [key, adjustment] : m_ViewmodelAdjustments)
    {
        adjustmentStream << key << '=' << adjustment.position.x << ',' << adjustment.position.y << ',' << adjustment.position.z;
        adjustmentStream << ';' << adjustment.angle.x << ',' << adjustment.angle.y << ',' << adjustment.angle.z << "\n";
    }

    Game::logMsg("[VR] Saved %zu viewmodel adjustment entries to %s", m_ViewmodelAdjustments.size(), m_ViewmodelAdjustmentSavePath.c_str());
    m_ViewmodelAdjustmentsDirty = false;
}

void VR::ParseConfigFile()
{
    std::ifstream configStream("VR\\config.txt");
    if (!configStream) {
        //  Ҳ    ͱ  ֹ   ʱ  Ĭ  ֵ
        return;
    }

    //  򵥵  trim
    auto ltrim = [](std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(),
            [](unsigned char ch) { return !std::isspace(ch); }));
        };
    auto rtrim = [](std::string& s) {
        s.erase(std::find_if(s.rbegin(), s.rend(),
            [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        };
    auto trim = [&](std::string& s) { ltrim(s); rtrim(s); };

    std::unordered_map<std::string, std::string> userConfig;
    std::string line;
    while (std::getline(configStream, line))
    {
        // ȥ  ע  
        size_t cut = std::string::npos;
        size_t p1 = line.find("//");
        size_t p2 = line.find('#');
        size_t p3 = line.find(';');
        if (p1 != std::string::npos) cut = p1;
        if (p2 != std::string::npos) cut = (cut == std::string::npos) ? p2 : std::min(cut, p2);
        if (p3 != std::string::npos) cut = (cut == std::string::npos) ? p3 : std::min(cut, p3);
        if (cut != std::string::npos) line.erase(cut);

        trim(line);
        if (line.empty()) continue;

        //      key=value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key); trim(value);
        if (!key.empty())
            userConfig[key] = value;
    }

    // С   ߣ   Ĭ  ֵ İ ȫ  ȡ
    auto getBool = [&](const char* k, bool defVal)->bool {
        auto it = userConfig.find(k);
        if (it == userConfig.end()) return defVal;
        std::string v = it->second;
        // תСд
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
        if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
        if (v == "0" || v == "false" || v == "off" || v == "no")  return false;
        return defVal;
        };
    auto getFloat = [&](const char* k, float defVal)->float {
        auto it = userConfig.find(k);
        if (it == userConfig.end() || it->second.empty()) return defVal;
        try { return std::stof(it->second); }
        catch (...) { return defVal; }
        };
    auto getInt = [&](const char* k, int defVal)->int {
        auto it = userConfig.find(k);
        if (it == userConfig.end() || it->second.empty()) return defVal;
        try { return std::stoi(it->second); }
        catch (...) { return defVal; }
        };
    auto getColor = [&](const char* k, int defR, int defG, int defB, int defA)->std::array<int, 4> {
        std::array<int, 4> defaults{ defR, defG, defB, defA };
        auto it = userConfig.find(k);
        if (it == userConfig.end())
            return defaults;

        std::array<int, 4> color = defaults;
        std::stringstream ss(it->second);
        std::string token;
        int index = 0;
        while (std::getline(ss, token, ',') && index < 4)
        {
            trim(token);
            if (!token.empty())
            {
                try
                {
                    color[index] = std::clamp(std::stoi(token), 0, 255);
                }
                catch (...)
                {
                    color[index] = defaults[index];
                }
            }
            ++index;
        }

        for (int& component : color)
        {
            component = std::clamp(component, 0, 255);
        }

        return color;
        };
    auto getVector3 = [&](const char* k, const Vector& defVal)->Vector {
        auto it = userConfig.find(k);
        if (it == userConfig.end())
            return defVal;

        Vector result = defVal;
        std::stringstream ss(it->second);
        std::string token;
        float* components[3] = { &result.x, &result.y, &result.z };
        int index = 0;

        while (std::getline(ss, token, ',') && index < 3)
        {
            trim(token);
            if (!token.empty())
            {
                try
                {
                    *components[index] = std::stof(token);
                }
                catch (...)
                {
                }
            }
            ++index;
        }

        return result;
        };

    auto getFloatList = [&](const char* k) -> std::vector<float>
        {
            std::vector<float> values;
            auto it = userConfig.find(k);
            if (it == userConfig.end())
                return values;

            std::stringstream ss(it->second);
            std::string token;
            while (std::getline(ss, token, ','))
            {
                trim(token);
                if (token.empty())
                    continue;

                try
                {
                    values.push_back(std::stof(token));
                }
                catch (...)
                {
                }
            }

            return values;
        };

    auto getString = [&](const char* k, const std::string& defVal)->std::string {
        auto it = userConfig.find(k);
        if (it == userConfig.end())
            return defVal;

        std::string value = it->second;
        trim(value);

        return value.empty() ? defVal : value;
        };

    const std::string injectedCmd = getString("cmd", getString("Cmd", ""));
    if (!injectedCmd.empty())
    {
        m_Game->ClientCmd_Unrestricted(injectedCmd.c_str());
        Game::logMsg("[VR] Executed config cmd: %s", injectedCmd.c_str());
    }

    auto parseVirtualKey = [&](const std::string& rawValue)->std::optional<WORD>
        {
            std::string value = rawValue;
            trim(value);
            if (value.size() < 5) // key:<x>
                return std::nullopt;

            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });

            const std::string prefix = "key:";
            if (value.rfind(prefix, 0) != 0)
                return std::nullopt;

            std::string keyToken = value.substr(prefix.size());
            trim(keyToken);
            if (keyToken.empty())
                return std::nullopt;

            static const std::unordered_map<std::string, WORD> keyLookup = {
                { "space", VK_SPACE }, { "enter", VK_RETURN }, { "return", VK_RETURN },
                { "tab", VK_TAB }, { "escape", VK_ESCAPE }, { "esc", VK_ESCAPE },
                { "shift", VK_SHIFT }, { "lshift", VK_LSHIFT }, { "rshift", VK_RSHIFT },
                { "ctrl", VK_CONTROL }, { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL },
                { "alt", VK_MENU }, { "lalt", VK_LMENU }, { "ralt", VK_RMENU },
                { "backspace", VK_BACK }, { "delete", VK_DELETE }, { "insert", VK_INSERT },
                { "home", VK_HOME }, { "end", VK_END }, { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT },
                { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT }
            };

            auto lookupIt = keyLookup.find(keyToken);
            if (lookupIt != keyLookup.end())
                return lookupIt->second;

            if (keyToken.size() == 1)
            {
                char ch = keyToken[0];
                if (ch >= 'a' && ch <= 'z')
                    return static_cast<WORD>('A' + (ch - 'a'));
                if (ch >= '0' && ch <= '9')
                    return static_cast<WORD>(ch);
            }

            if (keyToken.size() > 1 && keyToken[0] == 'f')
            {
                try
                {
                    int functionIndex = std::stoi(keyToken.substr(1));
                    if (functionIndex >= 1 && functionIndex <= 24)
                        return static_cast<WORD>(VK_F1 + functionIndex - 1);
                }
                catch (...)
                {
                }
            }

            return std::nullopt;
        };

    auto parseCustomActionBinding = [&](const char* key, CustomActionBinding& binding)
        {
            binding.command = getString(key, binding.command);
            binding.holdVirtualKey = false;
            binding.releaseCommand.clear();
            binding.usePressReleaseCommands = false;

            std::string trimmedCommand = binding.command;
            trim(trimmedCommand);
            std::string normalized = trimmedCommand;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return std::tolower(c); });

            const std::string holdPrefix = "hold:";
            if (normalized.rfind(holdPrefix, 0) == 0)
            {
                binding.holdVirtualKey = true;
                trimmedCommand = trimmedCommand.substr(holdPrefix.size());
                trim(trimmedCommand);
                binding.command = trimmedCommand;
            }
            // Custom alias helper: CustomActionXCommand=alias:aliasName:+speed|wait 30|-speed
            // The section after the alias name uses '|' as a stand-in for ';' to avoid config comment stripping.
            const std::string aliasPrefix = "alias:";
            if (normalized.rfind(aliasPrefix, 0) == 0)
            {
                std::string aliasDefinition = trimmedCommand.substr(aliasPrefix.size());
                trim(aliasDefinition);

                size_t separator = aliasDefinition.find(':');
                if (separator != std::string::npos)
                {
                    std::string aliasName = aliasDefinition.substr(0, separator);
                    std::string aliasBody = aliasDefinition.substr(separator + 1);
                    trim(aliasName);
                    trim(aliasBody);

                    if (!aliasName.empty() && !aliasBody.empty())
                    {
                        std::replace(aliasBody.begin(), aliasBody.end(), '|', ';');
                        std::string aliasCommand = "alias " + aliasName + " \"" + aliasBody + "\"";
                        m_Game->ClientCmd_Unrestricted(aliasCommand.c_str());
                        Game::logMsg("[VR] %s defined alias '%s' = %s", key, aliasName.c_str(), aliasBody.c_str());

                        binding.command = aliasName;
                        normalized = aliasName;
                    }
                }
            }

            binding.virtualKey = parseVirtualKey(binding.command);
            if (!binding.command.empty() && binding.virtualKey.has_value())
            {
                Game::logMsg("[VR] %s mapped to virtual key 0x%02X%s", key, *binding.virtualKey,
                    binding.holdVirtualKey ? " (hold)" : "");
            }
            else if (!trimmedCommand.empty() && trimmedCommand[0] == '+' && trimmedCommand.size() > 1)
            {
                binding.usePressReleaseCommands = true;
                binding.releaseCommand = "-" + trimmedCommand.substr(1);
                binding.command = trimmedCommand;
                Game::logMsg("[VR] %s mapped to command press/release: %s / %s", key, binding.command.c_str(), binding.releaseCommand.c_str());
            }
        };

    //  õ ǰ  Ա  ֵ  ΪĬ  ֵ      ʱ ѳ ʼ    
    m_SnapTurning = getBool("SnapTurning", m_SnapTurning);
    m_SnapTurnAngle = getFloat("SnapTurnAngle", m_SnapTurnAngle);
    m_TurnSpeed = getFloat("TurnSpeed", m_TurnSpeed);
    m_InventoryGestureRange = getFloat("InventoryGestureRange", m_InventoryGestureRange);
    m_InventoryChestOffset = getVector3("InventoryChestOffset", m_InventoryChestOffset);
    m_InventoryBackOffset = getVector3("InventoryBackOffset", m_InventoryBackOffset);
    m_InventoryLeftWaistOffset = getVector3("InventoryLeftWaistOffset", m_InventoryLeftWaistOffset);
    m_InventoryRightWaistOffset = getVector3("InventoryRightWaistOffset", m_InventoryRightWaistOffset);
    m_InventoryBodyOriginOffset = getVector3("InventoryBodyOriginOffset", m_InventoryBodyOriginOffset);
    m_InventoryHudMarkerDistance = getFloat("InventoryHudMarkerDistance", m_InventoryHudMarkerDistance);
    m_InventoryHudMarkerUpOffset = getFloat("InventoryHudMarkerUpOffset", m_InventoryHudMarkerUpOffset);
    m_InventoryHudMarkerSeparation = getFloat("InventoryHudMarkerSeparation", m_InventoryHudMarkerSeparation);
    m_DrawInventoryAnchors = getBool("ShowInventoryAnchors", m_DrawInventoryAnchors);
    const auto inventoryColor = getColor("InventoryAnchorColor", m_InventoryAnchorColorR, m_InventoryAnchorColorG, m_InventoryAnchorColorB, m_InventoryAnchorColorA);
    m_InventoryAnchorColorR = inventoryColor[0];
    m_InventoryAnchorColorG = inventoryColor[1];
    m_InventoryAnchorColorB = inventoryColor[2];
    m_InventoryAnchorColorA = inventoryColor[3];
    const std::unordered_map<std::string, vr::VRActionHandle_t*> actionLookup =
    {
        {"jump", &m_ActionJump},
        {"primaryattack", &m_ActionPrimaryAttack},
        {"secondaryattack", &m_ActionSecondaryAttack},
        {"reload", &m_ActionReload},
        {"walk", &m_ActionWalk},
        {"turn", &m_ActionTurn},
        {"use", &m_ActionUse},
        {"nextitem", &m_ActionNextItem},
        {"previtem", &m_ActionPrevItem},
        {"resetposition", &m_ActionResetPosition},
        {"crouch", &m_ActionCrouch},
        {"flashlight", &m_ActionFlashlight},
        {"activatevr", &m_ActionActivateVR},
        {"menuselect", &m_MenuSelect},
        {"menuback", &m_MenuBack},
        {"menuup", &m_MenuUp},
        {"menudown", &m_MenuDown},
        {"menuleft", &m_MenuLeft},
        {"menuright", &m_MenuRight},
        {"spray", &m_Spray},
        {"scoreboard", &m_Scoreboard},
        {"togglehud", &m_ToggleHUD},
        {"pause", &m_Pause}
    };

    auto parseActionCombo = [&](const char* key, const ActionCombo& defaultCombo) -> ActionCombo
        {
            auto it = userConfig.find(key);
            if (it == userConfig.end())
                return defaultCombo;

            std::string rawValue = it->second;
            trim(rawValue);
            std::transform(rawValue.begin(), rawValue.end(), rawValue.begin(), [](unsigned char c) { return std::tolower(c); });

            // Allow disabling a combo by setting it to "false" in the config file.
            if (rawValue == "false")
            {
                Game::logMsg("[VR] %s combo disabled via config", key);
                return ActionCombo{};
            }

            ActionCombo combo{};
            std::stringstream ss(rawValue);
            std::string token;
            int index = 0;
            while (std::getline(ss, token, '+') && index < 2)
            {
                trim(token);
                std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return std::tolower(c); });
                if (token.empty())
                {
                    ++index;
                    continue;
                }

                auto actionIt = actionLookup.find(token);
                if (actionIt != actionLookup.end())
                {
                    if (index == 0)
                        combo.primary = actionIt->second;
                    else
                        combo.secondary = actionIt->second;
                }
                ++index;
            }

            if (!combo.primary || !combo.secondary)
            {
                Game::logMsg("[VR] Invalid %s combo '%s', using defaults.", key, it->second.c_str());
                return defaultCombo;
            }

            return combo;
        };

    m_VoiceRecordCombo = parseActionCombo("VoiceRecordCombo", m_VoiceRecordCombo);
    m_QuickTurnCombo = parseActionCombo("QuickTurnCombo", m_QuickTurnCombo);
    m_ViewmodelAdjustCombo = parseActionCombo("ViewmodelAdjustCombo", m_ViewmodelAdjustCombo);
    parseCustomActionBinding("CustomAction1Command", m_CustomAction1Binding);
    parseCustomActionBinding("CustomAction2Command", m_CustomAction2Binding);
    parseCustomActionBinding("CustomAction3Command", m_CustomAction3Binding);
    parseCustomActionBinding("CustomAction4Command", m_CustomAction4Binding);
    parseCustomActionBinding("CustomAction5Command", m_CustomAction5Binding);

    m_LeftHanded = getBool("LeftHanded", m_LeftHanded);
    m_VRScale = getFloat("VRScale", m_VRScale);
    m_IpdScale = getFloat("IPDScale", m_IpdScale);
    m_ThirdPersonVRCameraOffset = std::max(0.0f, getFloat("ThirdPersonVRCameraOffset", m_ThirdPersonVRCameraOffset));
    m_HideArms = getBool("HideArms", m_HideArms);
    m_HudDistance = getFloat("HudDistance", m_HudDistance);
    m_HudSize = getFloat("HudSize", m_HudSize);
    m_ControllerHudSize = getFloat("ControllerHudSize", m_ControllerHudSize);
    m_ControllerHudYOffset = getFloat("ControllerHudYOffset", m_ControllerHudYOffset);
    m_ControllerHudZOffset = getFloat("ControllerHudZOffset", m_ControllerHudZOffset);
    m_ControllerHudRotation = getFloat("ControllerHudRotation", m_ControllerHudRotation);
    m_ControllerHudXOffset = getFloat("ControllerHudXOffset", m_ControllerHudXOffset);
    m_ControllerHudCut = getBool("ControllerHudCut", m_ControllerHudCut);
    m_HudAlwaysVisible = getBool("HudAlwaysVisible", m_HudAlwaysVisible);
    m_HudToggleState = m_HudAlwaysVisible;
    m_AntiAliasing = std::stol(userConfig["AntiAliasing"]);
    m_FixedHudYOffset = getFloat("FixedHudYOffset", m_FixedHudYOffset);
    m_FixedHudDistanceOffset = getFloat("FixedHudDistanceOffset", m_FixedHudDistanceOffset);
    float controllerSmoothingValue = m_ControllerSmoothing;
    const bool hasControllerSmoothing = userConfig.find("ControllerSmoothing") != userConfig.end();
    const bool hasHeadSmoothing = userConfig.find("HeadSmoothing") != userConfig.end();
    if (hasControllerSmoothing)
        controllerSmoothingValue = getFloat("ControllerSmoothing", controllerSmoothingValue);
    else if (hasHeadSmoothing) // Backward compatibility: old configs used HeadSmoothing
        controllerSmoothingValue = getFloat("HeadSmoothing", controllerSmoothingValue);
    m_ControllerSmoothing = std::clamp(controllerSmoothingValue, 0.0f, 0.99f);

    float headSmoothingValue = m_HeadSmoothing;
    if (hasHeadSmoothing)
        headSmoothingValue = getFloat("HeadSmoothing", headSmoothingValue);
    else
        headSmoothingValue = controllerSmoothingValue; // Match controller smoothing by default
    m_HeadSmoothing = std::clamp(headSmoothingValue, 0.0f, 0.99f);
    m_MotionGestureSwingThreshold = std::max(0.0f, getFloat("MotionGestureSwingThreshold", m_MotionGestureSwingThreshold));
    m_MotionGestureDownSwingThreshold = std::max(0.0f, getFloat("MotionGestureDownSwingThreshold", m_MotionGestureDownSwingThreshold));
    m_MotionGestureJumpThreshold = std::max(0.0f, getFloat("MotionGestureJumpThreshold", m_MotionGestureJumpThreshold));
    m_MotionGestureCooldown = std::max(0.0f, getFloat("MotionGestureCooldown", m_MotionGestureCooldown));
    m_MotionGestureHoldDuration = std::max(0.0f, getFloat("MotionGestureHoldDuration", m_MotionGestureHoldDuration));
    m_ViewmodelAdjustEnabled = getBool("ViewmodelAdjustEnabled", m_ViewmodelAdjustEnabled);
    m_AimLineThickness = std::max(0.0f, getFloat("AimLineThickness", m_AimLineThickness));
    m_AimLineEnabled = getBool("AimLineEnabled", m_AimLineEnabled);
    m_AimLineConfigEnabled = m_AimLineEnabled;
    m_MeleeAimLineEnabled = getBool("MeleeAimLineEnabled", m_MeleeAimLineEnabled);
    auto aimColor = getColor("AimLineColor", m_AimLineColorR, m_AimLineColorG, m_AimLineColorB, m_AimLineColorA);
    m_AimLineColorR = aimColor[0];
    m_AimLineColorG = aimColor[1];
    m_AimLineColorB = aimColor[2];
    m_AimLineColorA = aimColor[3];
    m_AimLinePersistence = std::max(0.0f, getFloat("AimLinePersistence", m_AimLinePersistence));
    m_AimLineFrameDurationMultiplier = std::max(0.0f, getFloat("AimLineFrameDurationMultiplier", m_AimLineFrameDurationMultiplier));
    m_AimLineMaxHz = std::max(0.0f, getFloat("AimLineMaxHz", m_AimLineMaxHz));
    m_ThrowArcLandingOffset = std::max(-10000.0f, std::min(10000.0f, getFloat("ThrowArcLandingOffset", m_ThrowArcLandingOffset)));
    m_ThrowArcMaxHz = std::max(0.0f, getFloat("ThrowArcMaxHz", m_ThrowArcMaxHz));

    // Gun-mounted scope
    m_ScopeEnabled = getBool("ScopeEnabled", m_ScopeEnabled);
    m_ScopeRTTSize = std::clamp(getInt("ScopeRTTSize", m_ScopeRTTSize), 128, 4096);
    m_ScopeFov = std::clamp(getFloat("ScopeFov", m_ScopeFov), 1.0f, 179.0f);
    m_ScopeZNear = std::clamp(getFloat("ScopeZNear", m_ScopeZNear), 0.1f, 64.0f);
    {
        const float configuredScopeFov = m_ScopeFov;
        const auto magnifications = getFloatList("ScopeMagnification");
        if (!magnifications.empty())
        {
            m_ScopeMagnificationOptions.clear();
            for (float mag : magnifications)
            {
                if (std::isfinite(mag))
                    m_ScopeMagnificationOptions.push_back(std::clamp(mag, 1.0f, 179.0f));
            }
        }

        if (m_ScopeMagnificationOptions.empty())
            m_ScopeMagnificationOptions.push_back(m_ScopeFov);

        m_ScopeMagnificationIndex = 0;
        for (size_t i = 0; i < m_ScopeMagnificationOptions.size(); ++i)
        {
            if (fabs(m_ScopeMagnificationOptions[i] - configuredScopeFov) < 0.01f)
            {
                m_ScopeMagnificationIndex = i;
                break;
            }
        }
        m_ScopeFov = std::clamp(m_ScopeMagnificationOptions[m_ScopeMagnificationIndex], 1.0f, 179.0f);
    }
    m_ScopeCameraOffset = getVector3("ScopeCameraOffset", m_ScopeCameraOffset);
    { Vector tmp = getVector3("ScopeCameraAngleOffset", Vector{ m_ScopeCameraAngleOffset.x, m_ScopeCameraAngleOffset.y, m_ScopeCameraAngleOffset.z }); m_ScopeCameraAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z }; }

    m_ScopeOverlayWidthMeters = std::max(0.001f, getFloat("ScopeOverlayWidthMeters", m_ScopeOverlayWidthMeters));
    m_ScopeOverlayXOffset = getFloat("ScopeOverlayXOffset", m_ScopeOverlayXOffset);
    m_ScopeOverlayYOffset = getFloat("ScopeOverlayYOffset", m_ScopeOverlayYOffset);
    m_ScopeOverlayZOffset = getFloat("ScopeOverlayZOffset", m_ScopeOverlayZOffset);
    { Vector tmp = getVector3("ScopeOverlayAngleOffset", Vector{ m_ScopeOverlayAngleOffset.x, m_ScopeOverlayAngleOffset.y, m_ScopeOverlayAngleOffset.z }); m_ScopeOverlayAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z }; }

    m_ScopeOverlayAlwaysVisible = getBool("ScopeOverlayAlwaysVisible", m_ScopeOverlayAlwaysVisible);
    m_ScopeOverlayIdleAlpha = std::clamp(getFloat("ScopeOverlayIdleAlpha", m_ScopeOverlayIdleAlpha), 0.0f, 1.0f);
    m_ScopeRequireLookThrough = getBool("ScopeRequireLookThrough", m_ScopeRequireLookThrough);
    m_ScopeLookThroughDistanceMeters = std::clamp(getFloat("ScopeLookThroughDistanceMeters", m_ScopeLookThroughDistanceMeters), 0.01f, 2.0f);
    m_ScopeLookThroughAngleDeg = std::clamp(getFloat("ScopeLookThroughAngleDeg", m_ScopeLookThroughAngleDeg), 1.0f, 89.0f);

    // Scope stabilization (visual only)
    m_ScopeStabilizationEnabled = getBool("ScopeStabilizationEnabled", m_ScopeStabilizationEnabled);
    m_ScopeStabilizationMinCutoff = std::clamp(getFloat("ScopeStabilizationMinCutoff", m_ScopeStabilizationMinCutoff), 0.05f, 30.0f);
    m_ScopeStabilizationBeta = std::clamp(getFloat("ScopeStabilizationBeta", m_ScopeStabilizationBeta), 0.0f, 5.0f);
    m_ScopeStabilizationDCutoff = std::clamp(getFloat("ScopeStabilizationDCutoff", m_ScopeStabilizationDCutoff), 0.05f, 30.0f);
    if (!m_ScopeStabilizationEnabled)
    {
        m_ScopeStabilizationInit = false;
        m_ScopeStabilizationLastTime = {};
    }

    // Rear mirror
    m_RearMirrorEnabled = getBool("RearMirrorEnabled", m_RearMirrorEnabled);
    m_RearMirrorRTTSize = std::clamp(getInt("RearMirrorRTTSize", m_RearMirrorRTTSize), 128, 4096);
    m_RearMirrorFov = std::clamp(getFloat("RearMirrorFov", m_RearMirrorFov), 1.0f, 179.0f);
    m_RearMirrorZNear = std::clamp(getFloat("RearMirrorZNear", m_RearMirrorZNear), 0.1f, 64.0f);

    m_RearMirrorCameraOffset = getVector3("RearMirrorCameraOffset", m_RearMirrorCameraOffset);
    {
        Vector tmp = getVector3(
            "RearMirrorCameraAngleOffset",
            Vector{ m_RearMirrorCameraAngleOffset.x, m_RearMirrorCameraAngleOffset.y, m_RearMirrorCameraAngleOffset.z });
        m_RearMirrorCameraAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z };
    }

    m_RearMirrorOverlayWidthMeters = std::max(0.01f, getFloat("RearMirrorOverlayWidthMeters", m_RearMirrorOverlayWidthMeters));
    m_RearMirrorOverlayXOffset = getFloat("RearMirrorOverlayXOffset", m_RearMirrorOverlayXOffset);
    m_RearMirrorOverlayYOffset = getFloat("RearMirrorOverlayYOffset", m_RearMirrorOverlayYOffset);
    m_RearMirrorOverlayZOffset = getFloat("RearMirrorOverlayZOffset", m_RearMirrorOverlayZOffset);
    {
        Vector tmp = getVector3(
            "RearMirrorOverlayAngleOffset",
            Vector{ m_RearMirrorOverlayAngleOffset.x, m_RearMirrorOverlayAngleOffset.y, m_RearMirrorOverlayAngleOffset.z });
        m_RearMirrorOverlayAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z };
    }
    m_RearMirrorAlpha = std::clamp(getFloat("RearMirrorAlpha", m_RearMirrorAlpha), 0.0f, 1.0f);

    m_ForceNonVRServerMovement = getBool("ForceNonVRServerMovement", m_ForceNonVRServerMovement);

    // Non-VR server melee feel tuning (ForceNonVRServerMovement=true only)
    m_NonVRMeleeSwingThreshold = std::max(0.0f, getFloat("NonVRMeleeSwingThreshold", m_NonVRMeleeSwingThreshold));
    m_NonVRMeleeSwingCooldown = std::max(0.0f, getFloat("NonVRMeleeSwingCooldown", m_NonVRMeleeSwingCooldown));
    m_NonVRMeleeHoldTime = std::max(0.0f, getFloat("NonVRMeleeHoldTime", m_NonVRMeleeHoldTime));
    m_NonVRMeleeAimLockTime = std::max(0.0f, getFloat("NonVRMeleeAimLockTime", m_NonVRMeleeAimLockTime));
    m_NonVRMeleeHysteresis = std::clamp(getFloat("NonVRMeleeHysteresis", m_NonVRMeleeHysteresis), 0.1f, 0.95f);
    m_NonVRMeleeAngVelThreshold = std::max(0.0f, getFloat("NonVRMeleeAngVelThreshold", m_NonVRMeleeAngVelThreshold));
    m_NonVRMeleeSwingDirBlend = std::clamp(getFloat("NonVRMeleeSwingDirBlend", m_NonVRMeleeSwingDirBlend), 0.0f, 1.0f);
    m_RequireSecondaryAttackForItemSwitch = getBool("RequireSecondaryAttackForItemSwitch", m_RequireSecondaryAttackForItemSwitch);
    m_SpecialInfectedWarningActionEnabled = getBool("SpecialInfectedAutoEvade", m_SpecialInfectedWarningActionEnabled);
    m_SpecialInfectedArrowEnabled = getBool("SpecialInfectedArrowEnabled", m_SpecialInfectedArrowEnabled);
    m_SpecialInfectedDebug = getBool("SpecialInfectedDebug", m_SpecialInfectedDebug);
    m_SpecialInfectedArrowSize = std::max(0.0f, getFloat("SpecialInfectedArrowSize", m_SpecialInfectedArrowSize));
    m_SpecialInfectedArrowHeight = std::max(0.0f, getFloat("SpecialInfectedArrowHeight", m_SpecialInfectedArrowHeight));
    m_SpecialInfectedArrowThickness = std::max(0.0f, getFloat("SpecialInfectedArrowThickness", m_SpecialInfectedArrowThickness));
    m_SpecialInfectedBlindSpotDistance = std::max(0.0f, getFloat("SpecialInfectedBlindSpotDistance", m_SpecialInfectedBlindSpotDistance));
    m_SpecialInfectedPreWarningAutoAimConfigEnabled = getBool("SpecialInfectedPreWarningAutoAimEnabled", m_SpecialInfectedPreWarningAutoAimConfigEnabled);
    m_SpecialInfectedPreWarningEvadeDistance = std::max(0.0f, getFloat("SpecialInfectedPreWarningEvadeDistance", m_SpecialInfectedPreWarningEvadeDistance));
    if (!m_SpecialInfectedPreWarningAutoAimConfigEnabled)
        m_SpecialInfectedPreWarningAutoAimEnabled = false;
    m_SpecialInfectedPreWarningDistance = std::max(0.0f, getFloat("SpecialInfectedPreWarningDistance", m_SpecialInfectedPreWarningDistance));
    m_SpecialInfectedPreWarningTargetUpdateInterval = std::max(0.0f, getFloat("SpecialInfectedPreWarningTargetUpdateInterval", m_SpecialInfectedPreWarningTargetUpdateInterval));
    m_SpecialInfectedOverlayMaxHz = std::max(0.0f, getFloat("SpecialInfectedOverlayMaxHz", m_SpecialInfectedOverlayMaxHz));
    m_SpecialInfectedTraceMaxHz = std::max(0.0f, getFloat("SpecialInfectedTraceMaxHz", m_SpecialInfectedTraceMaxHz));
    const float preWarningAimAngle = getFloat("SpecialInfectedPreWarningAimAngle", m_SpecialInfectedPreWarningAimAngle);
    m_SpecialInfectedPreWarningAimAngle = m_SpecialInfectedDebug
        ? std::max(0.0f, preWarningAimAngle)
        : std::clamp(preWarningAimAngle, 0.0f, 10.0f);

    const float aimSnapDistance = getFloat("SpecialInfectedPreWarningAimSnapDistance", m_SpecialInfectedPreWarningAimSnapDistance);
    m_SpecialInfectedPreWarningAimSnapDistance = m_SpecialInfectedDebug
        ? std::max(0.0f, aimSnapDistance)
        : std::clamp(aimSnapDistance, 0.0f, 20.0f);

    const float releaseDistance = getFloat("SpecialInfectedPreWarningAimReleaseDistance", m_SpecialInfectedPreWarningAimReleaseDistance);
    m_SpecialInfectedPreWarningAimReleaseDistance = m_SpecialInfectedDebug
        ? std::max(m_SpecialInfectedPreWarningAimSnapDistance, std::max(0.0f, releaseDistance))
        : std::clamp(std::max(m_SpecialInfectedPreWarningAimSnapDistance, std::max(0.0f, releaseDistance)), 0.0f, 30.0f);

    const float autoAimLerp = getFloat("SpecialInfectedAutoAimLerp", m_SpecialInfectedAutoAimLerp);
    m_SpecialInfectedAutoAimLerp = m_SpecialInfectedDebug
        ? std::max(0.0f, autoAimLerp)
        : std::clamp(autoAimLerp, 0.0f, 0.3f);

    const float autoAimCooldown = getFloat("SpecialInfectedAutoAimCooldown", m_SpecialInfectedAutoAimCooldown);
    m_SpecialInfectedAutoAimCooldown = m_SpecialInfectedDebug
        ? std::max(0.0f, autoAimCooldown)
        : std::max(0.5f, autoAimCooldown);
    m_SpecialInfectedWarningSecondaryHoldDuration = std::max(0.0f, getFloat("SpecialInfectedWarningSecondaryHoldDuration", m_SpecialInfectedWarningSecondaryHoldDuration));
    m_SpecialInfectedWarningPostAttackDelay = std::max(0.0f, getFloat("SpecialInfectedWarningPostAttackDelay", m_SpecialInfectedWarningPostAttackDelay));
    m_SpecialInfectedWarningJumpHoldDuration = std::max(0.0f, getFloat("SpecialInfectedWarningJumpHoldDuration", m_SpecialInfectedWarningJumpHoldDuration));
    auto specialInfectedArrowColor = getColor("SpecialInfectedArrowColor", m_SpecialInfectedArrowDefaultColor.r, m_SpecialInfectedArrowDefaultColor.g, m_SpecialInfectedArrowDefaultColor.b, 255);
    const bool hasGlobalArrowColor = userConfig.find("SpecialInfectedArrowColor") != userConfig.end();
    m_SpecialInfectedArrowDefaultColor.r = specialInfectedArrowColor[0];
    m_SpecialInfectedArrowDefaultColor.g = specialInfectedArrowColor[1];
    m_SpecialInfectedArrowDefaultColor.b = specialInfectedArrowColor[2];

    if (hasGlobalArrowColor)
    {
        for (auto& color : m_SpecialInfectedArrowColors)
        {
            color.r = m_SpecialInfectedArrowDefaultColor.r;
            color.g = m_SpecialInfectedArrowDefaultColor.g;
            color.b = m_SpecialInfectedArrowDefaultColor.b;
        }
    }

    static const std::array<std::pair<SpecialInfectedType, const char*>, 8> arrowColorConfigKeys =
    {
        std::make_pair(SpecialInfectedType::Boomer, "Boomer"),
        std::make_pair(SpecialInfectedType::Smoker, "Smoker"),
        std::make_pair(SpecialInfectedType::Hunter, "Hunter"),
        std::make_pair(SpecialInfectedType::Spitter, "Spitter"),
        std::make_pair(SpecialInfectedType::Jockey, "Jockey"),
        std::make_pair(SpecialInfectedType::Charger, "Charger"),
        std::make_pair(SpecialInfectedType::Tank, "Tank"),
        std::make_pair(SpecialInfectedType::Witch, "Witch")
    };

    for (const auto& [type, suffix] : arrowColorConfigKeys)
    {
        const size_t typeIndex = static_cast<size_t>(type);
        if (typeIndex >= m_SpecialInfectedArrowColors.size())
            continue;

        std::string key = std::string("SpecialInfectedArrowColor") + suffix;
        auto color = getColor(key.c_str(), m_SpecialInfectedArrowColors[typeIndex].r, m_SpecialInfectedArrowColors[typeIndex].g, m_SpecialInfectedArrowColors[typeIndex].b, 255);
        m_SpecialInfectedArrowColors[typeIndex].r = color[0];
        m_SpecialInfectedArrowColors[typeIndex].g = color[1];
        m_SpecialInfectedArrowColors[typeIndex].b = color[2];
    }

    static const std::array<std::pair<SpecialInfectedType, const char*>, 8> preWarningOffsetConfigKeys =
    {
        std::make_pair(SpecialInfectedType::Boomer, "Boomer"),
        std::make_pair(SpecialInfectedType::Smoker, "Smoker"),
        std::make_pair(SpecialInfectedType::Hunter, "Hunter"),
        std::make_pair(SpecialInfectedType::Spitter, "Spitter"),
        std::make_pair(SpecialInfectedType::Jockey, "Jockey"),
        std::make_pair(SpecialInfectedType::Charger, "Charger"),
        std::make_pair(SpecialInfectedType::Tank, "Tank"),
        std::make_pair(SpecialInfectedType::Witch, "Witch")
    };

    for (const auto& [type, suffix] : preWarningOffsetConfigKeys)
    {
        const size_t typeIndex = static_cast<size_t>(type);
        if (typeIndex >= m_SpecialInfectedPreWarningAimOffsets.size())
            continue;

        std::string key = std::string("SpecialInfectedPreWarningAimOffset") + suffix;
        m_SpecialInfectedPreWarningAimOffsets[typeIndex] = getVector3(key.c_str(), m_SpecialInfectedPreWarningAimOffsets[typeIndex]);
    }
}

void VR::WaitForConfigUpdate()
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char configDir[MAX_STR_LEN];
    sprintf_s(configDir, MAX_STR_LEN, "%s\\VR\\", currentDir);
    HANDLE fileChangeHandle = FindFirstChangeNotificationA(configDir, false, FILE_NOTIFY_CHANGE_LAST_WRITE);

    FILETIME configLastModified{};
    while (1)
    {
        WIN32_FILE_ATTRIBUTE_DATA fileAttributes{};
        if (!GetFileAttributesExA("VR\\config.txt", GetFileExInfoStandard, &fileAttributes))
        {
            m_Game->errorMsg("config.txt not found.");
            return;
        }

        if (CompareFileTime(&fileAttributes.ftLastWriteTime, &configLastModified) != 0)
        {
            configLastModified = fileAttributes.ftLastWriteTime;
            try
            {
                ParseConfigFile();
            }
            catch (const std::invalid_argument&)
            {
                m_Game->errorMsg("Failed to parse config.txt");
            }
        }

        FindNextChangeNotification(fileChangeHandle);
        WaitForSingleObject(fileChangeHandle, INFINITE);
        Sleep(100); // Sometimes the thread tries to read config.txt before it's finished writing
    }
}
