void VR::ProcessInput()
{
    if (!m_IsVREnabled)
        return;

    // Recomputed every frame from CustomAction bindings.
    m_CustomWalkHeld = false;

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


    // Detect spectator / observer / idle state early so we can re-route some inputs.
    C_BasePlayer* localPlayer = nullptr;
    {
        const int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        localPlayer = static_cast<C_BasePlayer*>(m_Game->GetClientEntity(playerIndex));
    }

    bool isObserverOrIdle = false;
    if (localPlayer)
    {
        const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
        const int teamNum = *reinterpret_cast<const int*>(base + kTeamNumOffset);
        const unsigned char lifeState = *reinterpret_cast<const unsigned char*>(base + kLifeStateOffset);
        const int obsMode = *reinterpret_cast<const int*>(base + kObserverModeOffset);
        isObserverOrIdle = (teamNum == 1) || (lifeState != 0) || (obsMode != 0);
    }

    const bool jumpGestureActive = currentTime < m_JumpGestureHoldUntil;
    if (isObserverOrIdle)
    {
        // Avoid leaving +jump latched while spectating.
        if (m_JumpCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("-jump");
            m_JumpCmdOwned = false;
        }
    }
    else
    {
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
    bool secondaryAttackJustPressed = false;
    bool secondaryAttackDataValid = getActionState(&m_ActionSecondaryAttack, secondaryAttackActionData, secondaryAttackActive, secondaryAttackJustPressed);

    const bool gestureSecondaryAttackActive = currentTime < m_SecondaryAttackGestureHoldUntil;
    const bool gestureReloadActive = currentTime < m_ReloadGestureHoldUntil;

    vr::InputDigitalActionData_t flashlightActionData{};
    bool flashlightButtonDown = false;
    bool flashlightJustPressed = false;
    bool flashlightDataValid = getActionState(&m_ActionFlashlight, flashlightActionData, flashlightButtonDown, flashlightJustPressed);

    vr::InputDigitalActionData_t inventoryQuickSwitchActionData{};
    bool inventoryQuickSwitchDown = false;
    bool inventoryQuickSwitchJustPressed = false;
    bool inventoryQuickSwitchDataValid = getActionState(&m_ActionInventoryQuickSwitch, inventoryQuickSwitchActionData, inventoryQuickSwitchDown, inventoryQuickSwitchJustPressed);

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

    // When quick-switch is enabled, disable legacy inventory switching entirely.
    // Quick-switch (HL:Alyx style): press/hold a bind, origin snaps to right hand, then move right hand into 4 zones.
    if (m_InventoryQuickSwitchEnabled)
    {
        // NOTE: For selection, work in tracking-local space (relative to the camera anchor).
        // This lets the quick-switch "follow the hand" visually while still allowing you to move
        // the player with a stick without accidentally arming/selecting zones from world translation.
        const Vector trackingOrigin = m_CameraAnchor - Vector(0, 0, 64);
        const Vector rightControllerLocal = m_RightControllerPosAbs - trackingOrigin;
        // Avoid stale swallow flags if the user toggles modes.
        m_BlockReloadUntilRelease = false;
        m_BlockCrouchUntilRelease = false;

        if (!inventoryQuickSwitchDataValid)
        {
            m_InventoryQuickSwitchActive = false;
            m_InventoryQuickSwitchArmed = false;
        }

        // Start quick-switch on press (origin = right controller position at press time).
        if (inventoryQuickSwitchDataValid && inventoryQuickSwitchJustPressed)
        {
            const Vector worldUp(0.f, 0.f, 1.f);

            // Build yaw-only basis from HMD forward so zones feel consistent regardless of wrist rotation.
            Vector fwd = m_HmdForward;
            fwd.z = 0.f;
            float fwdLen = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
            if (fwdLen < 0.001f)
            {
                fwd = Vector(1.f, 0.f, 0.f);
                fwdLen = 1.f;
            }
            fwd *= (1.f / fwdLen);

            Vector right(
                fwd.y * worldUp.z - fwd.z * worldUp.y,
                fwd.z * worldUp.x - fwd.x * worldUp.z,
                fwd.x * worldUp.y - fwd.y * worldUp.x
            );
            float rightLen = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
            if (rightLen < 0.001f)
            {
                right = Vector(0.f, 1.f, 0.f);
                rightLen = 1.f;
            }
            right *= (1.f / rightLen);

            m_InventoryQuickSwitchOrigin = rightControllerLocal;
            m_InventoryQuickSwitchForward = fwd;
            m_InventoryQuickSwitchRight = right;
            m_InventoryQuickSwitchActive = true;
            m_InventoryQuickSwitchArmed = false;
        }

        // Release-to-cancel if still active.
        if (inventoryQuickSwitchDataValid && m_InventoryQuickSwitchActive && !inventoryQuickSwitchDown)
        {
            m_InventoryQuickSwitchActive = false;
            m_InventoryQuickSwitchArmed = false;
        }

        if (m_InventoryQuickSwitchActive)
        {
            const Vector worldUp(0.f, 0.f, 1.f);
            const float zoneRadius = m_InventoryQuickSwitchZoneRadius * m_VRScale;

            // Allow a small travel before arming to avoid instant selection if offsets are small.
            const float armDistance = 0.03f * m_VRScale;
            if (!m_InventoryQuickSwitchArmed && VectorLength(rightControllerLocal - m_InventoryQuickSwitchOrigin) > armDistance)
                m_InventoryQuickSwitchArmed = true;

            const Vector base = m_InventoryQuickSwitchOrigin
                + (m_InventoryQuickSwitchForward * (m_InventoryQuickSwitchOffset.x * m_VRScale));

            // Offsets are expressed as (forward,right,up).
            // We use one offset vector to push all zones away from the origin:
            //   - x: common forward bias (moves the whole cross forward)
            //   - y: left/right distance
            //   - z: up/down distance
            const Vector chestZone = base + (worldUp * (m_InventoryQuickSwitchOffset.z * m_VRScale));
            const Vector backZone = base - (worldUp * (m_InventoryQuickSwitchOffset.z * m_VRScale));
            const Vector leftZone = base - (m_InventoryQuickSwitchRight * (m_InventoryQuickSwitchOffset.y * m_VRScale));
            const Vector rightZone = base + (m_InventoryQuickSwitchRight * (m_InventoryQuickSwitchOffset.y * m_VRScale));

            auto inZone = [&](const Vector& zoneCenter) -> bool
                {
                    return VectorLength(rightControllerLocal - zoneCenter) <= zoneRadius;
                };

            if (m_InventoryQuickSwitchArmed)
            {
                // Mapping matches legacy inventory anchors:
                //  - Back (down): primary/secondary toggle
                //  - Left: slot3 (throwables)
                //  - Chest (up): slot4 (medkit)
                //  - Right: slot5 (pills/adrenaline)
                if (inZone(backZone))
                {
                    togglePrimarySecondary();
                    m_InventoryQuickSwitchActive = false;
                }
                else if (inZone(leftZone))
                {
                    m_Game->ClientCmd_Unrestricted("slot3");
                    m_InventoryQuickSwitchActive = false;
                }
                else if (inZone(chestZone))
                {
                    m_Game->ClientCmd_Unrestricted("slot4");
                    m_InventoryQuickSwitchActive = false;
                }
                else if (inZone(rightZone))
                {
                    m_Game->ClientCmd_Unrestricted("slot5");
                    m_InventoryQuickSwitchActive = false;
                }
            }

            if (m_DrawInventoryAnchors && m_Game->m_DebugOverlay)
            {
                auto drawZone = [&](const Vector& center, bool active)
                    {
                        int r = m_InventoryAnchorColorR;
                        int g = m_InventoryAnchorColorG;
                        int b = m_InventoryAnchorColorB;
                        int a = 180;
                        if (active)
                        {
                            r = 255; g = 255; b = 0;
                            a = 220;
                        }

                        const float box = std::max(1.0f, zoneRadius * 0.35f);
                        Vector mins(-box, -box, -box);
                        Vector maxs(box, box, box);
                        QAngle ang(0.f, 0.f, 0.f);
                        m_Game->m_DebugOverlay->AddBoxOverlay(center, mins, maxs, ang, r, g, b, a, m_LastFrameDuration * 2.0f);
                    };

                drawZone(chestZone + trackingOrigin, inZone(chestZone));
                drawZone(backZone + trackingOrigin, inZone(backZone));
                drawZone(leftZone + trackingOrigin, inZone(leftZone));
                drawZone(rightZone + trackingOrigin, inZone(rightZone));
            }
        }
    }
    else
    {
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

        // Anchor origin: estimate a more stable "body / pelvis" point (in body space).
        // IMPORTANT: Do NOT base this on m_HmdPosAbs, otherwise room-scale/head translation will make the anchors
        // drift around as you move your head. m_CameraAnchor is the stable player/tracking anchor.     
        const Vector bodyOrigin = m_CameraAnchor
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

        if (reloadDataValid && m_BlockReloadUntilRelease)
        {
            if (reloadActionData.bState)
            {
                reloadButtonDown = false;
                reloadJustPressed = false;
            }
            else
            {
                m_BlockReloadUntilRelease = false;
            }
        }

        if (crouchDataValid && m_BlockCrouchUntilRelease)
        {
            if (crouchActionData.bState)
            {
                crouchButtonDown = false;
                crouchJustPressed = false;
            }
            else
            {
                m_BlockCrouchUntilRelease = false;
            }
        }

        if (reloadDataValid && reloadJustPressed)
        {
            if (triggerInventoryFromOrigin(reloadActionData.activeOrigin))
            {
                reloadButtonDown = false;
                reloadJustPressed = false;
                m_BlockReloadUntilRelease = true;
            }
        }

        if (crouchDataValid && crouchJustPressed)
        {
            if (triggerInventoryFromOrigin(crouchActionData.activeOrigin))
            {
                crouchButtonDown = false;
                crouchJustPressed = false;
                m_BlockCrouchUntilRelease = true;
            }
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

    // Spectator/idle: map VR buttons to Source observer controls.
    // SecondaryAttack -> spec_next, PrimaryAttack -> spec_prev, Jump -> spec_mode.
    if (isObserverOrIdle && m_Game->m_EngineClient->IsInGame())
    {
        if (secondaryAttackJustPressed)
            m_Game->ClientCmd_Unrestricted("spec_prev");
        if (primaryAttackJustPressed)
            m_Game->ClientCmd_Unrestricted("spec_next");
        if (jumpJustPressed)
            m_Game->ClientCmd_Unrestricted("spec_mode");
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
    if (isObserverOrIdle)
    {
        // Don't hold +attack while spectating.
        if (m_PrimaryAttackCmdOwned)
        {
            m_Game->ClientCmd_Unrestricted("-attack");
            m_PrimaryAttackCmdOwned = false;
        }
    }
    else
    {
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
        if (isObserverOrIdle)
        {
            // Don't hold +attack2 while spectating.
            if (m_SecondaryAttackCmdOwned)
            {
                m_Game->ClientCmd_Unrestricted("-attack2");
                m_SecondaryAttackCmdOwned = false;
            }
        }
        else
        {
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

    auto showTopHud = [&]()
        {
            vr::VROverlay()->ShowOverlay(m_HUDTopHandle);
        };

    auto hideTopHud = [&]()
        {
            vr::VROverlay()->HideOverlay(m_HUDTopHandle);
        };

    auto hideBottomHud = [&]()
        {
            for (vr::VROverlayHandle_t& overlay : m_HUDBottomHandles)
                vr::VROverlay()->HideOverlay(overlay);
        };

    const bool isControllerVertical =
        m_RightControllerAngAbs.x > 60.0f || m_RightControllerAngAbs.x < -45.0f ||
        m_LeftControllerAngAbs.x > 60.0f || m_LeftControllerAngAbs.x < -45.0f;
    bool menuActive = m_Game->m_EngineClient->IsPaused();
    bool cursorVisible = m_Game->m_VguiSurface && m_Game->m_VguiSurface->IsCursorVisible();
    if (cursorVisible)
        m_HudChatVisibleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const bool chatRecent = std::chrono::steady_clock::now() < m_HudChatVisibleUntil;

    if (PressedDigitalAction(m_ToggleHUD, true))
        m_HudToggleState = !m_HudToggleState;

    const bool wantsTopHud = PressedDigitalAction(m_Scoreboard) || isControllerVertical || m_HudToggleState || cursorVisible || chatRecent;
    if ((wantsTopHud && m_RenderedHud.load(std::memory_order_acquire)) || menuActive)
    {
        RepositionOverlays();

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

    hideBottomHud();
    m_RenderedHud.store(false, std::memory_order_release);

    if (PressedDigitalAction(m_Pause, true))
    {
        m_Game->ClientCmd_Unrestricted("gameui_activate");
        RepositionOverlays();
        showTopHud();
        hideBottomHud();
    }
}

