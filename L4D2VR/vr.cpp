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
#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <vector>
#include <d3d9_vr.h>

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

    m_Overlay->SetOverlayInputMethod(m_MainMenuHandle, vr::VROverlayInputMethod_Mouse);
    m_Overlay->SetOverlayInputMethod(m_HUDTopHandle, vr::VROverlayInputMethod_Mouse);
    for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
    {
        m_Overlay->SetOverlayInputMethod(overlay, vr::VROverlayInputMethod_Mouse);
    }

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
    m_Input->GetActionHandle("/actions/main/in/MenuSelect", &m_MenuSelect);
    m_Input->GetActionHandle("/actions/main/in/MenuBack", &m_MenuBack);
    m_Input->GetActionHandle("/actions/main/in/MenuUp", &m_MenuUp);
    m_Input->GetActionHandle("/actions/main/in/MenuDown", &m_MenuDown);
    m_Input->GetActionHandle("/actions/main/in/MenuLeft", &m_MenuLeft);
    m_Input->GetActionHandle("/actions/main/in/MenuRight", &m_MenuRight);
    m_Input->GetActionHandle("/actions/main/in/Spray", &m_Spray);
    m_Input->GetActionHandle("/actions/main/in/Scoreboard", &m_Scoreboard);
    m_Input->GetActionHandle("/actions/main/in/ShowHUD", &m_ShowHUD);
    m_Input->GetActionHandle("/actions/main/in/Pause", &m_Pause);
    m_Input->GetActionHandle("/actions/main/in/CustomAction1", &m_CustomAction1);
    m_Input->GetActionHandle("/actions/main/in/CustomAction2", &m_CustomAction2);

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
    if (GetAnalogActionData(m_ActionWalk, d)) {  // m_ActionWalk 已在现有代码中使用
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
    m_LeftEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("leftEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);

    m_CreatingTextureID = Texture_RightEye;
    m_RightEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("rightEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);

    m_CreatingTextureID = Texture_HUD;
    m_HUDTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("vrHUD", windowWidth, windowHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);

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

    // 若这帧没有新内容，就走菜单/Overlay 路径
    if (!m_RenderedNewFrame)
    {
        if (!m_BlankTexture)
            CreateVRTextures();

        if (!vr::VROverlay()->IsOverlayVisible(m_MainMenuHandle))
            RepositionOverlays();

        vr::VROverlay()->SetOverlayTexture(m_MainMenuHandle, &m_VKBackBuffer.m_VRTexture);
        vr::VROverlay()->ShowOverlay(m_MainMenuHandle);
        hideHudOverlays();

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
    Vector hudDistance = hmdForward * m_HudDistance;
    Vector hudNewPos = hudDistance + hmdPosition;
    hudNewPos.y -= 0.25f;

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
        return;

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
    const float chestGestureRange = gestureRange * 0.5f;

    auto buildAnchor = [&](const Vector& offsets)
        {
            return m_HmdPosAbs
                + (m_HmdForward * (offsets.x * m_VRScale))
                + (m_HmdRight * (offsets.y * m_VRScale))
                + (m_HmdUp * (offsets.z * m_VRScale));
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
        auto drawCircle = [&](const Vector& center, const Vector& axisA, const Vector& axisB, float range)
            {
                const int segments = 24;
                const float twoPi = 6.28318530718f;
                for (int i = 0; i < segments; ++i)
                {
                    const float t0 = (twoPi * i) / segments;
                    const float t1 = (twoPi * (i + 1)) / segments;
                    const Vector dir0 = (axisA * std::cos(t0)) + (axisB * std::sin(t0));
                    const Vector dir1 = (axisA * std::cos(t1)) + (axisB * std::sin(t1));
                    const Vector start = center + (dir0 * range);
                    const Vector end = center + (dir1 * range);
                    m_Game->m_DebugOverlay->AddLineOverlay(start, end, m_InventoryAnchorColorR, m_InventoryAnchorColorG, m_InventoryAnchorColorB, true, m_LastFrameDuration * 2.0f);
                }
            };

        auto drawAnchor = [&](const Vector& anchor, float range)
            {
                drawCircle(anchor, m_HmdRight, m_HmdForward, range);
                drawCircle(anchor, m_HmdUp, m_HmdRight, range);
                drawCircle(anchor, m_HmdForward, m_HmdUp, range);
            };

        drawAnchor(chestAnchor, chestGestureRange);
        drawAnchor(backAnchor, gestureRange);
        drawAnchor(leftWaistAnchor, gestureRange);
        drawAnchor(rightWaistAnchor, gestureRange);
    }

    bool inventoryGripActiveLeft = false;
    bool inventoryGripActiveRight = false;

    auto togglePrimarySecondary = [&]()
        {
            C_WeaponCSBase* activeWeapon = localPlayer ? static_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon()) : nullptr;
            const int currentSlot = getWeaponSlot(activeWeapon);
            const char* targetSlotCmd = "slot1";

            if (currentSlot == 1)
                targetSlotCmd = "slot2";
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

    auto triggerCustomAction = [&](const std::string& command)
        {
            if (command.empty())
                return;

            m_Game->ClientCmd_Unrestricted(command.c_str());
        };

    if (PressedDigitalAction(m_CustomAction1, true))
        triggerCustomAction(m_CustomAction1Command);

    if (PressedDigitalAction(m_CustomAction2, true))
        triggerCustomAction(m_CustomAction2Command);

    auto showHudOverlays = [&](bool attachToControllers)
        {
            vr::VROverlay()->ShowOverlay(m_HUDTopHandle);
            for (size_t i = 0; i < m_HUDBottomHandles.size(); ++i)
            {
                if (attachToControllers)
                {
                    if (i == 0 && m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand) == vr::k_unTrackedDeviceIndexInvalid)
                        continue;
                    if (i == 3 && m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand) == vr::k_unTrackedDeviceIndexInvalid)
                        continue;
                }

                vr::VROverlay()->ShowOverlay(m_HUDBottomHandles[i]);
            }
        };

    auto hideHudOverlays = [&]()
        {
            vr::VROverlay()->HideOverlay(m_HUDTopHandle);
            for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
                vr::VROverlay()->HideOverlay(overlay);
        };

    bool isControllerVertical = m_RightControllerAngAbs.x > 60 || m_RightControllerAngAbs.x < -45;
    bool menuActive = m_Game->m_EngineClient->IsPaused();
    bool wantsHud = PressedDigitalAction(m_ShowHUD) || PressedDigitalAction(m_Scoreboard) || isControllerVertical || m_HudAlwaysVisible;
    if ((wantsHud && m_RenderedHud) || menuActive)
    {
        RepositionOverlays(!menuActive);

        if (PressedDigitalAction(m_Scoreboard))
            m_Game->ClientCmd_Unrestricted("+showscores");
        else
            m_Game->ClientCmd_Unrestricted("-showscores");

        showHudOverlays(!menuActive);
    }
    else
    {
        hideHudOverlays();
    }
    m_RenderedHud = false;

    if (PressedDigitalAction(m_Pause, true))
    {
        m_Game->ClientCmd_Unrestricted("gameui_activate");
        RepositionOverlays(false);
        showHudOverlays(false);
    }
}

void VR::SendFunctionKey(WORD virtualKey)
{
    INPUT inputs[2] = {};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = virtualKey;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = virtualKey;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
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
    if (!localPlayer)
        return;

    m_Game->m_IsMeleeWeaponActive = localPlayer->IsMeleeWeaponActive();
    RefreshActiveViewmodelAdjustment(localPlayer);

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

    Vector hmdPosSmoothed = hmdPosCorrected;
    QAngle hmdAngSmoothed = hmdAngLocal;

    hmdAngSmoothed.x = wrapAngle(hmdAngSmoothed.x);
    hmdAngSmoothed.y = wrapAngle(hmdAngSmoothed.y);
    hmdAngSmoothed.z = wrapAngle(hmdAngSmoothed.z);

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

    const float leftOutwardSpeed = std::max(0.0f, DotProduct(leftDelta, leftForwardHorizontalNorm)) / deltaSeconds;
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

void VR::UpdateAimingLaser(C_BasePlayer* localPlayer)
{
    UpdateSpecialInfectedWarningState();

    if (!m_Game->m_DebugOverlay)
        return;

    C_WeaponCSBase* activeWeapon = nullptr;
    if (localPlayer)
        activeWeapon = static_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());

    if (!ShouldShowAimLine(activeWeapon))
    {
        m_LastAimDirection = Vector{ 0.0f, 0.0f, 0.0f };
        m_HasAimLine = false;
        m_HasThrowArc = false;
        m_LastAimWasThrowable = false;
        return;
    }

    bool isThrowable = IsThrowableWeapon(activeWeapon);
    Vector direction = m_RightControllerForward;
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

    Vector origin = m_RightControllerPosAbs + direction * 2.0f;

    if (isThrowable)
    {
        Vector pitchSource = direction;
        if (!m_ForceNonVRServerMovement && !m_HmdForward.IsZero())
            pitchSource = m_HmdForward;

        DrawThrowArc(origin, direction, pitchSource);
        return;
    }

    const float maxDistance = 8192.0f;
    Vector target = origin + direction * maxDistance;

    m_AimLineStart = origin;
    m_AimLineEnd = target;
    m_HasAimLine = true;
    m_HasThrowArc = false;

    DrawAimLine(origin, target);
}

bool VR::ShouldShowAimLine(C_WeaponCSBase* weapon) const
{
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
    if (!m_Game->m_DebugOverlay)
        return;

    if (!m_AimLineEnabled)
        return;

    // Keep the overlay alive for at least two frames so it doesn't disappear when the framerate drops.
    const float duration = std::max(m_AimLinePersistence, m_LastFrameDuration * m_AimLineFrameDurationMultiplier);

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

VR::SpecialInfectedType VR::GetSpecialInfectedType(const std::string& modelName) const
{
    std::string lower = modelName;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const std::array<std::pair<const char*, SpecialInfectedType>, 19> specialKeywords =
    {
        // L4D2 defaults
        std::make_pair("boomer", SpecialInfectedType::Boomer),
        std::make_pair("smoker", SpecialInfectedType::Smoker),
        std::make_pair("hunter", SpecialInfectedType::Hunter),
        std::make_pair("spitter", SpecialInfectedType::Spitter),
        std::make_pair("jockey", SpecialInfectedType::Jockey),
        std::make_pair("charger", SpecialInfectedType::Charger),
        std::make_pair("tank", SpecialInfectedType::Tank),
        std::make_pair("hulk", SpecialInfectedType::Tank),
        std::make_pair("witch", SpecialInfectedType::Witch),
        // L4D1 variants share the same colors
        std::make_pair("boomer_l4d1", SpecialInfectedType::Boomer),
        std::make_pair("l4d1_boomer", SpecialInfectedType::Boomer),
        std::make_pair("smoker_l4d1", SpecialInfectedType::Smoker),
        std::make_pair("l4d1_smoker", SpecialInfectedType::Smoker),
        std::make_pair("hunter_l4d1", SpecialInfectedType::Hunter),
        std::make_pair("l4d1_hunter", SpecialInfectedType::Hunter),
        std::make_pair("tank_l4d1", SpecialInfectedType::Tank),
        std::make_pair("l4d1_tank", SpecialInfectedType::Tank),
        std::make_pair("hulk_l4d1", SpecialInfectedType::Tank),
        std::make_pair("l4d1_hulk", SpecialInfectedType::Tank)
    };

    auto it = std::find_if(specialKeywords.begin(), specialKeywords.end(), [&](const auto& entry)
        {
            return lower.find(entry.first) != std::string::npos;
        });

    if (it != specialKeywords.end())
        return it->second;

    return SpecialInfectedType::None;
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

void VR::UpdateSpecialInfectedWarningState()
{
    if (!m_SpecialInfectedBlindSpotWarningActive)
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsedSeconds = std::chrono::duration<float>(now - m_LastSpecialInfectedWarningTime).count();

    if (elapsedSeconds > m_SpecialInfectedBlindSpotWarningDuration)
        m_SpecialInfectedBlindSpotWarningActive = false;
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
        Game::logMsg("[VR] Active viewmodel adjust key: %s", m_CurrentViewmodelKey.c_str());
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
        // 找不到就保持构造时的默认值
        return;
    }

    // 简单的 trim
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
        // 去掉注释
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

        // 解析 key=value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key); trim(value);
        if (!key.empty())
            userConfig[key] = value;
    }

    // 小工具：带默认值的安全读取
    auto getBool = [&](const char* k, bool defVal)->bool {
        auto it = userConfig.find(k);
        if (it == userConfig.end()) return defVal;
        std::string v = it->second;
        // 转小写
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

    auto getString = [&](const char* k, const std::string& defVal)->std::string {
        auto it = userConfig.find(k);
        if (it == userConfig.end())
            return defVal;

        std::string value = it->second;
        trim(value);

        return value.empty() ? defVal : value;
        };

    // 用当前成员的值作为默认值（构造时已初始化）
    m_SnapTurning = getBool("SnapTurning", m_SnapTurning);
    m_SnapTurnAngle = getFloat("SnapTurnAngle", m_SnapTurnAngle);
    m_TurnSpeed = getFloat("TurnSpeed", m_TurnSpeed);
    m_InventoryGestureRange = getFloat("InventoryGestureRange", m_InventoryGestureRange);
    m_InventoryChestOffset = getVector3("InventoryChestOffset", m_InventoryChestOffset);
    m_InventoryBackOffset = getVector3("InventoryBackOffset", m_InventoryBackOffset);
    m_InventoryLeftWaistOffset = getVector3("InventoryLeftWaistOffset", m_InventoryLeftWaistOffset);
    m_InventoryRightWaistOffset = getVector3("InventoryRightWaistOffset", m_InventoryRightWaistOffset);
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
        {"showhud", &m_ShowHUD},
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
    m_CustomAction1Command = getString("CustomAction1Command", m_CustomAction1Command);
    m_CustomAction2Command = getString("CustomAction2Command", m_CustomAction2Command);

    m_LeftHanded = getBool("LeftHanded", m_LeftHanded);
    m_VRScale = getFloat("VRScale", m_VRScale);
    m_IpdScale = getFloat("IPDScale", m_IpdScale);
    m_HideArms = getBool("HideArms", m_HideArms);
    m_HudDistance = getFloat("HudDistance", m_HudDistance);
    m_HudSize = getFloat("HudSize", m_HudSize);
    m_ControllerHudSize = getFloat("ControllerHudSize", m_ControllerHudSize);
    m_ControllerHudYOffset = getFloat("ControllerHudYOffset", m_ControllerHudYOffset);
    m_ControllerHudZOffset = getFloat("ControllerHudZOffset", m_ControllerHudZOffset);
    m_ControllerHudRotation = getFloat("ControllerHudRotation", m_ControllerHudRotation);
    m_ControllerHudXOffset = getFloat("ControllerHudXOffset", m_ControllerHudXOffset);
    m_HudAlwaysVisible = getBool("HudAlwaysVisible", m_HudAlwaysVisible);
    float controllerSmoothingValue = m_ControllerSmoothing;
    if (userConfig.find("ControllerSmoothing") != userConfig.end())
        controllerSmoothingValue = getFloat("ControllerSmoothing", controllerSmoothingValue);
    else if (userConfig.find("HeadSmoothing") != userConfig.end())
        controllerSmoothingValue = getFloat("HeadSmoothing", controllerSmoothingValue);
    m_ControllerSmoothing = std::clamp(controllerSmoothingValue, 0.0f, 0.99f);
    m_MotionGestureSwingThreshold = std::max(0.0f, getFloat("MotionGestureSwingThreshold", m_MotionGestureSwingThreshold));
    m_MotionGestureDownSwingThreshold = std::max(0.0f, getFloat("MotionGestureDownSwingThreshold", m_MotionGestureDownSwingThreshold));
    m_MotionGestureJumpThreshold = std::max(0.0f, getFloat("MotionGestureJumpThreshold", m_MotionGestureJumpThreshold));
    m_MotionGestureCooldown = std::max(0.0f, getFloat("MotionGestureCooldown", m_MotionGestureCooldown));
    m_MotionGestureHoldDuration = std::max(0.0f, getFloat("MotionGestureHoldDuration", m_MotionGestureHoldDuration));
    m_ViewmodelAdjustEnabled = getBool("ViewmodelAdjustEnabled", m_ViewmodelAdjustEnabled);
    m_AimLineThickness = std::max(0.0f, getFloat("AimLineThickness", m_AimLineThickness));
    m_AimLineEnabled = getBool("AimLineEnabled", m_AimLineEnabled);
    m_MeleeAimLineEnabled = getBool("MeleeAimLineEnabled", m_MeleeAimLineEnabled);
    auto aimColor = getColor("AimLineColor", m_AimLineColorR, m_AimLineColorG, m_AimLineColorB, m_AimLineColorA);
    m_AimLineColorR = aimColor[0];
    m_AimLineColorG = aimColor[1];
    m_AimLineColorB = aimColor[2];
    m_AimLineColorA = aimColor[3];
    m_AimLinePersistence = std::max(0.0f, getFloat("AimLinePersistence", m_AimLinePersistence));
    m_AimLineFrameDurationMultiplier = std::max(0.0f, getFloat("AimLineFrameDurationMultiplier", m_AimLineFrameDurationMultiplier));
    m_ForceNonVRServerMovement = getBool("ForceNonVRServerMovement", m_ForceNonVRServerMovement);
    m_RequireSecondaryAttackForItemSwitch = getBool("RequireSecondaryAttackForItemSwitch", m_RequireSecondaryAttackForItemSwitch);
    m_SpecialInfectedWarningActionEnabled = getBool("SpecialInfectedAutoEvade", m_SpecialInfectedWarningActionEnabled);
    m_SpecialInfectedArrowEnabled = getBool("SpecialInfectedArrowEnabled", m_SpecialInfectedArrowEnabled);
    m_SpecialInfectedArrowSize = std::max(0.0f, getFloat("SpecialInfectedArrowSize", m_SpecialInfectedArrowSize));
    m_SpecialInfectedArrowHeight = std::max(0.0f, getFloat("SpecialInfectedArrowHeight", m_SpecialInfectedArrowHeight));
    m_SpecialInfectedArrowThickness = std::max(0.0f, getFloat("SpecialInfectedArrowThickness", m_SpecialInfectedArrowThickness));
    m_SpecialInfectedBlindSpotDistance = std::max(0.0f, getFloat("SpecialInfectedBlindSpotDistance", m_SpecialInfectedBlindSpotDistance));
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
