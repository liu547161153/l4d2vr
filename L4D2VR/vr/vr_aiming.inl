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
        // Aim-line teammate HUD hint is only meaningful while the aim line is active.
        UpdateAimTeammateHudTarget(localPlayer, Vector{}, Vector{}, false);

        return;
    }

    // If debug overlay isn't available, don't draw, but keep the guard working.
    if (!canDraw)
    {
        UpdateFriendlyFireAimHit(localPlayer);
        UpdateAimTeammateHudTarget(localPlayer, Vector{}, Vector{}, false);

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
        UpdateAimTeammateHudTarget(localPlayer, Vector{}, Vector{}, false);

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

    // Teammate info hint: track which teammate the aim line is resting on.
    UpdateAimTeammateHudTarget(localPlayer, origin, target, true);


    m_AimLineStart = origin;
    m_AimLineEnd = target;
    m_HasAimLine = true;
    m_HasThrowArc = false;

    if (canDraw && allowAimLineDraw)
        DrawAimLine(origin, target);
}

void VR::UpdateAimTeammateHudTarget(C_BasePlayer* localPlayer, const Vector& start, const Vector& end, bool aimLineActive)
{
    using namespace std::chrono;
    const auto now = steady_clock::now();

    // Behavior update: show immediately on teammate hit, hide 1s after leaving teammate.
    constexpr float kLingerSeconds = 1.0f;
    // Small stickiness to prevent hitbox-edge flicker from causing false leave/reacquire.
    constexpr float kStickinessSeconds = 0.15f;

    if (!localPlayer || !m_Game || !m_Game->m_EngineTrace || !m_Game->m_ClientEntityList)
    {
        m_AimTeammateCandidateIndex = -1;
        m_AimTeammateLastRawIndex = -1;
        if (m_AimTeammateDisplayIndex > 0 && now > m_AimTeammateDisplayUntil)
            m_AimTeammateDisplayIndex = -1;
        return;
    }

    if (!aimLineActive)
    {
        m_AimTeammateCandidateIndex = -1;
        m_AimTeammateLastRawIndex = -1;
        if (m_AimTeammateDisplayIndex > 0 && now > m_AimTeammateDisplayUntil)
            m_AimTeammateDisplayIndex = -1;
        return;
    }

    int hitIdx = -1;
    {
        CGameTrace tr;
        Ray_t ray;

        // Skip self and the mounted-gun use entity (if any) so the ray doesn't collide with the turret base.
        C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);
        CTraceFilterSkipSelf filterSelf((IHandleEntity*)localPlayer, 0);
        CTraceFilterSkipTwoEntities filterTwo((IHandleEntity*)localPlayer, (IHandleEntity*)mountedUseEnt, 0);
        CTraceFilter* pFilter = mountedUseEnt ? static_cast<CTraceFilter*>(&filterTwo) : static_cast<CTraceFilter*>(&filterSelf);

        ray.Init(start, end);
        m_Game->m_EngineTrace->TraceRay(ray, STANDARD_TRACE_MASK, pFilter, &tr);

        C_BaseEntity* hitEnt = reinterpret_cast<C_BaseEntity*>(tr.m_pEnt);
        if (hitEnt && hitEnt != localPlayer)
        {
            const unsigned char* eb = reinterpret_cast<const unsigned char*>(hitEnt);
            const unsigned char lifeState = *reinterpret_cast<const unsigned char*>(eb + kLifeStateOffset);
            const int team = *reinterpret_cast<const int*>(eb + kTeamNumOffset);

            if (lifeState == 0 && team == 2)
            {
                // Resolve to a player index by scanning the entity list (cheap: cap to 64).
                const int hi = std::min(64, m_Game->m_ClientEntityList->GetHighestEntityIndex());
                for (int i = 1; i <= hi; ++i)
                {
                    if (m_Game->GetClientEntity(i) == hitEnt)
                    {
                        hitIdx = i;
                        break;
                    }
                }
            }
        }
    }

    if (hitIdx > 0)
    {
        m_AimTeammateLastRawIndex = hitIdx;
        m_AimTeammateLastRawTime = now;
    }
    else if (m_AimTeammateLastRawIndex > 0)
    {
        const float dt = duration_cast<duration<float>>(now - m_AimTeammateLastRawTime).count();
        if (dt <= kStickinessSeconds)
            hitIdx = m_AimTeammateLastRawIndex;
        else
            m_AimTeammateLastRawIndex = -1;
    }

    // Leaving all teammates: keep last shown target for 1 second, then clear.
    if (hitIdx <= 0)
    {
        m_AimTeammateCandidateIndex = -1;
        if (m_AimTeammateDisplayIndex > 0 && now > m_AimTeammateDisplayUntil)
            m_AimTeammateDisplayIndex = -1;
        return;
    }

    // Aiming at teammate: show immediately and refresh linger window.
    m_AimTeammateCandidateIndex = hitIdx;
    m_AimTeammateCandidateSince = now;
    m_AimTeammateDisplayIndex = hitIdx;
    m_AimTeammateDisplayUntil = now + duration_cast<steady_clock::duration>(duration<float>(kLingerSeconds));
}

bool VR::GetAimTeammateHudInfo(int& outPlayerIndex, int& outPercent, char* outName, size_t outNameSize)
{
    if (!outName || outNameSize == 0)
        return false;

    outName[0] = 0;
    outPlayerIndex = -1;
    outPercent = 0;

    const auto now = std::chrono::steady_clock::now();
    if (m_AimTeammateDisplayIndex <= 0)
        return false;
    if (now > m_AimTeammateDisplayUntil)
    {
        m_AimTeammateDisplayIndex = -1;
        return false;
    }

    if (!m_Game)
        return false;

    C_BasePlayer* p = (C_BasePlayer*)m_Game->GetClientEntity(m_AimTeammateDisplayIndex);
    if (!p)
        return false;

    const unsigned char* pb = reinterpret_cast<const unsigned char*>(p);
    const unsigned char lifeState = *reinterpret_cast<const unsigned char*>(pb + kLifeStateOffset);
    const int team = *reinterpret_cast<const int*>(pb + kTeamNumOffset);
    if (lifeState != 0 || team != 2)
        return false;

    const int hp = *reinterpret_cast<const int*>(pb + kHealthOffset);
    const float tempHPf = *reinterpret_cast<const float*>(pb + kHealthBufferOffset);
    const int tempHP = (int)std::max(0.0f, std::round(tempHPf));
    const int eff = std::max(0, std::min(100, hp + tempHP));

    // Name
    if (m_Game->m_EngineClient)
    {
        player_info_t info{};
        if (m_Game->m_EngineClient->GetPlayerInfo(m_AimTeammateDisplayIndex, &info))
        {
            size_t n = 0;
            for (; n + 1 < outNameSize && info.name[n]; ++n)
                outName[n] = info.name[n];
            outName[n] = 0;
        }
    }

    if (!outName[0])
        std::snprintf(outName, outNameSize, "P%d", m_AimTeammateDisplayIndex);

    outPlayerIndex = m_AimTeammateDisplayIndex;
    outPercent = eff;
    return true;
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
bool VR::ShouldRunSecondaryPrediction(const CUserCmd* /*cmd*/) const { return false; }
void VR::PrepareSecondaryPredictionCmd(CUserCmd& /*cmd*/) const {}
void VR::OnPrimaryAttackServerDecision(CUserCmd* /*cmd*/, bool /*fromSecondaryPrediction*/) {}
void VR::UpdateSpecialInfectedPreWarningState()
{
    m_SpecialInfectedPreWarningActive = false;
    m_SpecialInfectedPreWarningInRange = false;
    m_SpecialInfectedPreWarningTargetEntityIndex = -1;
    m_SpecialInfectedPreWarningTargetIsPlayer = false;
    m_SpecialInfectedAutoAimDirection = {};
    m_SpecialInfectedAutoAimCooldownEnd = {};
}

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
