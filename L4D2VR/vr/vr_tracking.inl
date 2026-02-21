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

    // Observer state (used for in-eye spectator anchor/viewmodel stability).
    const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
    const int teamNum = *reinterpret_cast<const int*>(base + kTeamNumOffset);
    const unsigned char lifeState = *reinterpret_cast<const unsigned char*>(base + kLifeStateOffset);
    const bool isObserver = (teamNum == 1) || (lifeState != 0);
    const int obsMode = *reinterpret_cast<const int*>(base + kObserverModeOffset);
    const int obsTarget = *reinterpret_cast<const int*>(base + kObserverTargetOffset);
    auto handleValid = [](int h) { return (h != 0 && h != -1); };

    C_BasePlayer* viewPlayer = localPlayer;
    bool inEyeObserver = false;
    if (isObserver && obsMode == 4 && handleValid(obsTarget) && m_Game && m_Game->m_ClientEntityList)
    {
        if (C_BaseEntity* ent = (C_BaseEntity*)m_Game->m_ClientEntityList->GetClientEntityFromHandle(obsTarget))
        {
            viewPlayer = (C_BasePlayer*)ent;
            inEyeObserver = true;
        }
    }

    if (!inEyeObserver)
    {
        m_ObserverInEyeWasActivePrev = false;
        m_ObserverInEyeTargetPrev = 0;
        m_ResetPositionAfterObserverTargetSwitchPending = false;
    }
    else
    {
        if (!m_ObserverInEyeWasActivePrev || obsTarget != m_ObserverInEyeTargetPrev)
        {
            m_ResetPositionAfterObserverTargetSwitchPending = true;
            m_ObserverInEyeTargetPrev = obsTarget;
        }
        m_ObserverInEyeWasActivePrev = true;
    }

    // Spectator/observer: default to free-roaming camera (instead of chase cam locked to a teammate).
    // We only do this once per observer session, so the user can still manually switch modes afterwards.
    if (m_ObserverDefaultFreeCam)
    {
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
    m_Game->m_IsMeleeWeaponActive = viewPlayer->IsMeleeWeaponActive();

    // Scope: only render/show when holding a firearm
    m_ScopeWeaponIsFirearm = false;
    if (C_BaseCombatWeapon* active = viewPlayer->GetActiveWeapon())
    {
        if (C_WeaponCSBase* weapon = (C_WeaponCSBase*)active)
            m_ScopeWeaponIsFirearm = IsFirearmWeaponId(weapon->GetWeaponID());
    }
    RefreshActiveViewmodelAdjustment(viewPlayer);

    if (!m_IsThirdPersonCamera)
    {
        Vector eyeOrigin = viewPlayer->EyePosition();
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
        Vector absOrigin = viewPlayer->GetAbsOrigin();

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

    // Roomscale camera behavior:
    // - Legacy: only allow roomscale camera anchoring while "standing still" (velocity==0), and disable while thumbstick locomotion is active.
    // - 1:1 server roomscale (ForceNonVRServerMovement=false): optionally keep the camera fully decoupled from the tick-rate player origin
    //   so HMD motion stays smooth at headset refresh rate.
    const bool want1to1DecoupledCamera =
        m_Roomscale1To1Movement && !m_ForceNonVRServerMovement && m_Roomscale1To1DecoupleCamera && !m_LocomotionActive;

    if (inEyeObserver)
    {
        m_RoomscaleActive = false;
    }
    else if (want1to1DecoupledCamera)
    {
        // In 1:1 mode, the camera should not be dragged by the player's tick-rate origin updates.
        m_RoomscaleActive = true;
    }
    else if (localPlayer->m_hGroundEntity != -1 && localPlayer->m_vecVelocity.IsZero())
    {
        m_RoomscaleActive = true;
    }
    else
    {
        m_RoomscaleActive = false;
    }

    // Keep the legacy "camera following" escape hatch, but never override the explicit 1:1 decoupled camera mode.
    if (!want1to1DecoupledCamera)
    {
        if ((cameraFollowing < 0 && cameraDistance > 1) || (m_LocomotionActive))
            m_RoomscaleActive = false;
    }
    else
    {
        if (m_LocomotionActive)
            m_RoomscaleActive = false;
    }

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
    // Observer in-eye: when switching spectated target, re-align anchors once.
    if (m_ResetPositionAfterObserverTargetSwitchPending)
    {
        ResetPosition();
        m_ResetPositionAfterObserverTargetSwitchPending = false;
    }
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

