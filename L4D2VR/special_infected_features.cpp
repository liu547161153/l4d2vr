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
    auto ensureTraceCache = [&](int idx)
    {
        if (idx <= 0)
            return;
        const size_t need = static_cast<size_t>(idx) + 1;
        if (m_LastSpecialInfectedTraceTime.size() < need)
        {
            m_LastSpecialInfectedTraceTime.resize(need);
            m_LastSpecialInfectedTraceResult.resize(need, 0);
        }
    };
    // Cache expensive TraceRay calls per-entity to avoid spikes when DrawModelExecute fires many times.
    // Re-check LOS faster when the cached result is false to avoid "sticky" misses when an obstruction
    // disappears between cache ticks.
    if (entityIndex > 0 && m_SpecialInfectedTraceMaxHz > 0.0f)
    {
        ensureTraceCache(entityIndex);
        auto& last = m_LastSpecialInfectedTraceTime[entityIndex];
        const std::uint8_t cached = m_LastSpecialInfectedTraceResult[entityIndex];
        const auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count() != 0)
        {
            const float minInterval = 1.0f / std::max(1.0f, m_SpecialInfectedTraceMaxHz);
            const float elapsed = std::chrono::duration<float>(now - last).count();
            if (elapsed < minInterval)
            {
                if (cached == 2)
                    return true;

                if (cached == 1)
                {
                    const float falseRecheckInterval = minInterval * 0.25f;
                    if (elapsed < falseRecheckInterval)
                        return false;
                }
            }
        }
        last = now;
    }

    IHandleEntity* targetEntity = nullptr;
    if (entityIndex > 0)
        targetEntity = (IHandleEntity*)m_Game->GetClientEntity(entityIndex);

    CTraceFilterSkipNPCsAndEntity tracefilter((IHandleEntity*)localPlayer, targetEntity, 0);

    auto traceLos = [&](const Vector& start, const Vector& end)
        {
            CGameTrace trace;
            Ray_t ray;
            ray.Init(start, end);

            // Use a bullet-style mask here so grates/monsterclip don't incorrectly block auto-aim.
            m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT, &tracefilter, &trace);

            if (trace.fraction >= 1.0f)
                return true;

            // If we didn't (or couldn't) skip the target entity, consider a direct hit as LOS.
            if (targetEntity && trace.m_pEnt == targetEntity)
                return true;

            // If the controller is inside geometry (common near cover), fall back to a different start point.
            if (trace.startsolid || trace.allsolid)
                return false;

            return false;
        };

    // 1) Primary: controller -> target (matches aim-line intuition).
    bool hasLos = traceLos(m_RightControllerPosAbs, infectedOrigin);

    // 2) If controller start was inside something, nudge forward a bit and retry.
    if (!hasLos)
    {
        Vector dir = infectedOrigin - m_RightControllerPosAbs;
        const float len = VectorLength(dir);
        if (len > 0.001f)
        {
            dir /= len;
            const Vector nudgedStart = m_RightControllerPosAbs + dir * 4.0f;
            hasLos = traceLos(nudgedStart, infectedOrigin);
        }
    }

    // 3) Final fallback: HMD -> target (avoids false negatives when the controller origin is clipped).
    if (!hasLos)
        hasLos = traceLos(m_HmdPosAbs, infectedOrigin);

    if (entityIndex > 0)
    {
        ensureTraceCache(entityIndex);
        m_LastSpecialInfectedTraceResult[entityIndex] = hasLos ? 2 : 1;
    }

    return hasLos;
}

void VR::RefreshSpecialInfectedPreWarning(const Vector& infectedOrigin, SpecialInfectedType type, int entityIndex, bool isPlayerClass)
{
    if (m_SpecialInfectedPreWarningDistance <= 0.0f || !m_SpecialInfectedPreWarningAutoAimEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto secondsToDuration = [](float seconds)
        {
            return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(seconds));
        };
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

        // Use the same aim-offset target for LOS checks; tracing to raw origin (feet) can get
        // spuriously blocked by small ledges/props even when the body is visible.
        Vector adjustedTarget = infectedOrigin;
        const size_t typeIndex = static_cast<size_t>(type);
        if (typeIndex < m_SpecialInfectedPreWarningAimOffsets.size())
        {
            const Vector& offset = m_SpecialInfectedPreWarningAimOffsets[typeIndex];
            adjustedTarget += (m_HmdRight * offset.x) + (m_HmdForward * offset.y) + (m_HmdUp * offset.z);
        }

        if (!HasLineOfSightToSpecialInfected(adjustedTarget, entityIndex))
            return;

        const bool isPounceType = type == SpecialInfectedType::Hunter || type == SpecialInfectedType::Jockey;
        if (m_SpecialInfectedWarningActionEnabled && isPounceType && m_SpecialInfectedPreWarningEvadeDistance > 0.0f)
        {
            const float evadeDistanceSq = m_SpecialInfectedPreWarningEvadeDistance * m_SpecialInfectedPreWarningEvadeDistance;
            if (distanceSq <= evadeDistanceSq)
            {
                const bool sameTarget = m_LastSpecialInfectedEvadeEntityIndex == entityIndex;
                if (!sameTarget)
                {
                    m_SpecialInfectedPreWarningEvadeArmed = true;
                }
                else if (!m_SpecialInfectedPreWarningEvadeArmed)
                {
                    const float rearmDistanceSq = evadeDistanceSq * 1.44f;
                    if (distanceSq > rearmDistanceSq)
                        m_SpecialInfectedPreWarningEvadeArmed = true;
                }

                const bool cooldownActive = sameTarget
                    && m_SpecialInfectedPreWarningEvadeCooldown > 0.0f
                    && now < m_SpecialInfectedPreWarningEvadeCooldownEnd;

                m_SpecialInfectedWarningTarget = infectedOrigin;
                m_SpecialInfectedWarningTargetActive = true;
                m_LastSpecialInfectedWarningTime = now;
                m_SpecialInfectedBlindSpotWarningActive = true;

                if (!m_SpecialInfectedPreWarningEvadeTriggered
                    && m_SpecialInfectedPreWarningEvadeArmed
                    && !cooldownActive)
                {
                    StartSpecialInfectedWarningAction();
                    m_SpecialInfectedPreWarningEvadeTriggered = true;
                    m_SpecialInfectedPreWarningEvadeArmed = false;
                    m_LastSpecialInfectedEvadeEntityIndex = entityIndex;
                    if (m_SpecialInfectedPreWarningEvadeCooldown > 0.0f)
                        m_SpecialInfectedPreWarningEvadeCooldownEnd = now + secondsToDuration(m_SpecialInfectedPreWarningEvadeCooldown);
                }
            }
        }
        const bool isCloser = distanceSq < m_SpecialInfectedPreWarningTargetDistanceSq;
        const bool isCandidate = isLockedTarget || isCloser || distanceSq <= (m_SpecialInfectedPreWarningTargetDistanceSq + 0.01f);
        const float updateInterval = std::max(0.0f, m_SpecialInfectedPreWarningTargetUpdateInterval);
        const auto elapsedUpdate = std::chrono::duration<float>(now - m_LastSpecialInfectedPreWarningTargetUpdateTime).count();
        if (isCandidate && (isCloser || updateInterval <= 0.0f || elapsedUpdate >= updateInterval))
        {
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
        m_SpecialInfectedPreWarningEvadeArmed = true;
        m_LastSpecialInfectedEvadeEntityIndex = -1;
        m_SpecialInfectedPreWarningEvadeCooldownEnd = {};
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
        m_SpecialInfectedPreWarningEvadeArmed = true;
        m_LastSpecialInfectedEvadeEntityIndex = -1;
        m_SpecialInfectedPreWarningEvadeCooldownEnd = {};
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
    {
        m_SpecialInfectedPreWarningEvadeTriggered = false;
        m_SpecialInfectedPreWarningEvadeArmed = true;
        m_LastSpecialInfectedEvadeEntityIndex = -1;
        m_SpecialInfectedPreWarningEvadeCooldownEnd = {};
    }

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
        if (!m_SpecialInfectedWarningAttack2CmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("+attack2");
            m_SpecialInfectedWarningAttack2CmdOwned = true;
        }
        m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::ReleaseSecondaryAttack;
        m_SpecialInfectedWarningNextActionTime = now + secondsToDuration(m_SpecialInfectedWarningSecondaryHoldDuration);
        break;
    case SpecialInfectedWarningActionStep::ReleaseSecondaryAttack:
        if (m_SpecialInfectedWarningAttack2CmdOwned && !m_SecondaryAttackCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("-attack2");
        }
        m_SpecialInfectedWarningAttack2CmdOwned = false;
        ResetSpecialInfectedWarningAction();
        break;
    case SpecialInfectedWarningActionStep::PressJump:
        if (!m_SpecialInfectedWarningJumpCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("+jump");
            m_SpecialInfectedWarningJumpCmdOwned = true;
        }
        m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::ReleaseJump;
        m_SpecialInfectedWarningNextActionTime = now + secondsToDuration(m_SpecialInfectedWarningJumpHoldDuration);
        break;
    case SpecialInfectedWarningActionStep::ReleaseJump:
        if (m_SpecialInfectedWarningJumpCmdOwned && !m_JumpCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("-jump");
        }
        m_SpecialInfectedWarningJumpCmdOwned = false;
        ResetSpecialInfectedWarningAction();
        break;
    default:
        break;
    }
}

void VR::ResetSpecialInfectedWarningAction()
{
    // Do NOT spam "-attack2"/"-jump" here: it cancels real keyboard/mouse input.
    // Only release if this automation actually owned the command and the player's normal
    // input path isn't currently holding it.
    if (m_SpecialInfectedWarningAttack2CmdOwned && !m_SecondaryAttackCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("-attack2");
    }
    if (m_SpecialInfectedWarningJumpCmdOwned && !m_JumpCmdOwned)
    {
        m_Game->ClientCmd_Unrestricted("-jump");
    }

    m_SpecialInfectedWarningAttack2CmdOwned = false;
    m_SpecialInfectedWarningJumpCmdOwned = false;
    m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::None;
    m_SpecialInfectedWarningNextActionTime = {};
    m_SuppressPlayerInput = false;
}