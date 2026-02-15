

#include "vr.h"
#include <Windows.h>
#include "sdk.h"
#include "game.h"
#include "hooks.h"
#include "usercmd.h"
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
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <d3d9_vr.h>

namespace
{
    // NOTE: “被控放行”要宁可保守：只在「确实是控制者本人」且「目标非常贴近队友」时才放行。
    // Used by VR::UpdateFriendlyFireAimHit().
    constexpr float kAllowThroughControlledTeammateMaxDist = 64.0f; // units (conservative)
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
    m_Overlay->CreateOverlay("HUDOverlayKey", "HUDOverlay", &m_HUDHandle);

    // Gun-mounted scope lens overlay (render-to-texture)
    m_Overlay->CreateOverlay("ScopeOverlayKey", "ScopeOverlay", &m_ScopeHandle);
    m_Overlay->CreateOverlay("RearMirrorOverlayKey", "RearMirrorOverlay", &m_RearMirrorHandle);

    m_Overlay->SetOverlayInputMethod(m_MainMenuHandle, vr::VROverlayInputMethod_Mouse);
    m_Overlay->SetOverlayInputMethod(m_HUDHandle, vr::VROverlayInputMethod_Mouse);

    // Scope overlay is purely visual
    m_Overlay->SetOverlayInputMethod(m_ScopeHandle, vr::VROverlayInputMethod_None);
    m_Overlay->SetOverlayInputMethod(m_RearMirrorHandle, vr::VROverlayInputMethod_None);
    m_Overlay->SetOverlayFlag(m_ScopeHandle, vr::VROverlayFlags_IgnoreTextureAlpha, true);
    m_Overlay->SetOverlayFlag(m_RearMirrorHandle, vr::VROverlayFlags_IgnoreTextureAlpha, true);


    m_Overlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    m_Overlay->SetOverlayFlag(m_HUDHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);

    int windowWidth, windowHeight;
    m_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

    const vr::HmdVector2_t mouseScaleHUD = { windowWidth, windowHeight };
    m_Overlay->SetOverlayMouseScale(m_HUDHandle, &mouseScaleHUD);

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
    m_Input->GetActionHandle("/actions/main/in/InventoryGripLeft", &m_ActionInventoryGripLeft);
    m_Input->GetActionHandle("/actions/main/in/InventoryGripRight", &m_ActionInventoryGripRight);
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
    // Aim-line friendly-fire guard toggle (bindable in SteamVR)
    m_Input->GetActionHandle("/actions/main/in/FriendlyFireBlockToggle", &m_ActionFriendlyFireBlockToggle);
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
    // mat_queue_mode=2 can call Update() re-entrantly / from multiple threads.
   // OpenVR expects Submit() and WaitGetPoses() to happen once per frame boundary.
    struct UpdateReentryGuard
    {
        std::atomic_flag& flag;
        bool acquired;
        explicit UpdateReentryGuard(std::atomic_flag& f)
            : flag(f), acquired(!flag.test_and_set(std::memory_order_acquire)) {
        }
        ~UpdateReentryGuard() { if (acquired) flag.clear(std::memory_order_release); }
    } updateGuard(m_UpdateInProgress);

    if (!updateGuard.acquired)
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

    // SteamVR expects a steady "heartbeat" of Submit() calls.
    // With mat_queue_mode=2, Update() can run before the RenderView hook flags m_RenderedNewFrame.
    // If we skip Submit in that case, SteamVR may treat the app as idle/occluded and downshift to ~30fps.
    // So: always submit once per Update(), and let SubmitVRTextures() decide whether to submit the new eye
    // textures (m_RenderedNewFrame) or re-submit the last ones.

    // Debug (rate-limited): detect long streaks with no new stereo frame while in-game.
    {
        static uint64_t s_lastNoFrameLogMs = 0;
        static int s_noNewStereoFrames = 0;
        const bool inGameNow = (m_Game && m_Game->m_EngineClient) ? m_Game->m_EngineClient->IsInGame() : false;
        const bool hadNewFrame = m_RenderedNewFrame;
        if (inGameNow && !hadNewFrame) ++s_noNewStereoFrames; else s_noNewStereoFrames = 0;
        const uint64_t nowMs2 = GetTickCount64();
        if (s_noNewStereoFrames >= 90 && (nowMs2 - s_lastNoFrameLogMs) > 5000)
        {
            Game::logMsg("[VR] No new stereo frame for %d Update() calls (in-game). Still submitting to keep SteamVR alive.", s_noNewStereoFrames);
            s_lastNoFrameLogMs = nowMs2;
        }
    }

    SubmitVRTextures();
   // Periodically re-sync fps_max in case the SteamVR refresh rate changes (e.g., 72/80/90/120).
   // We intentionally keep this lightweight and rate-limited.
   if (m_HudMatQueueModeLinkEnabled && m_Game && m_Game->m_MaterialSystem && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame())
   {
       static uint64_t s_lastFpsSyncMs = 0;
       const uint64_t nowMs = GetTickCount64();
       if ((nowMs - s_lastFpsSyncMs) > 2000)
       {
           s_lastFpsSyncMs = nowMs;
           const MaterialThreadMode_t threadMode = m_Game->m_MaterialSystem->GetThreadMode();
           const int mqm = (threadMode == MATERIAL_QUEUED_THREADED) ? 2 : 1;
           ApplyLinkedFpsMaxForMatQueueMode(mqm);
       }
   }
    bool posesValid = UpdatePosesAndActions();

    if (!posesValid)
    {
        // Continue using the last known poses so smoothing and aim helpers stay active.
    }

    // Auto ResetPosition shortly after a level finishes loading.

    {
        const bool inGame = m_Game->m_EngineClient->IsInGame();
        if (!inGame)
        {
            m_AutoResetPositionPending = false;
            m_AutoResetHadLocalPlayerPrev = false;
        }
        else
        {
            int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
            C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
            const bool hasLocalPlayer = (localPlayer != nullptr);

            if (hasLocalPlayer && !m_AutoResetHadLocalPlayerPrev && m_AutoResetPositionAfterLoadSeconds > 0.0f)
            {
                m_AutoResetPositionPending = true;
                const auto now = std::chrono::steady_clock::now();
                const int ms = (int)std::max(0.0f, m_AutoResetPositionAfterLoadSeconds * 1000.0f);
                m_AutoResetPositionDueTime = now + std::chrono::milliseconds(ms);
            }

            m_AutoResetHadLocalPlayerPrev = hasLocalPlayer;

            if (m_AutoResetPositionPending && hasLocalPlayer)
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= m_AutoResetPositionDueTime)
                {
                    ResetPosition();
                    m_AutoResetPositionPending = false;
                }
            }
        }
    }

    UpdateTrackingOncePerCompositorFrame();

    if (m_Game->m_VguiSurface->IsCursorVisible())
        ProcessMenuInput();
    else
        ProcessInput();
}

bool VR::GetWalkAxis(float& x, float& y) {
    vr::InputAnalogActionData_t d;
    bool hasAxis = false;
    if (GetAnalogActionData(m_ActionWalk, d)) {
        x = d.x;
        y = d.y;
        hasAxis = true;
    }
    else
    {
        x = y = 0.0f;
    }
    return hasAxis;
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
    // HUD alpha correctness (PC build):
    // Backbuffer can be BGRX8888 (no alpha). Overlay compositing is sensitive to alpha semantics,
    // so force an alpha-capable format for the HUD RT when needed.
    ImageFormat hudFmt = m_Game->m_MaterialSystem->GetBackBufferFormat();
    if (hudFmt == IMAGE_FORMAT_BGRX8888)
        hudFmt = IMAGE_FORMAT_BGRA8888;

    m_HUDTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("vrHUD", windowWidth, windowHeight, RT_SIZE_NO_CHANGE, hudFmt, MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
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

    // Guard against VRCompositorError_AlreadySubmitted (108): the compositor rejects submitting
    // the same eye twice within a single compositor frame (before the frame index advances).
    // Some Source engine paths (notably mat_queue_mode=2) can call into Update/Submit multiple
    // times per compositor frame, especially during no-present/menu periods.
    bool hasFrameTiming = false;
    uint32_t frameIndex = 0;
    {
        vr::Compositor_FrameTiming timing{};
        timing.m_nSize = sizeof(timing);
        if (m_Compositor->GetFrameTiming(&timing, 0))
        {
            hasFrameTiming = true;
            frameIndex = timing.m_nFrameIndex;

            if (!m_HasSubmitFrameIndex || frameIndex != m_LastSubmitFrameIndex)
            {
                m_LastSubmitFrameIndex = frameIndex;
                m_SubmittedLeftForFrame = false;
                m_SubmittedRightForFrame = false;
                m_HasSubmitFrameIndex = true;
            }

            // If both eyes were already submitted for this compositor frame, do nothing.
            if (m_SubmittedLeftForFrame && m_SubmittedRightForFrame)
                return;
        }
    }

    bool submittedAnyThisCall = false;
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
            if (hasFrameTiming)
            {
                bool& eyeSubmitted = (eye == vr::Eye_Left) ? m_SubmittedLeftForFrame : m_SubmittedRightForFrame;
                if (eyeSubmitted)
                    return true;
            }

            vr::EVRCompositorError submitError = m_Compositor->Submit(eye, texture, bounds, vr::Submit_Default);
            if (submitError != vr::VRCompositorError_None)
            {
                LogCompositorError("Submit", submitError);
                return false;
            }
            submittedAnyThisCall = true;
            if (hasFrameTiming)
            {
                bool& eyeSubmitted = (eye == vr::Eye_Left) ? m_SubmittedLeftForFrame : m_SubmittedRightForFrame;
                eyeSubmitted = true;
            }
            return true;
        };

    auto hideHudOverlays = [&]()
        {
            vr::VROverlay()->HideOverlay(m_HUDHandle);
        };

    // HUD overlay bounds need to account for cases where the underlying render target
    // is larger than the actual window/backbuffer size (common with some mat_queue_mode=2 paths).
    // In that situation VGUI will draw into the top-left region, leaving the rest blank.
    // Scale the texture bounds so overlays sample only the region that VGUI actually filled.
    int windowWidth = 0, windowHeight = 0;
    if (m_Game && m_Game->m_MaterialSystem)
    {
        if (IMatRenderContext* rc = m_Game->m_MaterialSystem->GetRenderContext())
            rc->GetWindowSize(windowWidth, windowHeight);
    }

    float uMax = 1.0f;
    float vMax = 1.0f;
    if (m_HUDTexture && windowWidth > 0 && windowHeight > 0)
    {
        const int texW = m_HUDTexture->GetActualWidth();
        const int texH = m_HUDTexture->GetActualHeight();
        if (texW > 0 && texH > 0)
        {
            uMax = std::min(1.0f, static_cast<float>(windowWidth) / static_cast<float>(texW));
            vMax = std::min(1.0f, static_cast<float>(windowHeight) / static_cast<float>(texH));
        }
    }

    const vr::VRTextureBounds_t hudBounds{ 0.0f, 0.0f, uMax, vMax };


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
            vr::VRTextureBounds_t bounds{ 0.0f, 0.0f, 1.0f, 1.0f };
            if (m_RearMirrorFlipHorizontal)
                std::swap(bounds.uMin, bounds.uMax);
            vr::VROverlay()->SetOverlayTextureBounds(overlay, &bounds);
            vr::VROverlay()->SetOverlayTexture(overlay, &m_VKRearMirror.m_VRTexture);
        };
    const bool inGame = (m_Game && m_Game->m_EngineClient) ? m_Game->m_EngineClient->IsInGame() : false;

    // Backbuffer/menu overlay fallback (desktop/window image) can cause severe stutter when RenderFrame falls back.
    // However, auto-disabling it globally is undesirable: it should only be forced off for mat_queue_mode=2
    // (queued + threaded rendering), where backbuffer transitions are also more prone to deadlock.
    bool matQueueMode2 = false;
    if (m_Game && m_Game->m_MaterialSystem)
    {
        const auto mode = m_Game->m_MaterialSystem->GetThreadMode();
        matQueueMode2 = (mode == MATERIAL_QUEUED_THREADED);
    }
    // In mat_queue_mode=2, keep the fallback disabled while in-game, and for a short grace window
    // after leaving a map to avoid freeze/deadlock during the transition back to the main menu.
    const uint64_t nowMs = GetTickCount64();
    if (matQueueMode2)
    {
        if (inGame)
            m_BackbufferFallbackLatchUntilMs = nowMs + 2000; // 2s grace
        m_BackbufferFallbackLatchedOff = inGame || (nowMs < m_BackbufferFallbackLatchUntilMs);
    }
    else
    {
        m_BackbufferFallbackLatchUntilMs = 0;
        m_BackbufferFallbackLatchedOff = false;
    }
    //     ֡û       ݣ    ߲˵ /Overlay ·  
    if (!m_RenderedNewFrame)
    {
        bool leftOk = false;
        bool rightOk = false;
        bool successfulSubmit = false;
        const bool disableBackbufferFallbackEffective =
            m_DisableBackbufferOverlayFallback || m_BackbufferFallbackLatchedOff;
        if (!m_BlankTexture)
            CreateVRTextures();

        if (!vr::VROverlay()->IsOverlayVisible(m_MainMenuHandle))
            RepositionOverlays();

        if (disableBackbufferFallbackEffective)
        {
            // Do NOT feed desktop/window backbuffer into VR. Keep stable by re-submitting last eye textures.
            vr::VROverlay()->HideOverlay(m_MainMenuHandle);
            hideHudOverlays();
            vr::VROverlay()->HideOverlay(m_ScopeHandle);
            vr::VROverlay()->HideOverlay(m_RearMirrorHandle);

            if (inGame)
            {
                // In-game: keep VR stable by re-submitting the last eye textures (no backbuffer overlay).
                leftOk = submitEye(vr::Eye_Left, &m_VKLeftEye.m_VRTexture, &(m_TextureBounds)[0]);
                rightOk = submitEye(vr::Eye_Right, &m_VKRightEye.m_VRTexture, &(m_TextureBounds)[1]);
            }
            else
            {
                // Out of game: submit blank textures so we don't show stale in-game frames.
                leftOk = submitEye(vr::Eye_Left, &m_VKBlankTexture.m_VRTexture, nullptr);
                rightOk = submitEye(vr::Eye_Right, &m_VKBlankTexture.m_VRTexture, nullptr);
            }
        }
        else
        {
            // Original behavior: show menu/backbuffer overlay when no new frame is rendered.
            // Refresh the shared backbuffer handle each time, since DXVK rotates swapchain images.
            if (g_D3DVR9)
                g_D3DVR9->GetBackBufferData(&m_VKBackBuffer);
            vr::VROverlay()->SetOverlayTexture(m_MainMenuHandle, &m_VKBackBuffer.m_VRTexture);
            vr::VROverlay()->ShowOverlay(m_MainMenuHandle);
            hideHudOverlays();
            vr::VROverlay()->HideOverlay(m_ScopeHandle);
            vr::VROverlay()->HideOverlay(m_RearMirrorHandle);
            if (!inGame)
            {
                leftOk = submitEye(vr::Eye_Left, &m_VKBlankTexture.m_VRTexture, nullptr);
                rightOk = submitEye(vr::Eye_Right, &m_VKBlankTexture.m_VRTexture, nullptr);
            }
        }

        if (!m_Game->m_EngineClient->IsInGame())
        {
            leftOk = submitEye(vr::Eye_Left, &m_VKBlankTexture.m_VRTexture, nullptr);
            rightOk = submitEye(vr::Eye_Right, &m_VKBlankTexture.m_VRTexture, nullptr);
        }
        successfulSubmit = leftOk && rightOk;
        if (successfulSubmit && m_CompositorExplicitTiming)
        {
            m_CompositorNeedsHandoff = true;
            FinishFrame();
        }

        return;
    }


    vr::VROverlay()->HideOverlay(m_MainMenuHandle);
    if (m_DisableHudRendering)
    {
        vr::VROverlay()->HideOverlay(m_HUDHandle);
    }
    else
    {
        applyHudTexture(m_HUDHandle, hudBounds);
        if (m_Game->m_VguiSurface->IsCursorVisible())
        {
            vr::VROverlay()->ShowOverlay(m_HUDHandle);
            if (inGame && m_HudMatQueueModeLinkEnabled)
                RequestMatQueueMode(1);
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

        // Mouse mode: the "gun" may not be a tracked controller.
        if (m_MouseModeEnabled)
            UpdateScopeOverlayTransform();

        const bool canShowScope = ShouldRenderScope() && (m_MouseModeEnabled || gunControllerIndex != vr::k_unTrackedDeviceIndexInvalid);
        if (canShowScope)
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

        // Body-anchored rear mirror: update absolute transform every frame.
        UpdateRearMirrorOverlayTransform();

        // Keep the "special warning" enlarge hint bounded even if the mirror RTT pass
        // is temporarily not running (e.g., pop-up mode).
        if (m_RearMirrorSpecialWarningDistance > 0.0f && m_RearMirrorSpecialEnlargeActive)
        {
            if (m_LastRearMirrorSpecialSeenTime.time_since_epoch().count() == 0)
            {
                m_RearMirrorSpecialEnlargeActive = false;
            }
            else
            {
                const auto now = std::chrono::steady_clock::now();
                const float elapsed = std::chrono::duration<float>(now - m_LastRearMirrorSpecialSeenTime).count();
                if (elapsed > m_RearMirrorSpecialEnlargeHoldSeconds)
                    m_RearMirrorSpecialEnlargeActive = false;
            }
        }

        const auto shouldHideRearMirrorDueToAimLine = [&]() -> bool
            {
                if (!m_RearMirrorHideWhenAimLineHits)
                    return false;

                const auto now = std::chrono::steady_clock::now();
                if (now < m_RearMirrorAimLineHideUntil)
                    return true;

                // Build an aim ray consistent with UpdateAimingLaser(), even if the aim line isn't rendered this frame.
                Vector dir = m_RightControllerForward;
                if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
                    dir = m_RightControllerForwardUnforced;
                if (dir.IsZero())
                    dir = m_LastAimDirection;
                if (dir.IsZero())
                    return false;
                VectorNormalize(dir);

                Vector rayStart = m_RightControllerPosAbs;
                // Keep non-3P codepath identical to legacy behavior; only use the new render-center delta in 3P.
                Vector camDelta = m_IsThirdPersonCamera
                    ? (m_ThirdPersonRenderCenter - m_SetupOrigin)
                    : (m_ThirdPersonViewOrigin - m_SetupOrigin);
                if (m_IsThirdPersonCamera && camDelta.LengthSqr() > (5.0f * 5.0f))
                    rayStart += camDelta;

                rayStart = rayStart + dir * 2.0f;
                Vector rayEnd = rayStart + dir * 8192.0f;

                // Mirror quad in world space (Source units), matching UpdateRearMirrorOverlayTransform().
                Vector fwd = m_HmdForward;
                fwd.z = 0.0f;
                if (VectorNormalize(fwd) == 0.0f)
                    fwd = { 1.0f, 0.0f, 0.0f };

                const Vector up = { 0.0f, 0.0f, 1.0f };
                Vector right = CrossProduct(fwd, up);
                if (VectorNormalize(right) == 0.0f)
                    right = { 0.0f, -1.0f, 0.0f };

                const Vector back = { -fwd.x, -fwd.y, -fwd.z };

                const Vector bodyOrigin =
                    m_HmdPosAbs
                    + (fwd * (m_InventoryBodyOriginOffset.x * m_VRScale))
                    + (right * (m_InventoryBodyOriginOffset.y * m_VRScale))
                    + (up * (m_InventoryBodyOriginOffset.z * m_VRScale));

                const Vector mirrorCenter =
                    bodyOrigin
                    + (fwd * (m_RearMirrorOverlayXOffset * m_VRScale))
                    + (right * (m_RearMirrorOverlayYOffset * m_VRScale))
                    + (up * (m_RearMirrorOverlayZOffset * m_VRScale));

                const float deg2rad = 3.14159265358979323846f / 180.0f;
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

                auto mul33 = [](const float a[3][3], const float b[3][3], float out[3][3])
                    {
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c)
                                out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
                    };

                float RyRx[3][3];
                float Roff[3][3];
                mul33(Ry, Rx, RyRx);
                mul33(Rz, RyRx, Roff);

                // Parent yaw-only basis (columns: right, up, back)
                const float B[3][3] = {
                    { right.x, up.x, back.x },
                    { right.y, up.y, back.y },
                    { right.z, up.z, back.z }
                };

                float Rworld[3][3];
                mul33(B, Roff, Rworld);

                Vector axisX = { Rworld[0][0], Rworld[1][0], Rworld[2][0] };
                Vector axisY = { Rworld[0][1], Rworld[1][1], Rworld[2][1] };
                Vector axisZ = { Rworld[0][2], Rworld[1][2], Rworld[2][2] };
                if (axisX.IsZero() || axisY.IsZero() || axisZ.IsZero())
                    return false;
                VectorNormalize(axisX);
                VectorNormalize(axisY);
                VectorNormalize(axisZ);

                float mirrorWidthMeters = std::max(0.01f, m_RearMirrorOverlayWidthMeters);
                if (m_RearMirrorSpecialWarningDistance > 0.0f && m_RearMirrorSpecialEnlargeActive)
                    mirrorWidthMeters *= 2.0f;

                const float halfExtent = 0.5f * mirrorWidthMeters * m_VRScale;
                const float pad = 0.01f * m_VRScale; // ~1cm padding to reduce flicker on edges
                const float halfX = halfExtent + pad;
                const float halfY = halfExtent + pad;

                const Vector seg = rayEnd - rayStart;
                const float denom = DotProduct(seg, axisZ);
                if (fabsf(denom) < 1e-6f)
                    return false;

                const float t = DotProduct(mirrorCenter - rayStart, axisZ) / denom;
                if (t < 0.0f || t > 1.0f)
                    return false;

                const Vector hit = rayStart + seg * t;
                const Vector d = hit - mirrorCenter;
                const float lx = DotProduct(d, axisX);
                const float ly = DotProduct(d, axisY);

                if (fabsf(lx) <= halfX && fabsf(ly) <= halfY)
                {
                    m_RearMirrorAimLineHideUntil = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<float>(m_RearMirrorAimLineHideHoldSeconds));
                    return true;
                }

                return false;
            };

        if (ShouldRenderRearMirror() && !shouldHideRearMirrorDueToAimLine())
            vr::VROverlay()->ShowOverlay(m_RearMirrorHandle);
        else
            vr::VROverlay()->HideOverlay(m_RearMirrorHandle);
    }
    else
    {
        vr::VROverlay()->HideOverlay(m_RearMirrorHandle);
    }

    const bool leftOk = submitEye(vr::Eye_Left, &m_VKLeftEye.m_VRTexture, &(m_TextureBounds)[0]);
    const bool rightOk = submitEye(vr::Eye_Right, &m_VKRightEye.m_VRTexture, &(m_TextureBounds)[1]);

    const bool successfulSubmit = leftOk && rightOk;

    // Only perform the explicit timing handoff after a complete stereo submit (both eyes).
    if (successfulSubmit && m_CompositorExplicitTiming && submittedAnyThisCall)
    {
        m_CompositorNeedsHandoff = true;
        FinishFrame();
    }


    m_RenderedNewFrame = false;
}

static const char* CompositorErrorName(vr::EVRCompositorError error)
{
    switch (error)
    {
    case vr::VRCompositorError_None: return "None";
    case vr::VRCompositorError_RequestFailed: return "RequestFailed";
    case vr::VRCompositorError_IncompatibleVersion: return "IncompatibleVersion";
    case vr::VRCompositorError_DoNotHaveFocus: return "DoNotHaveFocus";
    case vr::VRCompositorError_InvalidTexture: return "InvalidTexture";
    case vr::VRCompositorError_IsNotSceneApplication: return "IsNotSceneApplication";
    case vr::VRCompositorError_TextureIsOnWrongDevice: return "TextureIsOnWrongDevice";
    case vr::VRCompositorError_TextureUsesUnsupportedFormat: return "TextureUsesUnsupportedFormat";
    case vr::VRCompositorError_SharedTexturesNotSupported: return "SharedTexturesNotSupported";
    case vr::VRCompositorError_IndexOutOfRange: return "IndexOutOfRange";
    case vr::VRCompositorError_AlreadySubmitted: return "AlreadySubmitted";
    case vr::VRCompositorError_InvalidBounds: return "InvalidBounds";
    case vr::VRCompositorError_AlreadySet: return "AlreadySet";
    default: return "Unknown";
    }
}

void VR::LogCompositorError(const char* action, vr::EVRCompositorError error)
{
    if (error == vr::VRCompositorError_None || !action)
        return;

    constexpr auto kLogCooldown = std::chrono::seconds(5);
    const auto now = std::chrono::steady_clock::now();

    if (now - m_LastCompositorErrorLog < kLogCooldown)
        return;

    Game::logMsg("[VR] %s failed: %s (%d)", action, CompositorErrorName(error), static_cast<int>(error));
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

void VR::UpdateRearMirrorOverlayTransform()
{
    if (!m_RearMirrorEnabled || m_RearMirrorHandle == vr::k_ulOverlayHandleInvalid)
        return;

    // We place the rear mirror in tracking space (meters), anchored to the same "body origin"
    // used by the inventory system: InventoryBodyOriginOffset is (forward,right,up) in body space.
    const vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!hmdPose.bPoseIsValid)
        return;

    const vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;
    const Vector hmdPos = { hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] };

    const Vector up = { 0.0f, 1.0f, 0.0f }; // OpenVR tracking space: +Y is up

    // Yaw-only forward (flattened to the floor plane).
    Vector fwd = { -hmdMat.m[0][2], 0.0f, -hmdMat.m[2][2] };
    if (VectorNormalize(fwd) == 0.0f)
        fwd = { 0.0f, 0.0f, -1.0f };

    Vector right = CrossProduct(fwd, up);
    if (VectorNormalize(right) == 0.0f)
        right = { 1.0f, 0.0f, 0.0f };

    // "Back" axis for a right-handed basis (OpenVR commonly treats +Z as "back").
    const Vector back = { -fwd.x, -fwd.y, -fwd.z };

    // Body origin in tracking space (meters)
    const Vector bodyOrigin =
        hmdPos
        + (fwd * m_InventoryBodyOriginOffset.x)
        + (right * m_InventoryBodyOriginOffset.y)
        + (up * m_InventoryBodyOriginOffset.z);

    // Rear mirror overlay position relative to that body origin (meters)
    const Vector mirrorPos =
        bodyOrigin
        + (fwd * m_RearMirrorOverlayXOffset)
        + (right * m_RearMirrorOverlayYOffset)
        + (up * m_RearMirrorOverlayZOffset);

    // Build a yaw-only parent rotation from (right, up, back) and then apply the user angle offset.
    const float deg2rad = 3.14159265358979323846f / 180.0f;

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

    auto mul33 = [](const float a[3][3], const float b[3][3], float out[3][3])
        {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
        };

    float RyRx[3][3];
    float Roff[3][3];
    mul33(Ry, Rx, RyRx);
    mul33(Rz, RyRx, Roff);

    // Parent yaw-only basis (columns: right, up, back)
    const float B[3][3] = {
        { right.x, up.x, back.x },
        { right.y, up.y, back.y },
        { right.z, up.z, back.z }
    };

    float Rworld[3][3];
    mul33(B, Roff, Rworld);

    vr::HmdMatrix34_t mirrorAbs = {
        Rworld[0][0], Rworld[0][1], Rworld[0][2], mirrorPos.x,
        Rworld[1][0], Rworld[1][1], Rworld[1][2], mirrorPos.y,
        Rworld[2][0], Rworld[2][1], Rworld[2][2], mirrorPos.z
    };

    const vr::ETrackingUniverseOrigin trackingOrigin = vr::VRCompositor()->GetTrackingSpace();
    vr::VROverlay()->SetOverlayTransformAbsolute(m_RearMirrorHandle, trackingOrigin, &mirrorAbs);
    float mirrorWidth = std::max(0.01f, m_RearMirrorOverlayWidthMeters);
    if (m_RearMirrorSpecialWarningDistance > 0.0f && m_RearMirrorSpecialEnlargeActive)
        mirrorWidth *= 2.0f;
    vr::VROverlay()->SetOverlayWidthInMeters(m_RearMirrorHandle, mirrorWidth);
}

void VR::UpdateScopeOverlayTransform()
{
    if (!m_ScopeEnabled)
        return;

    // Default behavior (controllers available): scope overlay is tracked-device-relative and updated in RepositionOverlays().
    // Mouse mode needs an absolute transform since there may be no tracked gun controller.
    if (!m_MouseModeEnabled || m_ScopeHandle == vr::k_ulOverlayHandleInvalid)
        return;

    const float scopeWidth = std::max(0.01f, m_ScopeOverlayWidthMeters);

    // IMPORTANT:
    // - OpenVR absolute overlay transforms are in tracking-space *meters*.
    // - Most of our in-game positions (m_HmdPosAbs, viewmodel anchors, etc.) are in Source units.
    // If we feed Source units into SetOverlayTransformAbsolute, the overlay ends up kilometers away.
    // So for mouse mode we build the transform directly from the OpenVR HMD tracking pose.

    const vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!hmdPose.bPoseIsValid)
        return;

    const vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;
    const Vector hmdPos = { hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] }; // meters

    // Tracking-space basis (meters): columns of the 3x4 matrix
    // right = +X, up = +Y, back = +Z (OpenVR commonly treats -Z as forward)
    Vector parentRight = { hmdMat.m[0][0], hmdMat.m[1][0], hmdMat.m[2][0] };
    Vector parentUp = { hmdMat.m[0][1], hmdMat.m[1][1], hmdMat.m[2][1] };
    Vector parentBack = { hmdMat.m[0][2], hmdMat.m[1][2], hmdMat.m[2][2] };
    if (VectorNormalize(parentRight) == 0.0f || VectorNormalize(parentUp) == 0.0f || VectorNormalize(parentBack) == 0.0f)
        return;

    // Base position: HMD position plus optional mouse-mode HMD-anchored offset (meters).
    // If not set, fall back to the existing ScopeOverlay offsets.
    Vector overlayPos = hmdPos;
    if (!m_MouseModeScopeOverlayOffset.IsZero())
    {
        overlayPos += parentRight * m_MouseModeScopeOverlayOffset.x;
        overlayPos += parentUp * m_MouseModeScopeOverlayOffset.y;
        overlayPos += parentBack * m_MouseModeScopeOverlayOffset.z;
    }
    else
    {
        overlayPos += parentRight * m_ScopeOverlayXOffset;
        overlayPos += parentUp * m_ScopeOverlayYOffset;
        overlayPos += parentBack * m_ScopeOverlayZOffset;
    }

    const QAngle a = (m_MouseModeScopeOverlayAngleOffsetSet ? m_MouseModeScopeOverlayAngleOffset : m_ScopeOverlayAngleOffset);
    const float cx = std::cos(DEG2RAD(a.x)), sx = std::sin(DEG2RAD(a.x));
    const float cy = std::cos(DEG2RAD(a.y)), sy = std::sin(DEG2RAD(a.y));
    const float cz = std::cos(DEG2RAD(a.z)), sz = std::sin(DEG2RAD(a.z));

    const float Roff00 = cy * cz;
    const float Roff01 = -cy * sz;
    const float Roff02 = sy;
    const float Roff10 = sx * sy * cz + cx * sz;
    const float Roff11 = -sx * sy * sz + cx * cz;
    const float Roff12 = -sx * cy;
    const float Roff20 = -cx * sy * cz + sx * sz;
    const float Roff21 = cx * sy * sz + sx * cz;
    const float Roff22 = cx * cy;

    // World basis = ParentBasis * OffsetRotation.
    const float B00 = parentRight.x, B01 = parentUp.x, B02 = parentBack.x;
    const float B10 = parentRight.y, B11 = parentUp.y, B12 = parentBack.y;
    const float B20 = parentRight.z, B21 = parentUp.z, B22 = parentBack.z;

    const float R00 = B00 * Roff00 + B01 * Roff10 + B02 * Roff20;
    const float R01 = B00 * Roff01 + B01 * Roff11 + B02 * Roff21;
    const float R02 = B00 * Roff02 + B01 * Roff12 + B02 * Roff22;
    const float R10 = B10 * Roff00 + B11 * Roff10 + B12 * Roff20;
    const float R11 = B10 * Roff01 + B11 * Roff11 + B12 * Roff21;
    const float R12 = B10 * Roff02 + B11 * Roff12 + B12 * Roff22;
    const float R20 = B20 * Roff00 + B21 * Roff10 + B22 * Roff20;
    const float R21 = B20 * Roff01 + B21 * Roff11 + B22 * Roff21;
    const float R22 = B20 * Roff02 + B21 * Roff12 + B22 * Roff22;

    vr::HmdMatrix34_t scopeAbs = {
        R00, R01, R02, overlayPos.x,
        R10, R11, R12, overlayPos.y,
        R20, R21, R22, overlayPos.z
    };
    const vr::ETrackingUniverseOrigin trackingOrigin = vr::VRCompositor()->GetTrackingSpace();
    vr::VROverlay()->SetOverlayTransformAbsolute(m_ScopeHandle, trackingOrigin, &scopeAbs);
    vr::VROverlay()->SetOverlayWidthInMeters(m_ScopeHandle, scopeWidth);
}

bool VR::ShouldRenderRearMirror() const
{
    if (!m_RearMirrorEnabled)
        return false;

    // Default behavior: always render/show when enabled.
    if (!m_RearMirrorShowOnlyOnSpecialWarning)
        return true;

    // Pop-up mode: only render/show for a short time after a special-infected warning.
    if (m_RearMirrorSpecialShowHoldSeconds <= 0.0f)
        return false;

    const auto now = std::chrono::steady_clock::now();

    bool alertActive = false;
    if (m_LastRearMirrorAlertTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastRearMirrorAlertTime).count();
        alertActive = (elapsed <= m_RearMirrorSpecialShowHoldSeconds);
    }

    // Also keep it visible if the mirror pass recently saw special-infected arrows (enlarge hint).
    bool hintActive = false;
    if (m_LastRearMirrorSpecialSeenTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastRearMirrorSpecialSeenTime).count();
        hintActive = (elapsed <= m_RearMirrorSpecialEnlargeHoldSeconds);
    }

    return alertActive || hintActive;
}

void VR::NotifyRearMirrorSpecialWarning()
{
    m_LastRearMirrorAlertTime = std::chrono::steady_clock::now();
}

bool VR::ShouldUpdateScopeRTT()
{
    // Throttle the expensive offscreen render pass; leaving the last rendered texture in place is fine.
    return !ShouldThrottle(m_LastScopeRTTRenderTime, m_ScopeRTTMaxHz);
}
bool VR::ShouldUpdateRearMirrorRTT()
{
    // The rear mirror is a full extra scene render. Throttling this can significantly reduce CPU spikes.
    return !ShouldThrottle(m_LastScopeRTTRenderTime, m_ScopeRTTMaxHz);
}
void VR::RepositionOverlays()
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

    // Reposition HUD overlay (single full overlay)
    Vector hudDistance = hmdForward * (m_HudDistance + m_FixedHudDistanceOffset);
    Vector hudNewPos = hudDistance + hmdPosition;
    hudNewPos.y -= 0.25f;
    hudNewPos.y += m_FixedHudYOffset;
    vr::HmdMatrix34_t hudTransform = buildFacingTransform(hudNewPos);
    vr::VROverlay()->SetOverlayTransformAbsolute(m_HUDHandle, trackingOrigin, &hudTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_HUDHandle, m_HudSize);

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

            const QAngle scopeAngle = (m_MouseModeEnabled && m_MouseModeScopeOverlayAngleOffsetSet)
                ? m_MouseModeScopeOverlayAngleOffset
                : m_ScopeOverlayAngleOffset;
            const float pitch = scopeAngle.x * deg2rad;
            const float yaw = scopeAngle.y * deg2rad;
            const float roll = scopeAngle.z * deg2rad;

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

    // Rear mirror overlay is now body-anchored (InventoryBodyOriginOffset); keep it updated per-frame.
    if (m_RearMirrorEnabled)
    {
        UpdateRearMirrorOverlayTransform();
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
    if (!m_Compositor || !m_Input)
        return false;

    auto QueryFrameIndex = [this]() -> uint32_t
    {
            vr::Compositor_FrameTiming timing{};
            timing.m_nSize = sizeof(timing);
            if (!m_Compositor->GetFrameTiming(&timing, 0))
                return 0;
            return timing.m_nFrameIndex;
    };

    const uint32_t beforeIdx = QueryFrameIndex();
    if (beforeIdx != 0 && m_HasWaitGetPosesFrameIndex.load(std::memory_order_acquire) &&
        beforeIdx == m_LastWaitGetPosesFrameIndex.load(std::memory_order_acquire))
    {
        return m_LastWaitGetPosesValid.load(std::memory_order_acquire);
    }

    // Multiple call sites may want poses (RenderView hook + Update). Never block twice in the same SteamVR frame.
    if (m_WaitGetPosesInProgress.test_and_set(std::memory_order_acquire))
        return m_LastWaitGetPosesValid.load(std::memory_order_acquire);
    vr::EVRCompositorError result = m_Compositor->WaitGetPoses(m_Poses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
    const bool posesValid = (result == vr::VRCompositorError_None);

    if (!posesValid && m_CompositorExplicitTiming)
        m_CompositorNeedsHandoff = false;

    m_Input->UpdateActionState(&m_ActiveActionSet, sizeof(vr::VRActiveActionSet_t), 1);
    const uint32_t afterIdx = QueryFrameIndex();
    const uint32_t storeIdx = (afterIdx != 0) ? afterIdx : beforeIdx;
    if (storeIdx != 0)
    {
        m_LastWaitGetPosesFrameIndex.store(storeIdx, std::memory_order_release);
        m_HasWaitGetPosesFrameIndex.store(true, std::memory_order_release);
    }

    m_LastWaitGetPosesValid.store(posesValid, std::memory_order_release);

    m_WaitGetPosesInProgress.clear(std::memory_order_release);
    return posesValid;
}

void VR::RequestMatQueueMode(int mode)
{
    if (!m_Game || mode < 0)
        return;
    if (m_LastRequestedMatQueueMode == mode)
        return;

    char cmd[64] = {};
    sprintf_s(cmd, sizeof(cmd), "mat_queue_mode %d", mode);
    m_Game->ClientCmd_Unrestricted(cmd);
    m_LastRequestedMatQueueMode = mode;
    // If HUD linkage is enabled, also clamp fps_max relative to SteamVR's display refresh rate.
    // This helps avoid CPU/GPU timing weirdness when switching between mat_queue_mode=2 and single-thread modes.
    if (m_HudMatQueueModeLinkEnabled)
        ApplyLinkedFpsMaxForMatQueueMode(mode);
}

float VR::GetSteamVRDisplayFrequencyHz(bool forceRefresh)
{
    if (!m_System)
        return m_CachedSteamVRDisplayFrequencyHz;

    const uint64_t nowMs = GetTickCount64();
    // Refresh at most twice a second unless forced.
    if (!forceRefresh && m_CachedSteamVRDisplayFrequencyHz > 0.0f && (nowMs - m_LastSteamVRDisplayFrequencyQueryMs) < 500)
        return m_CachedSteamVRDisplayFrequencyHz;

    vr::ETrackedPropertyError err = vr::TrackedProp_Success;
    const float hz = m_System->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_DisplayFrequency_Float, &err);
    m_LastSteamVRDisplayFrequencyQueryMs = nowMs;
    if (err == vr::TrackedProp_Success && hz > 1.0f)
        m_CachedSteamVRDisplayFrequencyHz = hz;

    return m_CachedSteamVRDisplayFrequencyHz;
}

void VR::ApplyLinkedFpsMaxForMatQueueMode(int matQueueMode)
{
    if (!m_Game || !m_Game->m_EngineClient)
        return;
    if (!m_Game->m_EngineClient->IsInGame())
        return;

    const float hz = GetSteamVRDisplayFrequencyHz(false);
    if (hz <= 1.0f)
        return;

    // mat_queue_mode=2 (MATERIAL_QUEUED_THREADED) => cap to 80% of HMD refresh.
    // other modes => cap to 100%.
    const float ratio = (matQueueMode == 2) ? m_HudMatQueueModeLinkThreadedFpsRatio : 1.00f;
    int target = (int)std::lround(hz * ratio);
    // Source treats <=0 as "uncapped"; don't do that here.
    target = std::clamp(target, 30, 1000);

    if (m_LastRequestedFpsMax == target)
        return;

    char cmd[64] = {};
    sprintf_s(cmd, sizeof(cmd), "fps_max %d", target);
    m_Game->ClientCmd_Unrestricted(cmd);
    m_LastRequestedFpsMax = target;
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
    vr::VROverlayHandle_t currentOverlay = inGame ? m_HUDHandle : m_MainMenuHandle;

    const auto controllerHoveringOverlay = [&](vr::VROverlayHandle_t overlay)
        {
            return CheckOverlayIntersectionForController(overlay, vr::TrackedControllerRole_LeftHand) ||
                CheckOverlayIntersectionForController(overlay, vr::TrackedControllerRole_RightHand);
        };

    vr::VROverlayHandle_t hoveredOverlay = vr::k_ulOverlayHandleInvalid;

    if (inGame)
    {
        if (controllerHoveringOverlay(m_HUDHandle))
            hoveredOverlay = m_HUDHandle;
    }
    else if (controllerHoveringOverlay(m_MainMenuHandle))
    {
        hoveredOverlay = m_MainMenuHandle;
    }

    const bool isHoveringOverlay = hoveredOverlay != vr::k_ulOverlayHandleInvalid;

    if (isHoveringOverlay)
        currentOverlay = hoveredOverlay;

    const bool isHudOverlay = inGame && (currentOverlay == m_HUDHandle);

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

    // Recomputed every frame from CustomAction bindings.
    m_CustomWalkHeld = false;

    vr::VROverlay()->SetOverlayFlag(m_HUDHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);

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
        // If some automation temporarily suppresses player input, ensure we don't leave
        // +attack/+attack2 stuck (we used to spam "-attack" every frame; now we edge-trigger).
        if (m_PrimaryAttackCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("-attack");
            m_PrimaryAttackCmdOwned = false;
        }
        if (m_SecondaryAttackCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("-attack2");
            m_SecondaryAttackCmdOwned = false;
        }
        if (m_JumpCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("-jump");
            m_JumpCmdOwned = false;
        }
        if (m_UseCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("-use");
            m_UseCmdOwned = false;
        }
        if (m_ReloadCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("-reload");
            m_ReloadCmdOwned = false;
        }
        m_PrimaryAttackDown = false;
        return;
    }

    vr::InputAnalogActionData_t analogActionData;

    if (GetAnalogActionData(m_ActionTurn, analogActionData))
    {
        auto applyTurnFromStick = [&](float stickX)
            {
                if (m_SnapTurning)
                {
                    if (!m_PressedTurn && stickX > 0.5f)
                    {
                        m_RotationOffset -= m_SnapTurnAngle;
                        m_PressedTurn = true;
                    }
                    else if (!m_PressedTurn && stickX < -0.5f)
                    {
                        m_RotationOffset += m_SnapTurnAngle;
                        m_PressedTurn = true;
                    }
                    else if (stickX < 0.3f && stickX > -0.3f)
                    {
                        m_PressedTurn = false;
                    }
                }
                // Smooth turning
                else
                {
                    float deadzone = 0.2f;
                    float xNormalized = (abs(stickX) - deadzone) / (1 - deadzone);
                    if (stickX > deadzone)
                    {
                        m_RotationOffset -= m_TurnSpeed * deltaTime * xNormalized;
                    }
                    if (stickX < -deadzone)
                    {
                        m_RotationOffset += m_TurnSpeed * deltaTime * xNormalized;
                    }
                }
            };

        applyTurnFromStick(analogActionData.x);

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

    const bool wantJump = PressedDigitalAction(m_ActionJump) || jumpGestureActive;
    if (wantJump && !m_JumpCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("+jump");
        m_JumpCmdOwned = true;
    }
    else if (!wantJump && m_JumpCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("-jump");
        m_JumpCmdOwned = false;
    }

    const bool wantUse = PressedDigitalAction(m_ActionUse);
    if (wantUse && !m_UseCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("+use");
        m_UseCmdOwned = true;
    }
    else if (!wantUse && m_UseCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("-use");
        m_UseCmdOwned = false;
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

    vr::InputDigitalActionData_t jumpActionData{};
    bool jumpButtonDown = false;
    bool jumpJustPressed = false;
    getActionState(&m_ActionJump, jumpActionData, jumpButtonDown, jumpJustPressed);

    vr::InputDigitalActionData_t useActionData{};
    bool useButtonDown = false;
    bool useJustPressed = false;
    getActionState(&m_ActionUse, useActionData, useButtonDown, useJustPressed);

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

    bool suppressReload = false;
    bool suppressCrouch = false;

    auto originMatchesRole = [&](vr::VRInputValueHandle_t origin, vr::ETrackedControllerRole role) -> bool
        {
            if (!m_Input || !m_System)
                return false;
            if (origin == vr::k_ulInvalidInputValueHandle)
                return false;

            vr::InputOriginInfo_t info{};
            if (m_Input->GetOriginTrackedDeviceInfo(origin, &info, sizeof(info)) != vr::VRInputError_None)
                return false;

            const vr::TrackedDeviceIndex_t roleIndex = m_System->GetTrackedDeviceIndexForControllerRole(role);
            if (roleIndex == vr::k_unTrackedDeviceIndexInvalid)
                return false;

            return info.trackedDeviceIndex == roleIndex;
        };

    // --- Inventory switching: use SteamVR-bound buttons (Reload / Crouch) instead of RAW physical GRIP ---
    // Press reload/crouch while your controller is near an inventory anchor to switch items.
    // When consumed, we also swallow the button until release so it won't also reload/duck.
    auto triggerInventoryAtPos = [&](const Vector& controllerPos) -> bool
        {
            const bool nearBack = isControllerNear(controllerPos, backAnchor, gestureRange);
            const bool nearChest = isControllerNear(controllerPos, chestAnchor, chestGestureRange);
            const bool nearLeftWaist = isControllerNear(controllerPos, leftWaistAnchor, gestureRange);
            const bool nearRightWaist = isControllerNear(controllerPos, rightWaistAnchor, gestureRange);

            if (!(nearBack || nearChest || nearLeftWaist || nearRightWaist))
                return false;

            if (nearBack)
            {
                togglePrimarySecondary();
                return true;
            }
            if (nearLeftWaist)
            {
                m_Game->ClientCmd_Unrestricted("slot3");
                return true;
            }
            if (nearChest)
            {
                m_Game->ClientCmd_Unrestricted("slot4");
                return true;
            }
            if (nearRightWaist)
            {
                m_Game->ClientCmd_Unrestricted("slot5");
                return true;
            }
            return false;
        };

    auto triggerInventoryFromOrigin = [&](vr::VRInputValueHandle_t origin) -> bool
        {
            if (originMatchesRole(origin, vr::TrackedControllerRole_RightHand))
                return triggerInventoryAtPos(m_RightControllerPosAbs);
            if (originMatchesRole(origin, vr::TrackedControllerRole_LeftHand))
                return triggerInventoryAtPos(m_LeftControllerPosAbs);

            // If origin can't be resolved, check both hands (prefer right).
            if (triggerInventoryAtPos(m_RightControllerPosAbs))
                return true;
            return triggerInventoryAtPos(m_LeftControllerPosAbs);
        };

    static bool s_BlockReloadUntilRelease = false;
    static bool s_BlockCrouchUntilRelease = false;

    if (reloadDataValid && s_BlockReloadUntilRelease)
    {
        if (reloadActionData.bState)
        {
            reloadButtonDown = false;
            reloadJustPressed = false;
        }
        else
        {
            s_BlockReloadUntilRelease = false;
        }
    }

    if (crouchDataValid && s_BlockCrouchUntilRelease)
    {
        if (crouchActionData.bState)
        {
            crouchButtonDown = false;
            crouchJustPressed = false;
        }
        else
        {
            s_BlockCrouchUntilRelease = false;
        }
    }

    if (reloadDataValid && reloadJustPressed)
    {
        if (triggerInventoryFromOrigin(reloadActionData.activeOrigin))
        {
            reloadButtonDown = false;
            reloadJustPressed = false;
            s_BlockReloadUntilRelease = true;
        }
    }

    if (crouchDataValid && crouchJustPressed)
    {
        if (triggerInventoryFromOrigin(crouchActionData.activeOrigin))
        {
            crouchButtonDown = false;
            crouchJustPressed = false;
            s_BlockCrouchUntilRelease = true;
        }
    }

    auto suppressIfNeeded = [&](const vr::InputDigitalActionData_t& data,
        vr::ETrackedControllerRole role,
        bool suppressEnabled,
        bool& down,
        bool& justPressed) -> bool
        {
            if (!suppressEnabled)
                return false;
            if (!data.bActive)
                return false;
            if (!down && !justPressed)
                return false;

            if (originMatchesRole(data.activeOrigin, role))
            {
                down = false;
                justPressed = false;
                return true;
            }
            return false;
        };

    suppressIfNeeded(jumpActionData, vr::TrackedControllerRole_LeftHand, m_SuppressActionsUntilGripReleaseLeft, jumpButtonDown, jumpJustPressed);
    suppressIfNeeded(jumpActionData, vr::TrackedControllerRole_RightHand, m_SuppressActionsUntilGripReleaseRight, jumpButtonDown, jumpJustPressed);

    suppressIfNeeded(useActionData, vr::TrackedControllerRole_LeftHand, m_SuppressActionsUntilGripReleaseLeft, useButtonDown, useJustPressed);
    suppressIfNeeded(useActionData, vr::TrackedControllerRole_RightHand, m_SuppressActionsUntilGripReleaseRight, useButtonDown, useJustPressed);

    if (reloadDataValid)
    {
        suppressReload |= suppressIfNeeded(reloadActionData, vr::TrackedControllerRole_LeftHand, m_SuppressActionsUntilGripReleaseLeft, reloadButtonDown, reloadJustPressed);
        suppressReload |= suppressIfNeeded(reloadActionData, vr::TrackedControllerRole_RightHand, m_SuppressActionsUntilGripReleaseRight, reloadButtonDown, reloadJustPressed);
    }

    if (crouchDataValid)
    {
        suppressCrouch |= suppressIfNeeded(crouchActionData, vr::TrackedControllerRole_LeftHand, m_SuppressActionsUntilGripReleaseLeft, crouchButtonDown, crouchJustPressed);
        suppressCrouch |= suppressIfNeeded(crouchActionData, vr::TrackedControllerRole_RightHand, m_SuppressActionsUntilGripReleaseRight, crouchButtonDown, crouchJustPressed);
    }

    if (flashlightDataValid)
    {
        suppressIfNeeded(flashlightActionData, vr::TrackedControllerRole_LeftHand, m_SuppressActionsUntilGripReleaseLeft, flashlightButtonDown, flashlightJustPressed);
        suppressIfNeeded(flashlightActionData, vr::TrackedControllerRole_RightHand, m_SuppressActionsUntilGripReleaseRight, flashlightButtonDown, flashlightJustPressed);
    }

    if (resetDataValid)
    {
        suppressIfNeeded(resetActionData, vr::TrackedControllerRole_LeftHand, m_SuppressActionsUntilGripReleaseLeft, resetButtonDown, resetJustPressed);
        suppressIfNeeded(resetActionData, vr::TrackedControllerRole_RightHand, m_SuppressActionsUntilGripReleaseRight, resetButtonDown, resetJustPressed);
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
        m_SpecialInfectedRunCommandShotAimUntil = {};
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
        m_SpecialInfectedRunCommandShotAimUntil = {};
    }

    if (nonVrServerMovementToggleJustPressed)
    {
        m_NonVRServerMovementAngleOverride = !m_NonVRServerMovementAngleOverride;
    }

    if (scopeMagnificationToggleJustPressed)
    {
        CycleScopeMagnification();
    }

    // Drive +attack only from the VR action state. IMPORTANT: do NOT spam "-attack" every frame,
    // otherwise real mouse1 cannot work in MouseMode (mouse1 triggers +attack, but we instantly cancel it).
    if (primaryAttackDown && !m_PrimaryAttackCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("+attack");
        m_PrimaryAttackCmdOwned = true;
    }
    else if (!primaryAttackDown && m_PrimaryAttackCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("-attack");
        m_PrimaryAttackCmdOwned = false;
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

    const bool wantReload = (!crouchButtonDown && reloadButtonDown && !adjustViewmodelActive);
    if (wantReload && !m_ReloadCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("+reload");
        m_ReloadCmdOwned = true;
    }
    else if (!wantReload && m_ReloadCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("-reload");
        m_ReloadCmdOwned = false;
    }

    {
        const bool wantAttack2 = secondaryAttackActive && !adjustViewmodelActive;
        if (wantAttack2 && !m_SecondaryAttackCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("+attack2");
            m_SecondaryAttackCmdOwned = true;
        }
        else if (!wantAttack2 && m_SecondaryAttackCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("-attack2");
            m_SecondaryAttackCmdOwned = false;
        }
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

    const bool wantDuck = (!suppressCrouch) && (crouchButtonDown || m_CrouchToggleActive);
    // IMPORTANT: only release -duck if *we* previously issued +duck.
    // Otherwise we would cancel the player's real keyboard input (e.g. Ctrl +duck in mouse mode).
    if (wantDuck && !m_DuckCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("+duck");
        m_DuckCmdOwned = true;
    }
    else if (!wantDuck && m_DuckCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("-duck");
        m_DuckCmdOwned = false;
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

    // Toggle: suppress primary fire when the aim line is currently hitting a teammate.
    // This is a pure client-side guard: we simply clear IN_ATTACK in CreateMove.
    if (PressedDigitalAction(m_ActionFriendlyFireBlockToggle, true))
    {
        m_BlockFireOnFriendlyAimEnabled = !m_BlockFireOnFriendlyAimEnabled;
    }

    auto isWalkPressCommand = [](const std::string& cmd) -> bool
        {
            // Match the first token only (allow "+walk;..." or "+walk ...").
            size_t start = cmd.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                return false;
            size_t end = cmd.find_first_of(" \t\r\n;\"", start);
            std::string token = cmd.substr(start, (end == std::string::npos) ? std::string::npos : (end - start));
            std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return token == "+walk";
        };

    auto handleCustomAction = [&](vr::VRActionHandle_t& actionHandle, const CustomActionBinding& binding)
        {
            vr::InputDigitalActionData_t actionData{};
            if (!GetDigitalActionData(actionHandle, actionData))
                return;

            // If a CustomAction is mapped to +walk (press/release), optionally use it as a
            // hint to force third-person *rendering* while held. This is useful for slide
            // mods that switch the gameplay camera to third-person when +walk is active.
            if (m_ThirdPersonRenderOnCustomWalk && binding.usePressReleaseCommands && isWalkPressCommand(binding.command))
            {
                m_CustomWalkHeld = m_CustomWalkHeld || actionData.bState;
            }

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

    // Single HUD overlay + mat_queue_mode auto-switch
    auto showHud = [&]() { vr::VROverlay()->ShowOverlay(m_HUDHandle); };
    auto hideHud = [&]() { vr::VROverlay()->HideOverlay(m_HUDHandle); };

    const bool inGame = m_Game->m_EngineClient->IsInGame();
    const bool isControllerVertical =
        m_RightControllerAngAbs.x > 60.0f || m_RightControllerAngAbs.x < -45.0f ||
        m_LeftControllerAngAbs.x > 60.0f || m_LeftControllerAngAbs.x < -45.0f;
    const bool menuActive = m_Game->m_EngineClient->IsPaused();
    const bool cursorVisible = m_Game->m_VguiSurface->IsCursorVisible();

    if (cursorVisible)
        m_HudChatVisibleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const bool chatRecent = std::chrono::steady_clock::now() < m_HudChatVisibleUntil;
 
    if (PressedDigitalAction(m_ToggleHUD, true))
    {
        m_HudToggleState = !m_HudToggleState;
        m_HudToggleStateFromAlwaysVisible = false;
    }

    // If linkage is enabled and we are currently in mat_queue_mode=2, force HudAlwaysVisible off.
    bool matQueueMode2 = false;
    if (m_HudMatQueueModeLinkEnabled && m_Game && m_Game->m_MaterialSystem)
    {
        MaterialThreadMode_t threadMode = m_Game->m_MaterialSystem->GetThreadMode();
        matQueueMode2 = (threadMode == MATERIAL_QUEUED_THREADED);
    }
    if (m_HudMatQueueModeLinkEnabled && matQueueMode2 && m_HudAlwaysVisible)
    {
        m_HudAlwaysVisible = false;
        if (m_HudToggleStateFromAlwaysVisible)
            m_HudToggleState = false;
        m_HudToggleStateFromAlwaysVisible = false;
    }

    const bool wantsHudVisibility =
        PressedDigitalAction(m_Scoreboard) ||
        isControllerVertical ||
        m_HudToggleState ||
        cursorVisible ||
        chatRecent ||
        menuActive;

    // Keep scoreboard command in sync (avoid sticky scores).
    if (PressedDigitalAction(m_Scoreboard))
        m_Game->ClientCmd_Unrestricted("+showscores");
    else
        m_Game->ClientCmd_Unrestricted("-showscores");

    if (m_HudMatQueueModeLinkEnabled && inGame)
    {
        if (wantsHudVisibility)
            RequestMatQueueMode(1);
    }

    const bool showHudNow = wantsHudVisibility && (m_RenderedHud || menuActive);
    if (showHudNow)
    {
        RepositionOverlays();
        showHud();
    }
    else
    {
        hideHud();
        // Only allow switching back to mat_queue_mode=2 when HUD is hidden AND we are not in first-person render.
        if (m_HudMatQueueModeLinkEnabled && inGame && !wantsHudVisibility)
            RequestMatQueueMode(2);
    }

    m_RenderedHud = false;

    if (PressedDigitalAction(m_Pause, true))
    {
        m_Game->ClientCmd_Unrestricted("gameui_activate");
        RepositionOverlays();
        if (m_HudMatQueueModeLinkEnabled && inGame)
            RequestMatQueueMode(1);
        showHud();
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
    if (const RenderFrameSnapshot* snap = GetActiveRenderFrameSnapshot())
        return snap->rightControllerAngAbs;
    return m_RightControllerAngAbs;
}

Vector VR::GetRightControllerAbsPos()
{
    if (const RenderFrameSnapshot* snap = GetActiveRenderFrameSnapshot())
        return snap->rightControllerPosAbs;
    return m_RightControllerPosAbs;
}

Vector VR::GetRecommendedViewmodelAbsPos()
{
    const RenderFrameSnapshot* snap = GetActiveRenderFrameSnapshot();

    Vector viewmodelPos = snap ? snap->rightControllerPosAbs : m_RightControllerPosAbs;
    const bool mouseModeEnabled = snap ? snap->mouseModeEnabled : m_MouseModeEnabled;

    if (mouseModeEnabled)
    {
        const bool mouseModeScopeActive = snap ? snap->mouseModeScopeActive : IsMouseModeScopeActive();
        const Vector& anchor = mouseModeScopeActive
            ? (snap ? snap->mouseModeScopedViewmodelAnchorOffset : m_MouseModeScopedViewmodelAnchorOffset)
            : (snap ? snap->mouseModeViewmodelAnchorOffset : m_MouseModeViewmodelAnchorOffset);

        const Vector hmdPos = snap ? snap->hmdPosAbs : m_HmdPosAbs;
        const Vector hmdForward = snap ? snap->hmdForward : m_HmdForward;
        const Vector hmdRight = snap ? snap->hmdRight : m_HmdRight;
        const Vector hmdUp = snap ? snap->hmdUp : m_HmdUp;
        const float vrScale = snap ? snap->vrScale : m_VRScale;

        viewmodelPos = hmdPos
            + (hmdForward * (anchor.x * vrScale))
            + (hmdRight * (anchor.y * vrScale))
            + (hmdUp * (anchor.z * vrScale));
    }

    const Vector vmForward = snap ? snap->viewmodelForward : m_ViewmodelForward;
    const Vector vmRight = snap ? snap->viewmodelRight : m_ViewmodelRight;
    const Vector vmUp = snap ? snap->viewmodelUp : m_ViewmodelUp;
    const Vector vmOffset = snap ? snap->viewmodelPosOffset : m_ViewmodelPosOffset;

    viewmodelPos -= vmForward * vmOffset.x;
    viewmodelPos -= vmRight * vmOffset.y;
    viewmodelPos -= vmUp * vmOffset.z;

    return viewmodelPos;
}

QAngle VR::GetRecommendedViewmodelAbsAngle()
{
    const RenderFrameSnapshot* snap = GetActiveRenderFrameSnapshot();
    const Vector vmForward = snap ? snap->viewmodelForward : m_ViewmodelForward;
    const Vector vmUp = snap ? snap->viewmodelUp : m_ViewmodelUp;

    QAngle result{};
    QAngle::VectorAngles(vmForward, vmUp, result);
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

void VR::GetMouseModeEyeRay(Vector& eyeDirOut, QAngle* eyeAngOut)
{
    eyeDirOut = { 0.0f, 0.0f, 0.0f };

    // Reset reference when not using HMD-driven aiming (so re-entering re-centers).
    if (!m_MouseModeEnabled || !m_MouseModeAimFromHmd)
        m_MouseModeHmdAimReferenceInitialized = false;

    QAngle eyeAng{ 0.0f, 0.0f, 0.0f };

    if (m_MouseModeAimFromHmd)
    {
        // Capture a reference direction the first frame HMD aiming becomes active.
        if (!m_MouseModeHmdAimReferenceInitialized)
        {
            // IMPORTANT:
            //  - m_HmdAngAbs already includes m_RotationOffset (mouse/body yaw).
            //  - MouseModeHmdAimSensitivity is meant to scale *HMD* deltas only.
            // If we capture/scale the full absolute angle, mouse yaw gets scaled too,
            // which makes MouseModeHmdAimSensitivity < 1 feel like mouse turning is "stuck",
            // and > 1 feel like mouse turning is too fast.
            //
            // So: keep the reference in "head-local" space (yaw with body yaw removed).
            m_MouseModeHmdAimReferenceAng = m_HmdAngAbs;
            m_MouseModeHmdAimReferenceAng.y -= m_RotationOffset;
            // Wrap yaw to [-180, 180]
            m_MouseModeHmdAimReferenceAng.y -= 360.0f * std::floor((m_MouseModeHmdAimReferenceAng.y + 180.0f) / 360.0f);
            m_MouseModeHmdAimReferenceAng.z = 0.0f;
            NormalizeAndClampViewAngles(m_MouseModeHmdAimReferenceAng);
            m_MouseModeHmdAimReferenceInitialized = true;
        }

        const float sens = std::clamp(m_MouseModeHmdAimSensitivity, 0.0f, 3.0f);

        // Scale pitch/yaw deltas around the captured reference.
        // Use "head-local" yaw (absolute yaw minus body yaw) so mouse turning stays 1:1.
        QAngle cur = m_HmdAngAbs;
        cur.y -= m_RotationOffset;
        // Wrap yaw to [-180, 180]
        cur.y -= 360.0f * std::floor((cur.y + 180.0f) / 360.0f);
        cur.z = 0.0f;
        NormalizeAndClampViewAngles(cur);

        auto wrapDelta = [](float deg)
            {
                deg -= 360.0f * std::floor((deg + 180.0f) / 360.0f);
                return deg;
            };

        const float dPitch = wrapDelta(cur.x - m_MouseModeHmdAimReferenceAng.x);
        const float dYaw = wrapDelta(cur.y - m_MouseModeHmdAimReferenceAng.y);

        eyeAng.x = m_MouseModeHmdAimReferenceAng.x + dPitch * sens;
        // Convert back to absolute yaw by re-applying body yaw.
        eyeAng.y = m_RotationOffset + (m_MouseModeHmdAimReferenceAng.y + dYaw * sens);
        eyeAng.z = 0.0f;
        NormalizeAndClampViewAngles(eyeAng);

        Vector right, up;
        QAngle::AngleVectors(eyeAng, &eyeDirOut, &right, &up);
        if (!eyeDirOut.IsZero())
            VectorNormalize(eyeDirOut);
    }
    else
    {
        // Default mouse-mode eye ray: mouse pitch + body yaw (m_RotationOffset).
        const float pitch = std::clamp(m_MouseAimPitchOffset, -89.f, 89.f);
        const float yaw = m_RotationOffset;
        eyeAng = QAngle(pitch, yaw, 0.f);
        NormalizeAndClampViewAngles(eyeAng);

        Vector right, up;
        QAngle::AngleVectors(eyeAng, &eyeDirOut, &right, &up);
        if (!eyeDirOut.IsZero())
            VectorNormalize(eyeDirOut);
    }

    if (eyeAngOut)
        *eyeAngOut = eyeAng;
}
 
void VR::UpdateTrackingOncePerCompositorFrame()
{
    if (!m_Compositor)
    {
        UpdateTracking();
        return;
    }

    vr::Compositor_FrameTiming timing{};
    timing.m_nSize = sizeof(timing);
    const uint32_t frameIdx = m_Compositor->GetFrameTiming(&timing, 0) ? timing.m_nFrameIndex : 0;

    if (frameIdx != 0 && m_HasTrackingFrameIndex.load(std::memory_order_acquire) &&
        frameIdx == m_LastTrackingFrameIndex.load(std::memory_order_acquire))
    {
        return;
    }

    if (m_TrackingUpdateInProgress.test_and_set(std::memory_order_acquire))
        return;

    UpdateTracking();

    vr::Compositor_FrameTiming timingAfter{};
    timingAfter.m_nSize = sizeof(timingAfter);
    const uint32_t afterIdx = m_Compositor->GetFrameTiming(&timingAfter, 0) ? timingAfter.m_nFrameIndex : 0;
    const uint32_t storeIdx = (afterIdx != 0) ? afterIdx : frameIdx;
    if (storeIdx != 0)
    {
        m_LastTrackingFrameIndex.store(storeIdx, std::memory_order_release);
        m_HasTrackingFrameIndex.store(true, std::memory_order_release);
    }

    m_TrackingUpdateInProgress.clear(std::memory_order_release);
}

void VR::UpdateTracking()
{
    GetPoses();
    // Map load / reconnect detection:
    // - Some transitions briefly report observer-like netvars on the local player (even when alive).
    // - We arm a short cooldown window whenever we (re)enter "in game" or we lose/regain the local player pointer.
    // - The render hook will use this window to suppress observer-driven third-person latching.
    const bool inGameNow = (m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame());
    if (!m_WasInGamePrev && inGameNow)
        m_ThirdPersonMapLoadCooldownPending = true;
    m_WasInGamePrev = inGameNow;
    int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
    if (!localPlayer) {
        m_ScopeWeaponIsFirearm = false;
        // If we temporarily lose the local player (connect/disconnect/map change),
        // clear mounted-gun edge tracking so we don't trigger a bogus reset later.
        m_UsingMountedGunPrev = false;
        m_ResetPositionAfterMountedGunExitPending = false;
        m_HadLocalPlayerPrev = false;
        m_ThirdPersonMapLoadCooldownPending = true;
        m_ThirdPersonMapLoadCooldownEnd = {};
        return;
    }
    // Rising edge: local player pointer recovered (after connect/disconnect/map load).
    if (!m_HadLocalPlayerPrev)
    {
        m_HadLocalPlayerPrev = true;
        if (m_ThirdPersonMapLoadCooldownPending && m_ThirdPersonMapLoadCooldownMs > 0)
        {
            const auto now = std::chrono::steady_clock::now();
            m_ThirdPersonMapLoadCooldownEnd = now + std::chrono::milliseconds(m_ThirdPersonMapLoadCooldownMs);
        }
        else
        {
            m_ThirdPersonMapLoadCooldownEnd = {};
        }
        m_ThirdPersonMapLoadCooldownPending = false;
        // Clear any latched third-person state from the previous map/session.
        m_IsThirdPersonCamera = false;
        m_ThirdPersonHoldFrames = 0;
    }
    // Death flicker guard: latch a short first-person window on alive->dead transition.
    RefreshDeathFirstPersonLock(localPlayer);
    if (IsDeathFirstPersonLockActive())
    {
        // Also clear third-person hold state so the render hook won't immediately reassert 3P.
        m_IsThirdPersonCamera = false;
        m_ThirdPersonHoldFrames = 0;
    }

    // Spectator/observer: default to free-roaming camera (instead of chase cam locked to a teammate).
    // We only do this once per observer session, so the user can still manually switch modes afterwards.
    if (m_ObserverDefaultFreeCam)
    {
        const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
        const int teamNum = *reinterpret_cast<const int*>(base + kTeamNumOffset);
        const unsigned char lifeState = *reinterpret_cast<const unsigned char*>(base + kLifeStateOffset);

        const bool isObserver = (teamNum == 1) || (lifeState != 0);
        if (!isObserver)
        {
            m_ObserverWasActivePrev = false;
            m_ObserverForcedFreeCamThisObserver = false;
            m_ObserverFreeCamAttemptCount = 0;
            m_ObserverLastFreeCamAttempt = {};
        }
        else
        {
            if (!m_ObserverWasActivePrev)
            {
                // New observer session: allow one auto-switch.
                m_ObserverForcedFreeCamThisObserver = false;
                m_ObserverFreeCamAttemptCount = 0;
                m_ObserverLastFreeCamAttempt = {};
            }

            const int obsMode = *reinterpret_cast<const int*>(base + kObserverModeOffset);
            // Source observer modes (typical):
            //   1=deathcam, 2=freeze-cam, 3=fixed, 4=in-eye, 5=chase, 6=roaming/free.
            // We avoid fighting 1/2, and we keep retrying spec_mode 6 a few times because
            // some servers/joins ignore early client commands during connection/spawn.
            if (obsMode == 6)
            {
                m_ObserverForcedFreeCamThisObserver = true; // success; stop retrying
            }
            else if (!m_ObserverForcedFreeCamThisObserver && m_Game->m_EngineClient->IsInGame() && obsMode >= 3)
            {
                constexpr int kMaxAttempts = 10;
                constexpr auto kRetryInterval = std::chrono::milliseconds(350);
                auto now = std::chrono::steady_clock::now();
                if (m_ObserverFreeCamAttemptCount < kMaxAttempts && (now - m_ObserverLastFreeCamAttempt) >= kRetryInterval)
                {
                    m_Game->ClientCmd_Unrestricted("spec_mode 6");
                    m_ObserverLastFreeCamAttempt = now;
                    m_ObserverFreeCamAttemptCount++;
                }
            }

            m_ObserverWasActivePrev = true;
        }
    }

    // Mounted gun (.50cal/minigun):
    // - Render hook forces first-person while mounted.
    // - When we exit the mounted gun, do a one-shot ResetPosition to re-align anchors.
    {
        const bool usingMountedGunNow = IsUsingMountedGun(localPlayer);
        if (m_UsingMountedGunPrev && !usingMountedGunNow)
            m_ResetPositionAfterMountedGunExitPending = true;
        m_UsingMountedGunPrev = usingMountedGunNow;
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

    // Mouse-mode yaw smoothing (scheme A): delta-drain.
    // - CreateMove converts cmd->mousedx to a yaw delta (degrees) and accumulates it into
    //   m_MouseModeYawDeltaRemainingDeg.
    // - UpdateTracking runs at VR render rate and applies a fraction of the remaining delta per frame.
    //   This prevents the "inertial coast" feeling (fast flick -> extra turning after the mouse stops),
    //   because we never apply more rotation than what was actually accumulated from the mouse.
    if (m_MouseModeEnabled)
    {
        if (m_MouseModeTurnSmoothing > 0.0f)
        {
            const float dt = std::max(0.0f, m_LastFrameDuration);
            const float tau = std::max(0.0001f, m_MouseModeTurnSmoothing);
            const float alpha = 1.0f - expf(-dt / tau);

            if (!m_MouseModeYawDeltaInitialized)
            {
                m_MouseModeYawDeltaRemainingDeg = 0.0f;
                m_MouseModeYawDeltaInitialized = true;
            }

            // Drain a fraction of the remaining yaw delta each frame.
            float step = m_MouseModeYawDeltaRemainingDeg * alpha;

            // Snap tiny remainders to zero to avoid denormal drift.
            if (fabsf(m_MouseModeYawDeltaRemainingDeg) < 0.00001f)
            {
                m_MouseModeYawDeltaRemainingDeg = 0.0f;
                step = 0.0f;
            }
            else
            {
                m_MouseModeYawDeltaRemainingDeg -= step;
            }

            m_RotationOffset += step;
            // Wrap to [0, 360)
            m_RotationOffset -= 360.0f * std::floor(m_RotationOffset / 360.0f);
        }
        else
        {
            // Legacy: yaw was already applied directly on CreateMove ticks.
            m_MouseModeYawDeltaRemainingDeg = 0.0f;
            m_MouseModeYawDeltaInitialized = false;
        }
    }
    else
    {
        m_MouseModeYawDeltaRemainingDeg = 0.0f;
        m_MouseModeYawDeltaInitialized = false;
        m_MouseModeYawTargetInitialized = false; // legacy field
    }

    // Mouse-mode pitch smoothing (aim pitch + optional view pitch):
    // - cmd->mousedy arrives at CreateMove rate (tickrate), but VR rendering/tracking updates faster.
    // - If we apply pitch only on CreateMove ticks, aiming up/down feels like it's "stuttering".
    // So: CreateMove updates pitch targets, and here we smoothly converge per-frame.
    if (m_MouseModeEnabled)
    {
        if (!m_MouseModePitchTargetInitialized)
        {
            m_MouseModePitchTarget = m_MouseAimPitchOffset;
            m_MouseModePitchTargetInitialized = true;
        }
        if (!m_MouseModeViewPitchTargetOffsetInitialized)
        {
            m_MouseModeViewPitchTargetOffset = m_MouseModeViewPitchOffset;
            m_MouseModeViewPitchTargetOffsetInitialized = true;
        }

        if (m_MouseModePitchSmoothing > 0.0f)
        {
            const float dt = std::max(0.0f, m_LastFrameDuration);
            const float tau = std::max(0.0001f, m_MouseModePitchSmoothing);
            const float alpha = 1.0f - expf(-dt / tau);

            m_MouseAimPitchOffset += (m_MouseModePitchTarget - m_MouseAimPitchOffset) * alpha;

            if (m_MouseModePitchAffectsView)
            {
                m_MouseModeViewPitchOffset += (m_MouseModeViewPitchTargetOffset - m_MouseModeViewPitchOffset) * alpha;
            }
        }
        else
        {
            // Smoothing disabled: keep legacy immediate behavior.
            m_MouseAimPitchOffset = m_MouseModePitchTarget;
            if (m_MouseModePitchAffectsView)
                m_MouseModeViewPitchOffset = m_MouseModeViewPitchTargetOffset;
        }

        if (m_MouseAimPitchOffset > 89.f)  m_MouseAimPitchOffset = 89.f;
        if (m_MouseAimPitchOffset < -89.f) m_MouseAimPitchOffset = -89.f;
    }
    else
    {
        m_MouseModePitchTargetInitialized = false;
        m_MouseModeViewPitchTargetOffsetInitialized = false;
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

    // Mouse-mode pitch: optional view tilt driven by mouse Y.
    if (m_MouseModeEnabled && m_MouseModePitchAffectsView)
    {
        hmdAngLocal.x += m_MouseModeViewPitchOffset;
        // Wrap to [-180, 180]
        hmdAngLocal.x -= 360 * std::floor((hmdAngLocal.x + 180) / 360);
        // Keep pitch in a sane range (Source viewangles expect [-89, 89]).
        if (hmdAngLocal.x > 89.f)  hmdAngLocal.x = 89.f;
        if (hmdAngLocal.x < -89.f) hmdAngLocal.x = -89.f;
    }

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
    // If we just exited a mounted gun (.50cal/minigun), re-align anchors.
   // Do this here (after m_HmdPosAbs has been updated for this frame) so the
   // reset uses the latest tracking pose.
    if (m_ResetPositionAfterMountedGunExitPending)
    {
        ResetPosition();
        m_ResetPositionAfterMountedGunExitPending = false;
    }
    m_HmdAngAbs = hmdAngSmoothed;

    // Mouse aim initialization: decouple aim pitch from HMD pitch.
    if (m_MouseModeEnabled)
    {
        if (!m_MouseAimInitialized)
        {
            m_MouseAimPitchOffset = m_HmdAngAbs.x;
            m_MouseModeViewPitchOffset = 0.0f;
            m_MouseAimInitialized = true;

            // Initialize targets to avoid a one-frame jump when smoothing is enabled.
            m_MouseModePitchTarget = m_MouseAimPitchOffset;
            m_MouseModePitchTargetInitialized = true;
            m_MouseModeViewPitchTargetOffset = m_MouseModeViewPitchOffset;
            m_MouseModeViewPitchTargetOffsetInitialized = true;
        }
        // Clamp to sane pitch range.
        if (m_MouseAimPitchOffset > 89.f)  m_MouseAimPitchOffset = 89.f;
        if (m_MouseAimPitchOffset < -89.f) m_MouseAimPitchOffset = -89.f;
    }
    else
    {
        m_MouseAimInitialized = false;
        m_MouseModeViewPitchOffset = 0.0f;
        m_MouseModePitchTargetInitialized = false;
        m_MouseModeViewPitchTargetOffsetInitialized = false;
    }

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
            float lerpSetting = m_SpecialInfectedAutoAimLerp;
            if (std::chrono::steady_clock::now() < m_SpecialInfectedRunCommandShotAimUntil)
                lerpSetting = std::max(lerpSetting, m_SpecialInfectedRunCommandShotLerp);

            const float lerpFactor = m_SpecialInfectedDebug
                ? std::max(0.0f, lerpSetting)
                : std::clamp(lerpSetting, 0.0f, 1.0f);
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
    if (m_MouseModeEnabled && m_ScopeEnabled)
    {
        // Mouse mode: scope activation is driven by a keyboard toggle (not look-through).
        m_ScopeActive = m_MouseModeScopeToggleActive && m_ScopeWeaponIsFirearm;
        if (m_ScopeActive)
        {
            const Vector& anchor = m_MouseModeScopedViewmodelAnchorOffset;
            const Vector gunOrigin = m_HmdPosAbs
                + (m_HmdForward * (anchor.x * m_VRScale))
                + (m_HmdRight * (anchor.y * m_VRScale))
                + (m_HmdUp * (anchor.z * m_VRScale));

            // Mouse-mode scope aiming must use the same yaw basis as the viewmodel/bullets.
            // - When MouseModeAimFromHmd is true: aim follows the HMD center ray.
            // - Otherwise: aim follows mouse pitch  body yaw (m_RotationOffset), independent of head yaw.
            Vector eyeDir;
            GetMouseModeEyeRay(eyeDir);

            const float converge = std::max(0.0f, m_MouseModeAimConvergeDistance);
            const Vector target = gunOrigin + eyeDir * (converge > 0.0f ? converge : 2048.0f);

            Vector aimDir = target - gunOrigin;
            if (aimDir.IsZero())
                aimDir = m_HmdForward;
            VectorNormalize(aimDir);

            QAngle aimAng;
            QAngle::VectorAngles(aimDir, m_HmdUp, aimAng);

            Vector f, r, u;
            QAngle::AngleVectors(aimAng, &f, &r, &u);
            m_RightControllerForward = f;
            m_RightControllerRight = r;
            m_RightControllerUp = u;

            m_ScopeCameraPosAbs = gunOrigin
                + f * m_ScopeCameraOffset.x
                + r * m_ScopeCameraOffset.y
                + u * m_ScopeCameraOffset.z;
            m_ScopeCameraAngAbs = aimAng + m_ScopeCameraAngleOffset;
        }
        else
        {
            m_ScopeAimSensitivityInit = false;
            m_ScopeStabilizationInit = false;
            m_ScopeStabilizationLastTime = {};
        }
    }
    else if (m_ScopeEnabled && m_ScopeWeaponIsFirearm)
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

        // Keep a working copy of the pose used for rendering. Look-through activation above is based on raw pose.
        Vector scopePosFinal = scopePosRaw;
        QAngle scopeAngFinal = scopeAngRaw;

        // Scoped aim sensitivity scaling: apply to controller aim so scope camera, aim line and bullets stay in sync.
        if (m_ScopeActive)
        {
            QAngle aimAngRaw;
            QAngle::VectorAngles(m_RightControllerForward, m_RightControllerUp, aimAngRaw);

            if (!m_ScopeAimSensitivityInit)
            {
                m_ScopeAimSensitivityInit = true;
                m_ScopeAimSensitivityBaseAng = aimAngRaw;
            }

            float gain = 1.0f;
            if (!m_ScopeAimSensitivityScales.empty())
            {
                const size_t idx = std::min(m_ScopeMagnificationIndex, m_ScopeAimSensitivityScales.size() - 1);
                gain = std::clamp(m_ScopeAimSensitivityScales[idx], 0.05f, 1.0f);
            }

            if (gain < 0.999f)
            {
                auto wrapDelta = [](float d) -> float
                    {
                        d -= 360.0f * std::floor((d + 180.0f) / 360.0f);
                        return d;
                    };

                QAngle aimAngScaled =
                {
                    wrapAngle(m_ScopeAimSensitivityBaseAng.x + wrapDelta(aimAngRaw.x - m_ScopeAimSensitivityBaseAng.x) * gain),
                    wrapAngle(m_ScopeAimSensitivityBaseAng.y + wrapDelta(aimAngRaw.y - m_ScopeAimSensitivityBaseAng.y) * gain),
                    wrapAngle(m_ScopeAimSensitivityBaseAng.z + wrapDelta(aimAngRaw.z - m_ScopeAimSensitivityBaseAng.z) * gain)
                };

                Vector f, r, u;
                QAngle::AngleVectors(aimAngScaled, &f, &r, &u);
                if (!f.IsZero()) VectorNormalize(f);
                if (!r.IsZero()) VectorNormalize(r);
                if (!u.IsZero()) VectorNormalize(u);

                m_RightControllerForward = f;
                m_RightControllerRight = r;
                m_RightControllerUp = u;

                // While scoped-in, keep the "unforced" aim direction consistent too (used by aim line in 3P).
                m_RightControllerForwardUnforced = m_RightControllerForward;
                if (!m_RightControllerForwardUnforced.IsZero())
                    m_LastUnforcedAimDirection = m_RightControllerForwardUnforced;

                // Recompute scope render pose from the scaled controller basis.
                scopePosFinal = m_RightControllerPosAbs
                    + m_RightControllerForward * m_ScopeCameraOffset.x
                    + m_RightControllerRight * m_ScopeCameraOffset.y
                    + m_RightControllerUp * m_ScopeCameraOffset.z;

                QAngle::VectorAngles(m_RightControllerForward, m_RightControllerUp, scopeAngFinal);
                scopeAngFinal.x += m_ScopeCameraAngleOffset.x;
                scopeAngFinal.y += m_ScopeCameraAngleOffset.y;
                scopeAngFinal.z += m_ScopeCameraAngleOffset.z;
                scopeAngFinal.x = wrapAngle(scopeAngFinal.x);
                scopeAngFinal.y = wrapAngle(scopeAngFinal.y);
                scopeAngFinal.z = wrapAngle(scopeAngFinal.z);

                m_ScopeCameraPosAbs = scopePosFinal;
                m_ScopeCameraAngAbs = scopeAngFinal;
            }
        }
        else
        {
            m_ScopeAimSensitivityInit = false;
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
                m_ScopeStabPos = scopePosFinal;
                m_ScopeStabPosDeriv = { 0.0f, 0.0f, 0.0f };
                m_ScopeStabAng = scopeAngFinal;
                m_ScopeStabAngDeriv = { 0.0f, 0.0f, 0.0f };
            }
            m_ScopeStabilizationLastTime = now;

            // Slightly increase smoothing at very low FOV (high magnification).
            const float fovScale = std::clamp(m_ScopeFov / 20.0f, 0.35f, 1.25f);
            const float minCutoff = std::max(0.0001f, m_ScopeStabilizationMinCutoff * fovScale);

            OneEuroFilterVec3(scopePosFinal, m_ScopeStabPos, m_ScopeStabPosDeriv, m_ScopeStabilizationInit,
                dt, minCutoff, m_ScopeStabilizationBeta, m_ScopeStabilizationDCutoff);

            OneEuroFilterAngles(scopeAngFinal, m_ScopeStabAng, m_ScopeStabAngDeriv, m_ScopeStabilizationInit,
                dt, minCutoff, m_ScopeStabilizationBeta, m_ScopeStabilizationDCutoff);

            m_ScopeCameraPosAbs = m_ScopeStabPos;
            m_ScopeCameraAngAbs = m_ScopeStabAng;
        }
        else
        {
            m_ScopeStabilizationInit = false;
            m_ScopeStabilizationLastTime = {};
        }


        // Final sync: while scoped-in, force the gameplay aim basis to match the scope camera.
        // This keeps aim line, bullets, and scope RTT perfectly consistent (including stabilization + angle offsets).
        if (m_ScopeActive)
        {
            Vector f, r, u;
            QAngle::AngleVectors(m_ScopeCameraAngAbs, &f, &r, &u);
            if (!f.IsZero()) VectorNormalize(f);
            if (!r.IsZero()) VectorNormalize(r);
            if (!u.IsZero()) VectorNormalize(u);
            m_RightControllerForward = f;
            m_RightControllerRight = r;
            m_RightControllerUp = u;

            // Keep the unforced direction consistent for 3P aim line code.
            m_RightControllerForwardUnforced = m_RightControllerForward;
            if (!m_RightControllerForwardUnforced.IsZero())
                m_LastUnforcedAimDirection = m_RightControllerForwardUnforced;
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

    if (m_MouseModeEnabled)
    {
        // Mouse mode viewmodel orientation:
        //  - Default: aim is driven by mouse pitch + body yaw.
        //  - Optional (MouseModeAimFromHmd): aim follows the HMD center ray, but the aim line
        //    and viewmodel origin remain at the mouse-mode viewmodel anchor.
        Vector eyeDir;
        GetMouseModeEyeRay(eyeDir);

        const Vector& anchor = IsMouseModeScopeActive() ? m_MouseModeScopedViewmodelAnchorOffset : m_MouseModeViewmodelAnchorOffset;
        Vector gunOrigin = m_HmdPosAbs
            + (m_HmdForward * (anchor.x * m_VRScale))
            + (m_HmdRight * (anchor.y * m_VRScale))
            + (m_HmdUp * (anchor.z * m_VRScale));

        const float convergeDist = (m_MouseModeAimConvergeDistance > 0.0f) ? m_MouseModeAimConvergeDistance : 8192.0f;
        Vector target = m_HmdPosAbs + eyeDir * convergeDist;

        Vector aimDir = target - gunOrigin;
        if (aimDir.IsZero())
            aimDir = eyeDir;

        if (aimDir.IsZero())
        {
            m_ViewmodelForward = m_RightControllerForward;
            m_ViewmodelUp = m_RightControllerUp;
            m_ViewmodelRight = m_RightControllerRight;
        }
        else
        {
            VectorNormalize(aimDir);
            QAngle aimAng;
            QAngle::VectorAngles(aimDir, aimAng);
            NormalizeAndClampViewAngles(aimAng);

            QAngle::AngleVectors(aimAng, &m_ViewmodelForward, &m_ViewmodelRight, &m_ViewmodelUp);
        }
    }
    else
    {
        m_ViewmodelForward = m_RightControllerForward;
        m_ViewmodelUp = m_RightControllerUp;
        m_ViewmodelRight = m_RightControllerRight;
    }

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

    const Vector leftForwardHorizontal{ m_LeftControllerForward.x, m_LeftControllerForward.y, 0.0f };
    const float leftForwardHorizontalLength = VectorLength(leftForwardHorizontal);
    const Vector leftForwardHorizontalNorm = leftForwardHorizontalLength > 0.0f
        ? leftForwardHorizontal / leftForwardHorizontalLength
        : Vector(0.0f, 0.0f, 0.0f);

    const float leftOutwardHorizontalSpeed = std::max(0.0f, DotProduct(leftDelta, leftForwardHorizontalNorm)) / deltaSeconds;
    const float leftHorizontalSpeed = VectorLength(Vector(leftDelta.x, leftDelta.y, 0.0f)) / deltaSeconds;
    const float leftOutwardSpeed = leftForwardHorizontalLength > 0.01f ? leftOutwardHorizontalSpeed : leftHorizontalSpeed;
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

bool VR::IsUsingMountedGun(const C_BasePlayer* localPlayer) const
{
    if (!localPlayer)
        return false;

    // L4D2 uses two adjacent netvars for mounted weapons:
    // - m_usingMountedGun: typically .50cal
    // - m_usingMountedWeapon: typically minigun/gatling
    const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
    const unsigned char usingGun = *reinterpret_cast<const unsigned char*>(base + kUsingMountedGunOffset);
    const unsigned char usingWeapon = *reinterpret_cast<const unsigned char*>(base + kUsingMountedWeaponOffset);
    return (usingGun | usingWeapon) != 0;
}

C_BaseEntity* VR::GetMountedGunUseEntity(C_BasePlayer* localPlayer) const
{
    if (!IsUsingMountedGun(localPlayer) || !m_Game || !m_Game->m_ClientEntityList)
        return nullptr;

    const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
    const uint32_t hUse = *reinterpret_cast<const uint32_t*>(base + kUseEntityHandleOffset);

    // EHANDLE / CBaseHandle is invalid when 0 or 0xFFFFFFFF (common patterns across Source builds).
    if (hUse == 0u || hUse == 0xFFFFFFFFu)
        return nullptr;

    return reinterpret_cast<C_BaseEntity*>(m_Game->m_ClientEntityList->GetClientEntityFromHandle(hUse));
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

    C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);

    if (m_MouseModeEnabled)
    {
        const Vector eye = localPlayer->EyePosition();
        QAngle eyeAng;
        Vector eyeDir;
        GetMouseModeEyeRay(eyeDir, &eyeAng);

        if (eyeDir.IsZero())
        {
            m_HasNonVRAimSolution = false;
            return;
        }

        const float maxDistance = 8192.0f;

        Vector endEye = eye + eyeDir * maxDistance;
        CGameTrace traceH;
        Ray_t rayH;
        CTraceFilterSkipSelf tracefilterSelf((IHandleEntity*)localPlayer, 0);
        CTraceFilterSkipTwoEntities tracefilterTwo((IHandleEntity*)localPlayer, (IHandleEntity*)mountedUseEnt, 0);
        CTraceFilter* pTraceFilter = mountedUseEnt ? static_cast<CTraceFilter*>(&tracefilterTwo) : static_cast<CTraceFilter*>(&tracefilterSelf);
        rayH.Init(eye, endEye);
        m_Game->m_EngineTrace->TraceRay(rayH, STANDARD_TRACE_MASK, pTraceFilter, &traceH);

        const Vector H = (traceH.fraction < 1.0f && traceH.fraction > 0.0f) ? traceH.endpos : endEye;
        m_NonVRAimHitPoint = H;

        const float convergeDist = (m_MouseModeAimConvergeDistance > 0.0f)
            ? std::min(m_MouseModeAimConvergeDistance, maxDistance)
            : maxDistance;
        m_NonVRAimDesiredPoint = eye + eyeDir * convergeDist;

        m_NonVRAimAngles = eyeAng;
        m_HasNonVRAimSolution = true;
        return;
    }

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
    // Keep non-3P codepath identical to legacy behavior; only use the new render-center delta in 3P.
    Vector camDelta = m_IsThirdPersonCamera
        ? (m_ThirdPersonRenderCenter - m_SetupOrigin)
        : (m_ThirdPersonViewOrigin - m_SetupOrigin);
    if (m_IsThirdPersonCamera && camDelta.LengthSqr() > (5.0f * 5.0f))
        originBase += camDelta;

    Vector origin = originBase + direction * 2.0f;

    const float maxDistance = 8192.0f;
    Vector target = origin + direction * maxDistance;

    // 1) Controller ray -> P
    CGameTrace traceP;
    Ray_t rayP;
    CTraceFilterSkipSelf tracefilterSelf((IHandleEntity*)localPlayer, 0);
    CTraceFilterSkipTwoEntities tracefilterTwo((IHandleEntity*)localPlayer, (IHandleEntity*)mountedUseEnt, 0);
    CTraceFilter* pTraceFilter = mountedUseEnt ? static_cast<CTraceFilter*>(&tracefilterTwo) : static_cast<CTraceFilter*>(&tracefilterSelf);
    rayP.Init(origin, target);
    m_Game->m_EngineTrace->TraceRay(rayP, STANDARD_TRACE_MASK, pTraceFilter, &traceP);

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
    m_Game->m_EngineTrace->TraceRay(rayH, STANDARD_TRACE_MASK, pTraceFilter, &traceH);

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

bool VR::UpdateFriendlyFireAimHit(C_BasePlayer* localPlayer)
{
    m_AimLineHitsFriendly = false;
    if (!m_BlockFireOnFriendlyAimEnabled)
        return false;
    if (!localPlayer || !m_Game || !m_Game->m_EngineTrace)
        return false;

    C_WeaponCSBase* activeWeapon = static_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());

    if (!activeWeapon || IsThrowableWeapon(activeWeapon))
        return false;

    C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);

    const bool useMouse = m_MouseModeEnabled;

    // Eye-center ray direction (mouse aim in mouse mode).
    Vector eyeDir{ 0.0f, 0.0f, 0.0f };
    if (useMouse)
        GetMouseModeEyeRay(eyeDir);

    // ---- Build the "aim line" (gun/hand) ray (existing behavior) ----
    Vector gunDir = m_RightControllerForward;
    if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
        gunDir = m_RightControllerForwardUnforced;

    Vector gunOrigin = m_RightControllerPosAbs;
    if (useMouse)
    {
        const Vector& anchor = IsMouseModeScopeActive() ? m_MouseModeScopedViewmodelAnchorOffset : m_MouseModeViewmodelAnchorOffset;
        gunOrigin = m_HmdPosAbs
            + (m_HmdForward * (anchor.x * m_VRScale))
            + (m_HmdRight * (anchor.y * m_VRScale))
            + (m_HmdUp * (anchor.z * m_VRScale));

        const float convergeDist = (m_MouseModeAimConvergeDistance > 0.0f) ? m_MouseModeAimConvergeDistance : 8192.0f;
        Vector target = m_HmdPosAbs + eyeDir * convergeDist;
        gunDir = target - gunOrigin;
    }

    if (gunDir.IsZero())
        return false;
    VectorNormalize(gunDir);

    Vector gunOriginBase = gunOrigin;
    // Keep non-3P codepath identical to legacy behavior; only use the new render-center delta in 3P.
    Vector camDelta = m_IsThirdPersonCamera
        ? (m_ThirdPersonRenderCenter - m_SetupOrigin)
        : (m_ThirdPersonViewOrigin - m_SetupOrigin);
    if (m_IsThirdPersonCamera && camDelta.LengthSqr() > (5.0f * 5.0f))
        gunOriginBase += camDelta;

    Vector gunStart = gunOriginBase + gunDir * 2.0f;
    Vector gunEnd = gunStart + gunDir * 8192.0f;

    // Filters (shared across traces).
    CTraceFilterSkipSelf tracefilterSelf((IHandleEntity*)localPlayer, 0);
    CTraceFilterSkipTwoEntities tracefilterTwo((IHandleEntity*)localPlayer, (IHandleEntity*)mountedUseEnt, 0);
    CTraceFilter* pTraceFilter = mountedUseEnt ? static_cast<CTraceFilter*>(&tracefilterTwo) : static_cast<CTraceFilter*>(&tracefilterSelf);

    auto hasValidHandle = [](uint32_t h) -> bool
        {
            // EHANDLE / CBaseHandle is invalid when 0 or 0xFFFFFFFF (common patterns across Source builds).
            return h != 0u && h != 0xFFFFFFFFu;
        };

    struct ControlHandles
    {
        uint32_t tongueOwner{};
        uint32_t pummelAttacker{};
        uint32_t carryAttacker{};
        uint32_t pounceAttacker{};
        uint32_t jockeyAttacker{};
    };

    auto hasAnyControlHandle = [&](const ControlHandles& h) -> bool
        {
            return hasValidHandle(h.tongueOwner)
                || hasValidHandle(h.pummelAttacker)
                || hasValidHandle(h.carryAttacker)
                || hasValidHandle(h.pounceAttacker)
                || hasValidHandle(h.jockeyAttacker);
        };

    auto getControlHandles = [&](C_BaseEntity* ent) -> ControlHandles
        {
            ControlHandles h{};
            if (!ent)
                return h;

            const unsigned char* base = reinterpret_cast<const unsigned char*>(ent);
            h.tongueOwner = *reinterpret_cast<const uint32_t*>(base + kTongueOwnerOffset);
            h.pummelAttacker = *reinterpret_cast<const uint32_t*>(base + kPummelAttackerOffset);
            h.carryAttacker = *reinterpret_cast<const uint32_t*>(base + kCarryAttackerOffset);
            h.pounceAttacker = *reinterpret_cast<const uint32_t*>(base + kPounceAttackerOffset);
            h.jockeyAttacker = *reinterpret_cast<const uint32_t*>(base + kJockeyAttackerOffset);
            return h;
        };

    auto isSameTeamAlivePlayer = [&](C_BaseEntity* a, C_BaseEntity* b) -> bool
        {
            if (!a || !b)
                return false;
            if (b->IsPlayer() == nullptr)
                return false;

            const unsigned char* aBase = reinterpret_cast<const unsigned char*>(a);
            const unsigned char* bBase = reinterpret_cast<const unsigned char*>(b);
            const int aTeam = *reinterpret_cast<const int*>(aBase + kTeamNumOffset);
            const int bTeam = *reinterpret_cast<const int*>(bBase + kTeamNumOffset);
            return (aTeam != 0 && aTeam == bTeam && IsEntityAlive(b));
        };

    auto resolveHandleEntity = [&](uint32_t h) -> C_BaseEntity*
        {
            if (!hasValidHandle(h) || !m_Game || !m_Game->m_ClientEntityList)
                return nullptr;
            return reinterpret_cast<C_BaseEntity*>(m_Game->m_ClientEntityList->GetClientEntityFromHandle(h));
        };

    // Evaluate: does this trace mean "friendly is in the way"?
    // IMPORTANT: In remote servers, the authoritative bullet trace is often closer to an EYE ray
    // (plus lag compensation), while our aim line is a GUN/HAND ray. To reduce "first few shots"
    // leaking through, we treat either ray hitting a teammate as a block.
    auto evalFriendlyHitForTrace = [&](const CGameTrace& tr1, const Vector& start, const Vector& end) -> bool
        {
            C_BaseEntity* hitEnt = reinterpret_cast<C_BaseEntity*>(tr1.m_pEnt);
            if (!hitEnt || hitEnt == localPlayer || !isSameTeamAlivePlayer(localPlayer, hitEnt))
                return false;

            // Default behavior: block when the ray hits a teammate.
            bool friendlyHit = true;

            // Tightened special-case (“被控队友放行”):
            // Only when teammate is ACTUALLY controlled (tongue/pummel/carry/pounce/jockey),
            // and the second trace hits the controller entity itself,
            // and that hit point is very close to the teammate hit point.
            const ControlHandles ctrl = getControlHandles(hitEnt);
            if (hasAnyControlHandle(ctrl))
            {
                CGameTrace tr2;
                Ray_t ray2;

                // If we're on a mounted gun, also skip the mounted gun entity itself so we can "see through"
                // both the teammate and the turret when checking what's behind them.
                CTraceFilterSkipTwoEntities tracefilter2((IHandleEntity*)localPlayer, (IHandleEntity*)hitEnt, 0);
                CTraceFilterSkipThreeEntities tracefilter3((IHandleEntity*)localPlayer, (IHandleEntity*)hitEnt, (IHandleEntity*)mountedUseEnt, 0);
                CTraceFilter* pTraceFilter2 = (mountedUseEnt && mountedUseEnt != hitEnt)
                    ? static_cast<CTraceFilter*>(&tracefilter3)
                    : static_cast<CTraceFilter*>(&tracefilter2);

                ray2.Init(start, end);
                m_Game->m_EngineTrace->TraceRay(ray2, STANDARD_TRACE_MASK, pTraceFilter2, &tr2);

                const Vector hitPos1 = (tr1.fraction < 1.0f && tr1.fraction > 0.0f) ? tr1.endpos : hitEnt->GetAbsOrigin();
                const Vector hitPos2 = (tr2.fraction < 1.0f && tr2.fraction > 0.0f) ? tr2.endpos : hitPos1;
                const float distSqr = (hitPos2 - hitPos1).LengthSqr();

                if (distSqr <= (kAllowThroughControlledTeammateMaxDist * kAllowThroughControlledTeammateMaxDist))
                {
                    C_BaseEntity* hitEnt2 = reinterpret_cast<C_BaseEntity*>(tr2.m_pEnt);
                    if (hitEnt2 && hitEnt2 != localPlayer)
                    {
                        C_BaseEntity* a1 = resolveHandleEntity(ctrl.tongueOwner);
                        C_BaseEntity* a2 = resolveHandleEntity(ctrl.pummelAttacker);
                        C_BaseEntity* a3 = resolveHandleEntity(ctrl.carryAttacker);
                        C_BaseEntity* a4 = resolveHandleEntity(ctrl.pounceAttacker);
                        C_BaseEntity* a5 = resolveHandleEntity(ctrl.jockeyAttacker);

                        if (hitEnt2 == a1 || hitEnt2 == a2 || hitEnt2 == a3 || hitEnt2 == a4 || hitEnt2 == a5)
                            friendlyHit = false;
                    }
                }
            }

            return friendlyHit;
        };

    // 1) Gun/hand ray (aim line)
    CGameTrace traceGun;
    Ray_t rayGun;
    rayGun.Init(gunStart, gunEnd);
    m_Game->m_EngineTrace->TraceRay(rayGun, STANDARD_TRACE_MASK, pTraceFilter, &traceGun);
    const bool friendlyGun = evalFriendlyHitForTrace(traceGun, gunStart, gunEnd);

    // 2) Eye ray (closer to authoritative server bullets, esp. with lag compensation)
    Vector eye = localPlayer->EyePosition();

    // Keep NonVR aim solution fresh when enabled; this is throttled internally.
    if (m_ForceNonVRServerMovement)
        UpdateNonVRAimSolution(localPlayer);

    Vector eyeTarget = gunEnd;

    if (useMouse)
    {
        if (!eyeDir.IsZero())
            eyeTarget = eye + eyeDir * 8192.0f;
    }
    else
    {
        // When ForceNonVRServerMovement is enabled, we already solved for a point P that the
        // controller ray wants, and an eye-ray towards P that better matches server logic.
        if (m_ForceNonVRServerMovement && m_HasNonVRAimSolution)
            eyeTarget = m_NonVRAimDesiredPoint;
    }

    Vector eyeDir2 = eyeTarget - eye;
    if (!eyeDir2.IsZero())
    {
        VectorNormalize(eyeDir2);
        Vector eyeStart = eye + eyeDir2 * 2.0f;
        Vector eyeEnd = eyeStart + eyeDir2 * 8192.0f;

        CGameTrace traceEye;
        Ray_t rayEye;
        rayEye.Init(eyeStart, eyeEnd);
        m_Game->m_EngineTrace->TraceRay(rayEye, STANDARD_TRACE_MASK, pTraceFilter, &traceEye);

        const bool friendlyEye = evalFriendlyHitForTrace(traceEye, eyeStart, eyeEnd);

        m_AimLineHitsFriendly = (friendlyGun || friendlyEye);
        return m_AimLineHitsFriendly;
    }

    m_AimLineHitsFriendly = friendlyGun;
    return m_AimLineHitsFriendly;
}

bool VR::ShouldSuppressPrimaryFire(const CUserCmd* cmd, C_BasePlayer* localPlayer)
{
    if (!m_BlockFireOnFriendlyAimEnabled)
    {
        m_FriendlyFireGuardLatched = false;
        return false;
    }

    const bool attackDown = (cmd != nullptr) && ((cmd->buttons & (1 << 0)) != 0); // IN_ATTACK

    // Keep the hit result current on input ticks (CreateMove).
    if (localPlayer)
        UpdateFriendlyFireAimHit(localPlayer);

    const bool friendlyNow = m_AimLineHitsFriendly;

    // Latch until attack is released: prevents “pause then resume firing” while still holding.
    if (m_FriendlyFireGuardLatched)
    {
        if (!attackDown)
            m_FriendlyFireGuardLatched = false;
        return true;
    }

    if (attackDown && friendlyNow)
    {
        m_FriendlyFireGuardLatched = true;
        return true;
    }

    return false;
}

void VR::UpdateAimingLaser(C_BasePlayer* localPlayer)
{
    UpdateSpecialInfectedWarningState();
    UpdateSpecialInfectedPreWarningState();

    const bool canDraw = (m_Game->m_DebugOverlay != nullptr);


    C_WeaponCSBase* activeWeapon = nullptr;
    if (localPlayer)
        activeWeapon = static_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());
    const bool allowAimLineDraw = ShouldDrawAimLine(activeWeapon);
    if (!ShouldShowAimLine(activeWeapon))
    {
        m_LastAimDirection = Vector{ 0.0f, 0.0f, 0.0f };
        m_HasAimLine = false;
        m_HasAimConvergePoint = false;
        m_HasThrowArc = false;
        m_LastAimWasThrowable = false;

        // Even when the aim line is hidden/disabled, still run the friendly-fire guard trace
        // if the user toggled it on.
        UpdateFriendlyFireAimHit(localPlayer);

        return;
    }

    // If debug overlay isn't available, don't draw, but keep the guard working.
    if (!canDraw)
    {
        UpdateFriendlyFireAimHit(localPlayer);

        return;
    }

    bool isThrowable = IsThrowableWeapon(activeWeapon);
    const bool useMouse = m_MouseModeEnabled;

    // Eye-center ray direction (mouse aim in mouse mode).
    Vector eyeDir = { 0.0f, 0.0f, 0.0f };
    if (useMouse)
        GetMouseModeEyeRay(eyeDir);

    // Aim direction:
    //  - Normal mode: controller forward (existing behavior).
    //  - Mouse mode (scheme B): start at the viewmodel anchor, but steer the ray to converge
    //    to the eye-center ray at MouseModeAimConvergeDistance.
    Vector direction = m_RightControllerForward;
    if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
        direction = m_RightControllerForwardUnforced;

    if (useMouse)
    {
        const Vector& anchor = IsMouseModeScopeActive() ? m_MouseModeScopedViewmodelAnchorOffset : m_MouseModeViewmodelAnchorOffset;
        Vector gunOrigin = m_HmdPosAbs
            + (m_HmdForward * (anchor.x * m_VRScale))
            + (m_HmdRight * (anchor.y * m_VRScale))
            + (m_HmdUp * (anchor.z * m_VRScale));

        const float convergeDist = (m_MouseModeAimConvergeDistance > 0.0f) ? m_MouseModeAimConvergeDistance : 8192.0f;
        Vector target = m_HmdPosAbs + eyeDir * convergeDist;

        direction = target - gunOrigin;
    }
    if (direction.IsZero())
    {
        if (m_LastAimDirection.IsZero())
        {
            const float duration = std::max(m_AimLinePersistence, m_LastFrameDuration * m_AimLineFrameDurationMultiplier);

            if (m_LastAimWasThrowable && m_HasThrowArc)
            {
                DrawThrowArcFromCache(duration);
                m_AimLineHitsFriendly = false;
                return;
            }

            if (!m_HasAimLine)
            {
                m_AimLineHitsFriendly = false;
                return;
            }


            if (allowAimLineDraw)
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
    if (useMouse)
    {
        const Vector& anchor = IsMouseModeScopeActive() ? m_MouseModeScopedViewmodelAnchorOffset : m_MouseModeViewmodelAnchorOffset;
        originBase = m_HmdPosAbs
            + (m_HmdForward * (anchor.x * m_VRScale))
            + (m_HmdRight * (anchor.y * m_VRScale))
            + (m_HmdUp * (anchor.z * m_VRScale));
    }
    // Keep non-3P codepath identical to legacy behavior; only use the new render-center delta in 3P.
    Vector camDelta = m_IsThirdPersonCamera
        ? (m_ThirdPersonRenderCenter - m_SetupOrigin)
        : (m_ThirdPersonViewOrigin - m_SetupOrigin);
    if (m_IsThirdPersonCamera && camDelta.LengthSqr() > (5.0f * 5.0f))
        originBase += camDelta;

    Vector origin = originBase + direction * 2.0f;

    if (isThrowable)
    {
        m_AimLineHitsFriendly = false;
        m_HasAimConvergePoint = false;

        Vector pitchSource = direction;
        if (useMouse && !eyeDir.IsZero())
            pitchSource = eyeDir;
        else if (!m_ForceNonVRServerMovement && !m_HmdForward.IsZero())
            pitchSource = m_HmdForward;

        DrawThrowArc(origin, direction, pitchSource);
        return;
    }

    const float maxDistance = 8192.0f;
    Vector target = origin + direction * maxDistance;


    if (!m_IsThirdPersonCamera && m_ForceNonVRServerMovement && m_HasNonVRAimSolution)
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
            C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);
            CTraceFilterSkipSelf tracefilterSelf((IHandleEntity*)localPlayer, 0);
            CTraceFilterSkipTwoEntities tracefilterTwo((IHandleEntity*)localPlayer, (IHandleEntity*)mountedUseEnt, 0);
            CTraceFilter* pTraceFilter = mountedUseEnt ? static_cast<CTraceFilter*>(&tracefilterTwo) : static_cast<CTraceFilter*>(&tracefilterSelf);

            ray.Init(origin, target);
            m_Game->m_EngineTrace->TraceRay(ray, STANDARD_TRACE_MASK, pTraceFilter, &trace);

            m_AimConvergePoint = (trace.fraction < 1.0f && trace.fraction > 0.0f) ? trace.endpos : target;
            m_HasAimConvergePoint = true;
            target = m_AimConvergePoint; // draw to P
        }
        else
        {
            m_HasAimConvergePoint = false;
        }
    }
    // Friendly-fire aim guard: see if the aim ray currently hits a teammate player.
    // This runs regardless of whether the line is actually drawn.
    UpdateFriendlyFireAimHit(localPlayer);


    m_AimLineStart = origin;
    m_AimLineEnd = target;
    m_HasAimLine = true;
    m_HasThrowArc = false;

    if (canDraw && allowAimLineDraw)
        DrawAimLine(origin, target);
}

bool VR::IsWeaponLaserSightActive(C_WeaponCSBase* weapon) const
{
    if (!weapon)
        return false;

    // L4D2 uses an upgrade bit-vector on most firearms (client netvar: m_upgradeBitVec).
    // We only care about whether the laser sight upgrade is active.
    // NOTE: The offset is stable across the common firearm weapon classes; if it ever changes, update it here.
    constexpr int kUpgradeBitVecOffset = 0xCF0;
    constexpr int kLaserSightBit = (1 << 2);

    const int bitVec = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(weapon) + kUpgradeBitVecOffset);
    return (bitVec & kLaserSightBit) != 0;
}

bool VR::ShouldDrawAimLine(C_WeaponCSBase* weapon) const
{
    if (!m_AimLineOnlyWhenLaserSight)
        return true;

    if (!weapon)
        return false;

    // Never suppress throwable arcs; those are handled separately and do not use DrawAimLine().
    if (IsThrowableWeapon(weapon))
        return false;

    // Only show for firearms that have an active in-game laser sight.
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
        return IsWeaponLaserSightActive(weapon);
    default:
        return false;
    }
}

bool VR::ShouldShowAimLine(C_WeaponCSBase* weapon) const
{
    // While pinned/controlled by SI, the player model/camera can be driven by animations,
// causing the aim line to wildly drift and feel broken. Disable it in those states.
    if (m_PlayerControlledBySI)
        return false;

    if (!weapon)
        return false;

    switch (weapon->GetWeaponID())
    {
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
        // Firearms and throwables use the main aim-line toggle.
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
        return m_AimLineEnabled;
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

    // Changing magnification changes the sensitivity scale. Rebase on next frame to avoid a sudden jump.
    if (m_ScopeActive)
        m_ScopeAimSensitivityInit = false;
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

void VR::ToggleMouseModeScope()
{
    if (!m_MouseModeEnabled)
        return;

    m_MouseModeScopeToggleActive = !m_MouseModeScopeToggleActive;

    // Clear lingering stabilization history.
    m_ScopeStabilizationInit = false;
    m_ScopeStabilizationLastTime = {};
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

bool VR::IsEntityAlive(const C_BaseEntity* entity) const
{
    if (!entity)
        return false;

    const auto base = reinterpret_cast<const std::uint8_t*>(entity);
    const unsigned char lifeState = *reinterpret_cast<const unsigned char*>(base + kLifeStateOffset);

    return lifeState == 0;
}

void VR::RefreshDeathFirstPersonLock(const C_BasePlayer* localPlayer)
{
    if (!localPlayer)
    {
        m_DeathFirstPersonLockEnd = {};
        m_DeathWasAlivePrev = true;
        return;
    }

    const bool aliveNow = IsEntityAlive(localPlayer);
    const auto now = std::chrono::steady_clock::now();

    if (aliveNow)
    {
        // Fully clear the lock once we're alive again.
        m_DeathFirstPersonLockEnd = {};
    }
    else if (m_DeathWasAlivePrev)
    {
        // Rising edge: alive -> dead. Start a first-person lock window.
        m_DeathFirstPersonLockEnd = now + std::chrono::seconds(10);
    }

    m_DeathWasAlivePrev = aliveNow;
}

bool VR::IsDeathFirstPersonLockActive() const
{
    if (m_DeathFirstPersonLockEnd == std::chrono::steady_clock::time_point{})
        return false;

    return std::chrono::steady_clock::now() < m_DeathFirstPersonLockEnd;
}
bool VR::IsThirdPersonMapLoadCooldownActive() const
{
    if (m_ThirdPersonMapLoadCooldownMs <= 0)
        return false;
    if (m_ThirdPersonMapLoadCooldownEnd == std::chrono::steady_clock::time_point{})
        return false;
    return std::chrono::steady_clock::now() < m_ThirdPersonMapLoadCooldownEnd;
}
// Special infected recognition

#if __has_include("special_infected_features.cpp")
#define L4D2VR_HAS_SPECIAL_INFECTED_FEATURES 1
#include "special_infected_features.cpp"
#else
#define L4D2VR_HAS_SPECIAL_INFECTED_FEATURES 0

VR::SpecialInfectedType VR::GetSpecialInfectedType(const C_BaseEntity* /*entity*/) const { return SpecialInfectedType::None; }
VR::SpecialInfectedType VR::GetSpecialInfectedTypeFromModel(const std::string& /*modelName*/) const { return SpecialInfectedType::None; }
void VR::DrawSpecialInfectedArrow(const Vector& /*origin*/, SpecialInfectedType /*type*/) {}
void VR::RefreshSpecialInfectedPreWarning(const Vector& /*infectedOrigin*/, SpecialInfectedType /*type*/, int /*entityIndex*/, bool /*isPlayerClass*/) {}
void VR::RefreshSpecialInfectedBlindSpotWarning(const Vector& /*infectedOrigin*/) {}
bool VR::HasLineOfSightToSpecialInfected(const Vector& /*infectedOrigin*/, int /*entityIndex*/) const { return false; }
bool VR::IsSpecialInfectedInBlindSpot(const Vector& /*infectedOrigin*/) const { return false; }

void VR::UpdateSpecialInfectedWarningState()
{
    m_SpecialInfectedBlindSpotWarningActive = false;
    m_SpecialInfectedPreWarningActive = false;
    m_SpecialInfectedPreWarningInRange = false;
    m_SpecialInfectedWarningTargetActive = false;
    m_SpecialInfectedPreWarningTargetEntityIndex = -1;
    m_SpecialInfectedPreWarningTargetIsPlayer = false;
    m_SpecialInfectedAutoAimDirection = {};
    m_SpecialInfectedAutoAimCooldownEnd = {};
}

void VR::UpdateSpecialInfectedPreWarningState()
{
    m_SpecialInfectedPreWarningActive = false;
    m_SpecialInfectedPreWarningInRange = false;
    m_SpecialInfectedPreWarningTargetEntityIndex = -1;
    m_SpecialInfectedPreWarningTargetIsPlayer = false;
    m_SpecialInfectedAutoAimDirection = {};
    m_SpecialInfectedAutoAimCooldownEnd = {};
}

void VR::OnPredictionRunCommand(CUserCmd* /*cmd*/) {}
void VR::StartSpecialInfectedWarningAction() {}
void VR::UpdateSpecialInfectedWarningAction() {}
void VR::ResetSpecialInfectedWarningAction()
{
    m_SpecialInfectedWarningAttack2CmdOwned = false;
    m_SpecialInfectedWarningJumpCmdOwned = false;
    m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::None;
    m_SpecialInfectedWarningNextActionTime = {};
    m_SuppressPlayerInput = false;
}

#endif

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

const VR::RenderFrameSnapshot* VR::GetActiveRenderFrameSnapshot() const
{
    if (m_RenderFrameSnapshotActive.load(std::memory_order_acquire))
        return &m_RenderFrameSnapshot;
    return nullptr;
}

void VR::BeginRenderFrameSnapshot()
{
    RenderFrameSnapshot snap{};

    snap.hmdPosAbs = m_HmdPosAbs;
    snap.hmdAngAbs = m_HmdAngAbs;
    snap.hmdForward = m_HmdForward;
    snap.hmdRight = m_HmdRight;
    snap.hmdUp = m_HmdUp;

    snap.vrScale = m_VRScale;
    snap.ipd = m_Ipd;
    snap.ipdScale = m_IpdScale;
    snap.eyeZ = m_EyeZ;

    snap.mouseModeEnabled = m_MouseModeEnabled;
    snap.mouseModeScopeActive = IsMouseModeScopeActive();
    snap.mouseModeViewmodelAnchorOffset = m_MouseModeViewmodelAnchorOffset;
    snap.mouseModeScopedViewmodelAnchorOffset = m_MouseModeScopedViewmodelAnchorOffset;

    snap.rightControllerPosAbs = m_RightControllerPosAbs;
    snap.rightControllerAngAbs = m_RightControllerAngAbs;

    snap.viewmodelForward = m_ViewmodelForward;
    snap.viewmodelRight = m_ViewmodelRight;
    snap.viewmodelUp = m_ViewmodelUp;
    snap.viewmodelPosOffset = m_ViewmodelPosOffset;

    // Write snapshot first, then publish active flag.
    m_RenderFrameSnapshot = snap;
    m_RenderFrameSnapshotActive.store(true, std::memory_order_release);
}

void VR::EndRenderFrameSnapshot()
{
    m_RenderFrameSnapshotActive.store(false, std::memory_order_release);
}

Vector VR::GetViewAngle()
{
    if (const RenderFrameSnapshot* snap = GetActiveRenderFrameSnapshot())
        return Vector(snap->hmdAngAbs.x, snap->hmdAngAbs.y, snap->hmdAngAbs.z);

    return Vector(m_HmdAngAbs.x, m_HmdAngAbs.y, m_HmdAngAbs.z);
}

float VR::GetMovementYawDeg()
{
    if (!m_MoveDirectionFromController)
    {
        if (m_MouseModeEnabled)
        {
            // In mouse mode, locomotion follows the body yaw (turning), not head yaw.
            float yaw = m_RotationOffset;
            // Wrap to [-180, 180]
            yaw -= 360.0f * std::floor((yaw + 180.0f) / 360.0f);
            return yaw;
        }

        Vector hmdAng = GetViewAngle();
        return hmdAng.y;
    }

    // Use the dominant (right) controller yaw as the movement basis.
    // This is intentionally yaw-only; pitch/roll should not affect locomotion.
    QAngle ctrlAng = GetRightControllerAbsAngle();
    return ctrlAng.y;
}

Vector VR::GetViewOriginLeft()
{
    const RenderFrameSnapshot* snap = GetActiveRenderFrameSnapshot();
    const Vector hmdPos = snap ? snap->hmdPosAbs : m_HmdPosAbs;
    const Vector hmdForward = snap ? snap->hmdForward : m_HmdForward;
    const Vector hmdRight = snap ? snap->hmdRight : m_HmdRight;
    const float vrScale = snap ? snap->vrScale : m_VRScale;
    const float eyeZ = snap ? snap->eyeZ : m_EyeZ;
    const float ipd = snap ? snap->ipd : m_Ipd;
    const float ipdScale = snap ? snap->ipdScale : m_IpdScale;

    Vector viewOriginLeft = hmdPos + (hmdForward * (-(eyeZ * vrScale)));
    viewOriginLeft = viewOriginLeft + (hmdRight * (-((ipd * ipdScale * vrScale) / 2)));
    return viewOriginLeft;
}

Vector VR::GetViewOriginRight()
{
    const RenderFrameSnapshot* snap = GetActiveRenderFrameSnapshot();
    const Vector hmdPos = snap ? snap->hmdPosAbs : m_HmdPosAbs;
    const Vector hmdForward = snap ? snap->hmdForward : m_HmdForward;
    const Vector hmdRight = snap ? snap->hmdRight : m_HmdRight;
    const float vrScale = snap ? snap->vrScale : m_VRScale;
    const float eyeZ = snap ? snap->eyeZ : m_EyeZ;
    const float ipd = snap ? snap->ipd : m_Ipd;
    const float ipdScale = snap ? snap->ipdScale : m_IpdScale;

    Vector viewOriginRight = hmdPos + (hmdForward * (-(eyeZ * vrScale)));
    viewOriginRight = viewOriginRight + (hmdRight * ((ipd * ipdScale * vrScale) / 2));
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

    auto getFloatList = [&](const char* k, const char* defVal = nullptr) -> std::vector<float>
        {
            std::vector<float> values;
            const auto it = userConfig.find(k);
            if (it == userConfig.end() && !defVal)
                return values;

            const std::string source = (it == userConfig.end()) ? defVal : it->second;
            std::stringstream ss(source);
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


    m_SnapTurnAngle = getFloat("SnapTurnAngle", m_SnapTurnAngle);
    m_TurnSpeed = getFloat("TurnSpeed", m_TurnSpeed);
    // Locomotion direction: default is HMD-yaw-based. Optional hand-yaw-based.
    m_MoveDirectionFromController = getBool("MoveDirectionFromController", m_MoveDirectionFromController);
    m_MoveDirectionFromController = getBool("MovementDirectionFromController", m_MoveDirectionFromController);

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
    m_ThirdPersonCameraSmoothing = std::clamp(getFloat("ThirdPersonCameraSmoothing", m_ThirdPersonCameraSmoothing), 0.0f, 0.99f);
    m_ThirdPersonMapLoadCooldownMs = std::max(0, getInt("ThirdPersonMapLoadCooldownMs", m_ThirdPersonMapLoadCooldownMs));
    m_ThirdPersonRenderOnCustomWalk = getBool("ThirdPersonRenderOnCustomWalk", m_ThirdPersonRenderOnCustomWalk);
    m_HideArms = getBool("HideArms", m_HideArms);
    m_HudDistance = getFloat("HudDistance", m_HudDistance);
    m_HudSize = getFloat("HudSize", m_HudSize);
    m_HudAlwaysVisible = getBool("HudAlwaysVisible", m_HudAlwaysVisible);
    m_HudMatQueueModeLinkEnabled = getBool("HudMatQueueModeLinkEnabled", m_HudMatQueueModeLinkEnabled);
    // Note: when linkage is enabled, we also force mat_queue_mode=1 during first-person rendering.
    if (m_HudMatQueueModeLinkEnabled && m_Game && m_Game->m_MaterialSystem)
    {
        MaterialThreadMode_t threadMode = m_Game->m_MaterialSystem->GetThreadMode();
        if (threadMode == MATERIAL_QUEUED_THREADED)
            m_HudAlwaysVisible = false;
    }
    m_HudCaptureViaVGuiPaint = getBool("HudCaptureViaVGuiPaint", m_HudCaptureViaVGuiPaint);
    m_DisableHudRendering = getBool("DisableHudRendering", m_DisableHudRendering);
    if (m_DisableHudRendering)
    {
        // HUD fully disabled: keep toggle off regardless of other HUD options.
        m_HudAlwaysVisible = false;
        m_HudToggleState = false;
        m_HudToggleStateFromAlwaysVisible = false;
    }
    else
    {
        m_HudToggleState = m_HudAlwaysVisible;
        m_HudToggleStateFromAlwaysVisible = m_HudAlwaysVisible;
    }
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
    m_MotionGesturePushThreshold = std::max(0.0f, getFloat("MotionGesturePushThreshold", m_MotionGesturePushThreshold));
    m_MotionGestureDownSwingThreshold = std::max(0.0f, getFloat("MotionGestureDownSwingThreshold", m_MotionGestureDownSwingThreshold));
    m_MotionGestureJumpThreshold = std::max(0.0f, getFloat("MotionGestureJumpThreshold", m_MotionGestureJumpThreshold));
    m_MotionGestureCooldown = std::max(0.0f, getFloat("MotionGestureCooldown", m_MotionGestureCooldown));
    m_MotionGestureHoldDuration = std::max(0.0f, getFloat("MotionGestureHoldDuration", m_MotionGestureHoldDuration));
    m_ViewmodelAdjustEnabled = getBool("ViewmodelAdjustEnabled", m_ViewmodelAdjustEnabled);
    m_AimLineThickness = std::max(0.0f, getFloat("AimLineThickness", m_AimLineThickness));
    m_AimLineEnabled = getBool("AimLineEnabled", m_AimLineEnabled);
    m_AimLineOnlyWhenLaserSight = getBool("AimLineOnlyWhenLaserSight", m_AimLineOnlyWhenLaserSight);
    m_AimLineConfigEnabled = m_AimLineEnabled;
    m_BlockFireOnFriendlyAimEnabled = getBool("BlockFireOnFriendlyAimEnabled", m_BlockFireOnFriendlyAimEnabled);
    m_AutoRepeatSemiAutoFire = getBool("AutoRepeatSemiAutoFire", m_AutoRepeatSemiAutoFire);
    m_AutoRepeatSemiAutoFireHz = std::max(0.0f, getFloat("AutoRepeatSemiAutoFireHz", m_AutoRepeatSemiAutoFireHz));
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
    m_ScopeRTTMaxHz = std::clamp(getFloat("ScopeRTTMaxHz", m_ScopeRTTMaxHz), 0.0f, 240.0f);
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


    // Scoped aim sensitivity scaling (mouse-style ADS / zoom sensitivity).
    // Accepts either:
    //   ScopeAimSensitivityScale=80            (80% for all magnifications)
    //   ScopeAimSensitivityScale=100,85,70,55  (per ScopeMagnification index)
    {
        const auto scalesRaw = getFloatList("ScopeAimSensitivityScale");
        if (!scalesRaw.empty())
        {
            m_ScopeAimSensitivityScales.clear();
            for (float s : scalesRaw)
            {
                if (!std::isfinite(s))
                    continue;

                // Allow both [0..1] and [0..100] styles.
                if (s > 1.5f)
                    s *= 0.01f;

                m_ScopeAimSensitivityScales.push_back(std::clamp(s, 0.05f, 1.0f));
            }
        }

        // Ensure the table matches magnification count.
        const size_t n = m_ScopeMagnificationOptions.size();
        if (n > 0)
        {
            if (m_ScopeAimSensitivityScales.empty())
                m_ScopeAimSensitivityScales.assign(n, 1.0f);
            else if (m_ScopeAimSensitivityScales.size() < n)
                m_ScopeAimSensitivityScales.resize(n, m_ScopeAimSensitivityScales.back());
            else if (m_ScopeAimSensitivityScales.size() > n)
                m_ScopeAimSensitivityScales.resize(n);
        }
    }

    // Changing config values should not cause a sudden jump mid-scope.
    m_ScopeAimSensitivityInit = false;
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
    m_RearMirrorShowOnlyOnSpecialWarning = getBool("RearMirrorShowOnlyOnSpecialWarning", m_RearMirrorShowOnlyOnSpecialWarning);
    m_RearMirrorSpecialShowHoldSeconds = std::max(0.0f, getFloat("RearMirrorSpecialShowHoldSeconds", m_RearMirrorSpecialShowHoldSeconds));
    m_RearMirrorRTTSize = std::clamp(getInt("RearMirrorRTTSize", m_RearMirrorRTTSize), 128, 4096);
    m_RearMirrorRTTMaxHz = std::clamp(getFloat("RearMirrorRTTMaxHz", m_RearMirrorRTTMaxHz), 0.0f, 240.0f);
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
    m_RearMirrorFlipHorizontal = getBool("RearMirrorFlipHorizontal", m_RearMirrorFlipHorizontal);

    // Hide the rear mirror when the aim line/ray intersects it (prevents blocking your view while aiming).
    m_RearMirrorHideWhenAimLineHits = getBool("RearMirrorHideWhenAimLineHits", m_RearMirrorHideWhenAimLineHits);
    m_RearMirrorAimLineHideHoldSeconds = std::clamp(getFloat("RearMirrorAimLineHideHoldSeconds", m_RearMirrorAimLineHideHoldSeconds), 0.0f, 1.0f);
    if (!m_RearMirrorHideWhenAimLineHits)
        m_RearMirrorAimLineHideUntil = {};

    // Rear mirror hint: enlarge overlay when special-infected arrows are visible in the mirror render pass
    m_RearMirrorSpecialWarningDistance = std::max(0.0f, getFloat("RearMirrorSpecialWarningDistance", m_RearMirrorSpecialWarningDistance));
    if (m_RearMirrorSpecialWarningDistance <= 0.0f)
        m_RearMirrorSpecialEnlargeActive = false;

    // Auto reset tracking origin after a level loads (0 disables)
    m_AutoResetPositionAfterLoadSeconds = std::clamp(
        getFloat("AutoResetPositionAfterLoadSeconds", m_AutoResetPositionAfterLoadSeconds),
        0.0f, 60.0f);

    m_ForceNonVRServerMovement = getBool("ForceNonVRServerMovement", m_ForceNonVRServerMovement);

    // Mouse mode (desktop-style aiming while staying in VR rendering)
    m_MouseModeEnabled = getBool("MouseModeEnabled", m_MouseModeEnabled);
    m_MouseModeAimFromHmd = getBool("MouseModeAimFromHmd", m_MouseModeAimFromHmd);
    m_MouseModeHmdAimSensitivity = std::clamp(getFloat("MouseModeHmdAimSensitivity", m_MouseModeHmdAimSensitivity), 0.0f, 3.0f);
    m_MouseModeYawSensitivity = getFloat("MouseModeYawSensitivity", m_MouseModeYawSensitivity);
    m_MouseModePitchSensitivity = getFloat("MouseModePitchSensitivity", m_MouseModePitchSensitivity);
    m_MouseModePitchAffectsView = getBool("MouseModePitchAffectsView", m_MouseModePitchAffectsView);
    m_MouseModeTurnSmoothing = getFloat("MouseModeTurnSmoothing", m_MouseModeTurnSmoothing);
    m_MouseModePitchSmoothing = getFloat("MouseModePitchSmoothing", m_MouseModePitchSmoothing);
    m_MouseModeViewmodelAnchorOffset = getVector3("MouseModeViewmodelAnchorOffset", m_MouseModeViewmodelAnchorOffset);
    m_MouseModeScopedViewmodelAnchorOffset = getVector3("MouseModeScopedViewmodelAnchorOffset", m_MouseModeViewmodelAnchorOffset);
    // Mouse-mode: if non-zero, place the scope overlay using the OpenVR HMD tracking pose
    // (meters in tracking space), so the overlay can't disappear due to unit mismatches.
    m_MouseModeScopeOverlayOffset = getVector3("MouseModeScopeOverlayOffset", m_MouseModeScopeOverlayOffset);
    m_MouseModeScopeOverlayAngleOffsetSet = (userConfig.find("MouseModeScopeOverlayAngleOffset") != userConfig.end());
    if (m_MouseModeScopeOverlayAngleOffsetSet)
    {
        Vector tmp = getVector3("MouseModeScopeOverlayAngleOffset", Vector{ m_MouseModeScopeOverlayAngleOffset.x, m_MouseModeScopeOverlayAngleOffset.y, m_MouseModeScopeOverlayAngleOffset.z });
        m_MouseModeScopeOverlayAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z };
    }
    m_MouseModeScopeToggleKey = parseVirtualKey(getString("MouseModeScopeToggleKey", "key:f9"));
    m_MouseModeScopeMagnificationKey = parseVirtualKey(getString("MouseModeScopeMagnificationKey", "key:f10"));

    // Optional bindable impulses for mouse-mode scope control.
    // Using impulses avoids GetAsyncKeyState issues and allows normal Source binds.
    auto mouseModeScopeSensitivityList = getFloatList("MouseModeScopeSensitivityScale", "100");
    if (mouseModeScopeSensitivityList.empty())
        mouseModeScopeSensitivityList.push_back(100.0f);
    for (auto& v : mouseModeScopeSensitivityList)
        v = std::clamp(v, 1.0f, 200.0f);
    if (mouseModeScopeSensitivityList.size() < m_ScopeMagnificationOptions.size())
        mouseModeScopeSensitivityList.resize(m_ScopeMagnificationOptions.size(), mouseModeScopeSensitivityList.back());
    m_MouseModeScopeSensitivityScales = mouseModeScopeSensitivityList;
    m_MouseModeAimConvergeDistance = getFloat("MouseModeAimConvergeDistance", m_MouseModeAimConvergeDistance);

    // Non-VR server melee feel tuning (ForceNonVRServerMovement=true only)
    m_NonVRMeleeSwingThreshold = std::max(0.0f, getFloat("NonVRMeleeSwingThreshold", m_NonVRMeleeSwingThreshold));
    m_NonVRMeleeSwingCooldown = std::max(0.0f, getFloat("NonVRMeleeSwingCooldown", m_NonVRMeleeSwingCooldown));
    m_NonVRMeleeHoldTime = std::max(0.0f, getFloat("NonVRMeleeHoldTime", m_NonVRMeleeHoldTime));
    m_NonVRMeleeAttackDelay = std::max(0.0f, getFloat("NonVRMeleeAttackDelay", m_NonVRMeleeAttackDelay));
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
    m_SpecialInfectedPreWarningEvadeCooldown = std::max(0.0f, getFloat("SpecialInfectedPreWarningEvadeCooldown", m_SpecialInfectedPreWarningEvadeCooldown));
    m_HudMatQueueModeLinkThreadedFpsRatio = std::clamp(getFloat("HudMatQueueModeLinkThreadedFpsRatio", m_HudMatQueueModeLinkThreadedFpsRatio), 0.10f, 1.00f);
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
        : std::clamp(aimSnapDistance, 0.0f, 30.0f);

    const float releaseDistance = getFloat("SpecialInfectedPreWarningAimReleaseDistance", m_SpecialInfectedPreWarningAimReleaseDistance);
    m_SpecialInfectedPreWarningAimReleaseDistance = m_SpecialInfectedDebug
        ? std::max(m_SpecialInfectedPreWarningAimSnapDistance, std::max(0.0f, releaseDistance))
        : std::clamp(std::max(m_SpecialInfectedPreWarningAimSnapDistance, std::max(0.0f, releaseDistance)), 0.0f, 40.0f);

    const float autoAimLerp = getFloat("SpecialInfectedAutoAimLerp", m_SpecialInfectedAutoAimLerp);
    m_SpecialInfectedAutoAimLerp = m_SpecialInfectedDebug
        ? std::max(0.0f, autoAimLerp)
        : std::clamp(autoAimLerp, 0.0f, 0.3f);

    const float autoAimCooldown = getFloat("SpecialInfectedAutoAimCooldown", m_SpecialInfectedAutoAimCooldown);
    m_SpecialInfectedAutoAimCooldown = m_SpecialInfectedDebug
        ? std::max(0.0f, autoAimCooldown)
        : std::max(0.5f, autoAimCooldown);

    const float runCommandShotWindow = getFloat("SpecialInfectedRunCommandShotWindow", m_SpecialInfectedRunCommandShotWindow);
    m_SpecialInfectedRunCommandShotWindow = std::max(0.0f, runCommandShotWindow);

    const float runCommandShotLerp = getFloat("SpecialInfectedRunCommandShotLerp", m_SpecialInfectedRunCommandShotLerp);
    m_SpecialInfectedRunCommandShotLerp = m_SpecialInfectedDebug
        ? std::max(0.0f, runCommandShotLerp)
        : std::clamp(runCommandShotLerp, 0.0f, 1.0f);

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
