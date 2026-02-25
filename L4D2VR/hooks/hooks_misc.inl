// ------------------------------------------------------------
// Multicore viewmodel stabilization helpers
//
// In Source, viewmodels are often drawn with pCustomBoneToWorld (bone matrices in world space).
// In that case, overriding ModelRenderInfo_t.origin/angles does NOT move the model.
//
// For queued rendering (mat_queue_mode!=0) we must instead apply a delta transform to the
// custom bone matrices for the draw call, so the viewmodel uses the controller-anchored pose
// sampled on the render thread (no shared-state writes, no tearing).
// ------------------------------------------------------------
namespace vr_vm_stabilize
{
    struct Mat3x4
    {
        float m[3][4];
    };

    template <typename T>
    inline bool SafeRead(const void* p, T& out)
    {
#if defined(_MSC_VER)
        __try
        {
            out = *reinterpret_cast<const T*>(p);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        // Non-MSVC builds are not expected for this project.
        // Keep it simple: attempt the read.
        out = *reinterpret_cast<const T*>(p);
        return true;
#endif
    }

    inline Vector GetOrigin(const Mat3x4& a)
    {
        return Vector(a.m[0][3], a.m[1][3], a.m[2][3]);
    }

    inline void BuildFromOrgAngles(const Vector& origin, const QAngle& ang, Mat3x4& out)
    {
        Vector f, r, u;
        QAngle::AngleVectors(ang, &f, &r, &u);

        out.m[0][0] = f.x; out.m[0][1] = r.x; out.m[0][2] = u.x; out.m[0][3] = origin.x;
        out.m[1][0] = f.y; out.m[1][1] = r.y; out.m[1][2] = u.y; out.m[1][3] = origin.y;
        out.m[2][0] = f.z; out.m[2][1] = r.z; out.m[2][2] = u.z; out.m[2][3] = origin.z;
    }

    // Invert a rigid transform (rotation + translation only)
    inline void InvertTR(const Mat3x4& in, Mat3x4& out)
    {
        // Transpose rotation
        out.m[0][0] = in.m[0][0]; out.m[0][1] = in.m[1][0]; out.m[0][2] = in.m[2][0];
        out.m[1][0] = in.m[0][1]; out.m[1][1] = in.m[1][1]; out.m[1][2] = in.m[2][1];
        out.m[2][0] = in.m[0][2]; out.m[2][1] = in.m[1][2]; out.m[2][2] = in.m[2][2];

        const float tx = -in.m[0][3];
        const float ty = -in.m[1][3];
        const float tz = -in.m[2][3];

        out.m[0][3] = tx * out.m[0][0] + ty * out.m[0][1] + tz * out.m[0][2];
        out.m[1][3] = tx * out.m[1][0] + ty * out.m[1][1] + tz * out.m[1][2];
        out.m[2][3] = tx * out.m[2][0] + ty * out.m[2][1] + tz * out.m[2][2];
    }

    inline void Mul(const Mat3x4& a, const Mat3x4& b, Mat3x4& out)
    {
        // Rotation
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            }
        }
        // Translation
        out.m[0][3] = a.m[0][0] * b.m[0][3] + a.m[0][1] * b.m[1][3] + a.m[0][2] * b.m[2][3] + a.m[0][3];
        out.m[1][3] = a.m[1][0] * b.m[0][3] + a.m[1][1] * b.m[1][3] + a.m[1][2] * b.m[2][3] + a.m[1][3];
        out.m[2][3] = a.m[2][0] * b.m[0][3] + a.m[2][1] * b.m[1][3] + a.m[2][2] * b.m[2][3] + a.m[2][3];
    }

    inline void ApplyDelta(const Mat3x4& delta, Mat3x4* bones, int numBones)
    {
        for (int i = 0; i < numBones; ++i)
        {
            Mat3x4 tmp{};
            Mul(delta, bones[i], tmp);
            bones[i] = tmp;
        }
    }
    // IMPORTANT for mat_queue_mode!=0:
    // DrawModelExecute may queue commands that reference pCustomBoneToWorld later on another thread.
    // If we pass a pointer to a temporary / thread_local scratch buffer, it can be overwritten before
    // the queued command executes, causing severe ghosting / double images.
    //
    // So we allocate per-draw bone copies from a small ring of per-frame slots. Each slot is kept
    // alive for kRing frames before being recycled. This makes the pointer stable long enough for
    // the material queue to consume it.
    struct BoneRingSlot
    {
        uint64_t frame = 0;
        std::vector<Mat3x4*> blocks;
    };

    inline Mat3x4* AllocStableBones(int numBones, uint32_t seqEven)
    {
        if (numBones <= 0 || numBones > 512)
            return nullptr;

        static constexpr uint32_t kRing = 64;
        static BoneRingSlot s_slots[kRing];
        static std::mutex s_mu;

        const uint64_t frame = (uint64_t)(seqEven >> 1);
        const uint32_t slot = (uint32_t)(frame % kRing);

        std::lock_guard<std::mutex> lock(s_mu);
        BoneRingSlot& s = s_slots[slot];
        if (s.frame != frame)
        {
            for (Mat3x4* p : s.blocks)
                delete[] p;
            s.blocks.clear();
            s.frame = frame;
        }

        Mat3x4* p = nullptr;
        try { p = new Mat3x4[(size_t)numBones]; } catch (...) { p = nullptr; }
        if (!p)
            return nullptr;

        s.blocks.push_back(p);
        return p;
    }
    // DrawModelState_t is opaque here, but in Source the first pointer is typically studiohdr_t*.
    // We avoid hard-crashing by SEH-guarding reads and probing common studiohdr_t offsets for numbones.
    inline bool TryGetNumBonesFromDrawState(void* drawState, int& outBones)
    {
        if (!drawState)
            return false;

        void* studioHdr = nullptr;
        if (!SafeRead(drawState, studioHdr) || !studioHdr)
            return false;

        // Common studiohdr_t::numbones offsets across Source branches.
        static const int kOffsets[] = { 0x9C, 0xA0, 0x98, 0x94, 0xA4, 0xA8, 0x90, 0x8C, 0xB0 };
        for (int off : kOffsets)
        {
            int n = 0;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(studioHdr) + off;
            if (SafeRead(p, n) && n > 0 && n <= 512)
            {
                outBones = n;
                return true;
            }
        }
        return false;
    }
}

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

	void* pBonesToWorldFinal = pCustomBoneToWorld;

	// Per-draw origin/angles override (used for multicore viewmodel stabilization).
	// We never write into shared entity state here; we only override the ModelRenderInfo_t
	// passed down to the renderer for this draw call (frame-stable, avoids queued-thread tearing).
	ModelRenderInfo_t drawInfo = info;
	const ModelRenderInfo_t* pDrawInfo = &info;

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


// --- Multicore viewmodel stabilization (first-person viewmodel ghosting fix) ---
// In queued rendering (mat_queue_mode!=0), viewmodels are frequently submitted with custom bone matrices.
// In that case, overriding ModelRenderInfo_t.origin/angles does NOT move the model (it stays "head-locked").
// So we apply a rigid delta to the bone matrices for this draw call, based on our controller-anchored target.
const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
if (m_VR->m_IsVREnabled && queueMode == 2 && m_VR->m_QueuedViewmodelStabilize)
{
	const bool isViewmodelClass = className &&
		(std::strcmp(className, "CBaseViewModel") == 0 || std::strcmp(className, "C_BaseViewModel") == 0);
	const bool isViewmodelModel =
		(modelName.find("models/weapons/v_") != std::string::npos) ||
		(modelName.find("/v_models/") != std::string::npos) ||
		(modelName.find("models/v_models/") != std::string::npos) ||

		// L4D2 melee viewmodels often live under models/weapons/melee/...
		(modelName.find("models/weapons/melee/v_") != std::string::npos) ||
		(modelName.find("models/weapons/melee/") != std::string::npos && modelName.find("/v_") != std::string::npos) ||
		(modelName.find("/melee/v_") != std::string::npos) ||

		// Arms/hands are frequently separate models from the gun.
		(modelName.find("models/weapons/arms/") != std::string::npos) ||
		(modelName.find("/arms/") != std::string::npos) ||
		(modelName.find("v_arms") != std::string::npos) ||
		(modelName.find("models/weapons/hands/") != std::string::npos) ||
		(modelName.find("/hands/") != std::string::npos) ||
		(modelName.find("v_hands") != std::string::npos);


	if (isViewmodelClass || isViewmodelModel)
	{
		struct RenderSnapshotTLSGuard
		{
			bool prev = false;
			RenderSnapshotTLSGuard()
			{
				prev = VR::t_UseRenderFrameSnapshot;
				VR::t_UseRenderFrameSnapshot = true;
			}
			~RenderSnapshotTLSGuard()
			{
				VR::t_UseRenderFrameSnapshot = prev;
			}
		} tlsGuard;

		const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos();
		const QAngle targetAngles = m_VR->GetRecommendedViewmodelAbsAngle();

		// Always override origin/angles for lighting/etc (even if bones are used).
		drawInfo = info;
		drawInfo.origin = targetOrigin;
		drawInfo.angles = targetAngles;
		pDrawInfo = &drawInfo;

		bool appliedBoneDelta = false;
		int numBones = 0;

		if (pCustomBoneToWorld)
		{
			if (vr_vm_stabilize::TryGetNumBonesFromDrawState(state, numBones) && numBones > 0)
			{
										uint32_t seqEven = m_VR->m_RenderFrameSeq.load(std::memory_order_acquire);
										seqEven &= ~1u;
										if (seqEven == 0)
											seqEven = 2;

										vr_vm_stabilize::Mat3x4* bonesCopy = vr_vm_stabilize::AllocStableBones(numBones, seqEven);
										if (bonesCopy)
										{
											memcpy(bonesCopy, pCustomBoneToWorld, (size_t)numBones * sizeof(vr_vm_stabilize::Mat3x4));

											// NOTE:
											// pCustomBoneToWorld is already in WORLD space. However, bone[0] is NOT guaranteed
											// to be at the entity origin (studio root can have a built-in offset). Using bone[0]
											// as the reference will mis-anchor the whole model (often looks like it's still HMD-bound).
											//
											// Correct approach: treat the bones as (EntityToWorld * BoneLocal). Recover BoneLocal
											// via inverse(EntityToWorld), then re-apply with TargetEntityToWorld.
											vr_vm_stabilize::Mat3x4 origEntity{};
											vr_vm_stabilize::BuildFromOrgAngles(info.origin, info.angles, origEntity);
											vr_vm_stabilize::Mat3x4 origInv{};
											vr_vm_stabilize::InvertTR(origEntity, origInv);
											vr_vm_stabilize::Mat3x4 targetEntity{};
											vr_vm_stabilize::BuildFromOrgAngles(targetOrigin, targetAngles, targetEntity);
											vr_vm_stabilize::Mat3x4 delta{};
											vr_vm_stabilize::Mul(targetEntity, origInv, delta);
											vr_vm_stabilize::ApplyDelta(delta, bonesCopy, numBones);

											pBonesToWorldFinal = bonesCopy;
											appliedBoneDelta = true;
										}
			}
		}

		if (m_VR->m_QueuedViewmodelStabilizeDebugLog)
		{
			static thread_local std::chrono::steady_clock::time_point s_last{};
			if (!ShouldThrottleLog(s_last, m_VR->m_QueuedViewmodelStabilizeDebugLogHz))
			{
				const uint32_t seq = m_VR->m_RenderFrameSeq.load(std::memory_order_relaxed);
				const uint32_t tid = (uint32_t)GetCurrentThreadId();
										Vector root0 = info.origin;
										Vector root1 = targetOrigin;
										if (pCustomBoneToWorld)
										{
											vr_vm_stabilize::Mat3x4 r0{};
											if (vr_vm_stabilize::SafeRead(pCustomBoneToWorld, r0))
												root0 = vr_vm_stabilize::GetOrigin(r0);
										}
										if (appliedBoneDelta && pBonesToWorldFinal)
										{
											root1 = vr_vm_stabilize::GetOrigin(reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pBonesToWorldFinal)[0]);
										}

										const Vector eyeO = m_VR->m_HmdPosAbs;
										const Vector rcO = m_VR->GetRightControllerAbsPos();
										const float dTgtRc = (targetOrigin - rcO).Length();
                                        const Vector entDelta = targetOrigin - info.origin;
                                        Vector bone0Off(0.0f, 0.0f, 0.0f);
                                        if (pCustomBoneToWorld)
                                        {
                                            vr_vm_stabilize::Mat3x4 r0{};
                                            if (vr_vm_stabilize::SafeRead(pCustomBoneToWorld, r0))
                                                bone0Off = vr_vm_stabilize::GetOrigin(r0) - info.origin;
                                        }

										Game::logMsg(
										"[VR][VM][draw] tid=%u qmode=%d seq=%u ent=%d model=\"%s\" customBones=%d bones=%d applied=%d slot=%u root0=(%.2f %.2f %.2f) root1=(%.2f %.2f %.2f) eyeO=(%.2f %.2f %.2f) rcO=(%.2f %.2f %.2f) dTgtRc=%.2f entD=(%.2f %.2f %.2f) bone0Off=(%.2f %.2f %.2f) origO=(%.2f %.2f %.2f) origA=(%.2f %.2f %.2f) tgtO=(%.2f %.2f %.2f) tgtA=(%.2f %.2f %.2f)"
										,
										tid, queueMode, seq, info.entity_index, modelName.c_str(),
										(pCustomBoneToWorld != nullptr) ? 1 : 0,
										numBones,
										appliedBoneDelta ? 1 : 0,
										(uint32_t)((seq >> 1) % 64),
										root0.x, root0.y, root0.z,
										root1.x, root1.y, root1.z,
										eyeO.x, eyeO.y, eyeO.z,
										rcO.x, rcO.y, rcO.z,
										dTgtRc,
                                        entDelta.x, entDelta.y, entDelta.z,
                                        bone0Off.x, bone0Off.y, bone0Off.z,
										info.origin.x, info.origin.y, info.origin.z,
										info.angles.x, info.angles.y, info.angles.z,
										targetOrigin.x, targetOrigin.y, targetOrigin.z,
										targetAngles.x, targetAngles.y, targetAngles.z);
			}
		}
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
		hkDrawModelExecute.fOriginal(ecx, state, *pDrawInfo, pBonesToWorldFinal);
		m_Game->m_ModelRender->ForcedMaterialOverride(NULL);
		return;
	}

	hkDrawModelExecute.fOriginal(ecx, state, *pDrawInfo, pBonesToWorldFinal);
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
