void __fastcall Hooks::dEndFrame(void* ecx, void* edx)
{
	return hkEndFrame.fOriginal(ecx);
}

static inline void CallCalcViewModelViewOriginal(void* ecx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
{
	if (!Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled || !Hooks::m_VR->m_ViewmodelDisableMoveBob || !owner)
	{
		Hooks::hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
		return;
	}

	C_BasePlayer* ownerPlayer = reinterpret_cast<C_BasePlayer*>(owner);
	const Vector savedVelocity = ownerPlayer->m_vecVelocity;
	ownerPlayer->m_vecVelocity = Vector{ 0.0f, 0.0f, 0.0f };
	Hooks::hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
	ownerPlayer->m_vecVelocity = savedVelocity;
}


void __fastcall Hooks::dCalcViewModelView(void* ecx, void* edx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
{
	Vector vecNewOrigin = eyePosition;
	QAngle vecNewAngles = eyeAngles;

	if (m_VR->m_IsVREnabled)
	{
		const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
		const bool multiCoreQueued = (queueMode == 2);
		const bool forceDisableMoveBob = m_VR->m_ViewmodelDisableMoveBob;

		// ------------------------------------------------------------
		// Single-thread path (mat_queue_mode 0/1)
		//
		// 你的“单线程 vm 正常版本”里就是这个行为：把 controller 侧的
		// viewmodel 推荐位姿当作 eye 输入喂给 CalcViewModelView。
		//
		// 这样 engine 生成的 viewmodel/weapon/attachment(枪口闪光/烟雾)
		// 都会围绕枪来算，而不会 head-locked 在 HMD 上。
		// ------------------------------------------------------------
		if (!multiCoreQueued)
		{
			vecNewOrigin = m_VR->GetRecommendedViewmodelAbsPos();
			vecNewAngles = m_VR->GetRecommendedViewmodelAbsAngle();
			if (!forceDisableMoveBob)
				return hkCalcViewModelView.fOriginal(ecx, owner, vecNewOrigin, vecNewAngles);

			CallCalcViewModelViewOriginal(ecx, owner, vecNewOrigin, vecNewAngles);
			if (ecx)
			{
				IClientEntity* ent = reinterpret_cast<IClientEntity*>(ecx);
				ent->GetAbsOrigin() = vecNewOrigin;
				ent->GetAbsAngles() = vecNewAngles;
			}
			return;
		}

		// ------------------------------------------------------------
		// Multi-core queued path (mat_queue_mode 2)
		//
		// queued 渲染下 CalcViewModelView 可能跑在 material/queue 线程。
		// 这里把它当“纯渲染期”逻辑：
		//  1) 先用真实 eye 输入跑一遍 engine 内部状态（避免把 controller 当 eye
		//     导致 bob/lag 状态被污染）；
		//  2) 再用 controller 目标位姿做一次 best-effort 覆盖（视觉稳定）。
		// ------------------------------------------------------------
		struct RenderSnapshotTLSGuard
		{
			bool enabled = false;
			bool prev = false;
			RenderSnapshotTLSGuard(bool en)
			{
				enabled = en;
				if (enabled)
				{
					prev = VR::t_UseRenderFrameSnapshot;
					VR::t_UseRenderFrameSnapshot = true;
				}
			}
			~RenderSnapshotTLSGuard()
			{
				if (enabled)
					VR::t_UseRenderFrameSnapshot = prev;
			}
		} tlsGuard(true);

		// Call engine logic (keeps internal viewmodel state up to date).
		CallCalcViewModelViewOriginal(ecx, owner, eyePosition, eyeAngles);

		if (m_VR->m_QueuedViewmodelStabilize || forceDisableMoveBob)
		{
			const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos();
			const QAngle targetAngles = m_VR->GetRecommendedViewmodelAbsAngle();

			// Capture what the engine produced (before we override) for debug.
			Vector engineOrigin = {};
			QAngle engineAngles = {};
			if (ecx)
			{
				IClientEntity* ent = reinterpret_cast<IClientEntity*>(ecx);
				engineOrigin = ent->GetAbsOrigin();
				engineAngles = ent->GetAbsAngles();
			}

			// Hard-override viewmodel transform (best-effort).
			// NOTE: queued 渲染下尽量别写 entity 状态，但为了让枪口 attachment/FX
			// 更贴近 controller，这里保留你当前工程的覆盖逻辑。
			bool originSet = false;
			if (m_Game && m_Game->m_Offsets && m_Game->m_Offsets->CBaseEntity_SetAbsOrigin_Client.address && ecx)
			{
				using SetAbsOriginFn = void(__thiscall*)(void*, const Vector&);
				auto setAbsOrigin = (SetAbsOriginFn)m_Game->m_Offsets->CBaseEntity_SetAbsOrigin_Client.address;
				setAbsOrigin(ecx, targetOrigin);
				originSet = true;
			}

			if (ecx)
			{
				IClientEntity* ent = reinterpret_cast<IClientEntity*>(ecx);
				ent->GetAbsAngles() = targetAngles;
				if (!originSet)
					ent->GetAbsOrigin() = targetOrigin;
			}

			if (m_VR->m_QueuedViewmodelStabilizeDebugLog)
			{
				static thread_local std::chrono::steady_clock::time_point s_last{};
				if (!ShouldThrottleLog(s_last, m_VR->m_QueuedViewmodelStabilizeDebugLogHz))
				{
					const uint32_t seq = m_VR->m_RenderFrameSeq.load(std::memory_order_relaxed);
					const uint32_t tid = (uint32_t)GetCurrentThreadId();

					const float dx = targetOrigin.x - engineOrigin.x;
					const float dy = targetOrigin.y - engineOrigin.y;
					const float dz = targetOrigin.z - engineOrigin.z;
					const float dpos = sqrtf(dx * dx + dy * dy + dz * dz);

					Game::logMsg(
						"[VR][VM][queue] tid=%u qmode=%d seq=%u dpos=%.2f eyeO=(%.2f %.2f %.2f) eyeA=(%.2f %.2f %.2f) tgtO=(%.2f %.2f %.2f) tgtA=(%.2f %.2f %.2f) engO=(%.2f %.2f %.2f) engA=(%.2f %.2f %.2f)",
						tid, queueMode, seq, dpos,
						eyePosition.x, eyePosition.y, eyePosition.z,
						eyeAngles.x, eyeAngles.y, eyeAngles.z,
						targetOrigin.x, targetOrigin.y, targetOrigin.z,
						targetAngles.x, targetAngles.y, targetAngles.z,
						engineOrigin.x, engineOrigin.y, engineOrigin.z,
						engineAngles.x, engineAngles.y, engineAngles.z);
				}
			}
		}

		return; // we already called original
	}

	return hkCalcViewModelView.fOriginal(ecx, owner, vecNewOrigin, vecNewAngles);
}
int Hooks::dServerFireTerrorBullets(int playerId, const Vector& vecOrigin, const QAngle& vecAngles, int a4, int a5, int a6, float a7)
{
	Vector vecNewOrigin = vecOrigin;
	QAngle vecNewAngles = vecAngles;

	// Server host
	if (m_VR->m_IsVREnabled && playerId == m_Game->m_EngineClient->GetLocalPlayer())
	{
		const bool scopeActive = m_VR->IsScopeActive();
		vecNewOrigin = m_VR->m_MouseModeEnabled ? GetMouseModeGunOriginAbs(m_VR) : m_VR->GetRightControllerAbsPos();

		// ForceNonVRServerMovement: aim the *visual* bullet line to the solved hit point (H)
		// so what you see matches what the remote non-VR server will hit.
		if (m_VR->m_ForceNonVRServerMovement && m_VR->m_HasNonVRAimSolution)
		{
			Vector to = m_VR->m_NonVRAimHitPoint - vecNewOrigin;
			if (!to.IsZero())
			{
				VectorNormalize(to);
				QAngle ang;
				QAngle::VectorAngles(to, ang);
				NormalizeAndClampViewAngles(ang);
				vecNewAngles = ang;
			}
			else
			{
				vecNewAngles = m_VR->m_NonVRAimAngles;
			}
		}
		else if (scopeActive)
		{
			vecNewOrigin = m_VR->GetScopeCameraAbsPos();
			vecNewAngles = m_VR->GetScopeCameraAbsAngle();
		}
		// Third-person convergence
		else if (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
		{
			Vector to = m_VR->m_AimConvergePoint - vecNewOrigin;
			if (!to.IsZero())
			{
				VectorNormalize(to);
				QAngle ang;
				QAngle::VectorAngles(to, ang);
				NormalizeAndClampViewAngles(ang);
				vecNewAngles = ang;
			}
			else
			{
				vecNewAngles = m_VR->m_MouseModeEnabled ? GetMouseModeFallbackAimAngles(m_VR) : m_VR->GetRightControllerAbsAngle();
			}
		}
		else if (m_VR->m_MouseModeEnabled)
		{
			const Vector target = (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
				? m_VR->m_AimConvergePoint
				: GetMouseModeDefaultTargetAbs(m_VR);

			QAngle ang;
			if (GetMouseModeAimAnglesToTarget(m_VR, vecNewOrigin, target, ang))
				vecNewAngles = ang;
			else
				vecNewAngles = GetMouseModeFallbackAimAngles(m_VR);
		}
		else
		{
			vecNewAngles = m_VR->GetRightControllerAbsAngle();
		}
	}
	// Clients
	else if (m_Game->IsValidPlayerIndex(playerId) && m_Game->m_PlayersVRInfo[playerId].isUsingVR)
	{
		vecNewOrigin = m_Game->m_PlayersVRInfo[playerId].controllerPos;
		vecNewAngles = m_Game->m_PlayersVRInfo[playerId].controllerAngle;
	}

	return hkServerFireTerrorBullets.fOriginal(playerId, vecNewOrigin, vecNewAngles, a4, a5, a6, a7);
}

int Hooks::dClientFireTerrorBullets(
	int playerId,
	const Vector& vecOrigin,
	const QAngle& vecAngles,
	int a4, int a5, int a6,
	float a7)
{
	Vector vecNewOrigin = vecOrigin;
	QAngle vecNewAngles = vecAngles;

	// 只改本地玩家的“本地预测/表现”
	if (m_VR->m_IsVREnabled && playerId == m_Game->m_EngineClient->GetLocalPlayer())
	{
		// If looking through scope: bullets originate from scope camera and go through its center
		const bool scopeActive = m_VR->IsScopeActive();
		const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
		// In mat_queue_mode!=0, client-side bullet FX can lag behind the rendered gun/aim line if they
		// read main-thread poses. Treat this hook as visual-only and allow it to consume the render-frame snapshot.
		struct RenderSnapshotTLSGuard
		{
			bool enabled = false;
			bool prev = false;
			RenderSnapshotTLSGuard(bool en)
			{
				enabled = en;
				if (enabled)
				{
					prev = VR::t_UseRenderFrameSnapshot;
					VR::t_UseRenderFrameSnapshot = true;
				}
			}
			~RenderSnapshotTLSGuard()
			{
				if (enabled)
					VR::t_UseRenderFrameSnapshot = prev;
			}
		} tlsGuard(queueMode == 2);


		if (!m_VR->m_ForceNonVRServerMovement)
		{
			// VR-aware server：默认起点/方向都跟控制器；鼠标模式改用鼠标枪口锚点 + 目标方向
			if (scopeActive)
			{
				vecNewOrigin = m_VR->GetScopeCameraAbsPos();
				vecNewAngles = m_VR->GetScopeCameraAbsAngle();
			}
			else
			{
				if (m_VR->m_MouseModeEnabled)
				{
					vecNewOrigin = GetMouseModeGunOriginAbs(m_VR);

					const Vector target = (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
						? m_VR->m_AimConvergePoint
						: GetMouseModeDefaultTargetAbs(m_VR);

					QAngle ang;
					if (GetMouseModeAimAnglesToTarget(m_VR, vecNewOrigin, target, ang))
						vecNewAngles = ang;
					else
						vecNewAngles = GetMouseModeFallbackAimAngles(m_VR);
				}
				else
				{
					vecNewOrigin = m_VR->GetRightControllerAbsPos();

					if (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
					{
						Vector to = m_VR->m_AimConvergePoint - vecNewOrigin;
						if (!to.IsZero())
						{
							VectorNormalize(to);
							QAngle ang;
							QAngle::VectorAngles(to, ang);
							NormalizeAndClampViewAngles(ang);
							vecNewAngles = ang;
						}
						else
						{
							vecNewAngles = m_VR->GetRightControllerAbsAngle();
						}
					}
					else
					{
						vecNewAngles = m_VR->GetRightControllerAbsAngle();
					}
				}
			}
		}
		else
		{
			// Non-VR server：服务器判定仍以 eye(vecOrigin)+cmd->viewangles 为准（我们不会改服务器判定）。
			// 但客户端的弹道线/枪口火焰/烟雾等特效可以选择从控制器枪口发出（纯视觉）。
			if (m_VR->m_NonVRServerMovementEffectsFromController)
			{
				if (scopeActive)
				{
					vecNewOrigin = m_VR->GetScopeCameraAbsPos();
				}
				else
				{
					vecNewOrigin = m_VR->m_MouseModeEnabled ? GetMouseModeGunOriginAbs(m_VR) : m_VR->GetRightControllerAbsPos();
				}

			// 开关决定：要不要把 angles 替换成“控制器纯角度/汇聚角度”
			// - true  : 覆盖 angles（通常会让本地弹道更“直/更跟手”，但只是本地表现）
			// - false : 保持 vecAngles（通常包含引擎/服务器那套散布偏转 → 看起来更“标准散布”）
			if (m_VR->m_NonVRServerMovementAngleOverride)
			{
				// Prefer the solved eye-based aim. This keeps client prediction + hit feedback
				// consistent with what the non-VR server will do.
				if (m_VR->m_HasNonVRAimSolution)
				{
					vecNewAngles = m_VR->m_NonVRAimAngles;
				}
				else
				{
					if (!scopeActive && m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
					{
						Vector to = m_VR->m_AimConvergePoint - vecNewOrigin; // 注意：这里是 vecOrigin
						if (!to.IsZero())
						{
							VectorNormalize(to);
							QAngle ang;
							QAngle::VectorAngles(to, ang);
							NormalizeAndClampViewAngles(ang);
							vecNewAngles = ang;
						}
						else
						{
							vecNewAngles = m_VR->GetRightControllerAbsAngle();
						}
					}
					else
					{
						vecNewAngles = m_VR->GetRightControllerAbsAngle();
					}
				}
			}
			// else：不动 vecNewAngles = vecAngles（保留“标准散布”的那套）
		}
	}

    // Final bullet FX alignment: apply hit-point offset AFTER all angle overrides.
    // This is intentionally visual-only (client FX). It does not affect server hit registration.
    if (m_VR->m_IsVREnabled
        && m_VR->m_ForceNonVRServerMovement
        && m_VR->m_HasNonVRAimSolution)
    {
        const int qmode = (m_Game ? m_Game->GetMatQueueMode() : 0);
        Vector visualOff = m_VR->m_BulletVisualHitOffset;
        if (qmode != 0)
        {
            visualOff.x += m_VR->m_QueuedBulletVisualHitOffset.x;
            visualOff.y += m_VR->m_QueuedBulletVisualHitOffset.y;
            visualOff.z += m_VR->m_QueuedBulletVisualHitOffset.z;
        }

        if (!visualOff.IsZero())
        {
            Vector originForDir = vecNewOrigin;
            Vector targetH = m_VR->m_NonVRAimHitPoint;

            Vector fwd = targetH - originForDir;
            if (!fwd.IsZero())
            {
                VectorNormalize(fwd);
                Vector worldUp(0.0f, 0.0f, 1.0f);
                Vector right;
                CrossProduct(worldUp, fwd, right);
                if (right.LengthSqr() < 1e-6f)
                {
                    worldUp = Vector(0.0f, 1.0f, 0.0f);
                    CrossProduct(worldUp, fwd, right);
                }
                VectorNormalize(right);
                Vector up;
                CrossProduct(fwd, right, up);
                VectorNormalize(up);

                const float su = m_VR->m_VRScale;
                const Vector offSu = visualOff * su;
                targetH += (fwd * offSu.x) + (right * offSu.y) + (up * offSu.z);

                Vector to = targetH - originForDir;
                if (!to.IsZero())
                {
                    VectorNormalize(to);
                    QAngle ang;
                    QAngle::VectorAngles(to, ang);
                    NormalizeAndClampViewAngles(ang);
                    vecNewAngles = ang;

                    if (m_VR->m_NonVRServerMovementEffectsDebugLog)
                    {
                        static thread_local std::chrono::steady_clock::time_point s_lastOff{};
                        if (!ShouldThrottleLog(s_lastOff, m_VR->m_NonVRServerMovementEffectsDebugLogHz))
                        {
                            const uint32_t tid = (uint32_t)GetCurrentThreadId();
                            const uint32_t seq = m_VR->m_RenderFrameSeq.load(std::memory_order_relaxed);
                            Game::logMsg(
                                "[VR][FX][bullets][offset] tid=%u qmode=%d seq=%u offTotal=(%.3f %.3f %.3f)m base=(%.3f %.3f %.3f)m qExtra=(%.3f %.3f %.3f)m origin=(%.2f %.2f %.2f) H=(%.2f %.2f %.2f)",
                                tid, qmode, seq,
                                visualOff.x, visualOff.y, visualOff.z,
                                m_VR->m_BulletVisualHitOffset.x, m_VR->m_BulletVisualHitOffset.y, m_VR->m_BulletVisualHitOffset.z,
                                m_VR->m_QueuedBulletVisualHitOffset.x, m_VR->m_QueuedBulletVisualHitOffset.y, m_VR->m_QueuedBulletVisualHitOffset.z,
                                originForDir.x, originForDir.y, originForDir.z,
                                targetH.x, targetH.y, targetH.z);
                        }
                    }
                }
            }
        }
    }



    if (m_VR->m_IsVREnabled && m_Game && m_Game->m_EngineClient
        && playerId == m_Game->m_EngineClient->GetLocalPlayer())
    {
        int weaponId = (int)C_WeaponCSBase::WeaponID::NONE;
        C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerId);
        if (localPlayer)
        {
            C_WeaponCSBase* activeWeapon = (C_WeaponCSBase*)localPlayer->GetActiveWeapon();
            if (activeWeapon)
                weaponId = (int)activeWeapon->GetWeaponID();
        }
        m_VR->TriggerWeaponFireHaptics(weaponId, false);
    }

    // RightAmmoHUD: hit-based target HP bar has been removed.
// The ammo HUD now shows HP%% for the *aimed* special infected (and Witch) and hides instantly on leave.
    if (m_VR->m_IsVREnabled)
        m_VR->RegisterPotentialKillSoundHit(vecNewOrigin, vecNewAngles);

    const auto original = hkClientFireTerrorBullets.fOriginal;
    if (!original)
        return 0;

    return original(playerId, vecNewOrigin, vecNewAngles, a4, a5, a6, a7);
}
}


// === 用下面这整个函数替换你当前的 Hooks::dProcessUsercmds ===
float __fastcall Hooks::dProcessUsercmds(void* ecx, void* edx, edict_t* player,
	void* buf, int numcmds, int totalcmds,
	int dropped_packets, bool ignore, bool paused)
{
	// ★ 进入该钩子，说明本进程正在跑“服务器”逻辑（listen/dedicated）
	Hooks::s_ServerUnderstandsVR = true;

	// Function pointer for CBaseEntity::entindex
	typedef int(__thiscall* tEntindex)(void* thisptr);
	static tEntindex oEntindex = (tEntindex)(m_Game->m_Offsets->CBaseEntity_entindex.address);

	IServerUnknown* pUnknown = player->m_pUnk;
	Server_BaseEntity* pPlayer = (Server_BaseEntity*)pUnknown->GetBaseEntity();
	m_Game->m_CurrentUsercmdPlayer = pPlayer;

	int index = oEntindex(pPlayer);
	m_Game->m_CurrentUsercmdID = index;

	float result = hkProcessUsercmds.fOriginal(ecx, player, buf, numcmds, totalcmds, dropped_packets, ignore, paused);

	// ===== 你原有的“近战挥砍检测/追踪”逻辑，保持不变 =====
	const bool hasValidPlayer = m_Game->IsValidPlayerIndex(index);

	if (hasValidPlayer && m_Game->m_PlayersVRInfo[index].isUsingVR && m_Game->m_PlayersVRInfo[index].isMeleeing)
	{
		typedef Server_WeaponCSBase* (__thiscall* tGetActiveWep)(void* thisptr);
		static tGetActiveWep oGetActiveWep = (tGetActiveWep)(m_Game->m_Offsets->GetActiveWeapon.address);
		Server_WeaponCSBase* curWep = oGetActiveWep(pPlayer);

		if (curWep)
		{
			int wepID = curWep->GetWeaponID();
			if (wepID == 19) // melee weapon
			{
				if (m_Game->m_PlayersVRInfo[index].isNewSwing)
				{
					m_Game->m_PlayersVRInfo[index].isNewSwing = false;
					curWep->entitiesHitThisSwing = 0;
				}

				typedef void* (__thiscall* tGetMeleeWepInfo)(void* thisptr);
				static tGetMeleeWepInfo oGetMeleeWepInfo = (tGetMeleeWepInfo)(m_Game->m_Offsets->GetMeleeWeaponInfo.address);
				void* meleeWepInfo = oGetMeleeWepInfo(curWep);

				Vector initialForward, initialRight, initialUp;
				QAngle::AngleVectors(m_Game->m_PlayersVRInfo[index].prevControllerAngle, &initialForward, &initialRight, &initialUp);
				Vector initialMeleeDirection = VectorRotate(initialForward, initialRight, 50.0f);
				VectorNormalize(initialMeleeDirection);

				Vector finalForward, finalRight, finalUp;
				QAngle::AngleVectors(m_Game->m_PlayersVRInfo[index].controllerAngle, &finalForward, &finalRight, &finalUp);
				Vector finalMeleeDirection = VectorRotate(finalForward, finalRight, 50.0f);
				VectorNormalize(finalMeleeDirection);

				Vector pivot;
				CrossProduct(initialMeleeDirection, finalMeleeDirection, pivot);
				VectorNormalize(pivot);

				float swingAngle = acosf(DotProduct(initialMeleeDirection, finalMeleeDirection)) * 180.0f / 3.14159265f;

				m_Game->m_Hooks->hkGetPrimaryAttackActivity.fOriginal(curWep, meleeWepInfo); // Needed to call TestMeleeSwingCollision

				m_Game->m_PerformingMelee = true;

				Vector traceDirection = initialMeleeDirection;
				int numTraces = 10;
				float traceAngle = swingAngle / numTraces;
				for (int i = 0; i < numTraces; ++i)
				{
					traceDirection = VectorRotate(traceDirection, pivot, traceAngle);
					m_Game->m_Hooks->hkTestMeleeSwingCollisionServer.fOriginal(curWep, traceDirection);
				}

				m_Game->m_PerformingMelee = false;
			}
		}
	}
	else if (hasValidPlayer)
	{
		m_Game->m_PlayersVRInfo[index].isNewSwing = true;
	}

	if (hasValidPlayer)
	{
		m_Game->m_PlayersVRInfo[index].prevControllerAngle = m_Game->m_PlayersVRInfo[index].controllerAngle;
	}

	return result;
}

int Hooks::dReadUsercmd(void* buf, CUserCmd* move, CUserCmd* from)
{
	hkReadUsercmd.fOriginal(buf, move, from);

	int i = m_Game->m_CurrentUsercmdID;
	const bool hasValidPlayer = m_Game->IsValidPlayerIndex(i);
	if (m_VR->m_EncodeVRUsercmd && move->tick_count < 0) // Signal for VR CUserCmd
	{
		move->tick_count *= -1;

		if (move->command_number < 0)
		{
			move->command_number *= -1;
			if (hasValidPlayer)
			{
				m_Game->m_PlayersVRInfo[i].isMeleeing = true;
			}
		}
		else
		{
			if (hasValidPlayer)
			{
				m_Game->m_PlayersVRInfo[i].isMeleeing = false;
			}
		}

		if (hasValidPlayer)
		{
			m_Game->m_PlayersVRInfo[i].isUsingVR = true;
			m_Game->m_PlayersVRInfo[i].controllerAngle.x = (float)move->mousedx / 10;
			m_Game->m_PlayersVRInfo[i].controllerAngle.y = (float)move->mousedy / 10;
			m_Game->m_PlayersVRInfo[i].controllerPos.x = move->viewangles.z;
			m_Game->m_PlayersVRInfo[i].controllerPos.y = move->upmove;
		}

		// Decode controllerAngle.z
		int rollEncoding = move->command_number / 10000000;
		move->command_number -= rollEncoding * 10000000;
		if (hasValidPlayer)
		{
			m_Game->m_PlayersVRInfo[i].controllerAngle.z = (rollEncoding * 2) - 180;
		}

		// Decode viewangles.x
		int decodedZInt = (move->viewangles.x / 10000);
		float decodedAngle = fabsf((float)(move->viewangles.x - (decodedZInt * 10000)) / 10);
		decodedAngle -= 360.0f;
		float decodedZ = (float)decodedZInt / 10.0f;

		if (hasValidPlayer)
		{
			m_Game->m_PlayersVRInfo[i].controllerPos.z = decodedZ;
		}

		move->viewangles.x = decodedAngle;
		move->viewangles.z = 0;
		move->upmove = 0;
	}
	else
	{
		if (hasValidPlayer)
		{
			m_Game->m_PlayersVRInfo[i].isUsingVR = false;
		}
	}

	// ---- roomscale 1:1 server apply ----
	if (m_Game && m_VR && m_VR->m_Roomscale1To1Movement && !m_VR->m_ForceNonVRServerMovement)
	{
		static constexpr uint32_t kRSButtonsMask = 0xFC000000u; // bits 26..31
		const uint32_t rawButtons = (uint32_t)move->buttons;
		const uint32_t rawPacked = (rawButtons & kRSButtonsMask);
		// NOTE: Do NOT reuse m_Roomscale1To1DebugLastServer for both "pre" and "apply".
		// Otherwise "pre" consumes the throttle budget and "apply" never prints.
		static std::chrono::steady_clock::time_point s_roomscaleServerPreLast{};
		if (m_VR->m_Roomscale1To1DebugLog && rawPacked != 0 && !ShouldThrottleLog(s_roomscaleServerPreLast, m_VR->m_Roomscale1To1DebugLogHz))
		{
			Game::logMsg("[VR][1to1][server] pre cmd=%d tick=%d player=%d wsel=%d buttons=0x%08X",
				move->command_number, move->tick_count, i, move->weaponselect, (unsigned)rawButtons);
		}
		Vector deltaM;
		if (VR::DecodeRoomscale1To1Delta((int)rawPacked, deltaM) && move->weaponselect == 0)
		{
			// Hide our custom packed bits from the stock server movement/weapon code.
			move->buttons &= ~(int)kRSButtonsMask;
			Server_BaseEntity* ent = m_Game->m_CurrentUsercmdPlayer;
			if (ent && m_Game->m_EngineTrace && m_Game->m_Offsets)
			{
				using SetAbsOriginFn = void(__thiscall*)(void*, const Vector&);
				auto setAbsOrigin = (SetAbsOriginFn)(m_Game->m_Offsets->CBaseEntity_SetAbsOrigin_Server.address);

				if (setAbsOrigin)
				{
					Vector origin = *(Vector*)((uintptr_t)ent + 0x2CC);
					Vector deltaU(deltaM.x * m_VR->m_VRScale, deltaM.y * m_VR->m_VRScale, 0.0f);
					Vector target = origin + deltaU;

					Vector mins(-16, -16, 0);
					Vector maxs(16, 16, 72);
					// NOTE: Our server entity type is a thin vtable stub; pointer-casting it to IHandleEntity
					// is not reliable for self-filtering. For 1:1 roomscale we only need world collision here.
					struct TraceFilterWorldOnly final : public CTraceFilter
					{
						TraceFilterWorldOnly() : CTraceFilter(nullptr, 0) {}
						bool ShouldHitEntity(IHandleEntity*, int) override { return false; }
						TraceType GetTraceType() const override { return TraceType::TRACE_WORLD_ONLY; }
					} filter;

					// Lift slightly to avoid rare floor-penetration marking the start as solid.
					Vector o2 = origin;
					Vector t2 = target;
					o2.z += 0.25f;
					t2.z += 0.25f;
					Ray_t ray;
					ray.Init(o2, t2, mins, maxs);

					trace_t tr;

					const unsigned int mask = CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_GRATE;
					m_Game->m_EngineTrace->TraceRay(ray, mask, &filter, &tr);

					bool didMove = false;
					if (!tr.startsolid)
					{
						Vector end = tr.endpos;
						end.z = origin.z; // keep vertical position unchanged (we only apply planar room-scale)
						setAbsOrigin(ent, end);
						didMove = true;
					}

					// Hard debug sample: once every 32 cmds, no throttle. This tells us whether the server actually moved the entity.
					if (m_VR->m_Roomscale1To1DebugLog && ((move->command_number & 31) == 0))
					{
						Game::logMsg("[VR][1to1][server] APPLY-SAMPLE cmd=%d tick=%d player=%d ent=%p setAbsOrigin=%p packed=0x%08X dM=(%.3f %.3f) origin=(%.2f %.2f %.2f) target=(%.2f %.2f %.2f) end=(%.2f %.2f %.2f) frac=%.3f startsolid=%d didMove=%d",
							move->command_number, move->tick_count, i, (void*)ent, (void*)setAbsOrigin, (unsigned)rawPacked,
							deltaM.x, deltaM.y,
							origin.x, origin.y, origin.z,
							target.x, target.y, target.z,
							tr.endpos.x, tr.endpos.y, tr.endpos.z,
							tr.fraction, (int)tr.startsolid, (int)didMove);
					}

					// Normal throttled "apply" log (separate from pre).
					if (m_VR->m_Roomscale1To1DebugLog && !ShouldThrottleLog(m_VR->m_Roomscale1To1DebugLastServer, m_VR->m_Roomscale1To1DebugLogHz))
					{
						Game::logMsg("[VR][1to1][server] apply player=%d dM=(%.3f %.3f) origin=(%.2f %.2f %.2f) target=(%.2f %.2f %.2f) end=(%.2f %.2f %.2f) frac=%.3f startsolid=%d",
							i, deltaM.x, deltaM.y, origin.x, origin.y, origin.z, target.x, target.y, target.z, tr.endpos.x, tr.endpos.y, tr.endpos.z, tr.fraction, (int)tr.startsolid);
					}
				}
			}
		}
	}
	return 1;
}

void __fastcall Hooks::dWriteUsercmdDeltaToBuffer(void* ecx, void* edx, int a1, void* buf, int from, int to, bool isnewcommand)
{
	return hkWriteUsercmdDeltaToBuffer.fOriginal(ecx, a1, buf, from, to, isnewcommand);
}

int Hooks::dWriteUsercmd(void* buf, CUserCmd* to, CUserCmd* from)
{
	// VR 未启用：原样走引擎
	if (!m_VR->m_IsVREnabled)
		return hkWriteUsercmd.fOriginal(buf, to, from);

	// 只有（配置开启编码）且（本进程确实在跑服务器钩子＝能解码）且（未强制走非 VR 标准）时才编码
	const bool canEncode = (m_VR->m_EncodeVRUsercmd && !m_VR->m_ForceNonVRServerMovement);

	if (!canEncode)
	{
		// 非 VR 服务器：不要动 tick_count/command_number/viewangles.z/upmove 等
		// 保持标准 CUserCmd，让 dCreateMove 写入的 forwardmove/sidemove 正常生效
		return hkWriteUsercmd.fOriginal(buf, to, from);
	}

	// ======== 以下为原有“编码”逻辑，保持不变，仅包在 canEncode 分支内 ========
	CInput* m_Input = **(CInput***)(m_Game->m_Offsets->g_pppInput.address);
	CVerifiedUserCmd* pVerifiedCommands = *(CVerifiedUserCmd**)((uintptr_t)m_Input + 0xF0);
	CVerifiedUserCmd* pVerified = &pVerifiedCommands[(to->command_number) % 150];

	if (to && m_VR->ShouldSuppressPrimaryFire(to, nullptr))
	{
		to->buttons &= ~(1 << 0); // IN_ATTACK
	}

	// Signal to the server that this CUserCmd has VR info
	to->tick_count *= -1;

	int originalCommandNum = to->command_number;

	QAngle controllerAngles = m_VR->GetRightControllerAbsAngle();
	to->mousedx = (int)(controllerAngles.x * 10.0f); // Strip off 2nd decimal to save bits.
	to->mousedy = (int)(controllerAngles.y * 10.0f);
	int rollEncoding = (((int)controllerAngles.z + 180) / 2 * 10000000);
	to->command_number += rollEncoding;

	if (VectorLength(m_VR->m_RightControllerPose.TrackedDeviceVel) > 1.1f)
	{
		to->command_number *= -1; // Signal to server that melee swing in motion
	}

	Vector controllerPos = m_VR->GetRightControllerAbsPos();
	float xAngleOrig = to->viewangles.x; // 备份

	to->viewangles.z = controllerPos.x;
	to->upmove = controllerPos.y;

	// Space in CUserCmd is tight, so encode viewangle.x and controllerPos.z together.
	// Encoding will overflow if controllerPos.z goes beyond +-21474.8
	int encodedAngle = (int)((xAngleOrig + 360.0f) * 10.0f);
	int encoding = (int)(controllerPos.z * 10.0f) * 10000;
	encoding += (encoding < 0) ? -encodedAngle : encodedAngle;
	to->viewangles.x = (float)encoding;

	// 写入
	hkWriteUsercmd.fOriginal(buf, to, from);

	// 还原本地 CUserCmd
	to->viewangles.x = xAngleOrig;
	to->tick_count *= -1;
	to->viewangles.z = 0.0f;
	to->upmove = 0.0f;
	to->command_number = originalCommandNum;

	// 重算校验，否则多人下枪声会异常
	pVerified->m_cmd = *to;
	pVerified->m_crc = to->GetChecksum();
	return 1;
}

