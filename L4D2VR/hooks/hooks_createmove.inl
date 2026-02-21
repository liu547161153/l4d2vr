bool __fastcall Hooks::dCreateMove(void* ecx, void* edx, float flInputSampleTime, CUserCmd* cmd)
{
	// When returning from spectator/observer back to a live player entity ("rescued"),
	// VR origin can be desynced. Force a recenter/reset once on that transition.
	// Netvars (client):
	// - m_lifeState     @ 0x147
	// - m_iObserverMode @ 0x1450
	static bool s_prevSpectatorLike = false;
	static bool s_prevSpectatorLikeInitialized = false;

	// Non-VR server melee feel state (ForceNonVRServerMovement=true only)
	static int s_nonvrMeleeHoldTicks = 0;
	static bool s_nonvrMeleeArmed = true;
	static QAngle s_nonvrMeleeLockedAngles = { 0,0,0 };
	static std::chrono::steady_clock::time_point s_nonvrMeleeLockUntil{};
	static std::chrono::steady_clock::time_point s_nonvrMeleeCooldownUntil{};
	static bool s_nonvrMeleePending = false;
	static std::chrono::steady_clock::time_point s_nonvrMeleeFireAt{};
	static QAngle s_nonvrMeleePendingAngles = { 0,0,0 };
	static int s_nonvrMeleePendingHoldTicks = 0;
	static bool s_nonvrMeleeHasPrev = false;
	static Vector s_nonvrMeleePrevCtrlPos = { 0,0,0 };
	static Vector s_nonvrMeleePrevHmdPos = { 0,0,0 };

	if (!cmd)
		return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

	if (!cmd->command_number)
		return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

	bool result = hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

	if (m_VR->m_IsVREnabled) {
		// Detect observer -> live transition and recenter once.
	   // In L4D2, the local player entity persists while dead and enters observer modes.
	   // When rescued, m_iObserverMode usually returns to 0 and m_lifeState becomes 0.
		{
			const int lpIdx = (m_Game->m_EngineClient) ? m_Game->m_EngineClient->GetLocalPlayer() : -1;
			C_BasePlayer* lp = (lpIdx > 0) ? (C_BasePlayer*)m_Game->GetClientEntity(lpIdx) : nullptr;
			if (lp)
			{
				const int lifeState = (int)ReadNetvar<uint8_t>(lp, 0x147); // m_lifeState
				const int observerMode = (int)ReadNetvar<int>(lp, 0x1450); // m_iObserverMode
				const bool spectatorLikeNow = (lifeState != 0) || (observerMode != 0);

				if (s_prevSpectatorLikeInitialized)
				{
					// Transition: was spectator-like -> now fully alive (not observer).
					if (s_prevSpectatorLike && !spectatorLikeNow)
					{
						// Note: ResetPosition is expected to exist on VR (used by the input action).
						m_VR->ResetPosition();
					}
				}

				s_prevSpectatorLike = spectatorLikeNow;
				s_prevSpectatorLikeInitialized = true;
			}
			else
			{
				// No local player entity yet; don't latch state.
				s_prevSpectatorLikeInitialized = false;
			}
		}
		const bool treatServerAsNonVR = m_VR->m_ForceNonVRServerMovement;

		// Mouse mode: consume raw mouse deltas to drive body yaw and independent aim pitch.
		// Mouse X -> yaw (m_RotationOffset), Mouse Y -> m_MouseAimPitchOffset.
		// We zero cmd->mousedx/y so Source doesn't also apply them to viewangles.
		if (m_VR->m_MouseModeEnabled)
		{
			// Mouse mode hotkeys (keyboard). Polled here because CreateMove runs at input tick rate.
			auto pollKeyPressed = [&](const std::optional<WORD>& vk, bool& prevDown) -> bool
				{
					if (!vk.has_value())
					{
						prevDown = false;
						return false;
					}
					const SHORT state = GetAsyncKeyState((int)*vk);
					const bool down = (state & 0x8000) != 0;
					const bool pressed = down && !prevDown;
					prevDown = down;
					return pressed;
				};

			if (pollKeyPressed(m_VR->m_MouseModeScopeToggleKey, m_VR->m_MouseModeScopeToggleKeyDownPrev))
				m_VR->ToggleMouseModeScope();
			if (pollKeyPressed(m_VR->m_MouseModeScopeMagnificationKey, m_VR->m_MouseModeScopeMagnificationKeyDownPrev) && m_VR->IsMouseModeScopeActive())
				m_VR->CycleScopeMagnification();

			const float mouseScopeGain = m_VR->GetMouseModeScopeSensitivityScale();

			const int dx = cmd->mousedx;
			const int dy = cmd->mousedy;

			// Mouse-mode yaw (scheme A): delta-drain smoothing.
			// - If MouseModeTurnSmoothing <= 0: legacy behavior (apply yaw directly on CreateMove ticks).
			// - If MouseModeTurnSmoothing  > 0: convert mouse X to a yaw delta and accumulate it.
			//   VR::UpdateTracking will drain/apply it smoothly per-frame.
			if (m_VR->m_MouseModeTurnSmoothing <= 0.0f)
			{
				if (dx != 0)
				{
					m_VR->m_RotationOffset += (-float(dx) * m_VR->m_MouseModeYawSensitivity) * mouseScopeGain;
					// Wrap to [0, 360)
					m_VR->m_RotationOffset -= 360.0f * std::floor(m_VR->m_RotationOffset / 360.0f);
				}
				m_VR->m_MouseModeYawDeltaRemainingDeg = 0.0f;
				m_VR->m_MouseModeYawDeltaInitialized = false;
			}
			else
			{
				if (!m_VR->m_MouseModeYawDeltaInitialized)
				{
					m_VR->m_MouseModeYawDeltaRemainingDeg = 0.0f;
					m_VR->m_MouseModeYawDeltaInitialized = true;
				}
				if (dx != 0)
				{
					const float yawDeltaDeg = (-float(dx) * m_VR->m_MouseModeYawSensitivity) * mouseScopeGain;
					m_VR->m_MouseModeYawDeltaRemainingDeg += yawDeltaDeg;
				}
			}

			if (!m_VR->m_MouseAimInitialized)
			{
				// Initialize current values (used immediately) and targets (smoothed per-frame in VR::UpdateTracking).
				m_VR->m_MouseAimPitchOffset = m_VR->m_HmdAngAbs.x;
				m_VR->m_MouseModeViewPitchOffset = 0.0f;
				m_VR->m_MouseAimInitialized = true;

				m_VR->m_MouseModePitchTarget = m_VR->m_MouseAimPitchOffset;
				m_VR->m_MouseModePitchTargetInitialized = true;
				m_VR->m_MouseModeViewPitchTargetOffset = m_VR->m_MouseModeViewPitchOffset;
				m_VR->m_MouseModeViewPitchTargetOffsetInitialized = true;
			}

			// Ensure targets are initialized even if MouseAimInitialized was set elsewhere.
			if (!m_VR->m_MouseModePitchTargetInitialized)
			{
				m_VR->m_MouseModePitchTarget = m_VR->m_MouseAimPitchOffset;
				m_VR->m_MouseModePitchTargetInitialized = true;
			}
			if (!m_VR->m_MouseModeViewPitchTargetOffsetInitialized)
			{
				m_VR->m_MouseModeViewPitchTargetOffset = m_VR->m_MouseModeViewPitchOffset;
				m_VR->m_MouseModeViewPitchTargetOffsetInitialized = true;
			}

			if (dy != 0)
			{
				const float deltaPitch = float(dy) * m_VR->m_MouseModePitchSensitivity * mouseScopeGain;

				if (m_VR->m_MouseModePitchAffectsView)
				{
					// Update targets only. VR::UpdateTracking will smooth the current values per-frame.
					const float curViewPitch = m_VR->m_MouseModePitchTarget;
					float newViewPitch = curViewPitch + deltaPitch;
					if (newViewPitch > 89.f)  newViewPitch = 89.f;
					if (newViewPitch < -89.f) newViewPitch = -89.f;

					const float appliedDelta = newViewPitch - curViewPitch;
					m_VR->m_MouseModeViewPitchTargetOffset += appliedDelta;
					m_VR->m_MouseModePitchTarget = newViewPitch;

					// If pitch smoothing is disabled, keep legacy immediate behavior.
					if (m_VR->m_MouseModePitchSmoothing <= 0.0f)
					{
						m_VR->m_MouseModeViewPitchOffset = m_VR->m_MouseModeViewPitchTargetOffset;
						m_VR->m_MouseAimPitchOffset = m_VR->m_MouseModePitchTarget;
					}
				}
				else
				{
					// Aim pitch only (camera remains driven purely by HMD).
					m_VR->m_MouseModePitchTarget += deltaPitch;
					if (m_VR->m_MouseModePitchTarget > 89.f)  m_VR->m_MouseModePitchTarget = 89.f;
					if (m_VR->m_MouseModePitchTarget < -89.f) m_VR->m_MouseModePitchTarget = -89.f;

					if (m_VR->m_MouseModePitchSmoothing <= 0.0f)
					{
						m_VR->m_MouseAimPitchOffset = m_VR->m_MouseModePitchTarget;
					}
				}
			}

			cmd->mousedx = 0;
			cmd->mousedy = 0;
		}
		const QAngle originalViewAngles = cmd->viewangles;
		bool hadWalkAxis = false;
		float walkNx = 0.f, walkNy = 0.f;
		float walkMaxSpeed = 0.f;
		float ax = 0.f, ay = 0.f;
		if (m_VR->GetWalkAxis(ax, ay)) {
			// 死区 + 归一化（和平滑转向一致的 0.2 死区）
			const float dz = 0.2f;
			auto norm = [&](float v) {
				float a = fabsf(v);
				if (a <= dz) return 0.f;
				float t = (a - dz) / (1.f - dz);
				return v < 0 ? -t : t;
				};
			const float nx = norm(ax);
			const float ny = norm(ay);

			// 最大移动速度：给一个安全常数；服务器会按自身规则再夹紧
			const float maxSpeed = m_VR->m_AdjustingViewmodel ? 25.f : 250.f;
			hadWalkAxis = true;
			walkNx = nx;
			walkNy = ny;
			walkMaxSpeed = maxSpeed;

			// VR-aware servers: we can apply movement directly in cmd space.
			// Non-VR servers: we will re-base movement later after overriding cmd->viewangles.
			if (!treatServerAsNonVR)
			{
				// Final cmd basis will be HMD yaw (see below). Convert walk input from the chosen
				// movement basis (HMD or controller) into that cmd basis.
				Vector hmdAng = m_VR->GetViewAngle();
				float viewYaw = hmdAng.y;
				if (m_VR->m_MouseModeEnabled)
				{
					viewYaw = m_VR->m_RotationOffset;
					// Wrap to [-180, 180]
					viewYaw -= 360.0f * std::floor((viewYaw + 180.0f) / 360.0f);
				}
				const float moveYaw = m_VR->GetMovementYawDeg();

				QAngle viewYawOnly(0.f, viewYaw, 0.f);
				QAngle moveYawOnly(0.f, moveYaw, 0.f);
				Vector viewForward, viewRight, viewUp;
				Vector moveForward, moveRight, moveUp;
				QAngle::AngleVectors(viewYawOnly, &viewForward, &viewRight, &viewUp);
				QAngle::AngleVectors(moveYawOnly, &moveForward, &moveRight, &moveUp);

				Vector worldMove = moveForward * (ny * maxSpeed) + moveRight * (nx * maxSpeed);
				cmd->forwardmove += DotProduct(worldMove, viewForward);
				cmd->sidemove += DotProduct(worldMove, viewRight);
			}

			// 可选：也把方向按钮位设置一下，增加兼容性
			// IN_FORWARD=1<<3, IN_BACK=1<<4, IN_MOVELEFT=1<<9, IN_MOVERIGHT=1<<10
			if (ny > 0.5f)      cmd->buttons |= (1 << 3);
			else if (ny < -0.5f)cmd->buttons |= (1 << 4);
			if (nx > 0.5f)      cmd->buttons |= (1 << 10);
			else if (nx < -0.5f)cmd->buttons |= (1 << 9);

		}

		// ② ★ 非 VR 服务器：把“右手手柄朝向”塞给服务器用的视角
		if (treatServerAsNonVR) {
			QAngle aim = m_VR->GetRightControllerAbsAngle();
			if (m_VR->m_MouseModeEnabled)
			{
				if (m_VR->m_HasNonVRAimSolution)
					aim = m_VR->m_NonVRAimAngles;
				else
				{
					if (m_VR->m_MouseModeAimFromHmd)
					{
						Vector v = m_VR->GetViewAngle();
						aim = QAngle(v.x, v.y, 0.0f);
					}
					else
					{
						aim = QAngle(m_VR->m_MouseAimPitchOffset, m_VR->m_RotationOffset, 0.0f);
					}
				}
			}
			// ForceNonVRServerMovement: prefer the eye-based solve (what the server will actually trace).
			if (m_VR->m_HasNonVRAimSolution)
				aim = m_VR->m_NonVRAimAngles;
			// 简单夹角，避免异常值
			if (aim.x > 89.f)  aim.x = 89.f;
			if (aim.x < -89.f) aim.x = -89.f;
			// yaw 归一到 [-180,180]
			while (aim.y > 180.f)  aim.y -= 360.f;
			while (aim.y < -180.f) aim.y += 360.f;

			cmd->viewangles.x = aim.x;   // pitch
			cmd->viewangles.y = aim.y;   // yaw
			cmd->viewangles.z = 0.f;     // roll 一般不用


			// Non-VR server melee feel: translate a controller swing into a normal melee attack (IN_ATTACK)
			// This only affects local *input* / presentation. The server still does normal melee resolution.
			if (m_Game->m_IsMeleeWeaponActive && !m_VR->m_AdjustingViewmodel)
			{
				using clock = std::chrono::steady_clock;
				const auto now = clock::now();

				auto lerpAngle = [](float a, float b, float t) -> float {
					float d = b - a;
					while (d > 180.f) d -= 360.f;
					while (d < -180.f) d += 360.f;
					return a + d * t;
					};

				// Aim lock: during lock window, keep viewangles stable so the melee direction doesn't jitter.
				if (now < s_nonvrMeleeLockUntil)
				{
					cmd->viewangles = s_nonvrMeleeLockedAngles;
				}

				// Pending fire: after a short delay, start IN_ATTACK and begin aim lock.
				// This makes melee feel more like a "wind-up -> hit" instead of an instant click.
				if (s_nonvrMeleePending && now >= s_nonvrMeleeFireAt)
				{
					s_nonvrMeleePending = false;

					s_nonvrMeleeLockedAngles = s_nonvrMeleePendingAngles;

					const float lockT = std::max(0.0f, m_VR->m_NonVRMeleeAimLockTime);
					s_nonvrMeleeLockUntil = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(lockT));

					s_nonvrMeleeHoldTicks = std::max(s_nonvrMeleeHoldTicks, s_nonvrMeleePendingHoldTicks);

					cmd->viewangles = s_nonvrMeleeLockedAngles;
				}

				// Hold/queue: keep IN_ATTACK pressed for a few ticks to reduce "dropped" swings.
				if (s_nonvrMeleeHoldTicks > 0)
				{
					cmd->buttons |= (1 << 0); // IN_ATTACK
					--s_nonvrMeleeHoldTicks;
				}

				// Edge trigger + hysteresis: only trigger once per swing, and require speed to fall below a lower
				// threshold before re-arming.
				// Controller velocity in tracking space can include whole-body/HMD motion; remove HMD velocity for cleaner gesture.
				Vector relVel = m_VR->m_RightControllerPose.TrackedDeviceVel - m_VR->m_HmdPose.TrackedDeviceVel;

				// Fallback: derive relative velocity from position delta (some runtimes report near-zero velocity).
				const float dt = (flInputSampleTime > 0.0001f) ? flInputSampleTime : 0.011111f;
				if (s_nonvrMeleeHasPrev)
				{
					Vector dCtrl = m_VR->m_RightControllerPose.TrackedDevicePos - s_nonvrMeleePrevCtrlPos;
					Vector dHmd = m_VR->m_HmdPose.TrackedDevicePos - s_nonvrMeleePrevHmdPos;
					Vector derivedRelVel = (dCtrl - dHmd) * (1.0f / dt);

					if (VectorLength(relVel) < 0.01f && VectorLength(derivedRelVel) > 0.01f)
						relVel = derivedRelVel;
				}
				s_nonvrMeleePrevCtrlPos = m_VR->m_RightControllerPose.TrackedDevicePos;
				s_nonvrMeleePrevHmdPos = m_VR->m_HmdPose.TrackedDevicePos;
				s_nonvrMeleeHasPrev = true;

				// Gesture speed: ignore vertical to reduce false triggers from raising/lowering hands.
				Vector swingVel = relVel;
				swingVel.z = 0.0f;
				const float v = (float)VectorLength(swingVel);
				const QAngle av = m_VR->m_RightControllerPose.TrackedDeviceAngVel;
				const float angV = sqrtf(av.x * av.x + av.y * av.y + av.z * av.z); // deg/s (tracking space)

				const float thr = std::max(0.0f, m_VR->m_NonVRMeleeSwingThreshold);
				const float hyst = std::clamp(m_VR->m_NonVRMeleeHysteresis, 0.1f, 0.95f);
				const float rearmThr = thr * hyst;

				const float angThr = std::max(0.0f, m_VR->m_NonVRMeleeAngVelThreshold);

				const bool above =
					(thr > 0.0f && v > thr) ||
					(angThr > 0.0f && angV > angThr);

				const bool below =
					(thr <= 0.0f || v < rearmThr) &&
					(angThr <= 0.0f || angV < angThr * hyst);

				if (below)
					s_nonvrMeleeArmed = true;

				if (above && s_nonvrMeleeArmed && !s_nonvrMeleePending && now >= s_nonvrMeleeCooldownUntil)
				{
					s_nonvrMeleeArmed = false;

					// Hold IN_ATTACK for a few ticks so we don't miss the server-side melee window.
					const float holdTime = std::max(0.0f, m_VR->m_NonVRMeleeHoldTime);
					int holdTicks = (int)ceilf(holdTime / dt);
					holdTicks = std::clamp(holdTicks, 1, 8);

					// Capture current aim direction, optionally blend toward swing velocity direction.
					QAngle lockedAngles = cmd->viewangles;

					const float blend = std::clamp(m_VR->m_NonVRMeleeSwingDirBlend, 0.0f, 1.0f);
					if (blend > 0.0f)
					{
						Vector velDir = swingVel;
						if (!velDir.IsZero())
						{
							VectorNormalize(velDir);
							QAngle velAng;
							QAngle::VectorAngles(velDir, velAng);
							NormalizeAndClampViewAngles(velAng);

							lockedAngles.x = lerpAngle(lockedAngles.x, velAng.x, blend);
							lockedAngles.y = lerpAngle(lockedAngles.y, velAng.y, blend);
							lockedAngles.z = 0.f;
							NormalizeAndClampViewAngles(lockedAngles);
						}
					}

					// Apply cooldown window immediately so one gesture maps to one swing.
					const float cd = std::max(0.05f, m_VR->m_NonVRMeleeSwingCooldown);
					s_nonvrMeleeCooldownUntil = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(cd));

					// Delay before attacking, then lock angles during the hit window.
					const float delayT = std::max(0.0f, m_VR->m_NonVRMeleeAttackDelay);
					if (delayT > 0.0f)
					{
						s_nonvrMeleePending = true;
						s_nonvrMeleeFireAt = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(delayT));
						s_nonvrMeleePendingAngles = lockedAngles;
						s_nonvrMeleePendingHoldTicks = holdTicks;
					}
					else
					{
						// Fire immediately (legacy behavior)
						s_nonvrMeleeLockedAngles = lockedAngles;

						const float lockT = std::max(0.0f, m_VR->m_NonVRMeleeAimLockTime);
						s_nonvrMeleeLockUntil = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(lockT));

						s_nonvrMeleeHoldTicks = std::max(s_nonvrMeleeHoldTicks, holdTicks);

						cmd->viewangles = s_nonvrMeleeLockedAngles;
						cmd->buttons |= (1 << 0); // IN_ATTACK
					}
				}
			}
			else
			{
				// Leaving melee state: clear any queued swings/locks so we don't "ghost swing" later.
				s_nonvrMeleePending = false;
				s_nonvrMeleePendingHoldTicks = 0;
				s_nonvrMeleeHoldTicks = 0;
				s_nonvrMeleeLockUntil = {};
				s_nonvrMeleeCooldownUntil = {};
				s_nonvrMeleeArmed = true;
				s_nonvrMeleeHasPrev = false;
			}

			// Re-base movement for non-VR servers:
			// - The server interprets forwardmove/sidemove in the basis of cmd->viewangles (aim).
			// - We want movement to follow a separate "movement yaw" (HMD yaw by default; optional controller yaw),
			//   not necessarily the hand aim yaw.
			// So we convert existing movement (built under originalViewAngles) into world space,
			// add VR stick movement in movement-yaw space, then project back into the final cmd basis.
			{
				// Existing movement (keyboard etc.) in world space
				QAngle origYawOnly(0.f, originalViewAngles.y, 0.f);
				Vector origForward, origRight, origUp;
				QAngle::AngleVectors(origYawOnly, &origForward, &origRight, &origUp);
				Vector worldMove = origForward * cmd->forwardmove + origRight * cmd->sidemove;

				// VR stick movement (movement basis = HMD yaw or controller yaw)
				if (hadWalkAxis)
				{
					const float moveYaw = m_VR->GetMovementYawDeg();
					QAngle bodyYawOnly(0.f, moveYaw, 0.f);
					Vector bodyForward, bodyRight, bodyUp;
					QAngle::AngleVectors(bodyYawOnly, &bodyForward, &bodyRight, &bodyUp);
					worldMove += bodyForward * (walkNy * walkMaxSpeed) + bodyRight * (walkNx * walkMaxSpeed);
				}

				// Project into the final cmd basis (after aim/melee lock)
				QAngle cmdYawOnly(0.f, cmd->viewangles.y, 0.f);
				Vector cmdForward, cmdRight, cmdUp;
				QAngle::AngleVectors(cmdYawOnly, &cmdForward, &cmdRight, &cmdUp);
				cmd->forwardmove = DotProduct(worldMove, cmdForward);
				cmd->sidemove = DotProduct(worldMove, cmdRight);
			}

		}
		else {
			// VR-aware servers: ensure cmd->viewangles matches HMD.
			// Otherwise forward/sidemove get interpreted in the wrong basis (push forward -> strafe).
			Vector hmdAng = m_VR->GetViewAngle();
			QAngle view(hmdAng.x, hmdAng.y, hmdAng.z);
			if (m_VR->m_MouseModeEnabled)
			{
				float yaw = m_VR->m_RotationOffset;
				while (yaw > 180.f)  yaw -= 360.f;
				while (yaw < -180.f) yaw += 360.f;
				float pitch = m_VR->m_MouseAimInitialized ? m_VR->m_MouseAimPitchOffset : hmdAng.x;
				view = QAngle(pitch, yaw, 0.f);
			}
			if (view.x > 89.f)  view.x = 89.f;
			if (view.x < -89.f) view.x = -89.f;
			while (view.y > 180.f)  view.y -= 360.f;
			while (view.y < -180.f) view.y += 360.f;
			view.z = 0.f;
			cmd->viewangles = view;
		}
	}

	// Auto-repeat for semi-auto / single-shot guns:
	// Many L4D2 weapons require a fresh IN_ATTACK edge per shot (press/release).
	// When enabled, we convert a held IN_ATTACK into short pulses for non-full-auto guns.
	if (m_VR->m_AutoRepeatSemiAutoFire && m_Game && m_Game->m_EngineClient)
	{
		const int lpIdx2 = m_Game->m_EngineClient->GetLocalPlayer();
		C_BasePlayer* lp2 = (lpIdx2 > 0) ? (C_BasePlayer*)m_Game->GetClientEntity(lpIdx2) : nullptr;
		C_BaseCombatWeapon* wpn = lp2 ? (C_BaseCombatWeapon*)lp2->GetActiveWeapon() : nullptr;
		const char* wpnName = (wpn != nullptr) ? wpn->GetName() : nullptr;
		const char* wpnNet = (wpn != nullptr) ? m_Game->GetNetworkClassName((uintptr_t*)wpn) : nullptr;
		// Mounted guns (minigun / .50 cal etc.) require a continuous IN_ATTACK hold.
		// While mounted, GetActiveWeapon() can still report the carried weapon, so our
		// semi-auto pulse heuristic would incorrectly pulse IN_ATTACK and "stutter" fire.
		const bool usingMountedWeapon = lp2 ? (
			ReadNetvar<bool>(lp2, VR::kUsingMountedGunOffset) ||
			ReadNetvar<bool>(lp2, VR::kUsingMountedWeaponOffset)
			) : false;
		auto startsWith = [](const char* s, const char* prefix) -> bool {
			if (!s || !prefix) return false;
			while (*prefix)
			{
				if (*s++ != *prefix++) return false;
			}
			return true;
			};
		auto contains = [](const char* s, const char* sub) -> bool {
			if (!s || !sub || !*sub) return false;
			return std::strstr(s, sub) != nullptr;
			};
		// If we can't identify the active weapon, be conservative: do NOT pulse.
		// (Some non-gun items can fail the network-name heuristic; pulsing breaks their hold/release behavior.)
		const bool unknownWeapon = (!wpnName && !wpnNet);
		// Heuristic: treat common full-auto weapons as full-auto and do NOT pulse them.
		// Everything else that uses IN_ATTACK will either be unaffected, or benefit from pulsing.
		const bool isFullAuto =
			startsWith(wpnNet, "CWeaponSMG") ||
			startsWith(wpnNet, "CWeaponRifle") ||
			startsWith(wpnNet, "CWeaponAutoshotgun") ||
			startsWith(wpnNet, "CWeaponShotgun_SPAS") ||
			startsWith(wpnNet, "CWeaponM60") ||
			startsWith(wpnNet, "CWeaponRifle_M60") ||
			contains(wpnNet, "Minigun");

		const bool holdingAttack = (cmd->buttons & (1 << 0)) != 0; // IN_ATTACK

		// If the player is currently performing a "use action" (healing, giving ammo/upgrade pack,
		// reviving, etc.), Source expects IN_ATTACK to be held continuously.
		// Pulsing IN_ATTACK will interrupt the action.
		const bool doingUseAction = lp2 ? (ReadNetvar<int>(lp2, 0x1ba8) != 0) : false; // m_iCurrentUseAction

		// Items that require a continuous hold on IN_ATTACK (healing / ammo packs / upgrade packs).
		// If we pulse IN_ATTACK here, these actions get interrupted and become unusable.
		const bool isHoldToUseItem =
			// First-aid kit
			contains(wpnNet, "FirstAid") ||
			contains(wpnNet, "FirstAidKit") ||
			contains(wpnNet, "WeaponFirstAidKit") ||
			contains(wpnNet, "AidKit") ||
			// Defib
			contains(wpnNet, "Defibrillator") ||
			contains(wpnNet, "WeaponDefibrillator") ||
			// Ammo/upgrade packs
			contains(wpnNet, "AmmoPack") ||
			contains(wpnNet, "DT_AmmoPack") ||
			contains(wpnNet, "UpgradePack") ||
			contains(wpnNet, "ItemBaseUpgradePack") ||
			contains(wpnNet, "UpgradePackExplosive") ||
			contains(wpnNet, "UpgradePackIncendiary");
		// Items that are *held* for a moment and released/thrown (or have their own continuous logic).
		// Pulsing IN_ATTACK here feels bad and can cancel/bug the action (chainsaw, throwables).
		const bool isChainsaw =
			(wpnName && (contains(wpnName, "chainsaw") || contains(wpnName, "weapon_chainsaw"))) ||
			contains(wpnNet, "Chainsaw") ||
			contains(wpnNet, "WeaponChainsaw") ||
			startsWith(wpnNet, "CChainsaw");

		const bool isThrowable =
			(wpnName && (
				// Be loose here: engine strings vary across builds/mods ("weapon_molotov" vs "molotov").
				contains(wpnName, "molotov") ||
				(contains(wpnName, "pipe") && contains(wpnName, "bomb")) ||
				contains(wpnName, "vomit") ||
				(contains(wpnName, "grenade") && !contains(wpnName, "grenade_launcher"))
				)) ||
			contains(wpnNet, "Molotov") ||
			contains(wpnNet, "PipeBomb") ||
			contains(wpnNet, "VomitJar") ||
			contains(wpnNet, "BaseCSGrenade") ||
			(contains(wpnNet, "Grenade") && !contains(wpnNet, "GrenadeLauncher"));

		// Only pulse when holding attack and weapon is NOT full-auto, and NOT a hold-to-use item.
		// NOT a hold-to-use item, and NOT during a continuous use action.
		if (holdingAttack && !unknownWeapon && !isFullAuto && !isHoldToUseItem && !isChainsaw && !isThrowable && !doingUseAction && !usingMountedWeapon)
		{
			using namespace std::chrono;
			const float hz = (m_VR->m_AutoRepeatSemiAutoFireHz > 0.0f) ? m_VR->m_AutoRepeatSemiAutoFireHz : 0.0f;
			const auto now = steady_clock::now();

			if (!m_VR->m_AutoRepeatHoldPrev)
			{
				// First tick of hold: fire immediately.
				m_VR->m_AutoRepeatNextPulse = now;
			}

			bool pulseThisTick = (hz > 0.0f) && (now >= m_VR->m_AutoRepeatNextPulse);
			if (pulseThisTick)
			{
				cmd->buttons |= (1 << 0); // IN_ATTACK
				// Single-tick pulse; next tick will be cleared (unless next period elapsed).
				const float clampedHz = std::max(1.0f, std::min(30.0f, hz));
				m_VR->m_AutoRepeatNextPulse = now + duration_cast<steady_clock::duration>(duration<float>(1.0f / clampedHz));
			}
			else
			{
				cmd->buttons &= ~(1 << 0); // IN_ATTACK
			}

			m_VR->m_AutoRepeatHoldPrev = true;
		}
		else
		{
			m_VR->m_AutoRepeatHoldPrev = false;
		}
	}


	// Aim-line friendly-fire guard:
	// Compute teammate hit at input-tick rate (CreateMove) and latch suppression until attack is released.
	C_BasePlayer* ffLocalPlayer = nullptr;
	if (m_VR->m_BlockFireOnFriendlyAimEnabled)
	{
		const int ffIdx = m_Game->m_EngineClient->GetLocalPlayer();
		if (ffIdx > 0)
			ffLocalPlayer = (C_BasePlayer*)m_Game->GetClientEntity(ffIdx);
	}

	// Do NOT suppress IN_ATTACK while the player is performing a continuous "use action"
	// (healing, giving ammo/upgrade pack, reviving, etc.). These actions intentionally target
	// teammates and require holding IN_ATTACK.
	const bool ffDoingUseAction = ffLocalPlayer ? (ReadNetvar<int>(ffLocalPlayer, 0x1ba8) != 0) : false; // m_iCurrentUseAction
	if (!ffDoingUseAction && m_VR->ShouldSuppressPrimaryFire(cmd, ffLocalPlayer))
	{
		cmd->buttons &= ~(1 << 0); // IN_ATTACK
	}

	if (m_VR)
	{
		// Debug: show whether the server-side hook is actually running in this process.
		// For listen-server play, Hooks::dProcessUsercmds should get hit and flip s_ServerUnderstandsVR to true.
		// If this stays false while in-game, then the ProcessUsercmds/ReadUsercmd offsets or hook installation are wrong.
		static std::chrono::steady_clock::time_point s_svHookDbgLast{};
		if (m_VR->m_Roomscale1To1DebugLog && !ShouldThrottleLog(s_svHookDbgLast, 1.0f))
		{
			const bool inGame = (m_Game && m_Game->m_EngineClient) ? m_Game->m_EngineClient->IsInGame() : false;
			Game::logMsg(
				"[VR][1to1][svhook] cmd=%d tick=%d cmdptr=%p inGame=%d serverUnderstandsVR=%d ProcessUsercmds=0x%p ReadUsercmd=0x%p",
				cmd->command_number,
				cmd->tick_count,
				(void*)cmd,
				(int)inGame,
				(int)Hooks::s_ServerUnderstandsVR,
				(void*)(uintptr_t)m_Game->m_Offsets->ProcessUsercmds.address,
				(void*)(uintptr_t)m_Game->m_Offsets->ReadUserCmd.address);
		}
		m_VR->EncodeRoomscale1To1Move(cmd);
	}

	// Debug: verify the packed 1:1 delta survives CreateMove and is still present for networking.
	if (m_VR && cmd && m_VR->m_Roomscale1To1DebugLog && !m_VR->m_ForceNonVRServerMovement && m_VR->m_Roomscale1To1Movement && cmd->weaponselect == 0)
	{
		Vector dm;
		if (VR::DecodeRoomscale1To1Delta(cmd->buttons, dm))
		{
			// Log at a low rate to avoid spam: once every 32 cmds.
			if ((cmd->command_number & 31) == 0)
				Game::logMsg("[VR][1to1][netprep] cmd=%d tick=%d cmdptr=%p buttons=0x%08X dM=(%.3f %.3f)",
					cmd->command_number, cmd->tick_count, (void*)cmd, (unsigned)cmd->buttons, dm.x, dm.y);
		}
	}
	return result;
}

