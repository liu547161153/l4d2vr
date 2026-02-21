ITexture* __fastcall Hooks::dGetRenderTarget(void* ecx, void* edx)
{
	ITexture* result = hkGetRenderTarget.fOriginal(ecx);
	return result;
}

void __fastcall Hooks::dRenderView(void* ecx, void* edx, CViewSetup& setup, CViewSetup& hudViewSetup, int nClearFlags, int whatToDraw)
{
	static EngineThirdPersonCamSmoother s_engineTpCam;

	if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire))
		m_VR->CreateVRTextures();

	// Scope / rear-mirror RTTs may be created lazily (see LazyScopeRearMirrorRTT).
	// Ensure they're available before any offscreen passes try to render into them.
	m_VR->EnsureOpticsRTTTextures();

	IMatRenderContext* rndrContext = m_Game->m_MaterialSystem->GetRenderContext();
	if (!rndrContext)
	{
		m_VR->HandleMissingRenderContext("Hooks::dRenderView");
		return hkRenderView.fOriginal(ecx, setup, hudViewSetup, nClearFlags, whatToDraw);
	}

	// Reset "HUD painted" flag once per VR frame (prevents double HUD captures across eyes).
	m_VR->m_HudPaintedThisFrame.store(false, std::memory_order_release);

	// --- Multicore (queued) rendering stabilization ---
	// When mat_queue_mode!=0, the render thread is decoupled from the main/update thread.
	// If we keep using main-thread-computed m_HmdPosAbs/m_HmdAngAbs/m_RightControllerPosAbs inside rendering,
	// we get tearing/jitter (data races) and head-turn ghosting (poses sampled on the wrong thread).
	//
	// Ported behavior from the old multicore branch: do a render-thread WaitGetPoses(), combine it with a
	// main-thread seqlock snapshot of camera anchor/scale/offsets, then publish a render-thread snapshot
	// that all render-time getters can read consistently during this dRenderView.
	const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
	struct RenderSnapshotTLSGuard
	{
		bool enable = false;
		RenderSnapshotTLSGuard(bool e) : enable(e) { if (enable) VR::t_UseRenderFrameSnapshot = true; }
		~RenderSnapshotTLSGuard() { if (enable) VR::t_UseRenderFrameSnapshot = false; }
	};
	RenderSnapshotTLSGuard __renderTls(queueMode != 0);

	if (queueMode != 0 && m_VR && m_VR->m_System && vr::VRCompositor())
	{
		// Remember which thread is producing render snapshots (used by other render-time hooks).
		m_VR->m_RenderThreadId.store(static_cast<uint32_t>(GetCurrentThreadId()), std::memory_order_relaxed);

		// Track per-render-call setup.origin deltas (tick-rate movement) to reduce model/camera stepping.
		static thread_local Vector s_prevSetupOrigin{};
		static thread_local bool s_prevSetupOriginValid = false;
		Vector pendingOriginDelta{};
		if (s_prevSetupOriginValid)
			pendingOriginDelta = setup.origin - s_prevSetupOrigin;
		s_prevSetupOrigin = setup.origin;
		s_prevSetupOriginValid = true;

		struct ViewParams
		{
			Vector cameraAnchor{};
			float rotationOffset = 0.0f;
			float vrScale = 1.0f;
			float ipdScale = 1.0f;
			float eyeZ = 0.0f;
			float ipd = 0.065f;
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
			vp.ipdScale = m_VR->m_RenderIpdScale.load(std::memory_order_relaxed);
			vp.eyeZ = m_VR->m_RenderEyeZ.load(std::memory_order_relaxed);
			vp.ipd = m_VR->m_RenderIpd.load(std::memory_order_relaxed);
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
			std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> renderPoses{};
			vr::EVRCompositorError err = vr::VRCompositor()->WaitGetPoses(renderPoses.data(), vr::k_unMaxTrackedDeviceCount, NULL, 0);
			if (err == vr::VRCompositorError_None && renderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
			{
				TrackedDevicePoseData hmdPose{};
				m_VR->GetPoseData(renderPoses[vr::k_unTrackedDeviceIndex_Hmd], hmdPose);
				QAngle hmdAngLocal = hmdPose.TrackedDeviceAng;
				Vector hmdPosLocal = hmdPose.TrackedDevicePos;

				// Predict corrected position using the last main-thread corrected frame as a base.
				Vector hmdPosCorrected = vp.hmdPosCorrectedPrev + (hmdPosLocal - vp.hmdPosLocalPrev);
				VectorPivotXY(hmdPosCorrected, vp.hmdPosCorrectedPrev, vp.rotationOffset);

				hmdAngLocal.y += vp.rotationOffset;
				hmdAngLocal.y -= 360.0f * std::floor((hmdAngLocal.y + 180.0f) / 360.0f);

				Vector hmdForward, hmdRight, hmdUp;
				QAngle::AngleVectors(hmdAngLocal, &hmdForward, &hmdRight, &hmdUp);

				// Advance the anchor by the tick-rate origin delta observed on the render thread.
				Vector cameraAnchor = vp.cameraAnchor + pendingOriginDelta;
				Vector hmdPosAbs = cameraAnchor - Vector(0, 0, 64) + (hmdPosCorrected * vp.vrScale);

				const float ipdSu = (vp.ipd * vp.ipdScale * vp.vrScale);
				const float eyeZSu = (vp.eyeZ * vp.vrScale);
				Vector viewCenter = hmdPosAbs + (hmdForward * (-eyeZSu));
				Vector viewLeft = viewCenter + (hmdRight * (-(ipdSu * 0.5f)));
				Vector viewRight = viewCenter + (hmdRight * (+(ipdSu * 0.5f)));

				// Right controller (visual/viewmodel only, no gameplay auto-aim overrides here).
				vr::TrackedDeviceIndex_t leftIdx = m_VR->m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
				vr::TrackedDeviceIndex_t rightIdx = m_VR->m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
				if (m_VR->m_LeftHanded)
					std::swap(leftIdx, rightIdx);

				Vector rightCtrlPosAbs = m_VR->m_RightControllerPosAbs;
				QAngle rightCtrlAngAbs = m_VR->m_RightControllerAngAbs;
				Vector vmForward = m_VR->m_ViewmodelForward;
				Vector vmRight = m_VR->m_ViewmodelRight;
				Vector vmUp = m_VR->m_ViewmodelUp;
				Vector vmPosAbs = m_VR->GetRecommendedViewmodelAbsPos();
				QAngle vmAngAbs = m_VR->GetRecommendedViewmodelAbsAngle();

				if (rightIdx != vr::k_unTrackedDeviceIndexInvalid && rightIdx < vr::k_unMaxTrackedDeviceCount && renderPoses[rightIdx].bPoseIsValid)
				{
					TrackedDevicePoseData rightPose{};
					m_VR->GetPoseData(renderPoses[rightIdx], rightPose);
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
					ctrlF = VectorRotate(ctrlF, ctrlR, -45.0);
					ctrlU = VectorRotate(ctrlU, ctrlR, -45.0);

					rightCtrlPosAbs = cameraAnchor - Vector(0, 0, 64) + (ctrlPosCorrected * vp.vrScale);
					QAngle::VectorAngles(ctrlF, ctrlU, rightCtrlAngAbs);

					// Viewmodel basis from controller + per-weapon offsets.
					vmForward = ctrlF;
					vmRight = ctrlR;
					vmUp = ctrlU;
					// Yaw offset
					vmForward = VectorRotate(vmForward, vmUp, vp.viewmodelAngOffset.y);
					vmRight = VectorRotate(vmRight, vmUp, vp.viewmodelAngOffset.y);
					// Pitch offset
					vmForward = VectorRotate(vmForward, vmRight, vp.viewmodelAngOffset.x);
					vmUp = VectorRotate(vmUp, vmRight, vp.viewmodelAngOffset.x);
					// Roll offset
					vmRight = VectorRotate(vmRight, vmForward, vp.viewmodelAngOffset.z);
					vmUp = VectorRotate(vmUp, vmForward, vp.viewmodelAngOffset.z);

					vmPosAbs = rightCtrlPosAbs
						- (vmForward * vp.viewmodelPosOffset.x)
						- (vmRight * vp.viewmodelPosOffset.y)
						- (vmUp * vp.viewmodelPosOffset.z);
					QAngle::VectorAngles(vmForward, vmUp, vmAngAbs);
				}

				// Publish render-frame snapshot with a seqlock.
				uint32_t seq = m_VR->m_RenderFrameSeq.load(std::memory_order_relaxed);
				m_VR->m_RenderFrameSeq.store(seq + 1, std::memory_order_release);

				m_VR->m_RenderViewAngX.store(hmdAngLocal.x, std::memory_order_relaxed);
				m_VR->m_RenderViewAngY.store(hmdAngLocal.y, std::memory_order_relaxed);
				m_VR->m_RenderViewAngZ.store(hmdAngLocal.z, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginLeftX.store(viewLeft.x, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginLeftY.store(viewLeft.y, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginLeftZ.store(viewLeft.z, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginRightX.store(viewRight.x, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginRightY.store(viewRight.y, std::memory_order_relaxed);
				m_VR->m_RenderViewOriginRightZ.store(viewRight.z, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerPosAbsX.store(rightCtrlPosAbs.x, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerPosAbsY.store(rightCtrlPosAbs.y, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerPosAbsZ.store(rightCtrlPosAbs.z, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerAngAbsX.store(rightCtrlAngAbs.x, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerAngAbsY.store(rightCtrlAngAbs.y, std::memory_order_relaxed);
				m_VR->m_RenderRightControllerAngAbsZ.store(rightCtrlAngAbs.z, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelPosX.store(vmPosAbs.x, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelPosY.store(vmPosAbs.y, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelPosZ.store(vmPosAbs.z, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelAngX.store(vmAngAbs.x, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelAngY.store(vmAngAbs.y, std::memory_order_relaxed);
				m_VR->m_RenderRecommendedViewmodelAngZ.store(vmAngAbs.z, std::memory_order_relaxed);

				m_VR->m_RenderFrameSeq.store(seq + 2, std::memory_order_release);
			}
		}
	}

	// ------------------------------
	// Third-person camera fix:
	// If engine is in third-person, setup.origin is a shoulder camera,
	// but our VR hook normally overwrites it with HMD first-person.
	// That makes the local player model show up "in your face" and looks like ghosting/double image.
	// ------------------------------
	int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
	C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
	const bool hasViewEntityOverride = (localPlayer && HandleValid(ReadNetvar<int>(localPlayer, 0x142c))); // m_hViewEntity

	Vector eyeOrigin = setup.origin;
	if (localPlayer)
		eyeOrigin = localPlayer->EyePosition();

	// Heuristic: in true third-person, the engine camera origin is noticeably away from eye position.
	// IMPORTANT: stairs/step-smoothing can create large Z deltas between setup.origin and EyePosition().
	// So prefer XY distance for "real" third-person detection.
	Vector camDelta = (setup.origin - eyeOrigin);
	const float camDist3D = camDelta.Length();
	const float camDz = camDelta.z;
	camDelta.z = 0.0f;
	const float camDistXY = camDelta.Length();

	// - XY threshold must be low enough to catch "near" third-person modes,
	//   but still high enough to ignore stairs/step-smoothing Z deltas.
	// - 3D is a fallback for edge cases.
	constexpr float kThirdPersonXY = 20.0f;
	constexpr float kThirdPerson3D = 90.0f;
	// Revive (being helped / helping someone) can temporarily offset setup.origin away from EyePosition()
	// even though we want to keep first-person VR rendering. Treat that as NOT third-person.
	bool playerReviving = false;
	if (localPlayer)
	{
		const int reviveOwner = ReadNetvar<int>(localPlayer, 0x1f88);   // m_reviveOwner
		const int reviveTarget = ReadNetvar<int>(localPlayer, 0x1f8c);  // m_reviveTarget
		playerReviving = HandleValid(reviveOwner) || HandleValid(reviveTarget);
	}

	bool engineThirdPersonNow = (localPlayer && (camDistXY > kThirdPersonXY || camDist3D > kThirdPerson3D));
	// Revive camera can temporarily offset setup.origin away from EyePosition() even though we want first-person VR rendering.
	if (playerReviving)
		engineThirdPersonNow = false;
	// Mounted gun (.50cal/minigun): always force first-person rendering.
	// The engine often runs these in third-person/shoulder cam, which feels wrong in VR
	// and can poison our tracking anchors.
	const bool usingMountedGun = m_VR->IsUsingMountedGun(localPlayer);
	if (usingMountedGun)
		engineThirdPersonNow = false;
	const bool customWalkThirdPersonNow = m_VR->m_ThirdPersonRenderOnCustomWalk && m_VR->m_CustomWalkHeld;

	// Some scripts/mods use point_viewcontrol_survivor via m_hViewEntity.
	// There are two very different cases:
	//  1) Real cinematic/cutscene camera control (we MUST keep the engine camera, or you lose camera choreography).
	//  2) Transitional revive/incap camera weirdness (can look like a "fake" third-person and will latch our hold frames).
	//
	// Heuristic: only ignore view-entity overrides during unstable revive/incap windows.
	// Otherwise, treat it as a real external camera and let the engine camera drive rendering.
	bool playerIncap = false;
	if (localPlayer)
		playerIncap = (ReadNetvar<int>(localPlayer, 0x1ea9) != 0); // m_isIncapacitated
	if (hasViewEntityOverride && !customWalkThirdPersonNow)
	{
		const bool unstableViewEntity = playerReviving || playerIncap;
		if (unstableViewEntity)
			engineThirdPersonNow = false;
	}
	QAngle rawSetupAngles(setup.angles.x, setup.angles.y, setup.angles.z);
	// Capture and optionally smooth the engine camera (tick-rate 3P -> HMD-rate continuous).
	if (engineThirdPersonNow)
		s_engineTpCam.PushRaw(setup.origin, rawSetupAngles);
	else
		s_engineTpCam.Reset();

	Vector engineCamOrigin = setup.origin;
	QAngle engineCamAngles = rawSetupAngles;
	if (s_engineTpCam.ShouldSmooth())
		s_engineTpCam.GetSmoothed(engineCamOrigin, engineCamAngles);

	// Detect third-person by comparing rendered camera origin to the real eye origin
	// Use a small threshold + hysteresis to avoid flicker.
	// Also expose a simple "player is pinned/controlled" flag so VR can disable jittery aim line.
	m_VR->m_PlayerControlledBySI = IsPlayerControlledBySI(localPlayer);
	ThirdPersonStateDebug tpStateDbg;
	const bool rawStateWantsThirdPerson = ShouldForceThirdPersonByState(localPlayer, m_Game->m_ClientEntityList, localPlayer, &tpStateDbg);
	const bool rawStateObserver = (tpStateDbg.observerMode != 0) && (tpStateDbg.dead || HandleValid(tpStateDbg.observerTarget));
	bool stateWantsThirdPerson = rawStateWantsThirdPerson;
	bool stateObserver = rawStateObserver;

	// Map-load / reconnect stabilization:
	// Right after joining/changing maps, the client can briefly report observer-like netvars even though
	// the player is alive. If we treat that as true observer mode, we latch third-person for ~1-2s then snap back.
	// Suppress *observer-driven* third-person in that window; other state reasons (ledge/tongue/pinned/self-heal) still apply.
	const bool inMapLoadCooldown = m_VR->IsThirdPersonMapLoadCooldownActive();
	if (inMapLoadCooldown && tpStateDbg.lifeState == 0 && rawStateObserver && !tpStateDbg.dead)
	{
		stateObserver = false;
		stateWantsThirdPerson = tpStateDbg.dead || tpStateDbg.ledge || tpStateDbg.tongue || tpStateDbg.pinned || tpStateDbg.selfMedkit;
	}
	// Observer render lock based on m_iObserverMode (prevents 1P/3P oscillation -> flash while spectating).
	enum class ObserverRenderPref : int { None = 0, InEye = 1, Third = 2 };
	static ObserverRenderPref s_obsPref = ObserverRenderPref::None;
	static ObserverRenderPref s_obsCandidate = ObserverRenderPref::None;
	static int s_obsCandidateFrames = 0;
	constexpr int kObserverPrefLatchFrames = 6;

	auto PrefFromObserverMode = [](int obsMode) -> ObserverRenderPref
		{
			if (obsMode == 4) return ObserverRenderPref::InEye;
			if (obsMode == 5 || obsMode == 6) return ObserverRenderPref::Third;
			return ObserverRenderPref::None;
		};

	if (!stateObserver)
	{
		s_obsPref = ObserverRenderPref::None;
		s_obsCandidate = ObserverRenderPref::None;
		s_obsCandidateFrames = 0;
	}
	else
	{
		const ObserverRenderPref desired = PrefFromObserverMode(tpStateDbg.observerMode);

		// Conservative default on entry: render 3P until we see a stable 4/5/6.
		if (s_obsPref == ObserverRenderPref::None && desired == ObserverRenderPref::None)
			s_obsPref = ObserverRenderPref::Third;

		if (desired != ObserverRenderPref::None && desired != s_obsPref)
		{
			if (desired != s_obsCandidate)
			{
				s_obsCandidate = desired;
				s_obsCandidateFrames = 1;
			}
			else if (++s_obsCandidateFrames >= kObserverPrefLatchFrames)
			{
				s_obsPref = s_obsCandidate;
				s_obsCandidate = ObserverRenderPref::None;
				s_obsCandidateFrames = 0;
			}
		}
		else
		{
			s_obsCandidate = ObserverRenderPref::None;
			s_obsCandidateFrames = 0;
		}
	}

	const bool observerForceInEye = stateObserver && (s_obsPref == ObserverRenderPref::InEye);
	const bool observerForceThird = stateObserver && (s_obsPref == ObserverRenderPref::Third);

	// If observer mode requests in-eye, don't treat "observer" as a reason to force third-person.
	if (observerForceInEye)
		stateWantsThirdPerson = tpStateDbg.dead || tpStateDbg.ledge || tpStateDbg.tongue || tpStateDbg.pinned || tpStateDbg.selfMedkit;

	const bool stateIsDeadOrObserver = tpStateDbg.dead || (stateObserver && !observerForceInEye);
	// Death transition anti-flicker: the engine can oscillate between 1P/3P while dying -> observer.
	// To avoid nauseating camera swaps in VR, lock to first-person for a short window right after death is detected.
	static std::chrono::steady_clock::time_point s_deathFpLockUntil{};
	static bool s_prevDead = false;
	const bool nowDead = tpStateDbg.dead;
	const bool nowObserver = stateObserver;
	const auto nowTp = std::chrono::steady_clock::now();
	// Revive recovery: getting up from incapacitated can leave the engine in a transient camera mode
	// (observer/view-entity/shoulder cam). If we latch third-person during that window, it may never decay.
	// Detect the incap -> standing edge and (optionally) force a short first-person recovery window.
	// NOTE: if the player was already in third-person before getting incapacitated, don't force first-person
	// after the revive; restore third-person immediately to match player expectation.
	static bool s_prevIncap = false;
	static bool s_incapEnteredThirdPerson = false;
	static bool s_reviveForceFirstPerson = true;
	static std::chrono::steady_clock::time_point s_reviveRecoverUntil{};
	const bool nowIncap = tpStateDbg.incap;
	if (!s_prevIncap && nowIncap)
	{
		// Capture the *previous* third-person intent/state so we can restore it after revive.
		// m_IsThirdPersonCamera / HoldFrames reflect the last frame's decision.
		const bool prevHadThirdPerson = m_VR->m_IsThirdPersonCamera || (m_VR->m_ThirdPersonHoldFrames > 0);
		s_incapEnteredThirdPerson = prevHadThirdPerson || customWalkThirdPersonNow || engineThirdPersonNow;
	}
	if (s_prevIncap && !nowIncap)
	{
		s_reviveForceFirstPerson = !s_incapEnteredThirdPerson;
		// Keep this window short; it's only meant to let transient camera state settle.
		// If we entered incap in third-person, don't force first-person after revive.
		if (s_reviveForceFirstPerson)
			s_reviveRecoverUntil = nowTp + std::chrono::milliseconds(400);
		else
			s_reviveRecoverUntil = std::chrono::steady_clock::time_point{};
		m_VR->m_ThirdPersonHoldFrames = 0;
		m_VR->m_IsThirdPersonCamera = false;
		s_engineTpCam.Reset();
		m_VR->ResetPosition();
	}
	s_prevIncap = nowIncap;
	const bool inReviveRecover = (s_reviveRecoverUntil.time_since_epoch().count() != 0) && (nowTp < s_reviveRecoverUntil);

	if (nowDead && !s_prevDead)
		s_deathFpLockUntil = nowTp + std::chrono::seconds(10);
	s_prevDead = nowDead;
	const bool forceFirstPersonAfterDeath =
		(!nowObserver && (s_deathFpLockUntil.time_since_epoch().count() != 0) && (nowTp < s_deathFpLockUntil));
	constexpr int kEngineThirdPersonHoldFrames = 2;
	constexpr int kStateThirdPersonHoldFrames = 2;
	constexpr int kSelfMedkitHoldFrames = 6; // stronger lock while self-healing
	constexpr int kDeadOrObserverHoldFrames = 30; // avoid flicker during death/observer transitions
	const bool hadThirdPerson = m_VR->m_IsThirdPersonCamera || (m_VR->m_ThirdPersonHoldFrames > 0);
	// If dead, allow immediately (no dependency on engineThirdPersonNow/hysteresis).
	bool allowStateThirdPerson = stateWantsThirdPerson && (stateIsDeadOrObserver || tpStateDbg.selfMedkit || engineThirdPersonNow || customWalkThirdPersonNow || hadThirdPerson);
	if (forceFirstPersonAfterDeath)
		allowStateThirdPerson = false;
	// 先按“状态”锁定（优先级最高）
	if (allowStateThirdPerson)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kStateThirdPersonHoldFrames);
	if (tpStateDbg.selfMedkit)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kSelfMedkitHoldFrames);
	if (stateIsDeadOrObserver)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kDeadOrObserverHoldFrames);
	// 再按“+walk”辅助锁定（滑铲模组会用 +walk 切换第三人称摄像机，但摄像机偏移可能很小，我们的几何检测不一定能抓到）
	constexpr int kWalkThirdPersonHoldFrames = 3;
	if (customWalkThirdPersonNow)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kWalkThirdPersonHoldFrames);

	// 再按“引擎第三人称”做短缓冲，但不要覆盖掉状态锁定
	if (engineThirdPersonNow)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kEngineThirdPersonHoldFrames);
	else if (localPlayer && !allowStateThirdPerson && !tpStateDbg.selfMedkit && m_VR->m_ThirdPersonHoldFrames > 0)
		m_VR->m_ThirdPersonHoldFrames--;

	bool renderThirdPerson = customWalkThirdPersonNow || engineThirdPersonNow || tpStateDbg.selfMedkit || (m_VR->m_ThirdPersonHoldFrames > 0);
	if (usingMountedGun)
	{
		// Hard override: mounted guns should never be third-person in VR.
		renderThirdPerson = false;
		m_VR->m_ThirdPersonHoldFrames = 0;
	}
	// Post-revive recovery override: force first-person for a short window after getting up.
	if (inReviveRecover && s_reviveForceFirstPerson && !usingMountedGun && !customWalkThirdPersonNow && !stateIsDeadOrObserver && !tpStateDbg.selfMedkit)
	{
		renderThirdPerson = false;
		m_VR->m_ThirdPersonHoldFrames = 0;
		s_engineTpCam.Reset();
	}
	// Death anti-flicker override (must run after all other third-person decisions).
	if (forceFirstPersonAfterDeath)
	{
		renderThirdPerson = false;
		m_VR->m_ThirdPersonHoldFrames = 0;
	}
	// Observer render lock final override:
	//  - obsMode 4   => force in-eye (1P) rendering.
	//  - obsMode 5/6 => force third-person rendering.
	// Runs late so nothing else can fight it and cause flicker.
	if (observerForceInEye)
	{
		renderThirdPerson = false;
		m_VR->m_ThirdPersonHoldFrames = 0;
		s_engineTpCam.Reset();
	}
	else if (observerForceThird)
	{
		renderThirdPerson = true;
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kDeadOrObserverHoldFrames);
	}
	// Debug: log third-person state + relevant netvars (throttled)
	{
		static std::chrono::steady_clock::time_point s_lastTpDbg{};
		static bool s_prevEngineTp = false;
		static bool s_prevStateTp = false;
		static bool s_prevRenderTp = false;
		static int s_prevHold = -999;

		const auto now = std::chrono::steady_clock::now();
		const bool changed = (engineThirdPersonNow != s_prevEngineTp) || (stateWantsThirdPerson != s_prevStateTp) ||
			(renderThirdPerson != s_prevRenderTp) || (m_VR->m_ThirdPersonHoldFrames != s_prevHold);
		const bool timeUp = (s_lastTpDbg.time_since_epoch().count() == 0) ||
			(std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastTpDbg).count() >= 1000);

		s_prevEngineTp = engineThirdPersonNow;
		s_prevStateTp = stateWantsThirdPerson;
		s_prevRenderTp = renderThirdPerson;
		s_prevHold = m_VR->m_ThirdPersonHoldFrames;
	}
	// Expose third-person camera to VR helpers (aim line, overlays, etc.)
	m_VR->m_IsThirdPersonCamera = renderThirdPerson;
	// ------------------------------
	// Third-person shake damping:
	// Tank stomps / explosions can apply strong screen-shake to the engine camera.
	// In VR third-person, that feels *way* worse than on a flat screen, so we apply an
	// extra low-pass filter to the engine camera origin/angles while actively rendering 3P.
	//
	// Notes:
	//  - Disabled for death/observer and view-entity cameras (cutscenes), to preserve intended choreography.
	//  - Controlled by config: ThirdPersonCameraSmoothing (0..0.99). Higher = smoother (less shake).
	// ------------------------------
	static bool s_tpShakeInit = false;
	static Vector s_tpShakeOrigin{ 0,0,0 };
	static QAngle s_tpShakeAngles{ 0,0,0 };

	auto ResetTpShake = [&]()
		{
			s_tpShakeInit = false;
			s_tpShakeOrigin = { 0,0,0 };
			s_tpShakeAngles = { 0,0,0 };
		};

	const float tpShakeSmooth = std::clamp(m_VR->m_ThirdPersonCameraSmoothing, 0.0f, 0.99f);

	if (!renderThirdPerson || stateIsDeadOrObserver || hasViewEntityOverride || tpShakeSmooth <= 0.0f)
	{
		ResetTpShake();
	}
	else
	{
		const float lerpFactor = 1.0f - tpShakeSmooth;

		if (!s_tpShakeInit)
		{
			s_tpShakeOrigin = engineCamOrigin;
			s_tpShakeAngles = engineCamAngles;
			s_tpShakeInit = true;
		}
		else
		{
			// Smooth origin (component-wise)
			s_tpShakeOrigin.x += (engineCamOrigin.x - s_tpShakeOrigin.x) * lerpFactor;
			s_tpShakeOrigin.y += (engineCamOrigin.y - s_tpShakeOrigin.y) * lerpFactor;
			s_tpShakeOrigin.z += (engineCamOrigin.z - s_tpShakeOrigin.z) * lerpFactor;

			// Smooth angles with wrap-around
			auto smoothAngle = [&](float target, float& cur)
				{
					float diff = target - cur;
					diff -= 360.0f * std::floor((diff + 180.0f) / 360.0f);
					cur += diff * lerpFactor;
				};

			smoothAngle(engineCamAngles.x, s_tpShakeAngles.x);
			smoothAngle(engineCamAngles.y, s_tpShakeAngles.y);
			smoothAngle(engineCamAngles.z, s_tpShakeAngles.z);
		}

		engineCamOrigin = s_tpShakeOrigin;
		engineCamAngles = s_tpShakeAngles;
	}

	// Always capture the view the engine is rendering this frame.
	// In true third-person, setup.origin is the shoulder camera; in first-person it matches the eye.
	m_VR->m_ThirdPersonViewOrigin = engineCamOrigin;
	m_VR->m_ThirdPersonViewAngles.Init(engineCamAngles.x, engineCamAngles.y, engineCamAngles.z);
	CViewSetup leftEyeView = setup;
	CViewSetup rightEyeView = setup;

	// Left eye CViewSetup
	leftEyeView.x = 0;
	leftEyeView.width = m_VR->m_RenderWidth;
	leftEyeView.height = m_VR->m_RenderHeight;
	leftEyeView.fov = m_VR->m_Fov;
	leftEyeView.y = 0;
	leftEyeView.m_nUnscaledY = 0;
	leftEyeView.fovViewmodel = m_VR->m_Fov;
	leftEyeView.m_flAspectRatio = m_VR->m_Aspect;
	leftEyeView.zNear = 6;
	leftEyeView.zNearViewmodel = 6;
	// Keep VR tracking base tied to the real player eye, NOT the shoulder camera.
	// IMPORTANT (VR compatibility):
	// Some VScript mods (e.g. slide mods) temporarily enable point_viewcontrol_survivor via
	// CBasePlayer::m_hViewEntity. In that case, setup.origin can jump to an attachment-driven
	// camera that does NOT match the HMD eye origin and can appear "too high" in VR.
	// So: only borrow setup.origin.z when the player has no active view-entity override.
	m_VR->m_SetupOrigin = eyeOrigin;
	if (!renderThirdPerson && !hasViewEntityOverride && !usingMountedGun)
		m_VR->m_SetupOrigin.z = setup.origin.z;
	m_VR->m_SetupAngles.Init(setup.angles.x, setup.angles.y, setup.angles.z);

	Vector leftOrigin, rightOrigin;
	Vector viewAngles = m_VR->GetViewAngle();

	// Recenter the VR anchors once per threshold when yaw turns left/right a lot.
	// Requirement: if yaw turns beyond 60° (left or right), do a one-shot ResetPosition.
	// Note: this now applies in both first-person and third-person rendering.
	{
		static bool s_yawResetInit = false;
		static float s_yawResetBase = 0.0f;
		const float bodyYaw = m_VR->m_RotationOffset; // wrapped to [0, 360)

		if (!s_yawResetInit)
		{
			s_yawResetBase = bodyYaw;
			s_yawResetInit = true;
		}
		else
		{
			float diff = bodyYaw - s_yawResetBase;
			// Normalize to [-180, 180] to handle wrap-around.
			diff -= 360.0f * std::floor((diff + 180.0f) / 360.0f);
			if (std::fabs(diff) >= 60.0f)
			{
				m_VR->ResetPosition();
				s_yawResetBase = bodyYaw;
			}
		}
	}

	if (renderThirdPerson)
	{
		// Render from the engine-provided third-person camera (setup.origin),
		// but aim the camera with the HMD so head look still works in third-person.
		QAngle camAng(viewAngles.x, viewAngles.y, viewAngles.z);
		if (m_VR->m_HmdForward.IsZero())
			camAng = engineCamAngles;

		Vector fwd, right, up;
		QAngle::AngleVectors(camAng, &fwd, &right, &up);

		const float ipd = (m_VR->m_Ipd * m_VR->m_IpdScale * m_VR->m_VRScale);
		const float eyeZ = (m_VR->m_EyeZ * m_VR->m_VRScale);

		// Treat camera origin as "head center", apply SteamVR eye-to-head offsets.
		// If we're forcing third-person (state) while the engine is in first-person, use HMD position to synthesize a stable 3p camera.
		// IMPORTANT:
		// engineThirdPersonNow can flicker during pinned/incap/use actions.
		// If stateWantsThirdPerson is true, always synthesize from HMD to avoid camera "jumping"
		// between setup.origin and HmdPosAbs.
		Vector baseCenter;
		if (stateWantsThirdPerson)
		{
			// Dead/observer camera must follow engine view, not HMD position.
			baseCenter = stateIsDeadOrObserver ? engineCamOrigin : m_VR->m_HmdPosAbs;
		}
		else
		{
			baseCenter = (engineThirdPersonNow || customWalkThirdPersonNow) ? engineCamOrigin : m_VR->m_HmdPosAbs;
		}
		Vector camCenter = baseCenter + (fwd * (-eyeZ));
		if (m_VR->m_ThirdPersonVRCameraOffset > 0.0f)
			camCenter = camCenter + (fwd * (-m_VR->m_ThirdPersonVRCameraOffset));
		// Expose the actual VR render camera center used for third-person this frame.
		// This includes HMD-aim yaw and any VR camera offsets, and is used to keep aim line and overlays aligned.
		m_VR->m_ThirdPersonRenderCenter = camCenter;
		leftOrigin = camCenter + (right * (-(ipd * 0.5f)));
		rightOrigin = camCenter + (right * (+(ipd * 0.5f)));
	}
	else if (observerForceInEye)
	{
		// Observer in-eye: use engine camera origin (target eye) but apply stereo IPD; aim with HMD.
		QAngle camAng(viewAngles.x, viewAngles.y, viewAngles.z);
		if (m_VR->m_HmdForward.IsZero())
			camAng = engineCamAngles;

		Vector fwd, right, up;
		QAngle::AngleVectors(camAng, &fwd, &right, &up);

		const float ipd = (m_VR->m_Ipd * m_VR->m_IpdScale * m_VR->m_VRScale);

		// engineCamOrigin is already an eye origin; don't apply EyeZ again.
		m_VR->m_ThirdPersonRenderCenter = engineCamOrigin;
		leftOrigin = engineCamOrigin + (right * (-(ipd * 0.5f)));
		rightOrigin = engineCamOrigin + (right * (+(ipd * 0.5f)));
	}
	else
	{
		// Normal VR first-person
		leftOrigin = m_VR->GetViewOriginLeft();
		rightOrigin = m_VR->GetViewOriginRight();
		// Keep this sane even in 1P (unused there, but prevents stale deltas if 3P toggles).
		m_VR->m_ThirdPersonRenderCenter = m_VR->m_SetupOrigin;
	}

	leftEyeView.origin = leftOrigin;
	leftEyeView.angles = viewAngles;

	// --- IMPORTANT: avoid "dragging/ghosting" when turning with thumbstick ---
	// Do NOT permanently overwrite engine viewangles. Only set them during our stereo renders,
	// then restore, so the engine's view history/interp isn't corrupted.
	QAngle prevEngineAngles;
	m_Game->m_EngineClient->GetViewAngles(prevEngineAngles);

	QAngle renderAngles(viewAngles.x, viewAngles.y, viewAngles.z);
	m_Game->m_EngineClient->SetViewAngles(renderAngles);

	// Align HUD view to the same origin/angles; otherwise you can get a second layer that
	// appears to "follow the controller / stick" (classic double-image artifact).
	CViewSetup hudLeft = hudViewSetup;
	hudLeft.origin = leftEyeView.origin;
	hudLeft.angles = viewAngles;
	rndrContext->SetRenderTarget(m_VR->m_LeftEyeTexture);
	hkRenderView.fOriginal(ecx, leftEyeView, hudLeft, nClearFlags, whatToDraw);
	m_PushedHud = false;

	// Right eye CViewSetup
	rightEyeView.x = 0;
	rightEyeView.width = m_VR->m_RenderWidth;
	rightEyeView.height = m_VR->m_RenderHeight;
	rightEyeView.fov = m_VR->m_Fov;
	rightEyeView.y = 0;
	rightEyeView.m_nUnscaledY = 0;
	rightEyeView.fovViewmodel = m_VR->m_Fov;
	rightEyeView.m_flAspectRatio = m_VR->m_Aspect;
	rightEyeView.zNear = 6;
	rightEyeView.zNearViewmodel = 6;
	rightEyeView.origin = rightOrigin;
	rightEyeView.angles = viewAngles;
	CViewSetup hudRight = hudViewSetup;
	hudRight.origin = rightEyeView.origin;
	hudRight.angles = viewAngles;

	rndrContext->SetRenderTarget(m_VR->m_RightEyeTexture);
	hkRenderView.fOriginal(ecx, rightEyeView, hudRight, nClearFlags, whatToDraw);

	auto renderToTexture_SetRT = [&](ITexture* target, int texW, int texH, QAngle passAngles,
		CViewSetup& view, CViewSetup& hud)
		{
			IMatRenderContext* rc = m_Game->m_MaterialSystem->GetRenderContext();
			if (!rc)
			{
				m_VR->HandleMissingRenderContext("Hooks::dRenderView(offscreen)");
				return;
			}

			// If viewport hooks aren't available, fall back (less ideal).
			if (!hkGetViewport.fOriginal || !hkViewport.fOriginal)
			{
				hkPushRenderTargetAndViewport.fOriginal(rc, target, nullptr, 0, 0, texW, texH);

				QAngle oldEngineAngles;
				m_Game->m_EngineClient->GetViewAngles(oldEngineAngles);
				m_Game->m_EngineClient->SetViewAngles(passAngles);

				hkRenderView.fOriginal(ecx, view, hud, nClearFlags, whatToDraw);

				m_Game->m_EngineClient->SetViewAngles(oldEngineAngles);
				hkPopRenderTargetAndViewport.fOriginal(rc);
				return;
			}

			const bool prevSuppress = m_VR->m_SuppressHudCapture;
			m_VR->m_SuppressHudCapture = true;

			int oldX = 0, oldY = 0, oldW = 0, oldH = 0;
			hkGetViewport.fOriginal(rc, oldX, oldY, oldW, oldH);
			ITexture* oldRT = rc->GetRenderTarget();

			rc->SetRenderTarget(target);
			hkViewport.fOriginal(rc, 0, 0, texW, texH);

			rc->ClearColor4ub(0, 0, 0, 255);
			rc->ClearBuffers(true, true, true);

			QAngle oldEngineAngles;
			m_Game->m_EngineClient->GetViewAngles(oldEngineAngles);
			m_Game->m_EngineClient->SetViewAngles(passAngles);

			hkRenderView.fOriginal(ecx, view, hud, nClearFlags, whatToDraw);

			m_Game->m_EngineClient->SetViewAngles(oldEngineAngles);

			rc->SetRenderTarget(oldRT);
			hkViewport.fOriginal(rc, oldX, oldY, oldW, oldH);

			m_VR->m_SuppressHudCapture = prevSuppress;
		};

	// ----------------------------
	// Scope RTT pass: render from scope camera into vrScope RTT
	// ----------------------------
	if (m_VR->m_CreatedVRTextures.load(std::memory_order_acquire) && m_VR->ShouldRenderScope() && m_VR->m_ScopeTexture && m_VR->ShouldUpdateScopeRTT())
	{
		CViewSetup scopeView = setup;
		scopeView.x = 0;
		scopeView.y = 0;
		scopeView.m_nUnscaledX = 0;
		scopeView.m_nUnscaledY = 0;
		scopeView.width = m_VR->m_ScopeRTTSize;
		scopeView.m_nUnscaledWidth = m_VR->m_ScopeRTTSize;
		scopeView.height = m_VR->m_ScopeRTTSize;
		scopeView.m_nUnscaledHeight = m_VR->m_ScopeRTTSize;
		scopeView.fov = m_VR->m_ScopeFov;
		scopeView.m_flAspectRatio = 1.0f;
		scopeView.fovViewmodel = scopeView.fov;
		scopeView.zNear = m_VR->m_ScopeZNear;
		scopeView.zNearViewmodel = 99999.0f; // hard-clip viewmodel so scope image is "world only"

		QAngle scopeAngles = m_VR->GetScopeCameraAbsAngle();
		scopeView.origin = m_VR->GetScopeCameraAbsPos();
		scopeView.angles.x = scopeAngles.x;
		scopeView.angles.y = scopeAngles.y;
		scopeView.angles.z = scopeAngles.z;

		CViewSetup hudScope = hudViewSetup;
		hudScope.origin = scopeView.origin;
		hudScope.angles = scopeView.angles;

		renderToTexture_SetRT(m_VR->m_ScopeTexture,
			m_VR->m_ScopeRTTSize, m_VR->m_ScopeRTTSize,
			scopeAngles, scopeView, hudScope);
	}

	// ----------------------------
	// Rear mirror RTT pass: render from HMD with 180 yaw into vrRearMirror RTT
	// ----------------------------
	if (m_VR->m_CreatedVRTextures.load(std::memory_order_acquire) && m_VR->ShouldRenderRearMirror() && m_VR->m_RearMirrorTexture && m_VR->ShouldUpdateRearMirrorRTT())
	{
		CViewSetup mirrorView = setup;
		mirrorView.x = 0;
		mirrorView.y = 0;
		mirrorView.m_nUnscaledX = 0;
		mirrorView.m_nUnscaledY = 0;
		mirrorView.width = m_VR->m_RearMirrorRTTSize;
		mirrorView.m_nUnscaledWidth = m_VR->m_RearMirrorRTTSize;
		mirrorView.height = m_VR->m_RearMirrorRTTSize;
		mirrorView.m_nUnscaledHeight = m_VR->m_RearMirrorRTTSize;
		mirrorView.fov = m_VR->m_RearMirrorFov;
		mirrorView.m_flAspectRatio = 1.0f;
		mirrorView.fovViewmodel = mirrorView.fov;
		mirrorView.zNear = m_VR->m_RearMirrorZNear;
		mirrorView.zNearViewmodel = 99999.0f;

		QAngle mirrorAngles = m_VR->GetRearMirrorCameraAbsAngle();
		mirrorView.origin = m_VR->GetRearMirrorCameraAbsPos();
		mirrorView.angles.x = mirrorAngles.x;
		mirrorView.angles.y = mirrorAngles.y;
		mirrorView.angles.z = mirrorAngles.z;

		CViewSetup hudMirror = hudViewSetup;
		hudMirror.origin = mirrorView.origin;
		hudMirror.angles = mirrorView.angles;

		// Mark mirror RTT pass so DrawModelExecute can tag special-infected arrows seen in this pass.
		m_VR->m_RearMirrorRenderingPass = true;
		m_VR->m_RearMirrorSawSpecialThisPass = false;

		renderToTexture_SetRT(m_VR->m_RearMirrorTexture,
			m_VR->m_RearMirrorRTTSize, m_VR->m_RearMirrorRTTSize,
			mirrorAngles, mirrorView, hudMirror);

		m_VR->m_RearMirrorRenderingPass = false;
		const auto rmNow = std::chrono::steady_clock::now();
		if (m_VR->m_RearMirrorSpecialWarningDistance > 0.0f)
		{
			if (m_VR->m_RearMirrorSawSpecialThisPass)
			{
				m_VR->m_LastRearMirrorSpecialSeenTime = rmNow;
				m_VR->m_RearMirrorSpecialEnlargeActive = true;
			}

			if (m_VR->m_RearMirrorSpecialEnlargeActive)
			{
				const float elapsed = std::chrono::duration<float>(rmNow - m_VR->m_LastRearMirrorSpecialSeenTime).count();
				if (elapsed > m_VR->m_RearMirrorSpecialEnlargeHoldSeconds)
					m_VR->m_RearMirrorSpecialEnlargeActive = false;
			}
		}
		else
		{
			// Disabled
			m_VR->m_RearMirrorSpecialEnlargeActive = false;
		}
	}

	// Restore engine angles immediately after our stereo render.
	m_Game->m_EngineClient->SetViewAngles(prevEngineAngles);
	m_VR->m_RenderedNewFrame.store(true, std::memory_order_release);
}

