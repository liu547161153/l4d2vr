bool VR::DecodeRoomscale1To1Delta(int weaponsubtype, Vector& outDeltaMeters)
{
    uint32_t v = (uint32_t)weaponsubtype;
    // Reliable on-wire format: packed into CUserCmd::buttons *unused high bits*.
    // Source's usercmd delta encoding for weaponsubtype is not guaranteed to survive when weaponselect==0.
    // L4D2 button flags are below bit 26 (IN_ATTACK3 is 1<<25), so bits 26..31 are safe for our payload.
    // Layout: dx_cm: bits 26..28 (signed 3-bit, [-4..3]), dy_cm: bits 29..31 (signed 3-bit, [-4..3]).
    static constexpr uint32_t kRSButtonsMask = 0xFC000000u; // bits 26..31
    if (v & kRSButtonsMask)
    {
        auto decodeSigned3 = [](int s3) -> int
            {
                s3 &= 0x7;
                if (s3 & 0x4)
                    s3 -= 8;
                return s3;
            };
        const int dx_cm = decodeSigned3((int)((v >> 26) & 0x7));
        const int dy_cm = decodeSigned3((int)((v >> 29) & 0x7));
        if (dx_cm == 0 && dy_cm == 0)
            return false;
        outDeltaMeters = Vector(dx_cm * 0.01f, dy_cm * 0.01f, 0.0f);
        return true;
    }
    // Legacy 32-bit format.
    if ((v & kRSMarker32) != 0)
    {
        int rx = (int)(v & kAxisMask32);
        int ry = (int)((v >> kAxisBits32) & kAxisMask32);
        int dx = rx - kAxisBias32;
        int dy = ry - kAxisBias32;
        outDeltaMeters = Vector(dx * 0.01f, dy * 0.01f, 0.0f);
        return true;
    }

    // Compact 15-bit format (survives 16-bit truncation).
    if ((v & kRSMarker15) != 0)
    {
        int rx = (int)(v & kAxisMask15);
        int ry = (int)((v >> kAxisBits15) & kAxisMask15);
        int dx = rx - kAxisBias15;
        int dy = ry - kAxisBias15;
        outDeltaMeters = Vector(dx * 0.01f, dy * 0.01f, 0.0f);
        return true;
    }

    return false;
}

void VR::EncodeRoomscale1To1Move(CUserCmd* cmd)
{
    if (!cmd || !m_Roomscale1To1Movement)
        return;

    // IMPORTANT: Do NOT rely on weaponsubtype for the 1:1 delta on the wire.
    // It often does not survive usercmd delta encoding when weaponselect==0.
    // Instead, pack into the unused high bits of cmd->buttons.
    static constexpr uint32_t kRSButtonsMask = 0xFC000000u; // bits 26..31

    const uint32_t buttonsBefore = (uint32_t)cmd->buttons;
    // Ensure we never accidentally re-send a previous packed delta.
    cmd->buttons &= ~(int)kRSButtonsMask;
    cmd->weaponsubtype = 0;

    // HARD RULE: never apply 1:1 SetAbsOrigin-style movement while the player is using
    // normal locomotion controls (keyboard WASD, thumbstick, etc.).
    // If we mix both, the server-side SetAbsOrigin runs alongside standard movement and
    // can manifest as a periodic "stop"/stutter in player movement.
    //
    // Note: we intentionally use the raw cmd move values here (not only m_LocomotionActive),
    // because some call paths can update m_LocomotionActive late or not at all.
    const bool controlLocomotionNow =
        (fabsf(cmd->forwardmove) > 0.5f) ||
        (fabsf(cmd->sidemove) > 0.5f) ||
        (fabsf(cmd->upmove) > 0.5f);
    if (controlLocomotionNow)
    {
        m_Roomscale1To1PrevCorrectedAbs = m_HmdPosCorrectedPrev;
        m_Roomscale1To1PrevValid = true;
        m_Roomscale1To1AccumMeters = Vector(0.0f, 0.0f, 0.0f);
        m_Roomscale1To1ChaseActive = false;
        m_Roomscale1To1LocomotionCooldownCmds = 8;
        return;
    }

    if (m_Roomscale1To1LocomotionCooldownCmds > 0)
    {
        --m_Roomscale1To1LocomotionCooldownCmds;
        m_Roomscale1To1PrevCorrectedAbs = m_HmdPosCorrectedPrev;
        m_Roomscale1To1PrevValid = true;
        m_Roomscale1To1AccumMeters = Vector(0.0f, 0.0f, 0.0f);
        m_Roomscale1To1ChaseActive = false;
        return;
    }

    const bool locomotionBlock = m_Roomscale1To1DisableWhileThumbstick && m_LocomotionActive;

    // Not in 1:1 server-teleport mode: keep continuity for when it re-enables.
    if (m_ForceNonVRServerMovement || locomotionBlock)
    {
        if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
            Game::logMsg("[VR][1to1][encode] skip (%s) cmd=%d", m_ForceNonVRServerMovement ? "ForceNonVRServerMovement=true" : "locomotion", cmd->command_number);

        m_Roomscale1To1PrevCorrectedAbs = m_HmdPosCorrectedPrev;
        m_Roomscale1To1PrevValid = true;
        m_Roomscale1To1AccumMeters = Vector(0.0f, 0.0f, 0.0f);
        m_Roomscale1To1ChaseActive = false;
        if (locomotionBlock)
            m_Roomscale1To1LocomotionCooldownCmds = 8;
        return;
    }

    if (cmd->weaponselect != 0)
    {
        if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
            Game::logMsg("[VR][1to1][encode] skip (weaponselect=%d) cmd=%d", cmd->weaponselect, cmd->command_number);

        m_Roomscale1To1PrevCorrectedAbs = m_HmdPosCorrectedPrev;
        m_Roomscale1To1PrevValid = true;
        m_Roomscale1To1AccumMeters = Vector(0.0f, 0.0f, 0.0f);
        m_Roomscale1To1ChaseActive = false;
        return;
    }

    // --------------------------------------------------------------------
    // New behavior (default): camera-only 1:1 render; pull the player entity
    // only when the camera has drifted too far away.
    // This avoids the "30Hz tick stepping" feeling on high-refresh HMDs.
    // --------------------------------------------------------------------
    if (m_Roomscale1To1UseCameraDistanceChase)
    {
        // Keep continuity for legacy mode toggles.
        m_Roomscale1To1PrevCorrectedAbs = m_HmdPosCorrectedPrev;
        m_Roomscale1To1PrevValid = true;

        // Chase mode should not fight normal locomotion (keyboard/thumbstick).
        // While locomotion is active, the engine is already moving the player and third-person camera
        // can introduce intentional camera/player offsets. So disable chase and reset state.
        const bool anyLocomotionNow = m_LocomotionActive || (fabsf(cmd->forwardmove) > 0.5f) || (fabsf(cmd->sidemove) > 0.5f) || (fabsf(cmd->upmove) > 0.5f);
        if (anyLocomotionNow)
        {
            m_Roomscale1To1ChaseActive = false;
            m_Roomscale1To1AccumMeters = Vector(0.0f, 0.0f, 0.0f);
            return;
        }

        Vector toCamU = m_SetupOriginToHMD;
        toCamU.z = 0.0f;

        const float distU = std::sqrt((toCamU.x * toCamU.x) + (toCamU.y * toCamU.y));
        const float scale = std::max(0.001f, m_VRScale);
        const float distM = distU / scale;

        const float start = std::max(0.0f, m_Roomscale1To1AllowedCameraDriftMeters);
        const float hyst = std::clamp(m_Roomscale1To1ChaseHysteresisMeters, 0.0f, start);
        const float stop = std::max(0.0f, start - hyst);

        if (!m_Roomscale1To1ChaseActive)
        {
            if (distM <= start)
                return;
            m_Roomscale1To1ChaseActive = true;
        }
        else
        {
            if (distM <= stop)
            {
                m_Roomscale1To1ChaseActive = false;
                return;
            }
        }

        if (distU < 0.0001f)
            return;

        const float needM = distM - stop;
        if (needM < std::max(0.0f, m_Roomscale1To1MinApplyMeters))
            return;

        const float stepM = std::min(std::max(0.0f, m_Roomscale1To1MaxStepMeters), needM);
        Vector deltaM((toCamU.x / distU) * stepM, (toCamU.y / distU) * stepM, 0.0f);

        int dx_cm = (int)roundf(deltaM.x * 100.0f);
        int dy_cm = (int)roundf(deltaM.y * 100.0f);
        if (dx_cm == 0 && dy_cm == 0)
            return;

        // We have 3 signed bits per axis => [-4..3] cm per cmd.
        int send_dx_cm = dx_cm;
        int send_dy_cm = dy_cm;
        if (send_dx_cm < -4) send_dx_cm = -4;
        if (send_dx_cm > 3)  send_dx_cm = 3;
        if (send_dy_cm < -4) send_dy_cm = -4;
        if (send_dy_cm > 3)  send_dy_cm = 3;

        if (send_dx_cm != 0 || send_dy_cm != 0)
        {
            const uint32_t dx3 = (uint32_t)(send_dx_cm & 0x7);
            const uint32_t dy3 = (uint32_t)(send_dy_cm & 0x7);
            const uint32_t packed = (dx3 << 26) | (dy3 << 29);
            cmd->buttons = (int)((cmd->buttons & ~(int)kRSButtonsMask) | (int)packed);
        }

        const uint32_t buttonsAfter = (uint32_t)cmd->buttons;
        if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
        {
            Game::logMsg("[VR][1to1][encode] chase cmd=%d tick=%d cmdptr=%p buttons 0x%08X->0x%08X drift=%.3fm start=%.3fm stop=%.3fm step=%.3fm send=(%d,%d)",
                +cmd->command_number, cmd->tick_count, (void*)cmd,
                (unsigned)buttonsBefore, (unsigned)buttonsAfter,
                distM, start, stop, stepM, send_dx_cm, send_dy_cm);
        }

        // Chase mode doesn't use the legacy delta accumulator.
        m_Roomscale1To1AccumMeters = Vector(0.0f, 0.0f, 0.0f);
        return;
    }

    // --------------------------------------------------------------------
    // Legacy behavior: push the player by per-tick HMD delta.
    // --------------------------------------------------------------------
    m_Roomscale1To1ChaseActive = false;

    Vector cur = m_HmdPosCorrectedPrev;
    if (!m_Roomscale1To1PrevValid)
    {
        m_Roomscale1To1PrevCorrectedAbs = cur;
        m_Roomscale1To1PrevValid = true;
        m_Roomscale1To1AccumMeters = Vector(0.0f, 0.0f, 0.0f);
        if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
            Game::logMsg("[VR][1to1][encode] init prev cmd=%d tick=%d cmdptr=%p cur=(%.3f %.3f %.3f)", cmd->command_number, cmd->tick_count, (void*)cmd, cur.x, cur.y, cur.z);

        return;
    }

    Vector d = cur - m_Roomscale1To1PrevCorrectedAbs;
    d.z = 0.0f;

    float len = std::sqrt((d.x * d.x) + (d.y * d.y));
    if (len < std::max(0.0f, m_Roomscale1To1MinApplyMeters))
    {
        // Ignore micro-movement/noise (helps avoid conflicts with thumbstick locomotion).
        m_Roomscale1To1PrevCorrectedAbs = cur;
        return;
    }

    if (len > m_Roomscale1To1MaxStepMeters && len > 0.0001f)
    {
        float s = m_Roomscale1To1MaxStepMeters / len;
        d.x *= s;
        d.y *= s;
    }

    m_Roomscale1To1PrevCorrectedAbs = cur;

    // Accumulate sub-centimeter movement so slow walking/leaning still moves the player.
    m_Roomscale1To1AccumMeters = m_Roomscale1To1AccumMeters + d;
    m_Roomscale1To1AccumMeters.z = 0.0f;

    int dx_cm = (int)roundf(m_Roomscale1To1AccumMeters.x * 100.0f);
    int dy_cm = (int)roundf(m_Roomscale1To1AccumMeters.y * 100.0f);

    if (dx_cm == 0 && dy_cm == 0)
        return;

    // We have 3 signed bits per axis => [-4..3] cm per cmd.
    // Keep remainder in m_Roomscale1To1AccumMeters for later cmds.
    int send_dx_cm = dx_cm;
    int send_dy_cm = dy_cm;
    if (send_dx_cm < -4) send_dx_cm = -4;
    if (send_dx_cm > 3)  send_dx_cm = 3;
    if (send_dy_cm < -4) send_dy_cm = -4;
    if (send_dy_cm > 3)  send_dy_cm = 3;
    const bool willSend = (send_dx_cm != 0 || send_dy_cm != 0);
    if (willSend)
    {
        const uint32_t dx3 = (uint32_t)(send_dx_cm & 0x7);
        const uint32_t dy3 = (uint32_t)(send_dy_cm & 0x7);
        const uint32_t packed = (dx3 << 26) | (dy3 << 29);
        cmd->buttons = (int)((cmd->buttons & ~(int)kRSButtonsMask) | (int)packed);
        // Keep the fractional remainder for future commands.
        m_Roomscale1To1AccumMeters.x -= send_dx_cm * 0.01f;
        m_Roomscale1To1AccumMeters.y -= send_dy_cm * 0.01f;
    }
    const uint32_t buttonsAfter = (uint32_t)cmd->buttons;
    if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
    {
        Game::logMsg("[VR][1to1][encode] cmd=%d tick=%d cmdptr=%p buttons 0x%08X->0x%08X cur=(%.3f %.3f) d=(%.3f %.3f) cm=(%d,%d) send=(%d,%d)",
            +cmd->command_number, cmd->tick_count, (void*)cmd, (unsigned)buttonsBefore, (unsigned)buttonsAfter,
            +cur.x, cur.y, d.x, d.y, dx_cm, dy_cm, send_dx_cm, send_dy_cm);
    }
}

void VR::OnPredictionRunCommand(CUserCmd* cmd)
{
    if (!cmd || !m_Roomscale1To1Movement || m_ForceNonVRServerMovement)
        return;

    // Prediction-side hard guard: never apply 1:1 SetAbsOrigin while control locomotion is active.
    // Even if a packed delta from an adjacent frame is present, mixing both paths can feel like a
    // periodic pull-back/stutter while moving.
    const bool controlLocomotionNow =
        (fabsf(cmd->forwardmove) > 0.5f) ||
        (fabsf(cmd->sidemove) > 0.5f) ||
        (fabsf(cmd->upmove) > 0.5f) ||
        m_LocomotionActive ||
        m_PushingThumbstick ||
        (m_Roomscale1To1LocomotionCooldownCmds > 0);
    if (controlLocomotionNow)
        return;

    Vector deltaM;
    if (!DecodeRoomscale1To1Delta(cmd->buttons, deltaM) || cmd->weaponselect != 0)
        return;
    if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastPredict, m_Roomscale1To1DebugLogHz))
    {
        Game::logMsg("[VR][1to1][predict] cmd=%d tick=%d cmdptr=%p wsel=%d buttons=0x%08X dM=(%.3f %.3f)",
            cmd->command_number, cmd->tick_count, (void*)cmd, cmd->weaponselect, (unsigned)(uint32_t)cmd->buttons, deltaM.x, deltaM.y);
    }
    // IMPORTANT: Do NOT clear the packed bits here.
    // The same CUserCmd instance is typically later serialized and sent to the server.
    // Clearing it during prediction would prevent the server from ever seeing/applying
    // the 1:1 roomscale delta, causing rubber-banding/pullback.

    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_ClientEntityList || !m_Game->m_EngineTrace || !m_Game->m_Offsets)
        return;

    int idx = m_Game->m_EngineClient->GetLocalPlayer();
    C_BaseEntity* ent = (idx > 0) ? m_Game->GetClientEntity(idx) : nullptr;
    if (!ent)
        return;

    using SetAbsOriginFn = void(__thiscall*)(void*, const Vector&);
    auto setAbsOrigin = (SetAbsOriginFn)m_Game->m_Offsets->CBaseEntity_SetAbsOrigin_Client.address;
    if (!setAbsOrigin)
        return;

    Vector origin = ent->GetAbsOrigin();
    Vector deltaU(deltaM.x * m_VRScale, deltaM.y * m_VRScale, 0.0f);
    Vector target = origin + deltaU;

    Vector mins(-16, -16, 0);
    Vector maxs(16, 16, 72);

    Ray_t ray;
    ray.Init(origin, target, mins, maxs);

    trace_t tr;
    CTraceFilterSkipSelf filter((IHandleEntity*)ent, 0);
    const unsigned int mask = CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP | CONTENTS_GRATE;
    m_Game->m_EngineTrace->TraceRay(ray, mask, &filter, &tr);

    if (!tr.startsolid)
        setAbsOrigin(ent, tr.endpos);
    if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastPredict, m_Roomscale1To1DebugLogHz))
    {
        Game::logMsg("[VR][1to1][predict] origin=(%.2f %.2f %.2f) target=(%.2f %.2f %.2f) end=(%.2f %.2f %.2f) frac=%.3f startsolid=%d",
            origin.x, origin.y, origin.z, target.x, target.y, target.z, tr.endpos.x, tr.endpos.y, tr.endpos.z, tr.fraction, (int)tr.startsolid);
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
    if (m_MouseModeEnabled)
    {
        const Vector& anchor = IsMouseModeScopeActive() ? m_MouseModeScopedViewmodelAnchorOffset : m_MouseModeViewmodelAnchorOffset;
        viewmodelPos = m_HmdPosAbs
            + (m_HmdForward * (anchor.x * m_VRScale))
            + (m_HmdRight * (anchor.y * m_VRScale))
            + (m_HmdUp * (anchor.z * m_VRScale));
    }
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
    m_CreatedVRTextures.store(false, std::memory_order_release);
    m_RenderedNewFrame = false;
    m_RenderedHud.store(false, std::memory_order_release);
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

