void Hooks::dAdjustEngineViewport(int& x, int& y, int& width, int& height)
{
	hkAdjustEngineViewport.fOriginal(x, y, width, height);
}

void Hooks::dViewport(void* ecx, void* edx, int x, int y, int width, int height)
{
	hkViewport.fOriginal(ecx, x, y, width, height);
}

void Hooks::dGetViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height)
{
	hkGetViewport.fOriginal(ecx, x, y, width, height);
}

int Hooks::dTestMeleeSwingCollisionClient(void* ecx, void* edx, Vector const& vec)
{
	return hkTestMeleeSwingCollisionClient.fOriginal(ecx, vec);
}

int Hooks::dTestMeleeSwingCollisionServer(void* ecx, void* edx, Vector const& vec)
{
	return hkTestMeleeSwingCollisionServer.fOriginal(ecx, vec);
}

void Hooks::dDoMeleeSwingServer(void* ecx, void* edx)
{
	return hkDoMeleeSwingServer.fOriginal(ecx);
}

void Hooks::dStartMeleeSwingServer(void* ecx, void* edx, void* player, bool a3)
{
	return hkStartMeleeSwingServer.fOriginal(ecx, player, a3);
}

int Hooks::dPrimaryAttackServer(void* ecx, void* edx)
{
	if (m_VR)
	{
		CUserCmd* decisionCmd = m_RunCommandFromSecondaryPredict
			? m_RunCommandSecondaryCmd
			: m_RunCommandCurrentCmd;
		m_VR->OnPrimaryAttackServerDecision(decisionCmd, m_RunCommandFromSecondaryPredict);
	}
	return hkPrimaryAttackServer.fOriginal(ecx);
}

void Hooks::dItemPostFrameServer(void* ecx, void* edx)
{
	hkItemPostFrameServer.fOriginal(ecx);
}

int Hooks::dGetPrimaryAttackActivity(void* ecx, void* edx, void* meleeInfo)
{
	return hkGetPrimaryAttackActivity.fOriginal(ecx, meleeInfo);
}

void Hooks::dRunCommand(void* ecx, void* edx, C_BasePlayer* player, CUserCmd* cmd, void* moveHelper)
{
	if (!cmd)
	{
		hkRunCommand.fOriginal(ecx, player, cmd, moveHelper);
		return;
	}

	// Keep the active command available to PrimaryAttackServer/Fire path hooks.
	m_RunCommandCurrentCmd = cmd;
	m_RunCommandSecondaryCmd = nullptr;

	if (m_VR)
		m_VR->OnPredictionRunCommand(cmd);
	// Debug: track whether packed 1:1 delta survives prediction/original RunCommand.
	static constexpr uint32_t kRSButtonsMask = 0xFC000000u; // bits 26..31
	uint32_t b_before = (uint32_t)cmd->buttons;
	Vector b_dm_before;
	const bool b_has_before = (m_VR && !m_VR->m_ForceNonVRServerMovement && m_VR->m_Roomscale1To1Movement && cmd->weaponselect == 0 && VR::DecodeRoomscale1To1Delta((int)(b_before & kRSButtonsMask), b_dm_before));
	if (b_has_before && m_VR->m_Roomscale1To1DebugLog && ((cmd->command_number & 31) == 0))
		Game::logMsg("[VR][1to1][runcmd] pre  cmd=%d tick=%d cmdptr=%p buttons=0x%08X dM=(%.3f %.3f)", cmd->command_number, cmd->tick_count, (void*)cmd, (unsigned)b_before, b_dm_before.x, b_dm_before.y);

	const bool canRunSecondaryPredict =
		m_VR
		&& !m_RunCommandInDetour
		&& !m_RunCommandFromSecondaryPredict
		&& m_VR->ShouldRunSecondaryPrediction(cmd);

	if (canRunSecondaryPredict)
	{
		CUserCmd secondaryCmd = *cmd;
		m_VR->PrepareSecondaryPredictionCmd(secondaryCmd);

		m_RunCommandInDetour = true;
		m_RunCommandFromSecondaryPredict = true;
		m_RunCommandSecondaryCmd = &secondaryCmd;
		hkRunCommand.fOriginal(ecx, player, &secondaryCmd, moveHelper);
		m_RunCommandSecondaryCmd = nullptr;
		m_RunCommandFromSecondaryPredict = false;
		m_RunCommandInDetour = false;
	}

	// If we packed a 1:1 room-scale delta into buttons high bits, keep it for networking,
	// but hide it from the stock prediction/movement code (weapon logic may peek at buttons).
	const int rsPackedButtons = cmd ? (cmd->buttons & (int)kRSButtonsMask) : 0;
	bool rsHideButtons = false;
	if (m_VR && cmd && cmd->weaponselect == 0 && m_VR->m_Roomscale1To1Movement && !m_VR->m_ForceNonVRServerMovement)
	{
		Vector _rsTmp;
		rsHideButtons = m_VR->DecodeRoomscale1To1Delta(rsPackedButtons, _rsTmp);
		if (rsHideButtons)
			cmd->buttons &= ~(int)kRSButtonsMask;
	}

	hkRunCommand.fOriginal(ecx, player, cmd, moveHelper);
	if (rsHideButtons)
		cmd->buttons = (cmd->buttons & ~(int)kRSButtonsMask) | rsPackedButtons;
	if (b_has_before && m_VR && m_VR->m_Roomscale1To1DebugLog && ((cmd->command_number & 31) == 0))
	{
		uint32_t b_after = (uint32_t)cmd->buttons;
		Vector b_dm_after;
		const bool b_has_after = VR::DecodeRoomscale1To1Delta((int)(b_after & kRSButtonsMask), b_dm_after);
		Game::logMsg("[VR][1to1][runcmd] post cmd=%d tick=%d cmdptr=%p buttons=0x%08X->0x%08X decoded=%d dM=(%.3f %.3f)",
			cmd->command_number, cmd->tick_count, (void*)cmd, (unsigned)b_before, (unsigned)b_after, (int)b_has_after, b_dm_after.x, b_dm_after.y);
	}

	m_RunCommandCurrentCmd = nullptr;
}

Vector* Hooks::dEyePosition(void* ecx, void* edx, Vector* eyePos)
{
	Vector* result = hkEyePosition.fOriginal(ecx, eyePos);

	if (m_Game->m_PerformingMelee)
	{
		int i = m_Game->m_CurrentUsercmdID;
		if (m_Game->IsValidPlayerIndex(i))
		{
			*result = m_Game->m_PlayersVRInfo[i].controllerPos;
		}
	}

	return result;
}

void Hooks::dDrawModelExecute(void* ecx, void* edx, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld)
{
	if (m_Game->m_SwitchedWeapons)
		m_Game->m_CachedArmsModel = false;

	bool hideArms = m_Game->m_IsMeleeWeaponActive || m_VR->m_HideArms;

	std::string modelName;
	if (info.pModel)
	{
		modelName = m_Game->m_ModelInfo->GetModelName(info.pModel);

		VR::SpecialInfectedType infectedType = VR::SpecialInfectedType::None;
		bool isAlive = true;
		const C_BaseEntity* entity = nullptr;
		if (m_Game->m_ClientEntityList && info.entity_index > 0)
		{
			const int maxEntityIndex = m_Game->m_ClientEntityList->GetHighestEntityIndex();
			if (info.entity_index <= maxEntityIndex)
				entity = m_Game->GetClientEntity(info.entity_index);
		}
		bool isPlayerClass = false;
		const char* className = nullptr;
		if (entity)
		{
			className = m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(const_cast<C_BaseEntity*>(entity)));
			isPlayerClass = className && (std::strcmp(className, "CTerrorPlayer") == 0 || std::strcmp(className, "C_TerrorPlayer") == 0);
			if (isPlayerClass)
			{
				isAlive = m_VR->IsEntityAlive(entity);
			}
		}

		// if (entity && info.entity_index > 0 && m_Game->IsValidPlayerIndex(info.entity_index))
		// {
		// 	infectedType = m_VR->GetSpecialInfectedType(entity);
		// }
		const bool isInfectedModel = modelName.find("models/infected/") != std::string::npos;
		if (isInfectedModel)
		{
			infectedType = m_VR->GetSpecialInfectedType(entity);
		}

		if (isAlive && infectedType == VR::SpecialInfectedType::None)
		{
			const auto modelType = m_VR->GetSpecialInfectedTypeFromModel(modelName);
			if (modelType == VR::SpecialInfectedType::Tank || modelType == VR::SpecialInfectedType::Witch)
				infectedType = modelType;
		}

		if (isAlive && infectedType != VR::SpecialInfectedType::None)
		{
			const bool isRagdoll = modelName.find("ragdoll") != std::string::npos;
			if (!isRagdoll)
			{
				// 1) 高优先级：自瞄/目标刷新不要被 Overlay 节流影响（否则锁定会飘）
				// RefreshSpecialInfectedPreWarning 内部会用到 Trace 缓存（TraceMaxHz），所以这里高频调用不会把 CPU 打爆。
				m_VR->RefreshSpecialInfectedPreWarning(info.origin, infectedType, info.entity_index, isPlayerClass);

				// Rear mirror pop-up: if enabled, show the mirror briefly when a special infected is behind you
				// within the configured warning distance. This detection runs on the main render pass so the
				// mirror can wake up without relying on the mirror RTT pass.
				if (m_VR->m_RearMirrorEnabled && m_VR->m_RearMirrorShowOnlyOnSpecialWarning
					&& m_VR->m_RearMirrorSpecialShowHoldSeconds > 0.0f && m_VR->m_RearMirrorSpecialWarningDistance > 0.0f)
				{
					Vector to = info.origin - m_VR->m_HmdPosAbs;
					to.z = 0.0f;
					const float maxD = m_VR->m_RearMirrorSpecialWarningDistance;
					if (!to.IsZero() && to.LengthSqr() <= (maxD * maxD))
					{
						Vector fwd = m_VR->m_HmdForward;
						fwd.z = 0.0f;
						if (VectorNormalize(fwd) == 0.0f)
							fwd = { 1.0f, 0.0f, 0.0f };
						VectorNormalize(to);
						// Behind = more likely you want the rear mirror.
						if (DotProduct(to, fwd) < 0.0f)
							m_VR->NotifyRearMirrorSpecialWarning();
					}
				}

				// 2) 低优先级：视觉 Overlay（箭头/盲区提示）继续按实体节流，避免 dDrawModelExecute 多次调用导致尖峰
				bool doOverlay = true;
				if (info.entity_index > 0 && m_VR->m_SpecialInfectedOverlayMaxHz > 0.0f)
				{
					auto& last = m_VR->m_LastSpecialInfectedOverlayTime[info.entity_index];
					const auto now = std::chrono::steady_clock::now();
					if (last.time_since_epoch().count() != 0)
					{
						const float minInterval = 1.0f / std::max(1.0f, m_VR->m_SpecialInfectedOverlayMaxHz);
						const float elapsed = std::chrono::duration<float>(now - last).count();
						if (elapsed < minInterval)
							doOverlay = false;
					}
					if (doOverlay)
						last = now;
				}

				if (doOverlay)
				{
					// Rear-mirror hint: if this special-infected arrow is being rendered during the rear-mirror RTT pass
					// and within the configured distance, enlarge the mirror overlay.
					if (m_VR->m_RearMirrorRenderingPass && m_VR->m_RearMirrorSpecialWarningDistance > 0.0f)
					{
						Vector to = info.origin - m_VR->m_HmdPosAbs;
						to.z = 0.0f;
						const float maxD = m_VR->m_RearMirrorSpecialWarningDistance;
						if (!to.IsZero() && to.LengthSqr() <= (maxD * maxD))
							m_VR->m_RearMirrorSawSpecialThisPass = true;
					}
					if (infectedType != VR::SpecialInfectedType::Tank
						&& infectedType != VR::SpecialInfectedType::Witch
						&& infectedType != VR::SpecialInfectedType::Charger)
					{
						m_VR->RefreshSpecialInfectedBlindSpotWarning(info.origin);
					}
					m_VR->DrawSpecialInfectedArrow(info.origin, infectedType);
				}
			}
		}
	}

	if (info.pModel && hideArms && !m_Game->m_CachedArmsModel)
	{
		if (modelName.find("/arms/") != std::string::npos)
		{
			m_Game->m_ArmsMaterial = m_Game->m_MaterialSystem->FindMaterial(modelName.c_str(), "Model textures");
			m_Game->m_ArmsModel = info.pModel;
			m_Game->m_CachedArmsModel = true;
		}
	}

	if (info.pModel && info.pModel == m_Game->m_ArmsModel && hideArms)
	{
		m_Game->m_ArmsMaterial->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, true);
		m_Game->m_ModelRender->ForcedMaterialOverride(m_Game->m_ArmsMaterial);
		hkDrawModelExecute.fOriginal(ecx, state, info, pCustomBoneToWorld);
		m_Game->m_ModelRender->ForcedMaterialOverride(NULL);
		return;
	}

	hkDrawModelExecute.fOriginal(ecx, state, info, pCustomBoneToWorld);
}

// Returns true if the engine RT being pushed looks like the HUD/VGUI render target.
// This is a heuristic (names + dimensions) to avoid hijacking other offscreen passes.
static bool IsHudRenderTarget(ITexture* texture, ITexture* hudTexture)
{
    if (!texture)
        return false;

    const char* name = texture->GetName();
    if (name && *name)
    {
        auto ciFind = [](const char* haystack, const char* needle) -> bool
            {
                const size_t nLen = strlen(needle);
                for (const char* p = haystack; *p; ++p)
                {
                    if (_strnicmp(p, needle, nLen) == 0)
                        return true;
                }
                return false;
            };

        // Exclude obvious non-HUD targets
        if (ciFind(name, "backbuffer") || ciFind(name, "left") || ciFind(name, "right") ||
            ciFind(name, "blank") || ciFind(name, "scope") || ciFind(name, "rearmirror"))
            return false;

        if (ciFind(name, "vgui") || ciFind(name, "hud"))
            return true;
    }

    // Fallback: match the HUD texture size
    if (hudTexture)
    {
        const int hudW = hudTexture->GetMappingWidth();
        const int hudH = hudTexture->GetMappingHeight();
        if (hudW > 0 && hudH > 0)
        {
            if (texture->GetMappingWidth() == hudW && texture->GetMappingHeight() == hudH)
                return true;
        }
    }

    return false;
}

void Hooks::dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
    if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire))
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

    // Extra offscreen passes (scope/rear-mirror RTT) must not hijack HUD capture
    if (m_VR->m_SuppressHudCapture)
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (queueMode != 0)
    {
        // Queued/multicore path: the Pop->IsSplitScreen->PrePush->Push sequence
        // isn't reliable, so never attempt RT hijack here.
        m_HUDStep = HUDPushStep::None;
        m_PushedHud = false;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    // Single-threaded path (mat_queue_mode 0): use state machine to detect HUD push
    bool overrideHudRT = (m_HUDStep == HUDPushStep::ReadyToOverride) &&
        !m_VR->m_HudPaintedThisFrame.load(std::memory_order_relaxed);

    if (overrideHudRT)
    {
        std::lock_guard<std::mutex> lock(m_VR->m_TextureMutex);
        if (!m_VR->m_HUDTexture || !IsHudRenderTarget(pTexture, m_VR->m_HUDTexture))
            overrideHudRT = false;
    }

    if (!overrideHudRT)
    {
        m_PushedHud = false;
        m_HUDStep = HUDPushStep::None;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    ITexture* hudTexture = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_VR->m_TextureMutex);
        hudTexture = m_VR->m_HUDTexture;
    }

    if (!hudTexture)
    {
        m_HUDStep = HUDPushStep::None;
        m_PushedHud = false;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    IMatRenderContext* renderContext = m_Game->m_MaterialSystem ? m_Game->m_MaterialSystem->GetRenderContext() : nullptr;
    if (!renderContext)
    {
        m_VR->HandleMissingRenderContext("Hooks::dPushRenderTargetAndViewport");
        m_HUDStep = HUDPushStep::None;
        m_PushedHud = false;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    // Clear depth/stencil first, then push RT and clear color to transparent.
    renderContext->ClearBuffers(false, true, true);
    hkPushRenderTargetAndViewport.fOriginal(ecx, hudTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    renderContext->OverrideAlphaWriteEnable(true, true);
    renderContext->ClearColor4ub(0, 0, 0, 0);
    renderContext->ClearBuffers(true, false);

    m_PushedHud = true;
    m_HUDStep = HUDPushStep::None;
}

void Hooks::dPopRenderTargetAndViewport(void* ecx, void* edx)
{
    if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire))
        return hkPopRenderTargetAndViewport.fOriginal(ecx);

    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    m_HUDStep = (queueMode == 0) ? HUDPushStep::AfterPop : HUDPushStep::None;

    if (m_PushedHud)
    {
        IMatRenderContext* renderContext = m_Game->m_MaterialSystem ? m_Game->m_MaterialSystem->GetRenderContext() : nullptr;
        if (renderContext)
        {
            renderContext->OverrideAlphaWriteEnable(false, true);
            renderContext->ClearColor4ub(0, 0, 0, 255);
        }
    }

    hkPopRenderTargetAndViewport.fOriginal(ecx);
    m_PushedHud = false;
}

void Hooks::dVGui_Paint(void* ecx, void* edx, int mode)
{
    if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire))
        return hkVgui_Paint.fOriginal(ecx, mode);

    // When scope/rear-mirror RTT is rendering, don't redirect HUD/VGUI
    if (m_VR->m_SuppressHudCapture)
        return;

    const bool inGame = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    const bool isPaused = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsPaused();
    const bool cursorVisible = (m_Game && m_Game->m_VguiSurface) ? m_Game->m_VguiSurface->IsCursorVisible() : false;
    const bool allowBackbufferVgui = !inGame || isPaused || cursorVisible;

    auto PaintToHudOnce = [&](int paintMode)
        {
            bool expected = false;
            if (!m_VR->m_HudPaintedThisFrame.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return;

            IMatRenderContext* ctx = m_Game && m_Game->m_MaterialSystem ? m_Game->m_MaterialSystem->GetRenderContext() : nullptr;
            if (!ctx)
            {
                m_VR->HandleMissingRenderContext("Hooks::dVGui_Paint");
                return;
            }

            ITexture* hudTexture = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_VR->m_TextureMutex);
                hudTexture = m_VR->m_HUDTexture;
            }

            if (!hudTexture)
                return;

            ITexture* prevTarget = ctx->GetRenderTarget();
            if (prevTarget != hudTexture)
            {
                ctx->SetRenderTarget(hudTexture);
                ctx->OverrideAlphaWriteEnable(true, true);
                const unsigned char clearAlpha = isPaused ? 255 : 0;
                ctx->ClearColor4ub(0, 0, 0, clearAlpha);
                ctx->ClearBuffers(true, false, false);
                hkVgui_Paint.fOriginal(ecx, paintMode);
                ctx->OverrideAlphaWriteEnable(false, true);
                ctx->SetRenderTarget(prevTarget);
            }
            else
            {
                // Already on the HUD RT (single-threaded PushRT hijack).
                hkVgui_Paint.fOriginal(ecx, paintMode);
            }

            m_VR->m_RenderedHud.store(true, std::memory_order_release);
        };

    // Prefer a compact paint mode when capturing the HUD.
    const int hudMode = PAINT_UIPANELS | PAINT_INGAMEPANELS;
    const int fullHudMode = PAINT_UIPANELS | PAINT_INGAMEPANELS | PAINT_CURSOR;
    const int paintMode = (!inGame || cursorVisible || isPaused) ? (mode | fullHudMode) : hudMode;

    if (inGame && !allowBackbufferVgui)
    {
        // In-game: paint HUD into our HUD texture, suppress backbuffer VGUI to avoid
        // duplicating HUD into the eye render targets (especially under multicore).
        PaintToHudOnce(paintMode);
        return;
    }

    // Menu/pause/cursor: keep normal VGUI on the backbuffer, but also update HUD texture once.
    if (inGame)
        PaintToHudOnce(paintMode);
    hkVgui_Paint.fOriginal(ecx, mode);
}

//
int Hooks::dIsSplitScreen()
{
    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (queueMode == 0)
    {
        if (m_HUDStep == HUDPushStep::AfterPop)
            m_HUDStep = HUDPushStep::AfterIsSplitScreen;
        else
            m_HUDStep = HUDPushStep::None;
    }
    else
    {
        m_HUDStep = HUDPushStep::None;
    }

    return hkIsSplitScreen.fOriginal();
}

DWORD* Hooks::dPrePushRenderTarget(void* ecx, void* edx, int a2)
{
    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (queueMode == 0)
    {
        if (m_HUDStep == HUDPushStep::AfterIsSplitScreen)
            m_HUDStep = HUDPushStep::ReadyToOverride;
        else
            m_HUDStep = HUDPushStep::None;
    }
    else
    {
        m_HUDStep = HUDPushStep::None;
    }

    return hkPrePushRenderTarget.fOriginal(ecx, a2);
}
