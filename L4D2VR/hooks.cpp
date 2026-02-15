
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
#include <algorithm> // std::clamp
#include <chrono>
#include <cmath>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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
// Engine third-person camera smoothing
//
// Some camera mods (e.g. slide) switch to an engine-controlled third-person camera
// that updates at tick rate (30/60Hz) while we still render at HMD rate (90Hz+).
// That produces a "feels like 30fps" stutter even when frametime is stable.
//
// We blend from prev tick camera to curr tick camera over an estimated tick interval.
// Only enabled when tick interval > ~16ms (~<60Hz) to avoid adding lag to 90Hz paths.
// ------------------------------------------------------------
static inline float AngleDeltaDeg(float to, float from)
{
	float delta = std::fmod(to - from, 360.0f);
	if (delta > 180.0f) delta -= 360.0f;
	if (delta < -180.0f) delta += 360.0f;
	return delta;
}

static inline float AngleLerpDeg(float a, float b, float t)
{
	return a + AngleDeltaDeg(b, a) * t;
}

static inline float SmoothStep01(float t)
{
	t = std::clamp(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

struct EngineThirdPersonCamSmoother
{
	bool valid = false;
	Vector prevOrigin{ 0,0,0 };
	Vector currOrigin{ 0,0,0 };
	QAngle prevAngles{ 0,0,0 };
	QAngle currAngles{ 0,0,0 };
	std::chrono::steady_clock::time_point lastRawUpdate{};
	std::chrono::steady_clock::time_point blendStart{};
	float tickIntervalSec = (1.0f / 30.0f); // pessimistic default

	void Reset()
	{
		valid = false;
	}

	void PushRaw(const Vector& rawOrigin, const QAngle& rawAngles)
	{
		const auto now = std::chrono::steady_clock::now();

		if (!valid)
		{
			valid = true;
			prevOrigin = currOrigin = rawOrigin;
			prevAngles = currAngles = rawAngles;
			lastRawUpdate = now;
			blendStart = now;
			return;
		}

		// Detect a new tick camera sample.
		const float posDeltaSqr = (rawOrigin - currOrigin).LengthSqr();
		const float angDelta =
			std::fabs(AngleDeltaDeg(rawAngles.x, currAngles.x)) +
			std::fabs(AngleDeltaDeg(rawAngles.y, currAngles.y)) +
			std::fabs(AngleDeltaDeg(rawAngles.z, currAngles.z));
		const bool changed = (posDeltaSqr > (0.25f * 0.25f)) || (angDelta > 0.25f);

		if (changed)
		{
			const float dt = std::chrono::duration<float>(now - lastRawUpdate).count();
			const float clamped = std::clamp(dt, 0.008f, 0.100f);
			tickIntervalSec = (tickIntervalSec * 0.8f) + (clamped * 0.2f);

			prevOrigin = currOrigin;
			prevAngles = currAngles;
			currOrigin = rawOrigin;
			currAngles = rawAngles;
			lastRawUpdate = now;
			blendStart = now;
		}
	}

	bool ShouldSmooth() const
	{
		// Only smooth when camera updates are slower than ~60Hz.
		return valid && (tickIntervalSec > 0.016f);
	}

	void GetSmoothed(Vector& outOrigin, QAngle& outAngles) const
	{
		if (!valid)
		{
			outOrigin = { 0,0,0 };
			outAngles = { 0,0,0 };
			return;
		}

		if (!ShouldSmooth())
		{
			outOrigin = currOrigin;
			outAngles = currAngles;
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		const float tRaw = std::chrono::duration<float>(now - blendStart).count() / std::max(0.001f, tickIntervalSec);
		const float t = SmoothStep01(tRaw);

		outOrigin = prevOrigin + (currOrigin - prevOrigin) * t;
		outAngles.x = AngleLerpDeg(prevAngles.x, currAngles.x, t);
		outAngles.y = AngleLerpDeg(prevAngles.y, currAngles.y, t);
		outAngles.z = AngleLerpDeg(prevAngles.z, currAngles.z, t);
	}
};


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

// Mouse-mode aiming helpers (mouse+keyboard play; no controllers required)
static inline Vector GetMouseModeGunOriginAbs(const VR* vr)
{
	return vr->m_HmdPosAbs
		+ (vr->m_HmdForward * (vr->m_MouseModeViewmodelAnchorOffset.x * vr->m_VRScale))
		+ (vr->m_HmdRight * (vr->m_MouseModeViewmodelAnchorOffset.y * vr->m_VRScale))
		+ (vr->m_HmdUp * (vr->m_MouseModeViewmodelAnchorOffset.z * vr->m_VRScale));
}

static inline Vector GetMouseModeEyeDir(const VR* vr)
{
	Vector eyeDir{ 0.0f, 0.0f, 0.0f };
	if (vr->m_MouseModeAimFromHmd)
	{
		eyeDir = vr->m_HmdForward;
	}
	else
	{
		const float pitch = std::clamp(vr->m_MouseAimPitchOffset, -89.f, 89.f);
		const float yaw = vr->m_RotationOffset;
		QAngle eyeAng(pitch, yaw, 0.f);
		NormalizeAndClampViewAngles(eyeAng);

		Vector right, up;
		QAngle::AngleVectors(eyeAng, &eyeDir, &right, &up);
	}

	if (!eyeDir.IsZero())
		VectorNormalize(eyeDir);
	return eyeDir;
}

static inline Vector GetMouseModeDefaultTargetAbs(const VR* vr)
{
	Vector eyeDir = GetMouseModeEyeDir(vr);
	const float dist = (vr->m_MouseModeAimConvergeDistance > 0.0f) ? vr->m_MouseModeAimConvergeDistance : 8192.0f;
	return vr->m_HmdPosAbs + eyeDir * dist;
}

static inline bool GetMouseModeAimAnglesToTarget(const VR* vr, const Vector& from, const Vector& target, QAngle& outAngles)
{
	Vector to = target - from;
	if (to.IsZero())
		return false;
	VectorNormalize(to);
	QAngle ang;
	QAngle::VectorAngles(to, ang);
	NormalizeAndClampViewAngles(ang);
	outAngles = ang;
	return true;
}

static inline QAngle GetMouseModeFallbackAimAngles(const VR* vr)
{
	Vector eyeDir = GetMouseModeEyeDir(vr);
	if (eyeDir.IsZero())
	{
		QAngle a(std::clamp(vr->m_MouseAimPitchOffset, -89.f, 89.f), vr->m_RotationOffset, 0.f);
		NormalizeAndClampViewAngles(a);
		return a;
	}
	QAngle a;
	QAngle::VectorAngles(eyeDir, a);
	NormalizeAndClampViewAngles(a);
	return a;
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
	bool dead = false;
	int lifeState = 0;
	bool goingToDie = false;
	int observerMode = 0;
	int observerTarget = 0;
	bool incap = false;
	bool ledge = false;
	bool hangingTongue = false;
	bool tongue = false;
	bool pinned = false;
	bool doingUseAction = false;
	bool reviving = false;
	bool selfMedkit = false;
	int useActionOwner = 0;
	int useActionTarget = 0;
	int tongueOwner = 0;
	int carryAttacker = 0;
	int pummelAttacker = 0;
	int pounceAttacker = 0;
	int jockeyAttacker = 0;
	int useAction = 0;
	int reviveOwner = 0;
	int reviveTarget = 0;
};

static inline bool ShouldForceThirdPersonByState(const C_BasePlayer* player,
	IClientEntityList* entList,
	const C_BasePlayer* localPlayer,
	ThirdPersonStateDebug* outDbg = nullptr)
{
	if (outDbg)
		*outDbg = ThirdPersonStateDebug{};

	if (!player)
		return false;

	ThirdPersonStateDebug dbg{};

	// When dead / dying / observer transitions happen, the engine camera can flicker
	// between views for a few frames. Force third-person rendering to avoid VR flicker.
	dbg.lifeState = (int)ReadNetvar<uint8_t>(player, 0x147); // m_lifeState
	dbg.goingToDie = ReadNetvar<uint8_t>(player, 0x1fb4) != 0;   // m_isGoingToDie
	dbg.observerMode = ReadNetvar<int>(player, 0x1450);          // m_iObserverMode
	dbg.observerTarget = ReadNetvar<int>(player, 0x1454);        // m_hObserverTarget


	// Offsets are client netvars (see offsets.txt)
	dbg.incap = ReadNetvar<uint8_t>(player, 0x1ea9) != 0;          // m_isIncapacitated
	dbg.ledge = ReadNetvar<uint8_t>(player, 0x25ec) != 0;          // m_isHangingFromLedge
	// IMPORTANT (L4D2):
	// m_isGoingToDie can stay true for "near death / black&white / scripted transitions" while the player is alive
	// (lifeState==0). Using it as "dead-ish" causes third-person latching after revive.
	// Only use lifeState to decide dead/dying here.
	// Typical Source: 0=ALIVE, 1=DYING, 2=DEAD.
	const bool lifeDead = (dbg.lifeState == 2);
	const bool lifeDying = (dbg.lifeState == 1);
	dbg.dead = lifeDead || (lifeDying && !dbg.incap);
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
	// Distinguish "being treated by teammate" vs "self-heal".
   // We ONLY force third-person for self-heal; teammate-treatment should NOT force third-person
   // (it causes flicker and is disorienting in VR).
	dbg.useActionOwner = ReadNetvar<int>(player, 0x1ba4);          // m_useActionOwner
	dbg.useActionTarget = ReadNetvar<int>(player, 0x1ba0);         // m_useActionTarget
	if (dbg.useAction == 1 && entList && localPlayer && HandleValid(dbg.useActionOwner) && HandleValid(dbg.useActionTarget))
	{
		auto* ownerEnt = (C_BaseEntity*)entList->GetClientEntityFromHandle(dbg.useActionOwner);
		auto* targetEnt = (C_BaseEntity*)entList->GetClientEntityFromHandle(dbg.useActionTarget);
		dbg.selfMedkit = (ownerEnt == localPlayer) && (targetEnt == localPlayer);
	}
	dbg.reviveOwner = ReadNetvar<int>(player, 0x1f88);             // m_reviveOwner
	dbg.reviveTarget = ReadNetvar<int>(player, 0x1f8c);            // m_reviveTarget
	dbg.reviving = HandleValid(dbg.reviveOwner) || HandleValid(dbg.reviveTarget);

	if (outDbg)
		*outDbg = dbg;

	// NOTE: user request:
	// - "倒地" (incapacitated) 不强制第三人称
	// Keep other pinned/use/tongue states.
	// Keep other pinned/tongue states. Only self-heal forces third-person from useAction.
	const bool observer = (dbg.observerMode != 0) && (dbg.dead || HandleValid(dbg.observerTarget));
	// NOTE: m_iObserverMode can be transiently non-zero during revive/incap camera transitions.
	// Guard it with either a dead-ish state or a valid observer target to avoid false third-person latching.
	// NOTE: do NOT force 3P for generic useAction; it includes teammate revive/assistance/interaction and can latch 3P.
	return dbg.dead || observer || dbg.ledge || dbg.tongue || dbg.pinned || dbg.selfMedkit;
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
	if (hkRunCommand.pTarget)
		hkRunCommand.enableHook();
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

	// Hook CPrediction::RunCommand via prediction interface vtable.
	void* prediction = m_Game->GetInterface("client.dll", "VClientPrediction001");
	if (!prediction)
		prediction = m_Game->GetInterface("client.dll", "VClientPrediction002");
	if (prediction)
	{
		void*** predictionPtr = reinterpret_cast<void***>(prediction);
		void** predictionVTable = predictionPtr ? *predictionPtr : nullptr;
		constexpr size_t kRunCommandIndex = 17;
		if (predictionVTable && predictionVTable[kRunCommandIndex])
			hkRunCommand.createHook(predictionVTable[kRunCommandIndex], dRunCommand);
		else
			Game::logMsg("RunCommand hook skipped: prediction vtable index %zu unavailable", kRunCommandIndex);
	}
	else
	{
		Game::logMsg("RunCommand hook skipped: VClientPrediction interface not found");
	}

	return 1;
}


ITexture* __fastcall Hooks::dGetRenderTarget(void* ecx, void* edx)
{
	ITexture* result = hkGetRenderTarget.fOriginal(ecx);
	return result;
}

void __fastcall Hooks::dRenderView(void* ecx, void* edx, CViewSetup& setup, CViewSetup& hudViewSetup, int nClearFlags, int whatToDraw)
{
	static EngineThirdPersonCamSmoother s_engineTpCam;

	if (!m_VR->m_CreatedVRTextures)
		m_VR->CreateVRTextures();

	IMatRenderContext* rndrContext = m_Game->m_MaterialSystem->GetRenderContext();
	if (!rndrContext)
	{
		m_VR->HandleMissingRenderContext("Hooks::dRenderView");
		return hkRenderView.fOriginal(ecx, setup, hudViewSetup, nClearFlags, whatToDraw);
	}
	if (m_VR->m_IsVREnabled)
	{
		// Late-latch poses on the render thread so head-turns don't smear under mat_queue_mode=2.
		// This also keeps HUD/viewmodel transforms in lockstep with the same SteamVR pose snapshot.
		m_VR->UpdatePosesAndActions();
		m_VR->UpdateTrackingOncePerCompositorFrame();
	}
	// Keep a consistent pose/state snapshot across both eyes and viewmodel (important for mat_queue_mode=2).
	struct RenderFrameSnapshotGuard
	{
		VR* vr = nullptr;
		bool active = false;
		RenderFrameSnapshotGuard(VR* v) : vr(v)
		{
			if (vr && vr->m_IsVREnabled)
			{
				vr->BeginRenderFrameSnapshot();
				active = true;
			}
		}
		~RenderFrameSnapshotGuard()
		{
			if (active)
				vr->EndRenderFrameSnapshot();
		}
	} renderFrameSnapshotGuard(m_VR);

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
	const bool stateIsDeadOrObserver = tpStateDbg.dead || stateObserver;
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

		if (!s_yawResetInit)
		{
			s_yawResetBase = viewAngles.y;
			s_yawResetInit = true;
		}
		else
		{
			float diff = viewAngles.y - s_yawResetBase;
			// Normalize to [-180, 180] to handle wrap-around.
			diff -= 360.0f * std::floor((diff + 180.0f) / 360.0f);
			if (std::fabs(diff) >= 30.0f)
			{
				m_VR->ResetPosition();
				s_yawResetBase = viewAngles.y;
			}
		}
	}

	if (renderThirdPerson)
	{
		// Render from the engine-provided third-person camera (setup.origin),
		// but aim the camera with the HMD so head look still works in third-person.
		QAngle camAng(viewAngles.x, viewAngles.y, viewAngles.z);
		const VR::RenderFrameSnapshot* snap = m_VR->GetActiveRenderFrameSnapshot();
		const Vector hmdForward = snap ? snap->hmdForward : m_VR->m_HmdForward;
		if (hmdForward.IsZero())
			camAng = engineCamAngles;

		Vector fwd, right, up;
		QAngle::AngleVectors(camAng, &fwd, &right, &up);

		const VR::RenderFrameSnapshot* snapParams = m_VR->GetActiveRenderFrameSnapshot();
		const float vrScale = snapParams ? snapParams->vrScale : m_VR->m_VRScale;
		const float ipdRaw = snapParams ? snapParams->ipd : m_VR->m_Ipd;
		const float ipdScale = snapParams ? snapParams->ipdScale : m_VR->m_IpdScale;
		const float eyeZRaw = snapParams ? snapParams->eyeZ : m_VR->m_EyeZ;
		const float ipd = (ipdRaw * ipdScale * vrScale);
		const float eyeZ = (eyeZRaw * vrScale);

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
			{
				const VR::RenderFrameSnapshot* snapCenter = m_VR->GetActiveRenderFrameSnapshot();
				baseCenter = stateIsDeadOrObserver ? engineCamOrigin : (snapCenter ? snapCenter->hmdPosAbs : m_VR->m_HmdPosAbs);
			}
		}
		else
		{
			{
				const VR::RenderFrameSnapshot* snapCenter = m_VR->GetActiveRenderFrameSnapshot();
				baseCenter = (engineThirdPersonNow || customWalkThirdPersonNow) ? engineCamOrigin : (snapCenter ? snapCenter->hmdPosAbs : m_VR->m_HmdPosAbs);
			}
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

	// Multicore (mat_queue_mode=2) safety:
	// RenderView may return before queued draw calls actually execute.
	// We switch/use render targets immediately after; force the queue to drain here for correctness.
	// NOTE: This reduces multicore rendering benefits; keep it as a minimal sync point.
	if (m_Game && m_Game->m_MaterialSystem)
		m_Game->m_MaterialSystem->FlushEb(true);
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

	// Multicore (mat_queue_mode=2) safety: see comment above (left eye).
	if (m_Game && m_Game->m_MaterialSystem)
		m_Game->m_MaterialSystem->FlushEb(true);

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

				// Multicore safety: offscreen RTTs are often consumed immediately after this call.
				if (m_Game && m_Game->m_MaterialSystem)
					m_Game->m_MaterialSystem->FlushEb(true);

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

			// Multicore safety: offscreen RTTs are often consumed immediately after this call.
			if (m_Game && m_Game->m_MaterialSystem)
				m_Game->m_MaterialSystem->FlushEb(true);

			m_Game->m_EngineClient->SetViewAngles(oldEngineAngles);

			rc->SetRenderTarget(oldRT);
			hkViewport.fOriginal(rc, oldX, oldY, oldW, oldH);

			m_VR->m_SuppressHudCapture = prevSuppress;
		};

	// ----------------------------
	// Scope RTT pass: render from scope camera into vrScope RTT
	// ----------------------------
	if (m_VR->m_CreatedVRTextures && m_VR->ShouldRenderScope() && m_VR->m_ScopeTexture && m_VR->ShouldUpdateScopeRTT())
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
	if (m_VR->m_CreatedVRTextures && m_VR->ShouldRenderRearMirror() && m_VR->m_RearMirrorTexture && m_VR->ShouldUpdateRearMirrorRTT())
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
	m_VR->m_RenderedNewFrame = true;
}

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

void Hooks::dRunCommand(void* ecx, void* edx, C_BasePlayer* player, CUserCmd* cmd, void* moveHelper)
{
	if (m_VR && cmd)
		m_VR->OnPredictionRunCommand(cmd);

	hkRunCommand.fOriginal(ecx, player, cmd, moveHelper);
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

void Hooks::dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
	if (!m_VR->m_CreatedVRTextures)
		return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
	// Diagnostic mode: do not redirect HUD/VGUI into our HUD render target.
	// This allows testing whether multicore rendering corruption is isolated to HUD capture.
	if (m_VR->m_DisableHudRendering)
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

	const bool wasHudPush = m_PushedHud;

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
	if (wasHudPush)
		m_PushedHud = false;
}

void Hooks::dVGui_Paint(void* ecx, void* edx, int mode)
{
	if (!m_VR->m_CreatedVRTextures)
		return hkVgui_Paint.fOriginal(ecx, mode);
	// Detect queued+threaded mode (mat_queue_mode=2)
	bool matQueueMode2 = false;
	if (m_Game && m_Game->m_MaterialSystem)
		matQueueMode2 = (m_Game->m_MaterialSystem->GetThreadMode() == MATERIAL_QUEUED_THREADED);

	// Diagnostic mode: completely skip VGUI paint to remove HUD/UI from the scene.
	if (m_VR->m_DisableHudRendering)
		return;
	// When scope RTT is rendering, don't redirect HUD/VGUI
	if (m_VR->m_SuppressHudCapture)
		return;
	// In mat_queue_mode=2, VGUI paint often runs on the game thread while the actual draw commands
	// execute later on the render thread. We therefore cannot rely on the thread_local HUD-capture
	// marker (m_PushedHud) to decide whether to request full VGUI layers. Always request full VGUI.
	if (matQueueMode2)
		mode |= (PAINT_UIPANELS | PAINT_INGAMEPANELS);
	else if (m_PushedHud)
		mode |= (PAINT_UIPANELS | PAINT_INGAMEPANELS);

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
