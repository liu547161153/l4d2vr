void __fastcall Hooks::dEndFrame(void* ecx, void* edx)
{
	return hkEndFrame.fOriginal(ecx);
}

void __fastcall Hooks::dCalcViewModelView(void* ecx, void* edx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
{
	Vector vecNewOrigin = eyePosition;
	QAngle vecNewAngles = eyeAngles;

	if (m_VR->m_IsVREnabled)
	{
		// In queued rendering, the engine may schedule CalcViewModelView work outside the main dRenderView
		// scope (and even outside the eye-render calls). If we only rely on the last viewmodel snapshot
		// produced in dRenderView, the hands/weapons can appear to "trail" or strobe under mat_queue_mode!=0.
		//
		// Old multicore branch behavior: viewmodel pose is derived from a render-thread pose sample.
		// We mirror that idea here by doing a *non-blocking* GetLastPoses() on the render thread when this
		// call happens outside dRenderView, then publishing a fresh viewmodel snapshot.
		Vector lateLatchedOrigin{};
		QAngle lateLatchedAngles{};
		bool haveLateLatch = false;

		// In mat_queue_mode!=0, CalcViewModelView can run on the render thread outside our dRenderView scope.
		// Gate render-frame snapshot usage to the render thread to avoid feeding render-only data into gameplay threads.
		const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
		const bool isRenderThread = (queueMode != 0) && (static_cast<uint32_t>(GetCurrentThreadId()) == m_VR->m_RenderThreadId.load(std::memory_order_relaxed));
		const bool wasInRenderViewScope = VR::t_UseRenderFrameSnapshot;

		if (queueMode != 0 && isRenderThread && !wasInRenderViewScope && m_VR && m_VR->m_System && vr::VRCompositor())
		{
			// Read the main-thread view params snapshot (camera anchor/scale/offsets) using the same seqlock
			// scheme as the render hook.
			struct ViewParams
			{
				Vector cameraAnchor{};
				float rotationOffset = 0.0f;
				float vrScale = 1.0f;
				Vector hmdPosLocalPrev{};
				Vector hmdPosCorrectedPrev{};
				Vector viewmodelPosOffset{};
				QAngle viewmodelAngOffset{};
			};

			ViewParams vp{};
			bool vpOk = false;
			for (int attempt = 0; attempt < 3 && !vpOk; ++attempt)
			{
				const uint32_t s1 = m_VR->m_RenderViewParamsSeq.load(std::memory_order_acquire);
				if (s1 == 0 || (s1 & 1u))
					continue;

				vp.cameraAnchor.x = m_VR->m_RenderCameraAnchorX.load(std::memory_order_relaxed);
				vp.cameraAnchor.y = m_VR->m_RenderCameraAnchorY.load(std::memory_order_relaxed);
				vp.cameraAnchor.z = m_VR->m_RenderCameraAnchorZ.load(std::memory_order_relaxed);
				vp.rotationOffset = m_VR->m_RenderRotationOffset.load(std::memory_order_relaxed);
				vp.vrScale = m_VR->m_RenderVRScale.load(std::memory_order_relaxed);
				vp.hmdPosLocalPrev.x = m_VR->m_RenderHmdPosLocalPrevX.load(std::memory_order_relaxed);
				vp.hmdPosLocalPrev.y = m_VR->m_RenderHmdPosLocalPrevY.load(std::memory_order_relaxed);
				vp.hmdPosLocalPrev.z = m_VR->m_RenderHmdPosLocalPrevZ.load(std::memory_order_relaxed);
				vp.hmdPosCorrectedPrev.x = m_VR->m_RenderHmdPosCorrectedPrevX.load(std::memory_order_relaxed);
				vp.hmdPosCorrectedPrev.y = m_VR->m_RenderHmdPosCorrectedPrevY.load(std::memory_order_relaxed);
				vp.hmdPosCorrectedPrev.z = m_VR->m_RenderHmdPosCorrectedPrevZ.load(std::memory_order_relaxed);
				vp.viewmodelPosOffset.x = m_VR->m_RenderViewmodelPosOffsetX.load(std::memory_order_relaxed);
				vp.viewmodelPosOffset.y = m_VR->m_RenderViewmodelPosOffsetY.load(std::memory_order_relaxed);
				vp.viewmodelPosOffset.z = m_VR->m_RenderViewmodelPosOffsetZ.load(std::memory_order_relaxed);
				vp.viewmodelAngOffset.x = m_VR->m_RenderViewmodelAngOffsetX.load(std::memory_order_relaxed);
				vp.viewmodelAngOffset.y = m_VR->m_RenderViewmodelAngOffsetY.load(std::memory_order_relaxed);
				vp.viewmodelAngOffset.z = m_VR->m_RenderViewmodelAngOffsetZ.load(std::memory_order_relaxed);

				const uint32_t s2 = m_VR->m_RenderViewParamsSeq.load(std::memory_order_acquire);
				if (s1 == s2 && !(s2 & 1u))
					vpOk = true;
			}

			if (vpOk)
			{
				std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> poses{};
				vr::EVRCompositorError poseErr = vr::VRCompositor()->GetLastPoses(poses.data(), vr::k_unMaxTrackedDeviceCount, NULL, 0);
				if (poseErr == vr::VRCompositorError_None && poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
				{
					TrackedDevicePoseData hmdPose{};
					m_VR->GetPoseData(poses[vr::k_unTrackedDeviceIndex_Hmd], hmdPose);
					Vector hmdPosLocal = hmdPose.TrackedDevicePos;
					QAngle hmdAngLocal = hmdPose.TrackedDeviceAng;

					// Reconstruct corrected HMD pos from the last main-thread corrected base.
					Vector hmdPosCorrected = vp.hmdPosCorrectedPrev + (hmdPosLocal - vp.hmdPosLocalPrev);
					VectorPivotXY(hmdPosCorrected, vp.hmdPosCorrectedPrev, vp.rotationOffset);

					hmdAngLocal.y += vp.rotationOffset;
					hmdAngLocal.y -= 360.0f * std::floor((hmdAngLocal.y + 180.0f) / 360.0f);

					// Resolve the right controller from the same pose sample.
					vr::TrackedDeviceIndex_t leftIdx = m_VR->m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
					vr::TrackedDeviceIndex_t rightIdx = m_VR->m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
					if (m_VR->m_LeftHanded)
						std::swap(leftIdx, rightIdx);

					if (rightIdx != vr::k_unTrackedDeviceIndexInvalid && rightIdx < vr::k_unMaxTrackedDeviceCount && poses[rightIdx].bPoseIsValid)
					{
						TrackedDevicePoseData rightPose{};
						m_VR->GetPoseData(poses[rightIdx], rightPose);
						Vector ctrlPosLocal = rightPose.TrackedDevicePos;
						QAngle ctrlAngLocal = rightPose.TrackedDeviceAng;

						Vector hmdToCtrl = ctrlPosLocal - hmdPosLocal;
						Vector ctrlPosCorrected = hmdPosCorrected + hmdToCtrl;
						VectorPivotXY(ctrlPosCorrected, hmdPosCorrected, vp.rotationOffset);

						ctrlAngLocal.y += vp.rotationOffset;
						ctrlAngLocal.y -= 360.0f * std::floor((ctrlAngLocal.y + 180.0f) / 360.0f);

						Vector ctrlF, ctrlR, ctrlU;
						QAngle::AngleVectors(ctrlAngLocal, &ctrlF, &ctrlR, &ctrlU);
						// 45° downward tilt, matches main tracking path.
						ctrlF = VectorRotate(ctrlF, ctrlR, -45.0f);
						ctrlU = VectorRotate(ctrlU, ctrlR, -45.0f);

						Vector rightCtrlPosAbs = vp.cameraAnchor - Vector(0, 0, 64) + (ctrlPosCorrected * vp.vrScale);

						// Viewmodel basis from controller + per-weapon offsets.
						Vector vmForward = ctrlF;
						Vector vmRight = ctrlR;
						Vector vmUp = ctrlU;
						// Yaw offset
						vmForward = VectorRotate(vmForward, vmUp, vp.viewmodelAngOffset.y);
						vmRight = VectorRotate(vmRight, vmUp, vp.viewmodelAngOffset.y);
						// Pitch offset
						vmForward = VectorRotate(vmForward, vmRight, vp.viewmodelAngOffset.x);
						vmUp = VectorRotate(vmUp, vmRight, vp.viewmodelAngOffset.x);
						// Roll offset
						vmRight = VectorRotate(vmRight, vmForward, vp.viewmodelAngOffset.z);
						vmUp = VectorRotate(vmUp, vmForward, vp.viewmodelAngOffset.z);

						lateLatchedOrigin = rightCtrlPosAbs
							- (vmForward * vp.viewmodelPosOffset.x)
							- (vmRight * vp.viewmodelPosOffset.y)
							- (vmUp * vp.viewmodelPosOffset.z);
						QAngle::VectorAngles(vmForward, vmUp, lateLatchedAngles);
						haveLateLatch = true;

						// Publish a fresh viewmodel snapshot so subsequent CalcViewModelView calls (and any other
						// viewmodel users) see the late-latched pose.
						uint32_t vmSeq = m_VR->m_RenderViewmodelSeq.load(std::memory_order_relaxed);
						m_VR->m_RenderViewmodelSeq.store(vmSeq + 1, std::memory_order_release);
						m_VR->m_RenderViewmodelPosX.store(lateLatchedOrigin.x, std::memory_order_relaxed);
						m_VR->m_RenderViewmodelPosY.store(lateLatchedOrigin.y, std::memory_order_relaxed);
						m_VR->m_RenderViewmodelPosZ.store(lateLatchedOrigin.z, std::memory_order_relaxed);
						m_VR->m_RenderViewmodelAngX.store(lateLatchedAngles.x, std::memory_order_relaxed);
						m_VR->m_RenderViewmodelAngY.store(lateLatchedAngles.y, std::memory_order_relaxed);
						m_VR->m_RenderViewmodelAngZ.store(lateLatchedAngles.z, std::memory_order_relaxed);
						m_VR->m_RenderViewmodelSeq.store(vmSeq + 2, std::memory_order_release);
					}
				}
			}
		}

		struct RenderSnapshotTLSGuard
		{
			bool enabled = false;
			explicit RenderSnapshotTLSGuard(bool on) : enabled(on)
			{
				if (enabled)
					VR::t_UseRenderFrameSnapshot = true;
			}
			~RenderSnapshotTLSGuard()
			{
				if (enabled)
					VR::t_UseRenderFrameSnapshot = false;
			}
		} tlsGuard(isRenderThread);

		if (haveLateLatch)
		{
			vecNewOrigin = lateLatchedOrigin;
			vecNewAngles = lateLatchedAngles;
		}
		else
		{
			vecNewOrigin = m_VR->GetRecommendedViewmodelAbsPos();
			vecNewAngles = m_VR->GetRecommendedViewmodelAbsAngle();
		}

		// Multicore (mat_queue_mode!=0): viewmodel can look like it "trails" during head turns if
		// the engine's eyePosition for this call is a different pose/sample than the one used to
		// build vecNewOrigin. Blend a small correction toward the current eyePosition relative to the
		// VR eye-center.
		if (queueMode != 0 && !m_VR->IsThirdPersonCameraActive())
		{
			static thread_local Vector s_LastRenderCenter{};
			static thread_local bool s_HaveLastRenderCenter = false;

			Vector renderCenter{};
			bool haveRenderCenter = false;
			for (int attempt = 0; attempt < 3; ++attempt)
			{
				const uint32_t s1 = m_VR->m_RenderCenterSeq.load(std::memory_order_acquire);
				if (s1 == 0 || (s1 & 1u))
					continue;

				renderCenter.x = m_VR->m_RenderCenterX.load(std::memory_order_relaxed);
				renderCenter.y = m_VR->m_RenderCenterY.load(std::memory_order_relaxed);
				renderCenter.z = m_VR->m_RenderCenterZ.load(std::memory_order_relaxed);

				const uint32_t s2 = m_VR->m_RenderCenterSeq.load(std::memory_order_acquire);
				if (s1 == s2 && !(s2 & 1u))
				{
					haveRenderCenter = true;
					break;
				}
			}

			if (haveRenderCenter)
			{
				s_LastRenderCenter = renderCenter;
				s_HaveLastRenderCenter = true;
				vecNewOrigin += (eyePosition - renderCenter) * 0.5f;
			}
			else if (s_HaveLastRenderCenter)
			{
				// If the seqlock read races the render-thread writer, keep a stable fallback rather than
				// occasionally snapping to the main-thread center (which can look like a strobe).
				vecNewOrigin += (eyePosition - s_LastRenderCenter) * 0.5f;
			}
			else
			{
				// Absolute fallback: old behavior.
				vecNewOrigin += (eyePosition - m_VR->m_HmdPosAbs) * 0.5f;
			}
		}
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
			// Non-VR server：服务器仍以常规射线起点(vecOrigin)为准，所以这里不要改 origin
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

	return hkClientFireTerrorBullets.fOriginal(playerId, vecNewOrigin, vecNewAngles, a4, a5, a6, a7);
}


// === 用下面这整个函数替换你当前的 Hooks::dProcessUsercmds ===
float __fastcall Hooks::dProcessUsercmds(void* ecx, void* edx, edict_t* player,
	void* buf, int numcmds, int totalcmds,
	int dropped_packets, bool ignore, bool paused)
{
	// ★ 进入该钩子，说明本进程正在跑“服务器”逻辑（listen/dedicated）
	MarkServerHookSeen();

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
	int result = hkReadUsercmd.fOriginal(buf, move, from);
	if (!result || !move)
		return result;

	int i = m_Game->m_CurrentUsercmdID;
	const bool hasValidPlayer = m_Game->IsValidPlayerIndex(i);
	if (m_VR->m_EncodeVRUsercmd && move->tick_count < 0) // Signal for VR CUserCmd
	{
		MarkServerHookSeen();
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
	return result;
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
	// Auto-fallback: if server-side VR decoding isn't running in this process (remote server or bad hooks),
	// force Non-VR server compatibility mode.
	const bool inGame = (m_Game && m_Game->m_EngineClient) ? m_Game->m_EngineClient->IsInGame() : false;
	const bool serverHookFresh = inGame && IsServerHookFreshNow();
	if (!serverHookFresh)
		Hooks::s_ServerUnderstandsVR.store(false, std::memory_order_relaxed);
	SyncForceNonVRRemoteOverride(m_VR, serverHookFresh);

	const bool canEncode = (m_VR->m_EncodeVRUsercmd && !m_VR->m_ForceNonVRServerMovement && serverHookFresh);

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
