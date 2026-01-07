#include "hooks.h"
#include "game.h"
#include "texture.h"
#include "sdk.h"
#include "sdk_server.h"
#include "vr.h"
#include "offsets.h"
#include <iostream>
#include <cstdint>
#include <string>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>

// Normalize Source-style angles:
// - Bring pitch/yaw into [-180, 180] first (avoid -30 becoming 330 and then clamped to 89).
// - Then clamp pitch to [-89, 89].
static inline void NormalizeAndClampViewAngles(QAngle& a)
{
	while (a.x > 180.f) a.x -= 360.f;
	while (a.x < -180.f) a.x += 360.f;
	while (a.y > 180.f) a.y -= 360.f;
	while (a.y < -180.f) a.y += 360.f;
	a.z = 0.f;
	if (a.x > 89.f) a.x = 89.f;
	if (a.x < -89.f) a.x = -89.f;
}


// ------------------------------------------------------------
// Third-person render stability helpers (netvars from offsets.txt)
// When the player is pinned / incapacitated / using certain actions,
// the engine can momentarily "snap" between first/third-person.
// We treat those states as "force third-person rendering" to avoid flicker.
// ------------------------------------------------------------
template <typename T>
static inline T ReadNetvar(const void* base, int ofs)
{
	return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(base) + ofs);
}

static inline bool HandleValid(int h)
{
	return (h != 0 && h != -1);
}

// Returns true if the local player is currently pinned/controlled by special infected.
// Used to disable jittery aim line while being dragged / pinned.
static inline bool IsPlayerControlledBySI(const C_BasePlayer* player)
{
	if (!player)
		return false;

	// Smoker
	const int tongueOwner = ReadNetvar<int>(player, 0x1f6c);              // m_tongueOwner
	const bool hangingTongue = ReadNetvar<uint8_t>(player, 0x1f84) != 0;  // m_isHangingFromTongue
	const bool tongue = hangingTongue || HandleValid(tongueOwner);

	// Hunter / Charger / Jockey pins
	const int carryAttacker = ReadNetvar<int>(player, 0x2714);           // m_carryAttacker
	const int pummelAttacker = ReadNetvar<int>(player, 0x2720);           // m_pummelAttacker
	const int pounceAttacker = ReadNetvar<int>(player, 0x272c);           // m_pounceAttacker
	const int jockeyAttacker = ReadNetvar<int>(player, 0x274c);           // m_jockeyAttacker
	const bool pinned = HandleValid(carryAttacker) || HandleValid(pummelAttacker) ||
		HandleValid(pounceAttacker) || HandleValid(jockeyAttacker);

	return tongue || pinned;
}

struct ThirdPersonStateDebug
{
	bool incap = false;
	bool ledge = false;
	bool hangingTongue = false;
	bool tongue = false;
	bool pinned = false;
	bool doingUseAction = false;
	bool reviving = false;

	int tongueOwner = 0;
	int carryAttacker = 0;
	int pummelAttacker = 0;
	int pounceAttacker = 0;
	int jockeyAttacker = 0;
	int useAction = 0;
	int reviveOwner = 0;
	int reviveTarget = 0;
};

static inline bool ShouldForceThirdPersonByState(const C_BasePlayer* player, ThirdPersonStateDebug* outDbg = nullptr)
{
	if (outDbg)
		*outDbg = ThirdPersonStateDebug{};

	if (!player)
		return false;

	ThirdPersonStateDebug dbg{};

	// Offsets are client netvars (see offsets.txt)
	dbg.incap = ReadNetvar<uint8_t>(player, 0x1ea9) != 0;          // m_isIncapacitated
	dbg.ledge = ReadNetvar<uint8_t>(player, 0x25ec) != 0;          // m_isHangingFromLedge

	dbg.tongueOwner = ReadNetvar<int>(player, 0x1f6c);             // m_tongueOwner
	dbg.hangingTongue = ReadNetvar<uint8_t>(player, 0x1f84) != 0;  // m_isHangingFromTongue
	dbg.tongue = dbg.hangingTongue || HandleValid(dbg.tongueOwner);

	dbg.carryAttacker = ReadNetvar<int>(player, 0x2714);           // m_carryAttacker
	dbg.pummelAttacker = ReadNetvar<int>(player, 0x2720);          // m_pummelAttacker
	dbg.pounceAttacker = ReadNetvar<int>(player, 0x272c);          // m_pounceAttacker
	dbg.jockeyAttacker = ReadNetvar<int>(player, 0x274c);          // m_jockeyAttacker
	dbg.pinned = HandleValid(dbg.carryAttacker) || HandleValid(dbg.pummelAttacker) ||
		HandleValid(dbg.pounceAttacker) || HandleValid(dbg.jockeyAttacker);

	dbg.useAction = ReadNetvar<int>(player, 0x1ba8);               // m_iCurrentUseAction
	dbg.doingUseAction = (dbg.useAction != 0);

	dbg.reviveOwner = ReadNetvar<int>(player, 0x1f88);             // m_reviveOwner
	dbg.reviveTarget = ReadNetvar<int>(player, 0x1f8c);            // m_reviveTarget
	dbg.reviving = HandleValid(dbg.reviveOwner) || HandleValid(dbg.reviveTarget);

	if (outDbg)
		*outDbg = dbg;

	// NOTE: user request:
	// - "倒地" (incapacitated) 不强制第三人称
	// Keep other pinned/use/revive/tongue states.
	return dbg.ledge || dbg.tongue || dbg.pinned || dbg.doingUseAction || dbg.reviving;
}

bool Hooks::s_ServerUnderstandsVR = false;
Hooks::Hooks(Game* game)
{
	if (MH_Initialize() != MH_OK)
	{
		Game::errorMsg("Failed to init MinHook");
	}

	m_Game = game;
	m_VR = m_Game->m_VR;

	m_PushHUDStep = -999;
	m_PushedHud = false;

	initSourceHooks();

	hkGetRenderTarget.enableHook();
	hkCalcViewModelView.enableHook();
	hkServerFireTerrorBullets.enableHook();
	hkClientFireTerrorBullets.enableHook();
	hkProcessUsercmds.enableHook();
	hkReadUsercmd.enableHook();
	hkWriteUsercmdDeltaToBuffer.enableHook();
	hkWriteUsercmd.enableHook();
	hkAdjustEngineViewport.enableHook();
	hkViewport.enableHook();
	hkGetViewport.enableHook();
	hkCreateMove.enableHook();
	hkTestMeleeSwingCollisionClient.enableHook();
	hkTestMeleeSwingCollisionServer.enableHook();
	hkDoMeleeSwingServer.enableHook();
	hkStartMeleeSwingServer.enableHook();
	hkPrimaryAttackServer.enableHook();
	hkItemPostFrameServer.enableHook();
	hkGetPrimaryAttackActivity.enableHook();
	hkEyePosition.enableHook();
	hkDrawModelExecute.enableHook();
	hkRenderView.enableHook();
	hkPushRenderTargetAndViewport.enableHook();
	hkPopRenderTargetAndViewport.enableHook();
	hkVgui_Paint.enableHook();
	hkIsSplitScreen.enableHook();
	hkPrePushRenderTarget.enableHook();
}

Hooks::~Hooks()
{
	if (MH_Uninitialize() != MH_OK)
	{
		Game::errorMsg("Failed to uninitialize MinHook");
	}
}


int Hooks::initSourceHooks()
{
	LPVOID pGetRenderTargetVFunc = (LPVOID)(m_Game->m_Offsets->GetRenderTarget.address);
	hkGetRenderTarget.createHook(pGetRenderTargetVFunc, &dGetRenderTarget);

	LPVOID pRenderViewVFunc = (LPVOID)(m_Game->m_Offsets->RenderView.address);
	hkRenderView.createHook(pRenderViewVFunc, &dRenderView);

	LPVOID calcViewModelViewAddr = (LPVOID)(m_Game->m_Offsets->CalcViewModelView.address);
	hkCalcViewModelView.createHook(calcViewModelViewAddr, &dCalcViewModelView);

	LPVOID serverFireTerrorBulletsAddr = (LPVOID)(m_Game->m_Offsets->ServerFireTerrorBullets.address);
	hkServerFireTerrorBullets.createHook(serverFireTerrorBulletsAddr, &dServerFireTerrorBullets);

	LPVOID clientFireTerrorBulletsAddr = (LPVOID)(m_Game->m_Offsets->ClientFireTerrorBullets.address);
	hkClientFireTerrorBullets.createHook(clientFireTerrorBulletsAddr, &dClientFireTerrorBullets);

	LPVOID ProcessUsercmdsAddr = (LPVOID)(m_Game->m_Offsets->ProcessUsercmds.address);
	hkProcessUsercmds.createHook(ProcessUsercmdsAddr, &dProcessUsercmds);

	LPVOID ReadUserCmdAddr = (LPVOID)(m_Game->m_Offsets->ReadUserCmd.address);
	hkReadUsercmd.createHook(ReadUserCmdAddr, &dReadUsercmd);

	LPVOID WriteUsercmdDeltaToBufferAddr = (LPVOID)(m_Game->m_Offsets->WriteUsercmdDeltaToBuffer.address);
	hkWriteUsercmdDeltaToBuffer.createHook(WriteUsercmdDeltaToBufferAddr, &dWriteUsercmdDeltaToBuffer);

	LPVOID WriteUsercmdAddr = (LPVOID)(m_Game->m_Offsets->WriteUsercmd.address);
	hkWriteUsercmd.createHook(WriteUsercmdAddr, &dWriteUsercmd);

	LPVOID AdjustEngineViewportAddr = (LPVOID)(m_Game->m_Offsets->AdjustEngineViewport.address);
	hkAdjustEngineViewport.createHook(AdjustEngineViewportAddr, &dAdjustEngineViewport);

	LPVOID ViewportAddr = (LPVOID)(m_Game->m_Offsets->Viewport.address);
	hkViewport.createHook(ViewportAddr, &dViewport);

	LPVOID GetViewportAddr = (LPVOID)(m_Game->m_Offsets->GetViewport.address);
	hkGetViewport.createHook(GetViewportAddr, &dGetViewport);

	LPVOID MeleeSwingClientAddr = (LPVOID)(m_Game->m_Offsets->TestMeleeSwingClient.address);
	hkTestMeleeSwingCollisionClient.createHook(MeleeSwingClientAddr, &dTestMeleeSwingCollisionClient);

	LPVOID MeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->TestMeleeSwingServer.address);
	hkTestMeleeSwingCollisionServer.createHook(MeleeSwingServerAddr, &dTestMeleeSwingCollisionServer);

	LPVOID DoMeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->DoMeleeSwingServer.address);
	hkDoMeleeSwingServer.createHook(DoMeleeSwingServerAddr, &dDoMeleeSwingServer);

	LPVOID StartMeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->StartMeleeSwingServer.address);
	hkStartMeleeSwingServer.createHook(StartMeleeSwingServerAddr, &dStartMeleeSwingServer);

	LPVOID PrimaryAttackServerAddr = (LPVOID)(m_Game->m_Offsets->PrimaryAttackServer.address);
	hkPrimaryAttackServer.createHook(PrimaryAttackServerAddr, &dPrimaryAttackServer);

	LPVOID ItemPostFrameServerAddr = (LPVOID)(m_Game->m_Offsets->ItemPostFrameServer.address);
	hkItemPostFrameServer.createHook(ItemPostFrameServerAddr, &dItemPostFrameServer);

	LPVOID GetPrimaryAttackActivityAddr = (LPVOID)(m_Game->m_Offsets->GetPrimaryAttackActivity.address);
	hkGetPrimaryAttackActivity.createHook(GetPrimaryAttackActivityAddr, &dGetPrimaryAttackActivity);

	LPVOID EyePositionAddr = (LPVOID)(m_Game->m_Offsets->EyePosition.address);
	hkEyePosition.createHook(EyePositionAddr, &dEyePosition);

	LPVOID DrawModelExecuteAddr = (LPVOID)(m_Game->m_Offsets->DrawModelExecute.address);
	hkDrawModelExecute.createHook(DrawModelExecuteAddr, &dDrawModelExecute);

	LPVOID PushRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PushRenderTargetAndViewport.address);
	hkPushRenderTargetAndViewport.createHook(PushRenderTargetAddr, &dPushRenderTargetAndViewport);

	LPVOID PopRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PopRenderTargetAndViewport.address);
	hkPopRenderTargetAndViewport.createHook(PopRenderTargetAddr, &dPopRenderTargetAndViewport);

	LPVOID VGui_PaintAddr = (LPVOID)(m_Game->m_Offsets->VGui_Paint.address);
	hkVgui_Paint.createHook(VGui_PaintAddr, &dVGui_Paint);

	LPVOID IsSplitScreenAddr = (LPVOID)(m_Game->m_Offsets->IsSplitScreen.address);
	hkIsSplitScreen.createHook(IsSplitScreenAddr, &dIsSplitScreen);

	LPVOID PrePushRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PrePushRenderTarget.address);
	hkPrePushRenderTarget.createHook(PrePushRenderTargetAddr, &dPrePushRenderTarget);

	uintptr_t clientModeAddress = m_Game->m_Offsets->g_pClientMode.address;
	if (!clientModeAddress)
	{
		Game::errorMsg("g_pClientMode address was null; aborting CreateMove hook installation");
		return 0;
	}

	void* clientMode = nullptr;
	constexpr int kMaxAttempts = 500;
	for (int attempt = 0; attempt < kMaxAttempts && !clientMode; ++attempt)
	{
		uintptr_t clientModePtr = *reinterpret_cast<uintptr_t*>(clientModeAddress);
		if (clientModePtr)
		{
			uintptr_t clientModeValue = *reinterpret_cast<uintptr_t*>(clientModePtr);
			if (clientModeValue)
			{
				clientMode = reinterpret_cast<void*>(clientModeValue);
				break;
			}
		}

		Sleep(10);
	}

	if (!clientMode)
	{
		Game::errorMsg("Timed out waiting for g_pClientMode; CreateMove hook not installed");
		return 0;
	}

	void*** clientModePtr = reinterpret_cast<void***>(clientMode);
	void** clientModeVTable = (clientModePtr != nullptr) ? *clientModePtr : nullptr;
	if (!clientModeVTable)
	{
		Game::errorMsg("Client mode vtable pointer was null; CreateMove hook not installed");
		return 0;
	}

	hkCreateMove.createHook(clientModeVTable[27], dCreateMove);

	return 1;
}


ITexture* __fastcall Hooks::dGetRenderTarget(void* ecx, void* edx)
{
	ITexture* result = hkGetRenderTarget.fOriginal(ecx);
	return result;
}

void __fastcall Hooks::dRenderView(void* ecx, void* edx, CViewSetup& setup, CViewSetup& hudViewSetup, int nClearFlags, int whatToDraw)
{
	if (!m_VR->m_CreatedVRTextures)
		m_VR->CreateVRTextures();

	IMatRenderContext* rndrContext = m_Game->m_MaterialSystem->GetRenderContext();
	if (!rndrContext)
	{
		m_VR->HandleMissingRenderContext("Hooks::dRenderView");
		return hkRenderView.fOriginal(ecx, setup, hudViewSetup, nClearFlags, whatToDraw);
	}

	// ------------------------------
	// Third-person camera fix:
	// If engine is in third-person, setup.origin is a shoulder camera,
	// but our VR hook normally overwrites it with HMD first-person.
	// That makes the local player model show up "in your face" and looks like ghosting/double image.
	// ------------------------------
	int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
	C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);

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
	constexpr float kThirdPersonXY = 18.0f;  // was 30.0f
	constexpr float kThirdPerson3D = 90.0f;
	const bool engineThirdPersonNow = (localPlayer && (camDistXY > kThirdPersonXY || camDist3D > kThirdPerson3D));
	// Always capture the view the engine is rendering this frame.
	// In true third-person, setup.origin is the shoulder camera; in first-person it matches the eye.
	m_VR->m_ThirdPersonViewOrigin = setup.origin;
	m_VR->m_ThirdPersonViewAngles.Init(setup.angles.x, setup.angles.y, setup.angles.z);

	// Detect third-person by comparing rendered camera origin to the real eye origin.
	// Use a small threshold + hysteresis to avoid flicker.
	// Also expose a simple "player is pinned/controlled" flag so VR can disable jittery aim line.
	m_VR->m_PlayerControlledBySI = IsPlayerControlledBySI(localPlayer);
	ThirdPersonStateDebug tpStateDbg;
	const bool stateWantsThirdPerson = ShouldForceThirdPersonByState(localPlayer, &tpStateDbg);
	const int holdBefore = m_VR->m_ThirdPersonHoldFrames;
	constexpr int kEngineThirdPersonHoldFrames = 2;
	constexpr int kStateThirdPersonHoldFrames = 40;

	// 先按“状态”锁定（优先级最高）
	if (stateWantsThirdPerson)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kStateThirdPersonHoldFrames);

	// 再按“引擎第三人称”做短缓冲，但不要覆盖掉状态锁定
	if (engineThirdPersonNow)
		m_VR->m_ThirdPersonHoldFrames = std::max(m_VR->m_ThirdPersonHoldFrames, kEngineThirdPersonHoldFrames);
	else if (!stateWantsThirdPerson && m_VR->m_ThirdPersonHoldFrames > 0)
		m_VR->m_ThirdPersonHoldFrames--;

	const bool renderThirdPerson = engineThirdPersonNow || (m_VR->m_ThirdPersonHoldFrames > 0);
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
			(std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastTpDbg).count() >= 1000)
	}
	// Expose third-person camera to VR helpers (aim line, overlays, etc.)
	m_VR->m_IsThirdPersonCamera = renderThirdPerson;
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
	// Keep VR tracking base tied to the real player eye, NOT the shoulder camera
	m_VR->m_SetupOrigin = eyeOrigin;
	if (!renderThirdPerson)
		m_VR->m_SetupOrigin.z = setup.origin.z;
	m_VR->m_SetupAngles.Init(setup.angles.x, setup.angles.y, setup.angles.z);

	Vector leftOrigin, rightOrigin;
	Vector viewAngles = m_VR->GetViewAngle();

	if (renderThirdPerson)
	{
		// Render from the engine-provided third-person camera (setup.origin),
		// but aim the camera with the HMD so head look still works in third-person.
		QAngle camAng(viewAngles.x, viewAngles.y, viewAngles.z);
		if (m_VR->m_HmdForward.IsZero())
			camAng = QAngle(setup.angles.x, setup.angles.y, setup.angles.z);

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
			baseCenter = m_VR->m_HmdPosAbs;
		else
			baseCenter = engineThirdPersonNow ? setup.origin : m_VR->m_HmdPosAbs;
		Vector camCenter = baseCenter + (fwd * (-eyeZ));
		if (m_VR->m_ThirdPersonVRCameraOffset > 0.0f)
			camCenter = camCenter + (fwd * (-m_VR->m_ThirdPersonVRCameraOffset));
		leftOrigin = camCenter + (right * (-(ipd * 0.5f)));
		rightOrigin = camCenter + (right * (+(ipd * 0.5f)));
	}
	else
	{
		// Normal VR first-person
		leftOrigin = m_VR->GetViewOriginLeft();
		rightOrigin = m_VR->GetViewOriginRight();
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

	// ----------------------------
	// Scope RTT pass: render from scope camera into vrScope RTT
	// ----------------------------
	if (m_VR->m_CreatedVRTextures && m_VR->ShouldRenderScope() && m_VR->m_ScopeTexture)
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

		// prevent HUD capture hooks during this pass
		m_VR->m_SuppressHudCapture = true;

		IMatRenderContext* renderContext = m_Game->m_MaterialSystem->GetRenderContext();
		if (renderContext)
		{
			hkPushRenderTargetAndViewport.fOriginal(renderContext, m_VR->m_ScopeTexture, nullptr, 0, 0, m_VR->m_ScopeRTTSize, m_VR->m_ScopeRTTSize);
			renderContext->ClearColor4ub(0, 0, 0, 255);
			renderContext->ClearBuffers(true, true, true);

			QAngle oldEngineAngles;
			m_Game->m_EngineClient->GetViewAngles(oldEngineAngles);
			m_Game->m_EngineClient->SetViewAngles(scopeAngles);

			hkRenderView.fOriginal(ecx, scopeView, hudScope, nClearFlags, whatToDraw);

			m_Game->m_EngineClient->SetViewAngles(oldEngineAngles);
			hkPopRenderTargetAndViewport.fOriginal(renderContext);
		}

		m_VR->m_SuppressHudCapture = false;
	}

	// Restore engine angles immediately after our stereo render.
	m_Game->m_EngineClient->SetViewAngles(prevEngineAngles);
	m_VR->m_RenderedNewFrame = true;
}

bool __fastcall Hooks::dCreateMove(void* ecx, void* edx, float flInputSampleTime, CUserCmd* cmd)
{
	// Non-VR server melee feel state (ForceNonVRServerMovement=true only)
	static int s_nonvrMeleeHoldTicks = 0;
	static bool s_nonvrMeleeArmed = true;
	static QAngle s_nonvrMeleeLockedAngles = { 0,0,0 };
	static std::chrono::steady_clock::time_point s_nonvrMeleeLockUntil{};
	static std::chrono::steady_clock::time_point s_nonvrMeleeCooldownUntil{};
	static bool s_nonvrMeleeHasPrev = false;
	static Vector s_nonvrMeleePrevCtrlPos = { 0,0,0 };
	static Vector s_nonvrMeleePrevHmdPos = { 0,0,0 };

	if (!cmd->command_number)
		return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

	bool result = hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

	if (m_VR->m_IsVREnabled) {
		const bool treatServerAsNonVR = m_VR->m_ForceNonVRServerMovement;
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
				cmd->forwardmove += ny * maxSpeed;
				cmd->sidemove += nx * maxSpeed;
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

				if (above && s_nonvrMeleeArmed && now >= s_nonvrMeleeCooldownUntil)
				{
					s_nonvrMeleeArmed = false;


					// Hold IN_ATTACK for a few ticks so we don't miss the server-side melee window.
					const float holdTime = std::max(0.0f, m_VR->m_NonVRMeleeHoldTime);
					int holdTicks = (int)ceilf(holdTime / dt);
					holdTicks = std::clamp(holdTicks, 1, 8);
					s_nonvrMeleeHoldTicks = std::max(s_nonvrMeleeHoldTicks, holdTicks);

					// Lock current aim direction, optionally blend toward swing velocity direction.
					s_nonvrMeleeLockedAngles = cmd->viewangles;

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

							s_nonvrMeleeLockedAngles.x = lerpAngle(s_nonvrMeleeLockedAngles.x, velAng.x, blend);
							s_nonvrMeleeLockedAngles.y = lerpAngle(s_nonvrMeleeLockedAngles.y, velAng.y, blend);
							s_nonvrMeleeLockedAngles.z = 0.f;
							NormalizeAndClampViewAngles(s_nonvrMeleeLockedAngles);
						}
					}

					// Apply lock window
					const float lockT = std::max(0.0f, m_VR->m_NonVRMeleeAimLockTime);
					s_nonvrMeleeLockUntil = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(lockT));

					// Apply cooldown window
					const float cd = std::max(0.05f, m_VR->m_NonVRMeleeSwingCooldown);
					s_nonvrMeleeCooldownUntil = now + std::chrono::duration_cast<clock::duration>(std::chrono::duration<float>(cd));

					// Fire now (this frame)
					cmd->viewangles = s_nonvrMeleeLockedAngles;
					cmd->buttons |= (1 << 0); // IN_ATTACK
				}
			}

			// Re-base movement for non-VR servers:
			// - The server interprets forwardmove/sidemove in the basis of cmd->viewangles (aim).
			// - We want movement to follow the HMD yaw (body direction), not the hand aim yaw.
			// So we convert existing movement (built under originalViewAngles) into world space,
			// add VR stick movement in HMD space, then project back into the final cmd basis.
			{
				// Existing movement (keyboard etc.) in world space
				QAngle origYawOnly(0.f, originalViewAngles.y, 0.f);
				Vector origForward, origRight, origUp;
				QAngle::AngleVectors(origYawOnly, &origForward, &origRight, &origUp);
				Vector worldMove = origForward * cmd->forwardmove + origRight * cmd->sidemove;

				// VR stick movement (body = HMD yaw)
				if (hadWalkAxis)
				{
					Vector hmdAng = m_VR->GetViewAngle();
					QAngle bodyYawOnly(0.f, hmdAng.y, 0.f);
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
			if (view.x > 89.f)  view.x = 89.f;
			if (view.x < -89.f) view.x = -89.f;
			while (view.y > 180.f)  view.y -= 360.f;
			while (view.y < -180.f) view.y += 360.f;
			view.z = 0.f;
			cmd->viewangles = view;
		}
	}

	return result;
}

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
		vecNewOrigin = m_VR->GetRecommendedViewmodelAbsPos();
		vecNewAngles = m_VR->GetRecommendedViewmodelAbsAngle();
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
		vecNewOrigin = m_VR->GetRightControllerAbsPos();

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
				vecNewAngles = m_VR->GetRightControllerAbsAngle();
			}
		}

		// Third-person convergence: aim bullets to the converge point so "bullet line" intersects
		// the rendered aim line at the actual hit point.
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
				vecNewAngles = m_VR->GetRightControllerAbsAngle();
			}
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
			// VR-aware server：起点/方向都跟控制器（你现在第三人称汇聚点逻辑保留）
			if (scopeActive)
			{
				vecNewOrigin = m_VR->GetScopeCameraAbsPos();
				vecNewAngles = m_VR->GetScopeCameraAbsAngle();
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
	Hooks::s_ServerUnderstandsVR = true;

	// Function pointer for CBaseEntity::entindex
	typedef int(__thiscall* tEntindex)(void* thisptr);
	static tEntindex oEntindex = (tEntindex)(m_Game->m_Offsets->CBaseEntity_entindex.address);

	IServerUnknown* pUnknown = player->m_pUnk;
	Server_BaseEntity* pPlayer = (Server_BaseEntity*)pUnknown->GetBaseEntity();

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

void Hooks::dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
	if (!m_VR->m_CreatedVRTextures)
		return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

	// Extra offscreen passes (scope RTT) must not hijack HUD capture
	if (m_VR->m_SuppressHudCapture)
		return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

	if (m_PushHUDStep == 2)
		++m_PushHUDStep;
	else
		m_PushHUDStep = -999;

	// RenderView calls PushRenderTargetAndViewport multiple times with different textures. 
	// When the call order goes PopRenderTargetAndViewport -> IsSplitScreen -> PrePushRenderTarget -> PushRenderTargetAndViewport,
	// then it pushed the HUD/GUI render target to the RT stack.
	if (m_PushHUDStep == 3)
	{
		ITexture* originalTexture = pTexture;
		pTexture = m_VR->m_HUDTexture;

		IMatRenderContext* renderContext = m_Game->m_MaterialSystem->GetRenderContext();
		if (!renderContext)
		{
			m_VR->HandleMissingRenderContext("Hooks::dPushRenderTargetAndViewport");
			return hkPushRenderTargetAndViewport.fOriginal(ecx, originalTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
		}

		renderContext->ClearBuffers(false, true, true);

		hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

		renderContext->OverrideAlphaWriteEnable(true, true);
		renderContext->ClearColor4ub(0, 0, 0, 0);
		renderContext->ClearBuffers(true, false);

		m_VR->m_RenderedHud = true;
		m_PushedHud = true;
	}
	else
	{
		hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
	}
}

void Hooks::dPopRenderTargetAndViewport(void* ecx, void* edx)
{
	if (!m_VR->m_CreatedVRTextures)
		return hkPopRenderTargetAndViewport.fOriginal(ecx);

	m_PushHUDStep = 0;

	if (m_PushedHud)
	{
		IMatRenderContext* renderContext = m_Game->m_MaterialSystem->GetRenderContext();
		if (!renderContext)
		{
			m_VR->HandleMissingRenderContext("Hooks::dPopRenderTargetAndViewport");
			return hkPopRenderTargetAndViewport.fOriginal(ecx);
		}

		renderContext->OverrideAlphaWriteEnable(false, true);
		renderContext->ClearColor4ub(0, 0, 0, 255);
	}

	hkPopRenderTargetAndViewport.fOriginal(ecx);
}

void Hooks::dVGui_Paint(void* ecx, void* edx, int mode)
{
	if (!m_VR->m_CreatedVRTextures)
		return hkVgui_Paint.fOriginal(ecx, mode);

	// When scope RTT is rendering, don't redirect HUD/VGUI
	if (m_VR->m_SuppressHudCapture)
		return hkVgui_Paint.fOriginal(ecx, mode);

	if (m_PushedHud)
		mode = PAINT_UIPANELS | PAINT_INGAMEPANELS;

	hkVgui_Paint.fOriginal(ecx, mode);
}

int Hooks::dIsSplitScreen()
{
	if (m_PushHUDStep == 0)
		++m_PushHUDStep;
	else
		m_PushHUDStep = -999;

	return hkIsSplitScreen.fOriginal();
}

DWORD* Hooks::dPrePushRenderTarget(void* ecx, void* edx, int a2)
{
	if (m_PushHUDStep == 1)
		++m_PushHUDStep;
	else
		m_PushHUDStep = -999;

	return hkPrePushRenderTarget.fOriginal(ecx, a2);
}
