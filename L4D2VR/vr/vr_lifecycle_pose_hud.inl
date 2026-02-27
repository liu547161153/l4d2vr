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

bool VR::ReadPoseWaiterSnapshot(vr::TrackedDevicePose_t* outPoses, uint32_t* outSeq) const
{
    if (!outPoses)
        return false;

    // Seqlock read: writer flips seq odd->even around the memcpy.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const uint32_t s1 = m_PoseWaiterSeq.load(std::memory_order_acquire);
        if (s1 == 0 || (s1 & 1u))
            continue;

        std::memcpy(outPoses, m_PoseWaiterPoses.data(), sizeof(vr::TrackedDevicePose_t) * vr::k_unMaxTrackedDeviceCount);

        const uint32_t s2 = m_PoseWaiterSeq.load(std::memory_order_acquire);
        if (s1 == s2 && !(s2 & 1u))
        {
            if (outSeq)
                *outSeq = s2;
            return true;
        }
    }

    return false;
}

void VR::PoseWaiterThreadMain()
{
    // Detached thread. Runs only when mat_queue_mode!=0 (m_PoseWaiterEnabled).
    // This keeps WaitGetPoses() off the queued render thread, preserving mat_queue_mode throughput.
    while (true)
    {
        if (!m_PoseWaiterEnabled.load(std::memory_order_acquire) || !m_Compositor)
        {
            Sleep(1);
            continue;
        }

        std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
        vr::EVRCompositorError result = m_Compositor->WaitGetPoses(poses.data(), vr::k_unMaxTrackedDeviceCount, NULL, 0);
        if (result != vr::VRCompositorError_None)
            continue;

        // Publish snapshot.
        m_PoseWaiterSeq.fetch_add(1, std::memory_order_acq_rel); // odd
        std::memcpy(m_PoseWaiterPoses.data(), poses.data(), sizeof(vr::TrackedDevicePose_t) * vr::k_unMaxTrackedDeviceCount);
        m_PoseWaiterSeq.fetch_add(1, std::memory_order_release); // even
		if (m_PoseWaiterEvent)
			SetEvent(m_PoseWaiterEvent);
    }
}

bool VR::UpdatePosesAndActions()
{
    if (!m_Compositor)
        return false;
    const bool queued = (m_Game && (m_Game->GetMatQueueMode() != 0));

	// Pose waiter publishes WaitGetPoses() snapshots on a dedicated thread in queued mode.
	// Optional render-thread pacing uses this event to wait for a fresh snapshot.
	if (queued && !m_PoseWaiterEvent)
		m_PoseWaiterEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    // Start pose waiter once; enable it only in queued mode.
    if (queued && !m_PoseWaiterStarted.exchange(true, std::memory_order_acq_rel))
    {
        std::thread t(&VR::PoseWaiterThreadMain, this);
        t.detach();
    }
    m_PoseWaiterEnabled.store(queued, std::memory_order_release);

    bool posesValid = false;
    if (queued && m_System)
    {
        // In mat_queue_mode!=0, keep the main thread non-blocking.
        // Read the latest WaitGetPoses() snapshot produced by the pose waiter thread.
        if (ReadPoseWaiterSnapshot(m_Poses))
        {
            posesValid = m_Poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
        }
        else
        {
            // Fallback (early frames before waiter publishes): non-blocking VRSystem prediction.
            const vr::ETrackingUniverseOrigin trackingOrigin = m_Compositor ? m_Compositor->GetTrackingSpace() : vr::TrackingUniverseStanding;
            float predicted = m_Compositor ? m_Compositor->GetFrameTimeRemaining() : 0.0f;
            if (!(predicted >= 0.0f && predicted <= 0.5f))
                predicted = 0.0f;
            m_System->GetDeviceToAbsoluteTrackingPose(trackingOrigin, predicted, m_Poses, vr::k_unMaxTrackedDeviceCount);
            posesValid = m_Poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
        }
    }
    else
    {
        vr::EVRCompositorError result = m_Compositor->WaitGetPoses(m_Poses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
        posesValid = (result == vr::VRCompositorError_None);
    }
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


void VR::UpdateHandHudOverlays()
{
    // Debug: hand HUD update diagnostics (rate-limited).
    const auto dbgNow = std::chrono::steady_clock::now();
    m_HandHudDebugLastCall = dbgNow;
    const bool dbgTick = m_HandHudDebugLog && !ShouldThrottle(m_HandHudDebugLastLog, m_HandHudDebugLogHz);

    // Refresh overlay interface pointer in case the OpenVR runtime recreated it (e.g. compositor restart).
    // Some runtimes keep the old pointer non-null but make calls fail with VROverlayError_RequestFailed.
    {
        std::lock_guard<std::mutex> _lk(m_VROverlayMutex);
        if (vr::IVROverlay* cur = vr::VROverlay())
            m_Overlay = cur;
    }

    if (!m_Overlay || !m_System || !m_Game || !m_Game->m_EngineClient)
    {
        if (dbgTick)
            Game::logMsg("[VR][HandHUD] tick: missing ptr overlay=%p system=%p game=%p engine=%p", (void*)m_Overlay, (void*)m_System, (void*)m_Game, m_Game ? (void*)m_Game->m_EngineClient : nullptr);
        return;
    }

    auto secsSince = [&](const std::chrono::steady_clock::time_point& tp) -> float
    {
        if (tp.time_since_epoch().count() == 0)
            return -1.0f;
        return std::chrono::duration<float>(dbgNow - tp).count();
    };

    auto resetHandHudCache = [&]()
    {
        m_LastHudHealth = -9999;
        m_LastHudTempHealth = -9999;
        m_LastHudThrowable = -1;
        m_LastHudMedItem = -1;
        m_LastHudPillItem = -1;
        m_LastHudIncap = false;
        m_LastHudLedge = false;
        m_LastHudThirdStrike = false;
        m_LastHudAimTargetVisible = false;
        m_LastHudAimTargetIndex = -1;
        m_LastHudAimTargetPct = -1;
        m_LastHudAimTargetNameHash = 0;
        m_LastHudTeammatesHash = 0;
        m_LastHudClip = -9999;
        m_LastHudReserve = -9999;
        m_LastHudUpg = -9999;
        m_LastHudUpgBits = 0;
    };

    const bool worldQuad = m_HandHudWorldQuadEnabled;
    const bool worldQuadAttachControllers = worldQuad && m_HandHudWorldQuadAttachToControllers && !m_MouseModeEnabled;

    auto SafeReleaseD3D = [](auto*& p)
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    };

    auto DestroyWorldQuadTextures = [&]()
    {
        SafeReleaseD3D(m_D9LeftWristHudDynSurface);
        SafeReleaseD3D(m_D9LeftWristHudDynTex);
        SafeReleaseD3D(m_D9RightAmmoHudDynSurface);
        SafeReleaseD3D(m_D9RightAmmoHudDynTex);
        m_D9LeftWristHudDynW = m_D9LeftWristHudDynH = 0;
        m_D9RightAmmoHudDynW = m_D9RightAmmoHudDynH = 0;
        std::memset(&m_VKLeftWristHudDyn, 0, sizeof(m_VKLeftWristHudDyn));
        std::memset(&m_VKRightAmmoHudDyn, 0, sizeof(m_VKRightAmmoHudDyn));
    };

    // If world-quad mode was used previously but is now off, free the backing textures.
    if (!worldQuad && (m_D9LeftWristHudDynTex || m_D9RightAmmoHudDynTex))
        DestroyWorldQuadTextures();

    auto GetD3DDeviceForHud = [&]() -> IDirect3DDevice9*
    {
        IDirect3DDevice9* dev = nullptr;
        if (m_D9HUDSurface)
            m_D9HUDSurface->GetDevice(&dev);
        else if (m_D9LeftEyeSurface)
            m_D9LeftEyeSurface->GetDevice(&dev);
        else if (m_D9RightEyeSurface)
            m_D9RightEyeSurface->GetDevice(&dev);
        return dev;
    };

    auto EnsureWorldQuadTexture = [&](bool isLeft) -> bool
    {
        if (!worldQuad)
            return false;

        const int wantW = isLeft ? m_LeftWristHudTexW : m_RightAmmoHudTexW;
        const int wantH = isLeft ? m_LeftWristHudTexH : m_RightAmmoHudTexH;
        if (wantW <= 0 || wantH <= 0)
            return false;

        IDirect3DTexture9*& tex = isLeft ? m_D9LeftWristHudDynTex : m_D9RightAmmoHudDynTex;
        IDirect3DSurface9*& surf = isLeft ? m_D9LeftWristHudDynSurface : m_D9RightAmmoHudDynSurface;
        SharedTextureHolder& vk = isLeft ? m_VKLeftWristHudDyn : m_VKRightAmmoHudDyn;
        int& curW = isLeft ? m_D9LeftWristHudDynW : m_D9RightAmmoHudDynW;
        int& curH = isLeft ? m_D9LeftWristHudDynH : m_D9RightAmmoHudDynH;

        if (tex && (curW != wantW || curH != wantH))
        {
            SafeReleaseD3D(surf);
            SafeReleaseD3D(tex);
            curW = curH = 0;
            std::memset(&vk, 0, sizeof(vk));
        }

        if (tex)
            return true;

        if (!g_D3DVR9)
            return false;

        IDirect3DDevice9* dev = GetD3DDeviceForHud();
        if (!dev)
            return false;

        // dxvk D3D9 is not generally thread-safe: lock the device while we create/describe resources.
        g_D3DVR9->LockDevice();

        HRESULT hr = dev->CreateTexture(
            (UINT)wantW,
            (UINT)wantH,
            1,
            D3DUSAGE_DYNAMIC,
            D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,
            &tex,
            nullptr);

        if (SUCCEEDED(hr) && tex)
        {
            tex->GetSurfaceLevel(0, &surf);
            if (surf)
            {
                D3D9_TEXTURE_VR_DESC desc{};
                if (SUCCEEDED(g_D3DVR9->GetVRDesc(surf, &desc)))
                {
                    std::memcpy(&vk.m_VulkanData, &desc, sizeof(vr::VRVulkanTextureData_t));
                    vk.m_VRTexture.handle = &vk.m_VulkanData;
                    vk.m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
                    vk.m_VRTexture.eType = vr::TextureType_Vulkan;
                    curW = wantW;
                    curH = wantH;
                }
                else
                {
                    SafeReleaseD3D(surf);
                    SafeReleaseD3D(tex);
                }
            }
            else
            {
                SafeReleaseD3D(tex);
            }
        }

        g_D3DVR9->UnlockDevice();
        dev->Release();

        return tex != nullptr && surf != nullptr;
    };

    auto UploadWorldQuadTextureRGBA = [&](bool isLeft, const uint8_t* rgba, int w, int h) -> bool
    {
        if (!worldQuad || !rgba || w <= 0 || h <= 0)
            return false;
        if (!EnsureWorldQuadTexture(isLeft))
            return false;

        IDirect3DTexture9* tex = isLeft ? m_D9LeftWristHudDynTex : m_D9RightAmmoHudDynTex;
        IDirect3DSurface9* surf = isLeft ? m_D9LeftWristHudDynSurface : m_D9RightAmmoHudDynSurface;
        if (!tex || !surf || !g_D3DVR9)
            return false;

        // Lock the device around LockRect to avoid dxvk multi-thread surprises.
        g_D3DVR9->LockDevice();
        D3DLOCKED_RECT lr{};
        const HRESULT hr = tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD);
        if (FAILED(hr) || !lr.pBits)
        {
            g_D3DVR9->UnlockDevice();
            return false;
        }

        // Our HUD pixels are RGBA; D3DFMT_A8R8G8B8 expects BGRA in memory.
        const uint8_t* src = rgba;
        uint8_t* dst0 = reinterpret_cast<uint8_t*>(lr.pBits);
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* srow = src + (size_t)y * (size_t)w * 4;
            uint8_t* drow = dst0 + (size_t)y * (size_t)lr.Pitch;
            for (int x = 0; x < w; ++x)
            {
                const uint8_t r = srow[x * 4 + 0];
                const uint8_t g = srow[x * 4 + 1];
                const uint8_t b = srow[x * 4 + 2];
                const uint8_t a = srow[x * 4 + 3];
                drow[x * 4 + 0] = b;
                drow[x * 4 + 1] = g;
                drow[x * 4 + 2] = r;
                drow[x * 4 + 3] = a;
            }
        }

        tex->UnlockRect(0);
        // Ensure the Vulkan-side resource is updated for OpenVR sampling.
        g_D3DVR9->TransferSurface(surf, FALSE);
        g_D3DVR9->UnlockDevice();
        return true;
    };


    // If SetOverlayRaw starts returning RequestFailed persistently, the overlay system can end up
    // "stuck" and the HUD freezes on the last successfully-uploaded frame. We recover by recreating
    // the hand HUD overlays after N consecutive failures.
    const uint32_t kHandHudRecoverFailThreshold = 5;     // consecutive SetOverlayRaw failures
    const float    kHandHudRecoverMinIntervalSec = 1.0f; // avoid thrashing
    bool needHandHudOverlayRecover = false;

    bool leftVisible = false;
    bool rightVisible = false;
    float rightUmaxUsed = 1.0f;

    if (!m_Game->m_EngineClient->IsInGame())
    {
        if (dbgTick)
            Game::logMsg("[VR][HandHUD] tick: not in game -> hide overlays (lastLUpload=%.2fs lastRUpload=%.2fs)", secsSince(m_HandHudDebugLastLeftUpload), secsSince(m_HandHudDebugLastRightUpload));
        resetHandHudCache();
        m_HandHudLeftConsecutiveRawFails = 0;
        m_HandHudRightConsecutiveRawFails = 0;
        vr::VROverlay()->HideOverlay(m_LeftWristHudHandle);
        leftVisible = false;
        vr::VROverlay()->HideOverlay(m_RightAmmoHudHandle);
        rightVisible = false;
        DestroyWorldQuadTextures();
        return;
    }

    const int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
    if (!localPlayer)
    {
        if (dbgTick)
            Game::logMsg("[VR][HandHUD] tick: localPlayer null (idx=%d) -> hide overlays", playerIndex);
        resetHandHudCache();
        vr::VROverlay()->HideOverlay(m_LeftWristHudHandle);
        leftVisible = false;
        vr::VROverlay()->HideOverlay(m_RightAmmoHudHandle);
        rightVisible = false;
        DestroyWorldQuadTextures();
        return;
    }

    const unsigned char* pBase = reinterpret_cast<const unsigned char*>(localPlayer);
    unsigned char lifeState = 0;
    if (!SafeReadU8(pBase, kLifeStateOffset, lifeState))
        lifeState = 1;
    if (lifeState != 0)
    {
        if (dbgTick)
            Game::logMsg("[VR][HandHUD] tick: localPlayer dead lifeState=%u -> hide overlays", (unsigned)lifeState);
        resetHandHudCache();
        vr::VROverlay()->HideOverlay(m_LeftWristHudHandle);
        leftVisible = false;
        vr::VROverlay()->HideOverlay(m_RightAmmoHudHandle);
        rightVisible = false;
        DestroyWorldQuadTextures();
        return;
    }

    vr::TrackedDeviceIndex_t leftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    vr::TrackedDeviceIndex_t rightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    if (m_LeftHanded)
        std::swap(leftControllerIndex, rightControllerIndex);

    const vr::TrackedDeviceIndex_t offHandIndex = leftControllerIndex;
    const vr::TrackedDeviceIndex_t gunHandIndex = rightControllerIndex;

    const vr::TrackedDeviceIndex_t leftRoleIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    const vr::TrackedDeviceIndex_t rightRoleIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    if (dbgTick)
    {
        const bool paused = m_Game->m_EngineClient ? m_Game->m_EngineClient->IsPaused() : false;
        Game::logMsg(
            "[VR][HandHUD] tick: inGame=1 paused=%d pidx=%d lp=%p life=%u offDev=%u gunDev=%u Lrole=%u Rrole=%u",
            paused ? 1 : 0, playerIndex, (void*)localPlayer, (unsigned)lifeState, (unsigned)offHandIndex, (unsigned)gunHandIndex, (unsigned)leftRoleIndex, (unsigned)rightRoleIndex);
    }

    // Safe memory reads: UpdateHandHudOverlays can run on the present/render thread while the game
    // is loading/unloading, so client entity pointers may become invalid mid-frame.
    auto TryReadU8 = [](const unsigned char* base, int off, unsigned char& out) -> bool { return SafeReadU8(base, off, out); };
    auto TryReadInt = [](const unsigned char* base, int off, int& out) -> bool { return SafeReadInt(base, off, out); };
    auto TryReadFloat = [](const unsigned char* base, int off, float& out) -> bool { return SafeReadFloat(base, off, out); };

    // Compute decayed temp HP from (m_healthBuffer, m_healthBufferTime) using wall-clock time.
    // The engine does: max(0, healthBuffer - decayRate * (curtime - healthBufferTime)).
    // We don't have gpGlobals->curtime here, so we approximate with steady_clock since the
    // last observed (bufferTime/buffer) update.
    auto computeDecayedTempHP = [&](int entIndex, const unsigned char* entBase) -> int
    {
        if (!entBase)
            return 0;

        float hb = 0.0f;
        float hbTime = 0.0f;

        // Guard against freed/unmapped entity memory (common during level transitions).
        if (!TryReadFloat(entBase, kHealthBufferOffset, hb) || !TryReadFloat(entBase, kHealthBufferTimeOffset, hbTime))
        {
            if (!m_HandHudTempHealthStates.empty())
            {
                const int slot = (std::max)(0, (std::min)((int)m_HandHudTempHealthStates.size() - 1, entIndex));
                m_HandHudTempHealthStates[(size_t)slot].initialized = false;
            }
            return 0;
        }

        const float raw = (std::max)(0.0f, hb);
        const float rawTime = hbTime;
        if (raw <= 0.0f)
            return 0;

        if (m_HandHudTempHealthStates.empty())
            return (int)std::round(raw);

        const int slot = (std::max)(0, (std::min)((int)m_HandHudTempHealthStates.size() - 1, entIndex));
        TempHealthDecayState& st = m_HandHudTempHealthStates[(size_t)slot];

        const auto now = std::chrono::steady_clock::now();

        const bool newDoseOrReset = (!st.initialized)
            || (std::fabs(rawTime - st.rawBufferTime) > 0.0001f)
            || (raw > st.rawBuffer + 0.01f)
            || (raw < st.rawBuffer - 0.01f);

        if (newDoseOrReset)
        {
            st.rawBuffer = raw;
            st.rawBufferTime = rawTime;
            st.wallStart = now;
            st.lastRemaining = raw;
            st.initialized = true;
        }

        // Freeze decay while paused.
        if (m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsPaused())
        {
            st.wallStart = now;
            return (int)std::round((std::max)(0.0f, st.lastRemaining));
        }

        const float elapsed = std::chrono::duration<float>(now - st.wallStart).count();
        const float decayRate = (std::max)(0.0f, m_HandHudTempHealthDecayRate);
        const float remaining = (std::max)(0.0f, st.rawBuffer - decayRate * elapsed);
        st.lastRemaining = remaining;
        return (int)std::round(remaining);
    };

    auto survivorNameFromCharacter = [&](int survivorChar) -> const char*
    {
        // L4D2 SurvivorCharacter enum (common ordering).
        switch (survivorChar)
        {
        case 0: return "NICK";
        case 1: return "ROCHELLE";
        case 2: return "COACH";
        case 3: return "ELLIS";
        case 4: return "BILL";
        case 5: return "ZOEY";
        case 6: return "FRANCIS";
        case 7: return "LOUIS";
        default: return nullptr;
        }
    };

    auto healthColorFor = [&](int hp, unsigned char a = 255) -> Rgba
    {
        if (hp < 15) return Rgba{ 255, 60, 60, a };
        if (hp < 40) return Rgba{ 255, 220, 60, a };
        return Rgba{ 60, 220, 255, a };
    };

    // Incapacitated (倒地/挂边) health coloring: yellow by default, red when <=30%.
    // We treat "30%" as hp<=30 since this HUD uses a 0-100 style scale for survivor health.
    auto downHealthColorFor = [&](int hp, unsigned char a = 255) -> Rgba
    {
        if (hp <= 30) return Rgba{ 255, 60, 60, a };
        return Rgba{ 255, 220, 60, a };
    };

    auto buildRel = [&](float xOff, float yOff, float zOff, const QAngle& ang) -> vr::HmdMatrix34_t
    {
        const float deg2rad = 3.14159265358979323846f / 180.0f;
        const float pitch = ang.x * deg2rad;
        const float yaw = ang.y * deg2rad;
        const float roll = ang.z * deg2rad;

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
        float R[3][3];
        mul33(Ry, Rx, RyRx);
        mul33(Rz, RyRx, R);

        vr::HmdMatrix34_t rel = {
            R[0][0], R[0][1], R[0][2], xOff,
            R[1][0], R[1][1], R[1][2], yOff,
            R[2][0], R[2][1], R[2][2], zOff
        };
        return rel;
    };

    const bool canShowLeft = m_LeftWristHudEnabled && m_LeftWristHudHandle != vr::k_ulOverlayHandleInvalid && (worldQuad || m_MouseModeEnabled || offHandIndex != vr::k_unTrackedDeviceIndexInvalid);
    if (canShowLeft)
    {
        const unsigned char bgA = (unsigned char)std::round((std::max)(0.0f, (std::min)(1.0f, m_LeftWristHudBgAlpha)) * 255.0f);

        if ((!worldQuad || worldQuadAttachControllers) && !m_MouseModeEnabled && offHandIndex != vr::k_unTrackedDeviceIndexInvalid)
        {
            vr::HmdMatrix34_t rel = buildRel(m_LeftWristHudXOffset, m_LeftWristHudYOffset, m_LeftWristHudZOffset, m_LeftWristHudAngleOffset);
            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_LeftWristHudHandle, offHandIndex, &rel);
        }
        vr::VROverlay()->SetOverlayWidthInMeters(m_LeftWristHudHandle, (std::max)(0.01f, m_LeftWristHudWidthMeters));
        // Texel aspect is per-texel pixel aspect, not texture aspect ratio. Our pixels are square.
        vr::VROverlay()->SetOverlayTexelAspect(m_LeftWristHudHandle, 1.0f);
        float leftCurv = (std::max)(0.0f, m_LeftWristHudCurvature);
        if (worldQuad && !worldQuadAttachControllers)
            leftCurv = 0.0f;
        vr::VROverlay()->SetOverlayCurvature(m_LeftWristHudHandle, leftCurv);
        // Opacity applies to background only; keep overlay alpha at 1 so text/bars stay readable.
        vr::VROverlay()->SetOverlayAlpha(m_LeftWristHudHandle, 1.0f);

        int hp = 0;
        TryReadInt(pBase, kHealthOffset, hp);
        const int tempHP = computeDecayedTempHP(playerIndex, pBase);

        unsigned char b = 0;
        const bool incap = TryReadU8(pBase, kIsIncapacitatedOffset, b) ? (b != 0) : false;
        const bool ledge = TryReadU8(pBase, kIsHangingFromLedgeOffset, b) ? (b != 0) : false;
        const bool third = TryReadU8(pBase, kIsOnThirdStrikeOffset, b) ? (b != 0) : false;

        int aimTargetIdx = -1;
        int aimTargetPct = 0;
        char aimTargetName[64] = { 0 };
        const bool hasAimTarget = GetAimTeammateHudInfo(aimTargetIdx, aimTargetPct, aimTargetName, sizeof(aimTargetName));
        auto getItemSlotWeaponId = [&](int slot) -> int
        {
            C_WeaponCSBase* w = (C_WeaponCSBase*)localPlayer->Weapon_GetSlot(slot);
            if (!w)
                return -1;

            const int wid = (int)w->GetWeaponID();

            // Fix: some item slots keep the weapon entity around with m_iClip1==0 after use.
            // Treat clip==0 as empty so HUD updates immediately when you throw/consume an item.
            const unsigned char* wb = reinterpret_cast<const unsigned char*>(w);
            int clip1 = -1;
            if (TryReadInt(wb, kClip1Offset, clip1) && clip1 == 0)
                return -1;

            return wid;
        };

        const int throwable = getItemSlotWeaponId(2);
        const int medItem   = getItemSlotWeaponId(3);
        const int pillItem  = getItemSlotWeaponId(4);


        // Snapshot teammates so changes in THEIR HP/name also trigger redraw (fix: teammates only updated when local changed).
        struct TeammateRow
        {
            int entIndex = -1;
            int hp = 0;
            int temp = 0;
            char name[64] = { 0 };
            bool nonAscii = false;
            bool incap = false;
            bool ledge = false;
        };
        TeammateRow mates[3]{};
        int mateCount = 0;
        uint32_t matesHash = 2166136261u;

        if (m_LeftWristHudShowTeammates && m_Game->m_ClientEntityList)
        {
            const int hi = (std::min)(64, m_Game->m_ClientEntityList->GetHighestEntityIndex());
            for (int i = 1; i <= hi && mateCount < 3; ++i)
            {
                if (i == playerIndex)
                    continue;
                C_BasePlayer* p = (C_BasePlayer*)m_Game->GetClientEntity(i);
                if (!p) continue;
                const unsigned char* pb = reinterpret_cast<const unsigned char*>(p);
                unsigned char ls = 0;
                if (!TryReadU8(pb, kLifeStateOffset, ls) || ls != 0) continue;

                int team = 0;
                if (!TryReadInt(pb, kTeamNumOffset, team) || team != 2) continue;

                TeammateRow& row = mates[mateCount];
                row.entIndex = i;
                row.hp = 0;
                if (!TryReadInt(pb, kHealthOffset, row.hp)) continue;
                row.temp = computeDecayedTempHP(i, pb);

                row.incap = false;
                row.ledge = false;
                if (TryReadU8(pb, kIsIncapacitatedOffset, ls)) row.incap = (ls != 0);
                if (TryReadU8(pb, kIsHangingFromLedgeOffset, ls)) row.ledge = (ls != 0);

                player_info_t info{};
                if (m_Game->m_EngineClient->GetPlayerInfo(i, &info) && info.name[0])
                {
                    std::snprintf(row.name, sizeof(row.name), "%s", info.name);
                }
                else
                {
                    int survivorChar = -1;
                    if (TryReadInt(pb, kSurvivorCharacterOffset, survivorChar))
                    {
                        const char* sname = survivorNameFromCharacter(survivorChar);
                        if (sname && sname[0])
                        {
                            std::snprintf(row.name, sizeof(row.name), "%s", sname);
                        }
                        else
                        {
                            std::snprintf(row.name, sizeof(row.name), "P%d", i);
                        }
                    }
                    else
                    {
                        std::snprintf(row.name, sizeof(row.name), "P%d", i);
                    }
                }

                row.nonAscii = ContainsNonAscii(row.name);

                matesHash = Fnv1a32(&row.entIndex, sizeof(row.entIndex), matesHash);
                matesHash = Fnv1a32(&row.hp, sizeof(row.hp), matesHash);
                matesHash = Fnv1a32(&row.temp, sizeof(row.temp), matesHash);
                matesHash = Fnv1a32(&row.incap, sizeof(row.incap), matesHash);
                matesHash = Fnv1a32(&row.ledge, sizeof(row.ledge), matesHash);
                matesHash = Fnv1aStr32(row.name, matesHash);

                ++mateCount;
            }
        }

        const uint32_t aimNameHash = hasAimTarget ? Fnv1aStr32(aimTargetName) : 0u;

        const bool changed = (hp != m_LastHudHealth) || (tempHP != m_LastHudTempHealth)
            || (throwable != m_LastHudThrowable) || (medItem != m_LastHudMedItem) || (pillItem != m_LastHudPillItem)
            || (incap != m_LastHudIncap) || (ledge != m_LastHudLedge) || (third != m_LastHudThirdStrike)
            || (matesHash != m_LastHudTeammatesHash);

        const bool aimChanged = (hasAimTarget != m_LastHudAimTargetVisible)
            || (aimTargetIdx != m_LastHudAimTargetIndex)
            || (aimTargetPct != m_LastHudAimTargetPct)
            || (aimNameHash != m_LastHudAimTargetNameHash);

        if (dbgTick)
        {
            Game::logMsg("[VR][HandHUD] left: hp=%d temp=%d incap=%d ledge=%d third=%d items=%d,%d,%d mates=%d hash=0x%08X changed=%d aim=%d idx=%d pct=%d aimChanged=%d",
                hp, tempHP, incap ? 1 : 0, ledge ? 1 : 0, third ? 1 : 0, throwable, medItem, pillItem, mateCount, matesHash,
                changed ? 1 : 0, hasAimTarget ? 1 : 0, aimTargetIdx, aimTargetPct, aimChanged ? 1 : 0);
        }
            m_LastHudHealth = hp;
            m_LastHudTempHealth = tempHP;
            m_LastHudThrowable = throwable;
            m_LastHudMedItem = medItem;
            m_LastHudPillItem = pillItem;
            m_LastHudIncap = incap;
            m_LastHudLedge = ledge;
            m_LastHudThirdStrike = third;

            m_LastHudAimTargetVisible = hasAimTarget;
            m_LastHudAimTargetIndex = aimTargetIdx;
            m_LastHudAimTargetPct = aimTargetPct;
            m_LastHudAimTargetNameHash = aimNameHash;
            m_LastHudTeammatesHash = matesHash;

            const int w = m_LeftWristHudTexW;
            const int h = m_LeftWristHudTexH;
            const uint8_t backIdx = (uint8_t)(m_LeftWristHudPixelsFront ^ 1);
            auto& pixels = m_LeftWristHudPixels[backIdx];
            pixels.resize((size_t)w * (size_t)h * 4);
            HudSurface s{ pixels.data(), w, h, w * 4 };

            // Static background cache (fix: background box blinking on updates)
            if (m_LeftWristHudBgCacheW != w || m_LeftWristHudBgCacheH != h || m_LeftWristHudBgCacheA != bgA
                || m_LeftWristHudBgCache.size() != (size_t)w * (size_t)h * 4)
            {
                m_LeftWristHudBgCacheW = w;
                m_LeftWristHudBgCacheH = h;
                m_LeftWristHudBgCacheA = bgA;
                m_LeftWristHudBgCache.assign((size_t)w * (size_t)h * 4, 0);
                HudSurface bg{ m_LeftWristHudBgCache.data(), w, h, w * 4 };
                Clear(bg, { 8, 10, 14, bgA });
                DrawCornerBrackets(bg, 2, 2, w - 4, h - 4, { 60, 220, 255, 220 });
                DrawRect(bg, 8, 8, w - 16, h - 16, { 20, 60, 70, bgA }, 1);
            }

            // Start from cached background
            memcpy(s.pixels, m_LeftWristHudBgCache.data(), m_LeftWristHudBgCache.size());

            const bool down = (incap || ledge);
            const Rgba hpCol = down ? downHealthColorFor(hp, 255) : healthColorFor(hp, 255);
            const SevenSegStyle hpSt{ 12, 3, 2, 4 };
            const int hpW = Draw7SegInt(s, 18, 18, (std::max)(0, hp), hpSt, hpCol);
            if (tempHP > 0)
            {
                // Temp HP: keep it tight to the main HP number (readability + less eye travel).
                char hpBuf[16];
                std::snprintf(hpBuf, sizeof(hpBuf), "+%d", tempHP);
                const int tempX = 18 + hpW + 8;
                DrawText5x7Outlined(s, tempX, 24, hpBuf, { 60, 255, 120, 255 }, 2);
            }
            if (hasAimTarget)
            {
                // Name fitting policy: 12 ASCII chars or 6 CJK chars at full size.
                // Beyond that: shrink 10% per +2 chars, cap at 40% shrink, then hard-truncate.
                const int units = Utf8HudUnits(aimTargetName);
                const float scale = HudNameScaleForUnits(units, 12);
                const std::string nameFit = (units > 20) ? Utf8TruncateHudUnits(aimTargetName, 20) : std::string(aimTargetName);

                char tgtBuf[128];
                std::snprintf(tgtBuf, sizeof(tgtBuf), "%s:%d%%", nameFit.c_str(), aimTargetPct);

                const int basePx = 16;
                int fontPx = (int)std::round((float)basePx * scale);
                fontPx = (std::max)(10, (std::min)(basePx, fontPx));

                // Always use the GDI path here so ASCII and Unicode names both obey the shrink/truncate policy.
                DrawTextUtf8OutlinedGdiClippedEx(s, 18, 64, 220, tgtBuf, fontPx, { 240, 240, 240, 255 }, false);
            }

            int dotX = w - 62;
            int dotY = 18;
            auto dot = [&](bool on, const Rgba& c)
            {
                DrawRect(s, dotX, dotY, 14, 14, { 80, 80, 80, 200 }, 1);
                if (on)
                    FillRect(s, dotX + 3, dotY + 3, 8, 8, c);
                dotX += 18;
            };
            dot(incap, { 255, 220, 60, 255 });
            dot(ledge, { 255, 200, 60, 255 });
            dot(third, { 220, 220, 220, 255 });

            if (m_LeftWristHudShowTeammates && mateCount > 0)
            {
                for (int row = 0; row < mateCount; ++row)
                {
                    const TeammateRow& tr = mates[row];
                    const int y0 = 18 + row * 18;

                    // Name: ASCII uses 5x7 (uppercase), Unicode uses GDI fallback.
                    char nameAscii[16] = { 0 };
                    if (!tr.nonAscii)
                    {
                        int n = 0;
                        for (; n < 7 && tr.name[n]; ++n)
                        {
                            char ch = tr.name[n];
                            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                            nameAscii[n] = ch;
                        }
                        nameAscii[n] = 0;
                        DrawText5x7Outlined(s, 124, y0, nameAscii, { 240, 240, 240, 255 }, 1);
                    }
                    else
                    {
                        // Tight name area; ellipsis if too long.
                        DrawHudTextAuto(s, 124, y0 - 2, 42, tr.name, { 240, 240, 240, 255 }, 1, 14);
                    }

                    const int barX = 170;
                    const int barW = 78;
                    const int barH = 10;
                    DrawRect(s, barX, y0, barW, barH, { 60, 60, 60, 190 }, 1);

                    const int innerW = barW - 2;
                    const int innerH = barH - 2;

                    const int perm = (std::max)(0, (std::min)(100, tr.hp));
                    const int permW = (innerW * perm) / 100;
                    const bool trDown = (tr.incap || tr.ledge);
                    const Rgba permCol = trDown ? downHealthColorFor(perm, 230) : healthColorFor(perm, 230);
                    FillRect(s, barX + 1, y0 + 1, permW, innerH, permCol);

                    const int extra = (std::max)(0, (std::min)(100, tr.temp));
                    const int extraW = (innerW * extra) / 100;
                    const int remW = (std::max)(0, innerW - permW);
                    const int tempFillW = (std::max)(0, (std::min)(remW, extraW));
                    FillRect(s, barX + 1 + permW, y0 + 1, tempFillW, innerH, { 60, 255, 120, 210 });
                }
            }

            auto itemAbbr = [](int wid) -> const char*
            {
                using W = C_WeaponCSBase::WeaponID;
                switch ((W)wid)
                {
                case W::MOLOTOV: return "MOL";
                case W::PIPE_BOMB: return "PIP";
                case W::VOMITJAR: return "BIL";
                case W::FIRST_AID_KIT: return "FAK";
                case W::DEFIBRILLATOR: return "DEF";
                case W::AMMO_PACK: return "AMP";
                case W::PAIN_PILLS: return "PIL";
                case W::ADRENALINE: return "ADR";
                default: return "";
                }
            };

            const int itemsY = 92;
            int itemsX = 18;
            const auto drawItem = [&](int wid)
            {
                const char* a = itemAbbr(wid);
                if (a && a[0])
                {
                    DrawText5x7(s, itemsX, itemsY, a, { 240, 240, 240, 255 }, 2);
                    itemsX += 48;
                }
            };
            drawItem(throwable);
            drawItem(medItem);
            drawItem(pillItem);
            // Upload every tick (no throttling/no CRC gating).
            {
                    vr::EVROverlayError err = vr::VROverlayError_None;
                    {
                        std::lock_guard<std::mutex> _lk(m_VROverlayMutex);
                        vr::IVROverlay* ov = vr::VROverlay();
                        if (!ov) ov = m_Overlay;
                        if (worldQuad)
                        {
                            // Upload into a dynamic GPU texture and bind it as the overlay texture.
                            const bool okUpload = UploadWorldQuadTextureRGBA(true, pixels.data(), w, h);
                            if (okUpload)
                            {
                                static const vr::VRTextureBounds_t full{ 0.0f, 0.0f, 1.0f, 1.0f };
                                ov->SetOverlayTextureBounds(m_LeftWristHudHandle, &full);
                                ov->SetOverlayTexture(m_LeftWristHudHandle, &m_VKLeftWristHudDyn.m_VRTexture);
                                err = vr::VROverlayError_None;
                            }
                            else
                            {
                                // Fallback: if dxvk VR bridge isn't available, still try raw upload.
                                err = ov ? ov->SetOverlayRaw(m_LeftWristHudHandle, pixels.data(), (uint32_t)w, (uint32_t)h, 4) : vr::VROverlayError_RequestFailed;
                            }
                        }
                        else
                        {
                            err = ov ? ov->SetOverlayRaw(m_LeftWristHudHandle, pixels.data(), (uint32_t)w, (uint32_t)h, 4) : vr::VROverlayError_RequestFailed;
                        }
                    }
                    m_HandHudDebugLastLeftSetRawErr = (int)err;
                    if (err == vr::VROverlayError_None)
                    {
                        m_HandHudDebugLastLeftUpload = dbgNow;
                        ++m_HandHudDebugLeftUploadCount;
                        m_HandHudLeftConsecutiveRawFails = 0;
                    }
                    else
                    {
                        if (!worldQuad)
                        {
                            ++m_HandHudLeftConsecutiveRawFails;
                            if (err == vr::VROverlayError_InvalidHandle || (err == vr::VROverlayError_RequestFailed && m_HandHudLeftConsecutiveRawFails >= kHandHudRecoverFailThreshold))
                                needHandHudOverlayRecover = true;
                        }
                        if (dbgTick)
                            Game::logMsg("[VR][HandHUD] left upload failed err=%d mode=%s", (int)err, worldQuad ? "world" : "raw");
                        // Important: raw upload failures (often transient when SteamVR is busy) must not
                        // commit cached state, otherwise the hand HUD can freeze forever.
                        resetHandHudCache();
                    }
                }
                m_LeftWristHudPixelsFront = backIdx;
        {
            const vr::EVROverlayError err = vr::VROverlay()->ShowOverlay(m_LeftWristHudHandle);
            m_HandHudDebugLastLeftShowErr = (int)err;
            if (err != vr::VROverlayError_None && dbgTick)
                Game::logMsg("[VR][HandHUD] left ShowOverlay failed err=%d", (int)err);
        }

        leftVisible = true;
    }
    else
    {
        // Force a full redraw next time it becomes visible.
        m_LastHudHealth = -9999;
        m_LastHudTempHealth = -9999;
        m_LastHudThrowable = -1;
        m_LastHudMedItem = -1;
        m_LastHudPillItem = -1;
        m_LastHudIncap = false;
        m_LastHudLedge = false;
        m_LastHudThirdStrike = false;
        m_LastHudAimTargetVisible = false;
        m_LastHudAimTargetIndex = -1;
        m_LastHudAimTargetPct = -1;
        m_LastHudAimTargetNameHash = 0;
        m_LastHudTeammatesHash = 0;
        vr::VROverlay()->HideOverlay(m_LeftWristHudHandle);
        leftVisible = false;
    }

    const bool canShowRight = m_RightAmmoHudEnabled && m_RightAmmoHudHandle != vr::k_ulOverlayHandleInvalid && (worldQuad || m_MouseModeEnabled || gunHandIndex != vr::k_unTrackedDeviceIndexInvalid);
    if (canShowRight)
    {
        const unsigned char bgA = (unsigned char)std::round((std::max)(0.0f, (std::min)(1.0f, m_RightAmmoHudBgAlpha)) * 255.0f);

        if ((!worldQuad || worldQuadAttachControllers) && !m_MouseModeEnabled && gunHandIndex != vr::k_unTrackedDeviceIndexInvalid)
        {
            vr::HmdMatrix34_t rel = buildRel(m_RightAmmoHudXOffset, m_RightAmmoHudYOffset, m_RightAmmoHudZOffset, m_RightAmmoHudAngleOffset);
            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_RightAmmoHudHandle, gunHandIndex, &rel);
        }
        // Opacity applies to background only; keep overlay alpha at 1 so text/bars stay readable.
        vr::VROverlay()->SetOverlayAlpha(m_RightAmmoHudHandle, 1.0f);

        // Texel aspect is per-texel pixel aspect, not texture aspect ratio. Our pixels are square.
        vr::VROverlay()->SetOverlayTexelAspect(m_RightAmmoHudHandle, 1.0f);

        int clip = 0;
        int reserve = 0;
        int upg = 0;
        int upgBits = 0;
        int weaponId = -1;
        bool pistolInfinite = false;

        if (C_WeaponCSBase* wpn = (C_WeaponCSBase*)localPlayer->GetActiveWeapon())
        {
            weaponId = (int)wpn->GetWeaponID();
            const unsigned char* wBase = reinterpret_cast<const unsigned char*>(wpn);
            clip = *reinterpret_cast<const int*>(wBase + kClip1Offset);
            const int ammoType = *reinterpret_cast<const int*>(wBase + kPrimaryAmmoTypeOffset);
            if (ammoType >= 0 && ammoType < 32)
            {
                reserve = *reinterpret_cast<const int*>(pBase + kAmmoArrayOffset + ammoType * 4);
            }
            upg = *reinterpret_cast<const int*>(wBase + kUpgradedPrimaryAmmoLoadedOffset);
            upgBits = *reinterpret_cast<const int*>(wBase + kUpgradeBitVecOffset);

            if (weaponId == (int)C_WeaponCSBase::PISTOL)
                pistolInfinite = true;
        }

        auto isAmmoHudEligible = [&](int wid) -> bool
        {
            using W = C_WeaponCSBase::WeaponID;
            switch ((W)wid)
            {
            case W::MELEE:
            case W::CHAINSAW:
            case W::MOLOTOV:
            case W::PIPE_BOMB:
            case W::VOMITJAR:
            case W::FIRST_AID_KIT:
            case W::DEFIBRILLATOR:
            case W::AMMO_PACK:
            case W::PAIN_PILLS:
            case W::ADRENALINE:
                return false;
            default:
                return true;
            }
        };

        if (weaponId <= 0 || !isAmmoHudEligible(weaponId) || clip < 0)
        {
            if (dbgTick)
                Game::logMsg("[VR][HandHUD] right: ineligible wid=%d clip=%d (melee/item/etc) -> hide", weaponId, clip);

            // Force a full redraw next time we become eligible again.
            m_LastHudClip = -9999;
            m_LastHudReserve = -9999;
            m_LastHudUpg = -9999;
            m_LastHudUpgBits = 0;
            vr::VROverlay()->HideOverlay(m_RightAmmoHudHandle);
            rightVisible = false;
            goto after_right;
        }

        if (weaponId != m_LastHudWeaponId)
        {
            m_LastHudWeaponId = weaponId;
            m_HudMaxClipObserved = (std::max)(0, clip);
            m_HudMaxReserveObserved = (std::max)(0, reserve);
        }
        else
        {
            m_HudMaxClipObserved = (std::max)(m_HudMaxClipObserved, (std::max)(0, clip));
            m_HudMaxReserveObserved = (std::max)(m_HudMaxReserveObserved, (std::max)(0, reserve));
        }
        // Auto-fit the visible overlay width so the panel tightly frames the ammo string.
        // IMPORTANT: OpenVR texture bounds (uMax) *stretch* the sampled region to fill the overlay quad.
        // If we reduce uMax without also shrinking the quad width in meters, the HUD appears "zoomed" and will clip.
        // Fix: scale overlay width in meters by the same uMax.
        const int texW = m_RightAmmoHudTexW;
        const int texH = m_RightAmmoHudTexH;
        const SevenSegStyle clipStAuto{ 12, 3, 2, 4 };
        const SevenSegStyle resStAuto{ 8, 2, 2, 3 };

        auto digitCountAuto = [](int v) -> int
        {
            if (v <= 0) return 1;
            int n = 0;
            while (v > 0) { v /= 10; ++n; }
            return n;
        };
        auto sevenSegWidthAuto = [&](int digits, const SevenSegStyle& st) -> int
        {
            const int digitW = SevenSegDigitW(st);
            if (digits <= 1) return digitW;
            return digits * digitW + (digits - 1) * st.digitGap;
        };

        const int slashW = 16;
        const int clipWMax = sevenSegWidthAuto(digitCountAuto((std::max)(0, m_HudMaxClipObserved)), clipStAuto);
        const int resWMax = pistolInfinite ? 24 : sevenSegWidthAuto(digitCountAuto((std::max)(0, m_HudMaxReserveObserved)), resStAuto);
        const int totalWMax = clipWMax + slashW + resWMax;
        const bool willDrawUpg = (upg > 0) && (((upgBits & 1) != 0) || ((upgBits & 2) != 0));

        // Padding chosen to match existing bracket/inner-box margins.
        int cropW = (std::max)(96, totalWMax + 28);
        if (willDrawUpg)
            cropW = (std::max)(cropW, totalWMax + 28 + 90);
        cropW = (std::min)(texW, cropW);

        const float uMaxAuto = (texW > 0) ? ((float)cropW / (float)texW) : 1.0f;
        const float uMaxCfg = (std::max)(0.05f, (std::min)(1.0f, m_RightAmmoHudUVMaxU));
        const float uMax = (std::max)(0.05f, (std::min)(uMaxAuto, uMaxCfg));
        rightUmaxUsed = uMax;
        const int visW = (std::max)(64, (std::min)(texW, (int)std::round((float)texW * uMax)));

        vr::VRTextureBounds_t bounds{};
        bounds.uMin = 0.0f;
        bounds.vMin = 0.0f;
        bounds.uMax = uMax;
        bounds.vMax = 1.0f;
        vr::VROverlay()->SetOverlayTextureBounds(m_RightAmmoHudHandle, &bounds);
        vr::VROverlay()->SetOverlayWidthInMeters(m_RightAmmoHudHandle, (std::max)(0.01f, m_RightAmmoHudWidthMeters) * uMax);
        const bool changed = (clip != m_LastHudClip) || (reserve != m_LastHudReserve) || (upg != m_LastHudUpg) || (upgBits != m_LastHudUpgBits);
        if (dbgTick)
        {
            Game::logMsg("[VR][HandHUD] right: wid=%d clip=%d res=%d upg=%d bits=0x%X pistolInf=%d changed=%d",
                weaponId, clip, reserve, upg, upgBits, pistolInfinite ? 1 : 0, changed ? 1 : 0);
        }
            m_LastHudClip = clip;
            m_LastHudReserve = reserve;
            m_LastHudUpg = upg;
            m_LastHudUpgBits = upgBits;

            const int w = texW;
            const int h = texH;
            const uint8_t backIdx = (uint8_t)(m_RightAmmoHudPixelsFront ^ 1);
            auto& pixels = m_RightAmmoHudPixels[backIdx];
            pixels.resize((size_t)w * (size_t)h * 4);
            HudSurface s{ pixels.data(), w, h, w * 4 };

            // Static background cache (fix: background box blinking on updates)
            if (m_RightAmmoHudBgCacheW != w || m_RightAmmoHudBgCacheH != h || m_RightAmmoHudBgCacheVisW != visW || m_RightAmmoHudBgCacheA != bgA
                || m_RightAmmoHudBgCache.size() != (size_t)w * (size_t)h * 4)
            {
                m_RightAmmoHudBgCacheW = w;
                m_RightAmmoHudBgCacheH = h;
                m_RightAmmoHudBgCacheVisW = visW;
                m_RightAmmoHudBgCacheA = bgA;
                m_RightAmmoHudBgCache.assign((size_t)w * (size_t)h * 4, 0);
                HudSurface bg{ m_RightAmmoHudBgCache.data(), w, h, w * 4 };
                Clear(bg, { 0, 0, 0, 0 });
                FillRect(bg, 0, 0, visW, h, { 6, 10, 14, bgA });
                DrawCornerBrackets(bg, 2, 2, visW - 4, h - 4, { 120, 255, 220, 220 });
                DrawRect(bg, 8, 18, visW - 16, h - 36, { 20, 80, 60, 220 }, 1);
            }

            // Start from cached background
            memcpy(s.pixels, m_RightAmmoHudBgCache.data(), m_RightAmmoHudBgCache.size());

            const int clipLowTh = (std::max)(1, (m_HudMaxClipObserved + 2) / 3);
            const int resLowTh = (std::max)(1, (m_HudMaxReserveObserved + 4) / 5);
            const bool clipLow = (clip > 0 && clip <= clipLowTh);
            const bool resLow = (!pistolInfinite && reserve >= 0 && reserve <= resLowTh);

            const Rgba clipColor = clipLow ? Rgba{ 255, 80, 80, 255 } : Rgba{ 240, 240, 240, 255 };
            const Rgba resColor = resLow ? Rgba{ 255, 80, 80, 230 } : Rgba{ 200, 200, 200, 230 };

            const SevenSegStyle clipSt{ 12, 3, 2, 4 };
            const SevenSegStyle resSt{ 8, 2, 2, 3 };

            auto digitCount = [](int v) -> int
            {
                if (v <= 0) return 1;
                int n = 0;
                while (v > 0) { v /= 10; ++n; }
                return n;
            };
            auto sevenSegWidth = [&](int digits, const SevenSegStyle& st) -> int
            {
                const int digitW = SevenSegDigitW(st);
                if (digits <= 1) return digitW;
                return digits * digitW + (digits - 1) * st.digitGap;
            };

            const int clipW = sevenSegWidth(digitCount((std::max)(0, clip)), clipSt);
            const int resW = pistolInfinite ? 24 : sevenSegWidth(digitCount((std::max)(0, reserve)), resSt);
            const int slashW = 16;
            const int totalW = clipW + slashW + resW;
            const int yBase = 46;
            int x = (std::max)(6, (visW - totalW) / 2);

            Draw7SegInt(s, x, yBase, (std::max)(0, clip), clipSt, clipColor);
            x += clipW + 2;
            DrawText5x7(s, x, yBase + 4, "/", { 200, 200, 200, 220 }, 2);
            x += slashW;
            if (pistolInfinite)
                DrawInfinity(s, x, yBase + 4, 24, 10, { 240, 240, 240, 230 });
            else
                Draw7SegInt(s, x, yBase + 2, (std::max)(0, reserve), resSt, resColor);

            const bool hasInc = (upgBits & 1) != 0;
            const bool hasExp = (upgBits & 2) != 0;
            if (upg > 0 && (hasInc || hasExp))
            {
                DrawRect(s, visW - 84, 16, 76, 32, { 120, 255, 220, 200 }, 1);
                if (hasInc) DrawIconFlame(s, visW - 78, 20, 20);
                else DrawIconBomb(s, visW - 78, 20, 20);
                char upgBuf[16];
                std::snprintf(upgBuf, sizeof(upgBuf), "%d", upg);
                DrawText5x7(s, visW - 52, 22, upgBuf, { 240, 240, 240, 255 }, 2);
            }
            // Upload every tick (no throttling/no CRC gating).
            {
                    vr::EVROverlayError err = vr::VROverlayError_None;
                    {
                        std::lock_guard<std::mutex> _lk(m_VROverlayMutex);
                        vr::IVROverlay* ov = vr::VROverlay();
                        if (!ov) ov = m_Overlay;
                        if (worldQuad)
                        {
                            const bool okUpload = UploadWorldQuadTextureRGBA(false, pixels.data(), w, h);
                            if (okUpload)
                            {
                                ov->SetOverlayTexture(m_RightAmmoHudHandle, &m_VKRightAmmoHudDyn.m_VRTexture);
                                err = vr::VROverlayError_None;
                            }
                            else
                            {
                                // Fallback: if dxvk VR bridge isn't available, still try raw upload.
                                err = ov ? ov->SetOverlayRaw(m_RightAmmoHudHandle, pixels.data(), (uint32_t)w, (uint32_t)h, 4) : vr::VROverlayError_RequestFailed;
                            }
                        }
                        else
                        {
                            err = ov ? ov->SetOverlayRaw(m_RightAmmoHudHandle, pixels.data(), (uint32_t)w, (uint32_t)h, 4) : vr::VROverlayError_RequestFailed;
                        }
                    }
                    m_HandHudDebugLastRightSetRawErr = (int)err;
                    if (err == vr::VROverlayError_None)
                    {
                        m_HandHudDebugLastRightUpload = dbgNow;
                        ++m_HandHudDebugRightUploadCount;
                        m_HandHudRightConsecutiveRawFails = 0;
                    }
                    else
                    {
                        if (!worldQuad)
                        {
                            ++m_HandHudRightConsecutiveRawFails;
                            if (err == vr::VROverlayError_InvalidHandle || (err == vr::VROverlayError_RequestFailed && m_HandHudRightConsecutiveRawFails >= kHandHudRecoverFailThreshold))
                                needHandHudOverlayRecover = true;
                        }
                        if (dbgTick)
                            Game::logMsg("[VR][HandHUD] right upload failed err=%d mode=%s", (int)err, worldQuad ? "world" : "raw");
                        // Same as left: force retry next tick so it can't get stuck.
                        resetHandHudCache();
                    }
                }
                m_RightAmmoHudPixelsFront = backIdx;
        {
            const vr::EVROverlayError err = vr::VROverlay()->ShowOverlay(m_RightAmmoHudHandle);
            m_HandHudDebugLastRightShowErr = (int)err;
            if (err != vr::VROverlayError_None && dbgTick)
                Game::logMsg("[VR][HandHUD] right ShowOverlay failed err=%d", (int)err);
        }

        rightVisible = true;
    }
    else
    {
        // Force a full redraw next time it becomes visible.
        m_LastHudClip = -9999;
        m_LastHudReserve = -9999;
        m_LastHudUpg = -9999;
        m_LastHudUpgBits = 0;
        vr::VROverlay()->HideOverlay(m_RightAmmoHudHandle);
        rightVisible = false;
    }


after_right:
    if (needHandHudOverlayRecover)
    {
        float since = secsSince(m_HandHudLastOverlayRecover);
        if (since < 0.0f) since = 9999.0f;

        if (since >= kHandHudRecoverMinIntervalSec)
        {
            vr::EVROverlayError eLeft = vr::VROverlayError_None;
            vr::EVROverlayError eRight = vr::VROverlayError_None;

            {
                std::lock_guard<std::mutex> _lk(m_VROverlayMutex);
                vr::IVROverlay* ov = vr::VROverlay();
                if (!ov) ov = m_Overlay;
                if (ov) m_Overlay = ov;

                if (ov)
                {
                    auto destroyIfValid = [&](vr::VROverlayHandle_t& h)
                    {
                        if (h != vr::k_ulOverlayHandleInvalid)
                        {
                            ov->HideOverlay(h);
                            ov->DestroyOverlay(h);
                            h = vr::k_ulOverlayHandleInvalid;
                        }
                    };

                    destroyIfValid(m_LeftWristHudHandle);
                    destroyIfValid(m_RightAmmoHudHandle);

                    eLeft = ov->CreateOverlay("LeftWristHudOverlayKey", "LeftWristHUD", &m_LeftWristHudHandle);
                    if (eLeft != vr::VROverlayError_None)
                    {
                        vr::VROverlayHandle_t found = vr::k_ulOverlayHandleInvalid;
                        if (ov->FindOverlay("LeftWristHudOverlayKey", &found) == vr::VROverlayError_None)
                            m_LeftWristHudHandle = found;
                    }

                    eRight = ov->CreateOverlay("RightAmmoHudOverlayKey", "RightAmmoHUD", &m_RightAmmoHudHandle);
                    if (eRight != vr::VROverlayError_None)
                    {
                        vr::VROverlayHandle_t found = vr::k_ulOverlayHandleInvalid;
                        if (ov->FindOverlay("RightAmmoHudOverlayKey", &found) == vr::VROverlayError_None)
                            m_RightAmmoHudHandle = found;
                    }

                    if (m_LeftWristHudHandle != vr::k_ulOverlayHandleInvalid)
                        ov->SetOverlayInputMethod(m_LeftWristHudHandle, vr::VROverlayInputMethod_None);
                    if (m_RightAmmoHudHandle != vr::k_ulOverlayHandleInvalid)
                        ov->SetOverlayInputMethod(m_RightAmmoHudHandle, vr::VROverlayInputMethod_None);
                }
            }

            m_HandHudLastOverlayRecover = dbgNow;
            ++m_HandHudOverlayRecoverCount;
            m_HandHudLeftConsecutiveRawFails = 0;
            m_HandHudRightConsecutiveRawFails = 0;

            resetHandHudCache();

            if (dbgTick)
            {
                Game::logMsg("[VR][HandHUD] overlay recover: recreated (eL=%d eR=%d) count=%u newHandles L=%llu R=%llu",
                    (int)eLeft, (int)eRight, (unsigned)m_HandHudOverlayRecoverCount,
                    (unsigned long long)m_LeftWristHudHandle, (unsigned long long)m_RightAmmoHudHandle);
            }

            return;

        }
    }

    if (dbgTick)
    {
        Game::logMsg(
            "[VR][HandHUD] tick: Lcan=%d Rcan=%d Lvis=%d Rvis=%d LrawErr=%d RrawErr=%d LshowErr=%d RshowErr=%d Lcnt=%u Rcnt=%u Lsince=%.2fs Rsince=%.2fs Lfail=%u Rfail=%u Rec=%u",
            canShowLeft ? 1 : 0, canShowRight ? 1 : 0, leftVisible ? 1 : 0, rightVisible ? 1 : 0,
            m_HandHudDebugLastLeftSetRawErr, m_HandHudDebugLastRightSetRawErr,
            m_HandHudDebugLastLeftShowErr, m_HandHudDebugLastRightShowErr,
            (unsigned)m_HandHudDebugLeftUploadCount, (unsigned)m_HandHudDebugRightUploadCount,
            secsSince(m_HandHudDebugLastLeftUpload), secsSince(m_HandHudDebugLastRightUpload),
            (unsigned)m_HandHudLeftConsecutiveRawFails, (unsigned)m_HandHudRightConsecutiveRawFails, (unsigned)m_HandHudOverlayRecoverCount);
    }

    // World-quad mode (HMD-quad): place them as a pair of panels relative to the HMD.
    // If worldQuadAttachControllers is enabled, we keep the original controller anchor and skip this placement.
    if (worldQuad && !worldQuadAttachControllers && !m_MouseModeEnabled && (leftVisible || rightVisible))
    {
        const float wL = leftVisible ? (std::max)(0.01f, m_LeftWristHudWidthMeters) : 0.0f;
        const float wR = rightVisible ? (std::max)(0.01f, m_RightAmmoHudWidthMeters) * (std::max)(0.05f, (std::min)(1.0f, rightUmaxUsed)) : 0.0f;
        const float gapX = (std::max)(0.0f, m_HandHudWorldQuadXGapMeters);

        float xL = 0.0f;
        float xR = 0.0f;
        if (leftVisible && rightVisible)
        {
            xL = -(gapX * 0.5f + wL * 0.5f);
            xR = +(gapX * 0.5f + wR * 0.5f);
        }

        const float y = m_HandHudWorldQuadYOffsetMeters;
        const float z = -(std::max)(0.05f, m_HandHudWorldQuadDistanceMeters);
        const QAngle ang = m_HandHudWorldQuadAngleOffset;
        const vr::TrackedDeviceIndex_t parentDev = vr::k_unTrackedDeviceIndex_Hmd;

        if (leftVisible)
        {
            vr::HmdMatrix34_t rel = buildRel(xL, y, z, ang);
            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_LeftWristHudHandle, parentDev, &rel);
        }
        if (rightVisible)
        {
            vr::HmdMatrix34_t rel = buildRel(xR, y, z, ang);
            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_RightAmmoHudHandle, parentDev, &rel);
        }
    }

    // Mouse mode: hand HUDs are not bound to controllers.
    // Place them side-by-side under the RearMirrorOverlay, inheriting its pose.
    if (m_MouseModeEnabled && (leftVisible || rightVisible) && m_RearMirrorHandle != vr::k_ulOverlayHandleInvalid)
    {
        auto mul34 = [](const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b) -> vr::HmdMatrix34_t
        {
            vr::HmdMatrix34_t out{};
            for (int r = 0; r < 3; ++r)
            {
                for (int c = 0; c < 3; ++c)
                    out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
                out.m[r][3] = a.m[r][0] * b.m[0][3] + a.m[r][1] * b.m[1][3] + a.m[r][2] * b.m[2][3] + a.m[r][3];
            }
            return out;
        };

        auto translate34 = [](float x, float y, float z) -> vr::HmdMatrix34_t
        {
            vr::HmdMatrix34_t t{};
            t.m[0][0] = 1.0f; t.m[0][1] = 0.0f; t.m[0][2] = 0.0f; t.m[0][3] = x;
            t.m[1][0] = 0.0f; t.m[1][1] = 1.0f; t.m[1][2] = 0.0f; t.m[1][3] = y;
            t.m[2][0] = 0.0f; t.m[2][1] = 0.0f; t.m[2][2] = 1.0f; t.m[2][3] = z;
            return t;
        };

        auto getWidthMeters = [&](vr::VROverlayHandle_t h, float fallback) -> float
        {
            float w = 0.0f;
            if (vr::VROverlay()->GetOverlayWidthInMeters(h, &w) == vr::VROverlayError_None && w > 0.001f)
                return w;
            return fallback;
        };

        float mirrorW = 0.0f;
        if (vr::VROverlay()->GetOverlayWidthInMeters(m_RearMirrorHandle, &mirrorW) != vr::VROverlayError_None || mirrorW <= 0.001f)
            mirrorW = (std::max)(0.01f, m_RearMirrorOverlayWidthMeters);

        const float wL = leftVisible ? getWidthMeters(m_LeftWristHudHandle, (std::max)(0.01f, m_LeftWristHudWidthMeters)) : 0.0f;
        const float wR = rightVisible ? getWidthMeters(m_RightAmmoHudHandle, (std::max)(0.01f, m_RightAmmoHudWidthMeters)) : 0.0f;

        const float gapX = 0.01f;
        float xL = 0.0f;
        float xR = 0.0f;
        if (leftVisible && rightVisible)
        {
            // Keep the combined bounding box centered.
            xL = -(wR + gapX) * 0.5f;
            xR = +(wL + gapX) * 0.5f;
        }

        const float lAspect = (m_LeftWristHudTexW > 0) ? ((float)m_LeftWristHudTexH / (float)m_LeftWristHudTexW) : 0.5f;
        const float rAspect = (m_RightAmmoHudTexW > 0) ? ((float)m_RightAmmoHudTexH / (float)m_RightAmmoHudTexW) : 0.5f;
        const float hL = wL * lAspect;
        const float hR = wR * rAspect;
        const float rowH = (std::max)(hL, hR);
        const float gapY = 0.01f;
        const float yRow = -(mirrorW * 0.5f + rowH * 0.5f + gapY);

        vr::VROverlayTransformType parentType{};
        if (vr::VROverlay()->GetOverlayTransformType(m_RearMirrorHandle, &parentType) != vr::VROverlayError_None)
            parentType = vr::VROverlayTransform_Absolute;

        if (parentType == vr::VROverlayTransform_Absolute)
        {
            vr::ETrackingUniverseOrigin origin{};
            vr::HmdMatrix34_t originToMirror{};
            if (vr::VROverlay()->GetOverlayTransformAbsolute(m_RearMirrorHandle, &origin, &originToMirror) == vr::VROverlayError_None)
            {
                if (leftVisible)
                {
                    const vr::HmdMatrix34_t mat = mul34(originToMirror, translate34(xL, yRow, 0.0f));
                    vr::VROverlay()->SetOverlayTransformAbsolute(m_LeftWristHudHandle, origin, &mat);
                }
                if (rightVisible)
                {
                    const vr::HmdMatrix34_t mat = mul34(originToMirror, translate34(xR, yRow, 0.0f));
                    vr::VROverlay()->SetOverlayTransformAbsolute(m_RightAmmoHudHandle, origin, &mat);
                }
            }
        }
        else if (parentType == vr::VROverlayTransform_TrackedDeviceRelative)
        {
            vr::TrackedDeviceIndex_t parentDev = vr::k_unTrackedDeviceIndexInvalid;
            vr::HmdMatrix34_t devToMirror{};
            if (vr::VROverlay()->GetOverlayTransformTrackedDeviceRelative(m_RearMirrorHandle, &parentDev, &devToMirror) == vr::VROverlayError_None
                && parentDev != vr::k_unTrackedDeviceIndexInvalid)
            {
                if (leftVisible)
                {
                    const vr::HmdMatrix34_t mat = mul34(devToMirror, translate34(xL, yRow, 0.0f));
                    vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_LeftWristHudHandle, parentDev, &mat);
                }
                if (rightVisible)
                {
                    const vr::HmdMatrix34_t mat = mul34(devToMirror, translate34(xR, yRow, 0.0f));
                    vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(m_RightAmmoHudHandle, parentDev, &mat);
                }
            }
        }
    }

}
