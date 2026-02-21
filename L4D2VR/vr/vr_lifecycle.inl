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
    m_Input->GetActionHandle("/actions/main/in/InventoryGripLeft", &m_ActionInventoryGripLeft);
    m_Input->GetActionHandle("/actions/main/in/InventoryGripRight", &m_ActionInventoryGripRight);
    m_Input->GetActionHandle("/actions/main/in/InventoryQuickSwitch", &m_ActionInventoryQuickSwitch);
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

    UpdateTracking();


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

void VR::LogVAS(const char* tag)
{
    if (!m_DebugVASLog || !tag || !*tag)
        return;

    const VASStats st = QueryVASStats();
    Game::logMsg(
        "[VR][VAS] %s | free %.1f MiB (largest %.1f MiB) | reserved %.1f MiB | committed %.1f MiB",
        tag,
        BytesToMiB(st.freeTotal),
        BytesToMiB(st.freeLargest),
        BytesToMiB(st.reserved),
        BytesToMiB(st.committed));
}

void VR::CreateVRTextures()
{
    LogVAS("before CreateVRTextures");

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

    // Optional RTTs: scope + rear-mirror can be extremely expensive in 32-bit VAS
    // when their sizes are set high. Only pre-create them if requested.
    const bool wantScope = (!m_LazyScopeRearMirrorRTT) || m_ScopeEnabled;
    const bool wantRearMirror = (!m_LazyScopeRearMirrorRTT) || m_RearMirrorEnabled;

    if (wantScope)
    {
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
    }

    if (wantRearMirror)
    {
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
    }

    m_CreatingTextureID = Texture_Blank;
    m_BlankTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("blankTexture", 512, 512, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);

    m_CreatingTextureID = Texture_None;

    m_Game->m_MaterialSystem->EndRenderTargetAllocation();

    m_CreatedVRTextures = true;

    LogVAS("after CreateVRTextures");
}

void VR::EnsureOpticsRTTTextures()
{
    if (!m_CreatedVRTextures)
        return;

    const bool needScope = (m_ScopeEnabled && !m_ScopeTexture);
    const bool needRearMirror = (m_RearMirrorEnabled && !m_RearMirrorTexture);
    if (!needScope && !needRearMirror)
        return;

    LogVAS("before EnsureOpticsRTTTextures");

    m_Game->m_MaterialSystem->isGameRunning = false;
    m_Game->m_MaterialSystem->BeginRenderTargetAllocation();
    m_Game->m_MaterialSystem->isGameRunning = true;

    if (needScope)
    {
        m_CreatingTextureID = Texture_Scope;
        m_ScopeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx(
            "vrScope",
            static_cast<int>(m_ScopeRTTSize),
            static_cast<int>(m_ScopeRTTSize),
            RT_SIZE_NO_CHANGE,
            m_Game->m_MaterialSystem->GetBackBufferFormat(),
            MATERIAL_RT_DEPTH_SEPARATE,
            TEXTUREFLAGS_NOMIP);
        Game::logMsg("[VR] Created scope RTT on-demand (%dx%d)", (int)m_ScopeRTTSize, (int)m_ScopeRTTSize);
    }

    if (needRearMirror)
    {
        m_CreatingTextureID = Texture_RearMirror;
        m_RearMirrorTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx(
            "vrRearMirror",
            static_cast<int>(m_RearMirrorRTTSize),
            static_cast<int>(m_RearMirrorRTTSize),
            RT_SIZE_NO_CHANGE,
            m_Game->m_MaterialSystem->GetBackBufferFormat(),
            MATERIAL_RT_DEPTH_SEPARATE,
            TEXTUREFLAGS_NOMIP);
        Game::logMsg("[VR] Created rear-mirror RTT on-demand (%dx%d)", (int)m_RearMirrorRTTSize, (int)m_RearMirrorRTTSize);
    }

    m_CreatingTextureID = Texture_None;
    m_Game->m_MaterialSystem->EndRenderTargetAllocation();

    LogVAS("after EnsureOpticsRTTTextures");
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

    const vr::VRTextureBounds_t topBounds{ 0.0f, 0.0f, 1.0f, 1.0f };
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
    for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
        vr::VROverlay()->HideOverlay(overlay);
    if (m_Game->m_VguiSurface->IsCursorVisible())
    {
        vr::VROverlay()->ShowOverlay(m_HUDTopHandle);
        for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
            vr::VROverlay()->HideOverlay(overlay);
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
    return !ShouldThrottle(m_LastRearMirrorRTTRenderTime, m_RearMirrorRTTMaxHz);
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

    // Reposition HUD overlays
    Vector hudDistance = hmdForward * (m_HudDistance + m_FixedHudDistanceOffset);
    Vector hudNewPos = hudDistance + hmdPosition;
    hudNewPos.y -= 0.25f;
    hudNewPos.y += m_FixedHudYOffset;

    vr::HmdMatrix34_t hudTopTransform = buildFacingTransform(hudNewPos);

    vr::VROverlay()->SetOverlayTransformAbsolute(m_HUDTopHandle, trackingOrigin, &hudTopTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_HUDTopHandle, m_HudSize);

    for (size_t i = 0; i < m_HUDBottomHandles.size(); ++i)
    {
        vr::VROverlay()->HideOverlay(m_HUDBottomHandles[i]);
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

