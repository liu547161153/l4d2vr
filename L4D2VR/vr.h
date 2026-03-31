#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "openvr.h"
#include "vector.h"
#include <cstdint>
#include <array>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstring>
#define MAX_STR_LEN 256

class Game;
class C_BaseEntity;
class C_BasePlayer;
class C_WeaponCSBase;
class CUserCmd;
class IDirect3DDevice9;
class IDirect3DTexture9;
class IDirect3DSurface9;
class ITexture;
class IMaterial;
class IMatRenderContext;
class IGameEvent;
class IGameEventListener2;
class IGameEventManager2;

struct ViewmodelAdjustment
{
	Vector position;
	QAngle angle;
};


struct TrackedDevicePoseData
{
	std::string TrackedDeviceName;
	Vector TrackedDevicePos;
	Vector TrackedDeviceVel;
	QAngle TrackedDeviceAng;
	QAngle TrackedDeviceAngVel;
};

struct SharedTextureHolder
{
	vr::VRVulkanTextureData_t m_VulkanData{};
	vr::Texture_t m_VRTexture{};
};

using TextureStateMutex = std::recursive_mutex;

struct CustomActionBinding
{
	std::string command;
	std::string releaseCommand;
	std::optional<WORD> virtualKey;
	bool holdVirtualKey = false;
	bool usePressReleaseCommands = false;
};

struct WeaponHapticsProfile
{
	float durationSeconds = 0.0f;
	float frequency = 0.0f;
	float amplitude = 0.0f;
};

struct HapticMixState
{
	bool pending = false;
	float amplitude = 0.0f;
	float frequency = 0.0f;
	float durationSeconds = 0.0f;
	float weight = 0.0f;
	int priority = -1;
	std::chrono::steady_clock::time_point lastSubmit{};
};


class VR
{
public:
	Game* m_Game = nullptr;

	vr::IVRSystem* m_System = nullptr;
	vr::IVRInput* m_Input = nullptr;
	vr::IVROverlay* m_Overlay = nullptr;
	vr::IVRCompositor* m_Compositor = nullptr;

	vr::VROverlayHandle_t m_MainMenuHandle;
	vr::VROverlayHandle_t m_HUDTopHandle;
	std::array<vr::VROverlayHandle_t, 4> m_HUDBottomHandles{};
	// Gun-mounted scope overlay (render-to-texture lens)
	vr::VROverlayHandle_t m_ScopeHandle = vr::k_ulOverlayHandleInvalid;
	// Rear mirror overlay (off-hand)
	vr::VROverlayHandle_t m_RearMirrorHandle = vr::k_ulOverlayHandleInvalid;
	// Hand HUD overlays (raw, controller-anchored)
	vr::VROverlayHandle_t m_LeftWristHudHandle = vr::k_ulOverlayHandleInvalid;
	vr::VROverlayHandle_t m_RightAmmoHudHandle = vr::k_ulOverlayHandleInvalid;


	float m_HorizontalOffsetLeft;
	float m_VerticalOffsetLeft;
	float m_HorizontalOffsetRight;
	float m_VerticalOffsetRight;

	uint32_t m_RenderWidth;
	uint32_t m_RenderHeight;
	uint32_t m_AntiAliasing;
	float m_Aspect;
	float m_Fov;

	vr::VRTextureBounds_t m_TextureBounds[2];
	vr::TrackedDevicePose_t m_Poses[vr::k_unMaxTrackedDeviceCount];

	Vector m_EyeToHeadTransformPosLeft = { 0,0,0 };
	Vector m_EyeToHeadTransformPosRight = { 0,0,0 };

	Vector m_HmdForward;
	Vector m_HmdRight;
	Vector m_HmdUp;

	Vector m_HmdPosLocalInWorld = { 0,0,0 };

	Vector m_LeftControllerForward;
	Vector m_LeftControllerRight;
	Vector m_LeftControllerUp;

	Vector m_RightControllerForwardUnforced = { 0,0,0 };
	Vector m_RightControllerForward;
	Vector m_RightControllerRight;
	Vector m_RightControllerUp;

	Vector m_ViewmodelForward;
	Vector m_ViewmodelRight;
	Vector m_ViewmodelUp;

	Vector m_HmdPosAbs = { 0,0,0 };
	Vector m_HmdPosAbsPrev = { 0,0,0 };
	QAngle m_HmdAngAbs;

	Vector m_HmdPosCorrectedPrev = { 0,0,0 };
	Vector m_HmdPosLocalPrev = { 0,0,0 };
	Vector m_LeftControllerPosSmoothed = { 0,0,0 };
	Vector m_RightControllerPosSmoothed = { 0,0,0 };
	QAngle m_LeftControllerAngSmoothed = { 0,0,0 };
	QAngle m_RightControllerAngSmoothed = { 0,0,0 };

	Vector m_SetupOrigin = { 0,0,0 };
	QAngle m_SetupAngles = { 0,0,0 };
	Vector m_SetupOriginPrev = { 0,0,0 };
	Vector m_CameraAnchor = { 0,0,0 };
	Vector m_SetupOriginToHMD = { 0,0,0 };

	float m_HeightOffset = 0.0;
	bool m_RoomscaleActive = false;
	bool m_IsThirdPersonCamera = false;
	// Death camera flicker guard: after we detect the local player has died,
	// force first-person rendering for a short cooldown to avoid 1P<->3P thrash
	// during Source's deathcam/freeze-cam transitions.
	std::chrono::steady_clock::time_point m_DeathFirstPersonLockEnd{};
	bool m_DeathWasAlivePrev = true;
	// When a CustomAction is bound to +walk (press/release), we can optionally treat it
	// as a signal that the gameplay camera has been forced into a third-person mode
	// (e.g. slide mods that switch to 3P while +walk is held).
	bool m_CustomWalkHeld = false;
	bool m_ThirdPersonRenderOnCustomWalk = false;
	// If enabled, render in third-person by default to avoid camera mode flicker.
	// Only a small whitelist of explicitly-handled cases will remain first-person.
	bool m_ThirdPersonDefault = false;
	// If true, third-person camera placement/orbit follows HMD head turns.
	// If false, the rendered view still follows the HMD, but the third-person camera center/offset
	// is placed using the engine/body camera basis so turning your head does not drag the whole camera.
	bool m_ThirdPersonCameraFollowHmd = false;
	// Optional front-observer mode for third-person rendering.
	// When enabled, 3P camera is placed in front of the player and looks back at the player.
	bool m_ThirdPersonFrontViewEnabled = false;
	// In third-person front view, if true use eye/HMD as the scope+aim source.
	// If false, keep scope+aim driven by right controller (recommended).
	bool m_ThirdPersonFrontScopeFromEye = false;
	bool m_ObserverThirdPerson = false;
	// Map-load / reconnect camera stabilization.
	// Source can transiently report observer-like netvars right after joining/changing maps.
	// If we treat that as "real" observer state, we briefly force third-person then snap back.
	int m_ThirdPersonMapLoadCooldownMs = 1500;
	bool m_ThirdPersonMapLoadCooldownPending = false;
	bool m_HadLocalPlayerPrev = false;
	bool m_WasInGamePrev = false;
	std::chrono::steady_clock::time_point m_ThirdPersonMapLoadCooldownEnd{};

	int m_ThirdPersonHoldFrames = 0;
	Vector m_ThirdPersonViewOrigin = { 0,0,0 };
	QAngle m_ThirdPersonViewAngles = { 0,0,0 };
	// Center of the actual VR render camera used this frame (HMD-aimed 3P camera center).
	// Used to keep aim line / overlays in sync when third-person camera is smoothed.
	Vector m_ThirdPersonRenderCenter = { 0,0,0 };
	bool m_ThirdPersonPoseInitialized = false;
	float m_ThirdPersonCameraSmoothing = 0.85f;
	float m_ThirdPersonVRCameraOffset = 80.0f;
	// Front-view third-person camera local offset in camera basis:
	// x=front/back, y=left/right, z=up/down.
	Vector m_ThirdPersonFrontVRCameraOffset = { 80.0f, 0.0f, 0.0f };
	// Third-person scope overlay local offset in body basis (meters):
	// x=front/back, y=left/right, z=up/down.
	Vector m_ThirdPersonScopeOverlayOffset = { 0.35f, 0.18f, -0.04f };
	Vector m_LeftControllerPosAbs;
	QAngle m_LeftControllerAngAbs;
	Vector m_RightControllerPosAbs;
	QAngle m_RightControllerAngAbs;

	Vector m_ViewmodelPosOffset;
	QAngle m_ViewmodelAngOffset;

	// --- Multicore rendering snapshot bridging (mat_queue_mode!=0) ---
	// Main thread publishes a stable copy of key tracking/view parameters; render thread consumes it
	// and computes per-frame view/controller data from a render-thread pose sample.
	std::atomic<uint32_t> m_RenderViewParamsSeq{ 0 };
	std::atomic<float> m_RenderCameraAnchorX{ 0.0f };
	std::atomic<float> m_RenderCameraAnchorY{ 0.0f };
	std::atomic<float> m_RenderCameraAnchorZ{ 0.0f };
	std::atomic<float> m_RenderRotationOffset{ 0.0f };
	std::atomic<float> m_RenderVRScale{ 1.0f };
	std::atomic<float> m_RenderIpdScale{ 1.0f };
	std::atomic<float> m_RenderEyeZ{ 0.0f };
	std::atomic<float> m_RenderIpd{ 0.065f };
	std::atomic<float> m_RenderHmdPosLocalPrevX{ 0.0f };
	std::atomic<float> m_RenderHmdPosLocalPrevY{ 0.0f };
	std::atomic<float> m_RenderHmdPosLocalPrevZ{ 0.0f };
	std::atomic<float> m_RenderHmdPosCorrectedPrevX{ 0.0f };
	std::atomic<float> m_RenderHmdPosCorrectedPrevY{ 0.0f };
	std::atomic<float> m_RenderHmdPosCorrectedPrevZ{ 0.0f };
	std::atomic<float> m_RenderViewmodelPosOffsetX{ 0.0f };
	std::atomic<float> m_RenderViewmodelPosOffsetY{ 0.0f };
	std::atomic<float> m_RenderViewmodelPosOffsetZ{ 0.0f };
	std::atomic<float> m_RenderViewmodelAngOffsetX{ 0.0f };
	std::atomic<float> m_RenderViewmodelAngOffsetY{ 0.0f };
	std::atomic<float> m_RenderViewmodelAngOffsetZ{ 0.0f };

	// Local-player & camera state snapshot for the render thread (mat_queue_mode!=0).
	// NOTE: These are written under the same seqlock as m_RenderViewParamsSeq.
	std::atomic<uint32_t> m_RenderHasLocalPlayer{ 0 };
	std::atomic<float> m_RenderLocalEyePosX{ 0.0f };
	std::atomic<float> m_RenderLocalEyePosY{ 0.0f };
	std::atomic<float> m_RenderLocalEyePosZ{ 0.0f };
	std::atomic<uint32_t> m_RenderHasViewEntityOverride{ 0 };
	std::atomic<int> m_RenderViewEntityHandle{ 0 };
	std::atomic<uint32_t> m_RenderBeingRevived{ 0 };
	std::atomic<uint32_t> m_RenderRevivingOther{ 0 };
	std::atomic<uint32_t> m_RenderUsingMountedGun{ 0 };
	std::atomic<uint32_t> m_RenderPlayerIncap{ 0 };
	std::atomic<uint32_t> m_RenderPlayerControlledBySI{ 0 };
	std::atomic<uint32_t> m_RenderInThirdPersonMapLoadCooldown{ 0 };

	// Third-person state debug snapshot (subset used by the render hook).
	std::atomic<uint32_t> m_RenderTpWantsThirdPerson{ 0 };
	std::atomic<uint32_t> m_RenderTpObserver{ 0 };
	std::atomic<uint32_t> m_RenderTpDead{ 0 };
	std::atomic<int> m_RenderTpLifeState{ 0 };
	std::atomic<int> m_RenderTpObserverMode{ 0 };
	std::atomic<int> m_RenderTpObserverTarget{ 0 };
	std::atomic<uint32_t> m_RenderTpIncap{ 0 };
	std::atomic<uint32_t> m_RenderTpLedge{ 0 };
	std::atomic<uint32_t> m_RenderTpTongue{ 0 };
	std::atomic<uint32_t> m_RenderTpPinned{ 0 };
	std::atomic<uint32_t> m_RenderTpSelfMedkit{ 0 };

	// Aim-line gating computed on the update thread; render thread only consumes.
	std::atomic<uint32_t> m_RenderAimLineAllowed{ 0 };
	std::atomic<uint32_t> m_RenderAimLineShow{ 0 };


	// Render-thread computed snapshot (updated once per dRenderView call).
	std::atomic<uint32_t> m_RenderFrameSeq{ 0 };
	std::atomic<float> m_RenderViewAngX{ 0.0f };
	std::atomic<float> m_RenderViewAngY{ 0.0f };
	std::atomic<float> m_RenderViewAngZ{ 0.0f };
	std::atomic<float> m_RenderViewOriginLeftX{ 0.0f };
	std::atomic<float> m_RenderViewOriginLeftY{ 0.0f };
	std::atomic<float> m_RenderViewOriginLeftZ{ 0.0f };
	std::atomic<float> m_RenderViewOriginRightX{ 0.0f };
	std::atomic<float> m_RenderViewOriginRightY{ 0.0f };
	std::atomic<float> m_RenderViewOriginRightZ{ 0.0f };
	std::atomic<float> m_RenderLeftControllerPosAbsX{ 0.0f };
	std::atomic<float> m_RenderLeftControllerPosAbsY{ 0.0f };
	std::atomic<float> m_RenderLeftControllerPosAbsZ{ 0.0f };
	std::atomic<float> m_RenderLeftControllerAngAbsX{ 0.0f };
	std::atomic<float> m_RenderLeftControllerAngAbsY{ 0.0f };
	std::atomic<float> m_RenderLeftControllerAngAbsZ{ 0.0f };
	std::atomic<float> m_RenderRightControllerPosAbsX{ 0.0f };
	std::atomic<float> m_RenderRightControllerPosAbsY{ 0.0f };
	std::atomic<float> m_RenderRightControllerPosAbsZ{ 0.0f };
	std::atomic<float> m_RenderRightControllerAngAbsX{ 0.0f };
	std::atomic<float> m_RenderRightControllerAngAbsY{ 0.0f };
	std::atomic<float> m_RenderRightControllerAngAbsZ{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelPosX{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelPosY{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelPosZ{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelAngX{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelAngY{ 0.0f };
	std::atomic<float> m_RenderRecommendedViewmodelAngZ{ 0.0f };

	// Render thread id (captured in dRenderView) used to gate render-only snapshot reads.
	std::atomic<uint32_t> m_RenderThreadId{ 0 };

	// True on the render thread while inside dRenderView when mat_queue_mode!=0.
	static inline thread_local bool t_UseRenderFrameSnapshot = false;

	// --- Pose waiter (mat_queue_mode!=0) ---
	// WaitGetPoses() is a hard pacing barrier. If we call it on the queued render thread, we can
	// destroy mat_queue_mode 2 throughput. Instead, in queued mode we run a tiny "pose waiter" thread
	// that blocks in WaitGetPoses() and publishes a seqlock snapshot. Render/main threads only read.
	std::atomic<bool> m_PoseWaiterStarted{ false };
	std::atomic<bool> m_PoseWaiterEnabled{ false };
	std::atomic<uint32_t> m_PoseWaiterSeq{ 0 };
	std::array<vr::TrackedDevicePose_t, vr::k_unMaxTrackedDeviceCount> m_PoseWaiterPoses{};

	HANDLE m_PoseWaiterEvent = NULL;
	// In queued (mat_queue_mode!=0) rendering, optionally wait for a fresh pose snapshot on the render thread.
	// 0 = no wait (max FPS), >0 = wait up to N ms, -1 = strong sync (wait up to ~50ms).
	int m_QueuedRenderPoseWaitMs = 1;

	// Queued rendering: optional render-thread FPS cap, expressed as a percentage of the HMD refresh rate.
	// 0 = unlimited, 100 = match HMD refresh, 80 = 80% of HMD refresh, etc.
	int m_QueuedRenderMaxFps = 0;

	// Cached HMD refresh rate (Hz) used for FPS caps, etc. Updated on demand (thread-safe).
	std::atomic<float> m_HmdDisplayFrequencyHz{ 0.0f };
	std::atomic<uint32_t> m_HmdDisplayFrequencyHzLastUpdateMs{ 0 };

	inline float GetHmdDisplayFrequencyHz(bool forceRefresh = false)
	{
		float hz = m_HmdDisplayFrequencyHz.load(std::memory_order_relaxed);
		const uint32_t nowMs = static_cast<uint32_t>(::GetTickCount());
		const uint32_t lastMs = m_HmdDisplayFrequencyHzLastUpdateMs.load(std::memory_order_relaxed);

		const bool stale = (hz <= 1.0f) || (lastMs == 0) || ((nowMs - lastMs) > 2000u);
		if ((forceRefresh || stale) && m_System)
		{
			vr::ETrackedPropertyError err = vr::TrackedProp_Success;
			const float v = m_System->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &err);
			if (err == vr::TrackedProp_Success && std::isfinite(v) && v > 1.0f && v < 1000.0f)
			{
				hz = v;
				m_HmdDisplayFrequencyHz.store(hz, std::memory_order_relaxed);
			}
			// Always mark an update attempt so we don't spam the runtime if it fails.
			m_HmdDisplayFrequencyHzLastUpdateMs.store(nowMs, std::memory_order_relaxed);
		}
		return hz;
	}

	inline int GetQueuedRenderMaxFpsEffective(bool forceRefreshHz = false)
	{
		const int pct = std::max(0, m_QueuedRenderMaxFps);
		if (pct <= 0)
			return 0;

		float hz = GetHmdDisplayFrequencyHz(forceRefreshHz);
		if (!(hz > 1.0f))
			hz = 90.0f; // safe fallback

		const double cap = (double)hz * ((double)pct / 100.0);
		int capI = (int)std::lround(cap);
		capI = std::clamp(capI, 1, 360);
		return capI;
	}


	// Queued rendering: when true, the Max FPS cap is only applied when instability is detected
	// (e.g. pose snapshot reuse during locomotion/head turns). This avoids needlessly capping FPS
	// in already-stable scenes. When false, the cap is always enforced when QueuedRenderMaxFps>0.
	bool m_QueuedRenderMaxFpsSmart = true;
	// Queued rendering: limit how many extra render frames may reuse the same WaitGetPoses() snapshot.
	// -1 = disabled, 0 = never reuse (most stable), 1 = allow 1 reuse (2 frames per pose), etc.
	int m_QueuedRenderMaxFramesAhead = -1;

	// Queued rendering: render-thread smoothing time constant (ms) for cameraAnchor/rotationOffset.
	// 0 = off (follow snapshot exactly), 20~80 = typical. Higher = smoother but more latency.
	int m_QueuedRenderViewSmoothMs = 35;

	// Queued rendering: HMD pose smoothing time constant (ms) for visual stability.
	// 0 = off (follow pose exactly), 10~40 = reduce head-turn ghosting when pose-wait is low.
	// Higher = smoother but more latency between head movement and world.
	int m_QueuedRenderHmdSmoothMs = 0;

	// Queued (mat_queue_mode!=0) viewmodel stabilization: prevents first-person viewmodel ghosting
	// when engine viewmodel bob/lag runs on a decoupled thread. 
	bool m_QueuedViewmodelStabilize = true;
	// Global viewmodel stabilization: hard-lock first-person viewmodel pose after engine calc
	// in all queue modes (mat_queue_mode 0/1/2), useful to disable movement bob/sway.
	bool m_ViewmodelDisableMoveBob = false;
	// Debug logging for queued viewmodel stabilization (prints viewmodel pose + engine-produced pose).
	bool  m_QueuedViewmodelStabilizeDebugLog = false;
	float m_QueuedViewmodelStabilizeDebugLogHz = 4.0f; // max prints per second; 0 disables throttling
	// Bullet FX alignment: optional visual-only offset applied to
		// client-side bullet tracers/impact effects so they can be tuned to match the aim line.
		// Units: meters in aim-ray space (X=forward, Y=right, Z=up). Applies in all render modes.
	Vector m_BulletVisualHitOffset = { 0.0f, 0.0f, 0.0f };
	// Additional offset only when queued rendering is enabled (mat_queue_mode!=0).
	// Lets you apply extra correction for render-thread decoupling without affecting single-thread.
	Vector m_QueuedBulletVisualHitOffset = { 0.0f, 0.0f, 0.0f };

	Vector m_ViewmodelPosAdjust = { 0,0,0 };
	QAngle m_ViewmodelAngAdjust = { 0,0,0 };
	ViewmodelAdjustment m_DefaultViewmodelAdjust{ {0,0,0}, {0,0,0} };
	std::unordered_map<std::string, ViewmodelAdjustment> m_ViewmodelAdjustments{};
	std::string m_CurrentViewmodelKey;
	std::string m_LastLoggedViewmodelKey;
	bool m_ViewmodelAdjustmentsDirty = false;
	std::string m_ViewmodelAdjustmentSavePath;
	bool m_ViewmodelAdjustEnabled = true;

	bool m_AdjustingViewmodel = false;
	std::string m_AdjustingKey;
	Vector m_AdjustStartLeftPos = { 0,0,0 };
	QAngle m_AdjustStartLeftAng = { 0,0,0 };
	Vector m_AdjustStartViewmodelPos = { 0,0,0 };
	QAngle m_AdjustStartViewmodelAng = { 0,0,0 };
	Vector m_AdjustStartViewmodelForward = { 0,0,0 };
	Vector m_AdjustStartViewmodelRight = { 0,0,0 };
	Vector m_AdjustStartViewmodelUp = { 0,0,0 };

	Vector m_AimLineStart = { 0,0,0 };
	Vector m_AimLineEnd = { 0,0,0 };

	// Third-person convergence: point hit by the *rendered* aim ray (camera/reticle ray).
	// Bullets may be steered to aim at this point so the rendered line and bullet direction
	// intersect at P. If something blocks the bullet path, it will hit earlier  we do NOT
	// move P to the blocking surface.
	Vector m_AimConvergePoint = { 0,0,0 };
	bool m_HasAimConvergePoint = false;

	Vector m_LastAimDirection = { 0,0,0 };
	Vector m_LastUnforcedAimDirection = { 0,0,0 };
	bool m_HasAimLine = false;
	float m_AimLineThickness = 2.0f;
	bool m_AimLineEnabled = true;
	bool m_AimLineConfigEnabled = true;
	bool m_AimLineOnlyWhenLaserSight = false;
	bool m_ScopeForcingAimLine = false;
	bool m_MeleeAimLineEnabled = true;
	// Mounted gun (minigun/.50cal) state.
	// We force first-person rendering while using the mounted gun (see hooks.cpp).
	// On exit we do a one-shot ResetPosition to avoid accumulated anchor drift.
	bool m_UsingMountedGunPrev = false;
	bool m_ResetPositionAfterMountedGunExitPending = false;
	// When the local player is pinned / controlled by special infected (smoked, pounced, jockey,
	// charger carry/pummel), the body animation can cause the aim line to jitter wildly.
	// Disable the aim line in those states.
	bool m_PlayerControlledBySI = false;
	float m_AimLinePersistence = 0.02f;
	float m_AimLineFrameDurationMultiplier = 2.0f;
	int m_AimLineColorR = 0;
	int m_AimLineColorG = 255;
	int m_AimLineColorB = 0;
	int m_AimLineColorA = 192;
	static constexpr int THROW_ARC_SEGMENTS = 16;
	std::array<Vector, THROW_ARC_SEGMENTS + 1> m_LastThrowArcPoints{};
	bool m_HasThrowArc = false;
	bool m_LastAimWasThrowable = false;
	float m_ThrowArcBaseDistance = 500.0f;
	float m_ThrowArcMinDistance = 20.0f;
	float m_ThrowArcMaxDistance = 2200.0f;
	float m_ThrowArcHeightRatio = 0.25f;
	float m_ThrowArcPitchScale = 6.0f;
	float m_ThrowArcLandingOffset = -90.0f;
	// Tracks the duration of the previous frame so the aim line can persist when the framerate dips.
	float m_LastFrameDuration = 1.0f / 90.0f;

	// --- Spike control / throttling ---
	// Heavy work can happen many times per frame (notably from dDrawModelExecute).
	// These knobs cap how often we do expensive debug-overlay primitives and trace tests.
	float m_AimLineMaxHz = 60.0f;              // caps DrawLineWithThickness calls
	float m_ThrowArcMaxHz = 30.0f;             // caps throw arc overlay calls
	float m_SpecialInfectedOverlayMaxHz = 20.0f; // caps arrow drawing + prewarning refresh per entity
	float m_SpecialInfectedTraceMaxHz = 15.0f;   // caps TraceRay per entity

	std::chrono::steady_clock::time_point m_LastAimLineDrawTime{};
	std::chrono::steady_clock::time_point m_LastThrowArcDrawTime{};
	mutable std::unordered_map<int, std::chrono::steady_clock::time_point> m_LastSpecialInfectedOverlayTime{};
	mutable std::unordered_map<int, std::chrono::steady_clock::time_point> m_LastSpecialInfectedTraceTime{};
	mutable std::unordered_map<int, bool> m_LastSpecialInfectedTraceResult{};

	float m_Ipd;
	float m_EyeZ;

	Vector m_IntendedPositionOffset = { 0,0,0 };

	enum TextureID
	{
		Texture_None = -1,
		Texture_LeftEye,
		Texture_RightEye,
		Texture_HUD,
		Texture_Scope,
		Texture_RearMirror,
		Texture_Blank
	};

	ITexture* m_LeftEyeTexture;
	ITexture* m_RightEyeTexture;
	ITexture* m_HUDTexture;
	ITexture* m_ScopeTexture = nullptr;
	ITexture* m_RearMirrorTexture = nullptr;
	ITexture* m_BlankTexture = nullptr;

	IDirect3DSurface9* m_D9LeftEyeSurface;
	IDirect3DSurface9* m_D9RightEyeSurface;
	IDirect3DSurface9* m_D9HUDSurface;
	IDirect3DSurface9* m_D9ScopeSurface;
	IDirect3DSurface9* m_D9RearMirrorSurface = nullptr;
	IDirect3DSurface9* m_D9BlankSurface;

	SharedTextureHolder m_VKLeftEye;
	SharedTextureHolder m_VKRightEye;
	SharedTextureHolder m_VKBackBuffer;
	SharedTextureHolder m_VKHUD;
	SharedTextureHolder m_VKScope;
	SharedTextureHolder m_VKRearMirror;
	SharedTextureHolder m_VKBlankTexture;

	// Protects VR texture lifecycle and SteamVR texture submissions when render/update threads overlap.
	mutable TextureStateMutex m_TextureMutex;

	// If enabled, scope / rear-mirror render-target textures are created only when the feature is enabled.
	// This can save large chunks of 32-bit VAS when ScopeRTTSize/RearMirrorRTTSize are high.
	bool m_LazyScopeRearMirrorRTT = true;
	// Debug: log Virtual Address Space (VAS) stats at key allocation points.
	bool m_DebugVASLog = false;

	bool m_IsVREnabled = false;
	bool m_IsInitialized = false;
	std::atomic<bool> m_RenderedNewFrame{ false };
	std::atomic<bool> m_RenderedHud{ false };
	// Main menu only needs one blank stereo submit to clear the last scene frame.
	// After that, keep driving the menu with IVROverlay to avoid hammering the compositor.
	bool m_MenuBlankSubmitted = false;
	// Guard against duplicate compositor submits in the same pose frame.
	// Updated by UpdatePosesAndActions(), consumed by SubmitVRTextures().
	std::atomic<uint32_t> m_SubmitPoseToken{ 0 };
	std::atomic<uint32_t> m_LastSubmittedPoseToken{ 0 };
	std::atomic<bool> m_SubmitInFlight{ false };
	std::atomic<uint32_t> m_LastSubmittedCompositorFrameIndex{ 0 };
	// Render-thread -> submit-thread frame handoff (queued/multicore mode).
	// dRenderView increments m_RenderCompletedFrameId and signals m_RenderFrameReadyEvent
	// when a full stereo frame is rendered into eye textures.
	std::atomic<uint32_t> m_RenderCompletedFrameId{ 0 };
	std::atomic<uint32_t> m_LastSubmittedFrameId{ 0 };
	HANDLE m_RenderFrameReadyEvent = NULL;
	// Present-side wait budget (ms) for a fresh rendered frame in mat_queue_mode!=0.
	// 0 disables waiting. Used as an upper bound by adaptive submit-wait logic.
	int m_QueuedSubmitWaitMs = 2;
	// Count of consecutive presents where submit thread observes no newer rendered frame.
	// Used to apply submit wait adaptively only when stale-frame pressure persists.
	std::atomic<uint32_t> m_QueuedSubmitStaleStreak{ 0 };
	// True once VGui_Paint has been redirected into m_HUDTexture for the current VR frame.
	std::atomic<bool> m_HudPaintedThisFrame{ false };
	std::atomic<bool> m_CreatedVRTextures{ false };
	// Used by extra offscreen passes (scope RTT): prevents HUD hooks from hijacking RT stack
	bool m_SuppressHudCapture = false;
	bool m_CompositorExplicitTiming = false;
	bool m_CompositorNeedsHandoff = false;
	TextureID m_CreatingTextureID = Texture_None;

	bool m_PressedTurn = false;
	bool m_PushingThumbstick = false;
	// Any locomotion (keyboard WASD, thumbstick, etc.) detected in the current CreateMove.
	// Used to avoid conflicts with 1:1 roomscale movement/camera decoupling.
	bool m_LocomotionActive = false;
	bool m_CrouchToggleActive = false;
	bool m_VoiceRecordActive = false;
	bool m_QuickTurnTriggered = false;
	bool m_PrimaryAttackDown = false;
	// We drive +attack/+attack2 via ClientCmd for VR actions. In mouse-mode, spamming "-attack"
	// every frame prevents real mouse buttons from working, so we only send +/- when VR actually
	// changes state, and only release if we were the one who pressed.
	bool m_PrimaryAttackCmdOwned = false;
	bool m_SecondaryAttackCmdOwned = false;
	bool m_JumpCmdOwned = false;
	bool m_UseCmdOwned = false;
	bool m_ReloadCmdOwned = false;
	bool m_DuckCmdOwned = false;
	bool m_LeftGripPressedPrev = false;
	bool m_RightGripPressedPrev = false;

	// When GRIP is consumed for inventory switching, suppress SteamVR digital actions from that hand
	// until GRIP is released (prevents GRIP-bound Jump etc).
	bool m_SuppressActionsUntilGripReleaseLeft = false;
	bool m_SuppressActionsUntilGripReleaseRight = false;

	struct ActionCombo
	{
		vr::VRActionHandle_t* primary = nullptr;
		vr::VRActionHandle_t* secondary = nullptr;
	};

	ActionCombo m_VoiceRecordCombo{ &m_ActionCrouch, &m_ActionReload };
	ActionCombo m_QuickTurnCombo{ &m_ActionCrouch, &m_ActionSecondaryAttack };
	ActionCombo m_ViewmodelAdjustCombo{ &m_ActionReload, &m_ActionSecondaryAttack };

	// action set
	vr::VRActionSetHandle_t m_ActionSet;
	vr::VRActiveActionSet_t m_ActiveActionSet;

	// actions
	vr::VRActionHandle_t m_ActionJump;
	vr::VRActionHandle_t m_ActionPrimaryAttack;
	vr::VRActionHandle_t m_ActionSecondaryAttack;
	vr::VRActionHandle_t m_ActionReload;
	vr::VRActionHandle_t m_ActionWalk;
	vr::VRActionHandle_t m_ActionTurn;
	vr::VRActionHandle_t m_ActionUse;
	vr::VRActionHandle_t m_ActionNextItem;
	vr::VRActionHandle_t m_ActionPrevItem;
	vr::VRActionHandle_t m_ActionResetPosition;
	vr::VRActionHandle_t m_ActionCrouch;
	vr::VRActionHandle_t m_ActionFlashlight;
	vr::VRActionHandle_t m_ActionInventoryGripLeft;
	vr::VRActionHandle_t m_ActionInventoryGripRight;
	vr::VRActionHandle_t m_ActionInventoryQuickSwitch;
	vr::VRActionHandle_t m_ActionSpecialInfectedAutoAimToggle;
	vr::VRActionHandle_t m_ActionActivateVR;
	vr::VRActionHandle_t m_MenuSelect;
	vr::VRActionHandle_t m_MenuBack;
	vr::VRActionHandle_t m_MenuUp;
	vr::VRActionHandle_t m_MenuDown;
	vr::VRActionHandle_t m_MenuLeft;
	vr::VRActionHandle_t m_MenuRight;
	vr::VRActionHandle_t m_Spray;
	vr::VRActionHandle_t m_Scoreboard;
	vr::VRActionHandle_t m_ToggleHUD;
	vr::VRActionHandle_t m_Pause;
	vr::VRActionHandle_t m_NonVRServerMovementAngleToggle;
	vr::VRActionHandle_t m_CustomAction1;
	vr::VRActionHandle_t m_CustomAction2;
	vr::VRActionHandle_t m_CustomAction3;
	vr::VRActionHandle_t m_CustomAction4;
	vr::VRActionHandle_t m_CustomAction5;
	vr::VRActionHandle_t m_ActionScopeMagnificationToggle;
	bool m_WeaponHapticsEnabled = true;
	std::unordered_map<std::string, WeaponHapticsProfile> m_WeaponHapticsOverrides;
	WeaponHapticsProfile m_DefaultWeaponHapticsProfile = { 0.018f, 130.0f, 0.32f };
	WeaponHapticsProfile m_MeleeSwingHapticsProfile = { 0.035f, 95.0f, 0.72f };
	WeaponHapticsProfile m_ShoveHapticsProfile = { 0.022f, 120.0f, 0.58f };
	HapticMixState m_LeftHapticMix{};
	HapticMixState m_RightHapticMix{};
	float m_HapticMixMinIntervalSeconds = 0.005f;

	TrackedDevicePoseData m_HmdPose;
	TrackedDevicePoseData m_LeftControllerPose;
	TrackedDevicePoseData m_RightControllerPose;

	float m_RotationOffset = 0;
	std::chrono::steady_clock::time_point m_PrevFrameTime;
	std::chrono::steady_clock::time_point m_LastCompositorErrorLog{};

	float m_TurnSpeed = 0.3;
	bool m_SnapTurning = false;
	float m_SnapTurnAngle = 45.0;
	bool m_LeftHanded = false;
	// If false: movement (walk axis) follows HMD yaw ("head-oriented locomotion").
	// If true:  movement follows the right-hand controller yaw ("hand-oriented locomotion").
	bool m_MoveDirectionFromController = false;
	// Mouse aiming / mouse steering mode (desktop-style).
	// When enabled:
	//  - Mouse X rotates the body (yaw) via m_RotationOffset.
	//  - Mouse Y controls aim pitch (see MouseModePitchAffectsView).
	//  - Viewmodel is anchored to an HMD-relative offset (MouseModeViewmodelAnchorOffset).
	//  - Aim line starts at the anchored viewmodel point, but converges to the mouse-aim ray
	//    at MouseModeAimConvergeDistance (scheme B).
	bool m_MouseModeEnabled = false;
	// Mouse-mode aiming source.
	// If false (default): aim direction is driven by the accumulated mouse pitch + body yaw (m_RotationOffset).
	// If true:            aim direction follows the HMD center ray (view direction), while the aim line origin
	//                     remains at the mouse-mode viewmodel anchor (so we do NOT move the aim line to the HMD).
	bool m_MouseModeAimFromHmd = false;
	// Mouse-mode HMD aim sensitivity (only when MouseModeAimFromHmd is true).
	// 1.0 = 1:1 head rotation, 0 = frozen at enable, >1 amplifies head motion.
	float m_MouseModeHmdAimSensitivity = 1.0f;
	QAngle m_MouseModeHmdAimReferenceAng = { 0.0f, 0.0f, 0.0f };
	bool m_MouseModeHmdAimReferenceInitialized = false;
	// If true, mouse Y also tilts the rendered view (adds a pitch offset on top of head tracking).
	// This makes it possible to aim high/low without physically tilting your head (more like flatscreen).
	bool m_MouseModePitchAffectsView = true;
	// Additional pitch applied to the HMD view (degrees). Only used when MouseModePitchAffectsView is true.
	float m_MouseModeViewPitchOffset = 0.0f;
	// Degrees per mouse-count (tune to taste; negative inverts)
	float m_MouseModeYawSensitivity = 0.022f;
	float m_MouseModePitchSensitivity = 0.022f;
	// Mouse-mode yaw smoothing time constant (seconds).
	//
	// Implementation note (scheme A): we smooth by "draining" a remaining yaw delta.
	// - CreateMove converts cmd->mousedx to a yaw delta in degrees and accumulates it into
	//   m_MouseModeYawDeltaRemainingDeg.
	// - UpdateTracking runs at VR render rate and applies a fraction of that remaining delta per frame.
	//   This guarantees the total applied rotation equals the total mouse input (no "coasting" past the
	//   user's actual movement), while still smoothing the motion.
	// - 0 disables yaw smoothing (legacy: apply yaw directly on CreateMove ticks).
	float m_MouseModeTurnSmoothing = 0.05f;
	// Internal state for scheme A (delta drain).
	float m_MouseModeYawDeltaRemainingDeg = 0.0f;
	bool  m_MouseModeYawDeltaInitialized = false;
	// Legacy (scheme B) target-yaw smoothing fields (kept for compatibility / diff minimization).
	float m_MouseModeYawTarget = 0.0f;      // degrees in [0,360)
	bool m_MouseModeYawTargetInitialized = false;
	float m_MouseModePitchSmoothing = 0.05f; // seconds; 0 disables smoothing (pitch)
	float m_MouseModePitchTarget = 0.0f;      // degrees (aim pitch)
	bool m_MouseModePitchTargetInitialized = false;
	float m_MouseModeViewPitchTargetOffset = 0.0f; // degrees; only used when MouseModePitchAffectsView
	bool m_MouseModeViewPitchTargetOffsetInitialized = false;
	// Independent aim pitch (deg). Initialized to the current HMD pitch on enable.
	float m_MouseAimPitchOffset = 0.0f;
	bool m_MouseAimInitialized = false;
	// HMD-local offset for the viewmodel anchor (meters; scaled by VRScale).
	Vector m_MouseModeViewmodelAnchorOffset = { 0.0f, 0.0f, 0.0f };
	// Optional HMD-local anchor to use while mouse-mode scope is toggled on (meters; scaled by VRScale).
	// If you want a more "ADS"-like viewmodel position when using ScopeRTT in mouse mode, set this.
	Vector m_MouseModeScopedViewmodelAnchorOffset = { 0.0f, 0.0f, 0.0f };
	// Mouse-mode: scope overlay offset relative to the HMD in OpenVR tracking space (meters).
	// x = right, y = up, z = back (towards the player's face).
	// If non-zero, mouse mode will place the scope overlay using the HMD tracking pose
	// so it won't disappear due to mismatched game-units vs meters when using absolute overlays.
	Vector m_MouseModeScopeOverlayOffset = { 0.0f, 0.0f, 0.0f };
	QAngle m_MouseModeScopeOverlayAngleOffset = { 0.0f, 0.0f, 0.0f };
	bool   m_MouseModeScopeOverlayAngleOffsetSet = false;
	// Mouse-mode scope hotkeys (keyboard). Format in config.txt: key:X, key:F1..F12 (see parseVirtualKey).
	std::optional<WORD> m_MouseModeScopeToggleKey;
	std::optional<WORD> m_MouseModeScopeMagnificationKey;
	bool m_MouseModeScopeToggleActive = false;
	bool m_MouseModeScopeToggleKeyDownPrev = false;
	bool m_MouseModeScopeMagnificationKeyDownPrev = false;
	// Per-magnification sensitivity scale (%) for mouse-mode. First entry corresponds to 1x.
	std::vector<float> m_MouseModeScopeSensitivityScales = { 100.0f };
	// Convergence distance (Source units). Aim ray from the viewmodel anchor is steered to intersect
	// the HMD-center ray at this distance. Set <= 0 to disable convergence (use raw viewmodel ray).
	float m_MouseModeAimConvergeDistance = 2048.0f;

	float m_VRScale = 43.2;
	float m_IpdScale = 1.0;
	bool m_HideArms = false;
	bool m_SplitArmsToControllers = false;
	float m_HudDistance = 1.3;
	float m_FixedHudYOffset = 0.0f;
	float m_FixedHudDistanceOffset = 0.0f;
	float m_HudSize = 1.1;
	float m_TopHudCurvature = 0.0f;
	bool m_HudAlwaysVisible = false;
	bool m_HudToggleState = false;
	std::chrono::steady_clock::time_point m_HudChatVisibleUntil{};
	// Queued rendering (mat_queue_mode!=0): keep HUD visibility stable for a short
	// window after a successful HUD capture so transient render-thread misses don't
	// cause top-HUD flicker when frame rate dips.
	std::chrono::steady_clock::time_point m_QueuedHudFreshUntil{};

	// Hand HUD background opacity (0..1). Applies to the panel fill only (text/icons stay opaque).
	float m_LeftWristHudBgAlpha = 0.85f;
	float m_RightAmmoHudBgAlpha = 0.70f;

	// Right ammo HUD: maximum visible width fraction (U max).
	// The HUD now auto-computes a tight width that fits the ammo string and then clamps to this value.
	// 1.0 = no clamp (recommended).
	float m_RightAmmoHudUVMaxU = 1.0f;

	// Hand HUD alpha (legacy, 0..1): treated as an extra BACKGROUND opacity multiplier.
	// We intentionally do NOT apply IVROverlay::SetOverlayAlpha to the whole overlay,
	// because it makes text/bars fade too.
	float m_LeftWristHudAlpha = 1.0f;
	float m_RightAmmoHudAlpha = 1.0f;

	// Left wrist HUD: battery label font scale (1..4) for DrawText5x7.
	int   m_LeftWristHudBatteryTextScale = 1;

	// Hand HUD: client-side temp health (m_healthBuffer) decay rate (HP per second).
	// L4D2 default is ~0.27, but servers can override; expose as config for now.
	float m_HandHudTempHealthDecayRate = 0.27f;

	// ----------------------------
	// Hand HUD overlays (SteamVR overlays, raw textures)
	// ----------------------------
	bool  m_LeftWristHudEnabled = false;
	float m_LeftWristHudWidthMeters = 0.11f;
	float m_LeftWristHudXOffset = -0.02f;
	float m_LeftWristHudYOffset = 0.02f;
	float m_LeftWristHudZOffset = 0.07f;
	QAngle m_LeftWristHudAngleOffset = { -45.0f, 0.0f, 90.0f };

	bool  m_RightAmmoHudEnabled = false;
	float m_RightAmmoHudWidthMeters = 0.04f; // width reduced by ~2/3 by default
	float m_RightAmmoHudXOffset = 0.02f;
	float m_RightAmmoHudYOffset = 0.00f;
	float m_RightAmmoHudZOffset = 0.09f;
	QAngle m_RightAmmoHudAngleOffset = { 0.0f, 0.0f, 0.0f };

	float m_LeftWristHudCurvature = 0.20f;
	bool  m_LeftWristHudShowBattery = true;
	bool  m_LeftWristHudShowTeammates = true;

	// ----------------------------
	// Hand HUD: world-quad mode (HMD-relative overlays using GPU textures)
	//
	// This mode keeps the existing hand HUD drawing code, but uploads the pixels into
	// a dynamic D3D9 texture and presents it as a standard overlay texture (Vulkan)
	// placed relative to the HMD as a quad HUD.
	// ----------------------------
	bool  m_HandHudWorldQuadEnabled = false;
	// If true, keep the HUD attached to left/right controllers (tracked-device-relative),
	// but still use the GPU texture upload path.
	bool  m_HandHudWorldQuadAttachToControllers = false;
	// Distance in meters in front of the HMD (positive). Internally applied as -Z.
	float m_HandHudWorldQuadDistanceMeters = 0.55f;
	// Vertical offset in meters (positive up). Negative moves down.
	float m_HandHudWorldQuadYOffsetMeters = -0.18f;
	// Horizontal gap between left/right panels when both are visible.
	float m_HandHudWorldQuadXGapMeters = 0.01f;
	// Additional rotation applied to both panels (pitch,yaw,roll in degrees).
	QAngle m_HandHudWorldQuadAngleOffset = { -15.0f, 0.0f, 0.0f };

	// Backing textures for world-quad hand HUD (dynamic D3D9 textures mirrored to Vulkan).
	IDirect3DTexture9* m_D9LeftWristHudDynTex = nullptr;
	IDirect3DSurface9* m_D9LeftWristHudDynSurface = nullptr;
	SharedTextureHolder m_VKLeftWristHudDyn{};
	int m_D9LeftWristHudDynW = 0;
	int m_D9LeftWristHudDynH = 0;

	IDirect3DTexture9* m_D9RightAmmoHudDynTex = nullptr;
	IDirect3DSurface9* m_D9RightAmmoHudDynSurface = nullptr;
	SharedTextureHolder m_VKRightAmmoHudDyn{};
	int m_D9RightAmmoHudDynW = 0;
	int m_D9RightAmmoHudDynH = 0;
	// Debug logging for hand HUD update stalls (UpdateHandHudOverlays).
	bool  m_HandHudDebugLog = false;
	float m_HandHudDebugLogHz = 1.0f; // max prints per second; 0 disables throttling
	std::chrono::steady_clock::time_point m_HandHudDebugLastLog{};
	std::chrono::steady_clock::time_point m_HandHudDebugLastCall{};
	std::chrono::steady_clock::time_point m_HandHudDebugLastLeftUpload{};
	std::chrono::steady_clock::time_point m_HandHudDebugLastRightUpload{};
	uint32_t m_HandHudDebugLeftUploadCount = 0;
	uint32_t m_HandHudDebugRightUploadCount = 0;
	int m_HandHudDebugLastLeftSetRawErr = 0;
	int m_HandHudDebugLastRightSetRawErr = 0;
	int m_HandHudDebugLastLeftShowErr = 0;
	int m_HandHudDebugLastRightShowErr = 0;

	// Serialize all IVROverlay calls. OpenVR overlay APIs are not guaranteed thread-safe,
	// and concurrent access can lead to persistent EVROverlayError_RequestFailed.
	std::mutex m_VROverlayMutex{};
	// Hand HUD overlay recovery state (for SetOverlayRaw failures).
	uint32_t m_HandHudLeftConsecutiveRawFails = 0;
	uint32_t m_HandHudRightConsecutiveRawFails = 0;
	std::chrono::steady_clock::time_point m_HandHudLastOverlayRecover{};
	uint32_t m_HandHudOverlayRecoverCount = 0;

	// Double-buffered pixel storage to avoid flicker/tearing when SteamVR reads the same buffer we are updating.
	std::array<std::vector<uint8_t>, 2> m_LeftWristHudPixels{};
	std::array<std::vector<uint8_t>, 2> m_RightAmmoHudPixels{};
	uint8_t m_LeftWristHudPixelsFront = 0;
	uint8_t m_RightAmmoHudPixelsFront = 0;
	int m_LeftWristHudTexW = 256;
	int m_LeftWristHudTexH = 128;
	int m_RightAmmoHudTexW = 256;
	int m_RightAmmoHudTexH = 128;
	// Hand HUD temp-health decay state (per player index).
	// We only get m_healthBuffer (amount) + m_healthBufferTime (start time).
	// The engine computes the decayed remaining value at draw time; we replicate that using wall-clock.
	struct TempHealthDecayState
	{
		float rawBuffer = 0.0f;
		float rawBufferTime = 0.0f;
		std::chrono::steady_clock::time_point wallStart{};
		float lastRemaining = 0.0f;
		bool initialized = false;
	};
	static constexpr size_t kHandHudPlayerSlots = 65; // Source MAX_PLAYERS (incl. 1..64)
	std::array<TempHealthDecayState, kHandHudPlayerSlots> m_HandHudTempHealthStates{};

	// Cached values (avoid redrawing every frame)
	int  m_LastHudHealth = -9999;
	int  m_LastHudTempHealth = -9999;
	int  m_LastHudThrowable = -1;
	int  m_LastHudMedItem = -1;
	int  m_LastHudPillItem = -1;
	int  m_LastHudCommonKills = -9999;
	int  m_LastHudSpecialKills = -9999;
	bool m_LastHudIncap = false;
	bool m_LastHudLedge = false;
	bool m_LastHudThirdStrike = false;
	bool m_LastHudAimTargetVisible = false;
	int  m_LastHudAimTargetIndex = -1;
	int  m_LastHudAimTargetPct = -1;
	int  m_LastHudClip = -9999;
	int  m_LastHudReserve = -9999;
	int  m_LastHudUpg = -9999;
	int  m_LastHudUpgBits = 0;
	int  m_LastHudWeaponId = -1;

	// Right ammo HUD: last drawn hit-target state (for change detection).
	bool m_LastHudHitVisible = false;
	int  m_LastHudHitPct = -1;
	std::uintptr_t m_LastHudHitTag = 0;

	// Hand HUD rendering caches (avoid re-rendering static background)
	std::vector<uint8_t> m_LeftWristHudBgCache{};
	int m_LeftWristHudBgCacheW = 0;
	int m_LeftWristHudBgCacheH = 0;
	uint8_t m_LeftWristHudBgCacheA = 0;
	uint32_t m_LastHudTeammatesHash = 0;
	uint32_t m_LastHudAimTargetNameHash = 0;

	std::vector<uint8_t> m_RightAmmoHudBgCache{};
	int m_RightAmmoHudBgCacheW = 0;
	int m_RightAmmoHudBgCacheH = 0;
	int m_RightAmmoHudBgCacheVisW = 0;
	uint8_t m_RightAmmoHudBgCacheA = 0;
	// Dynamic maxima for percentage thresholds (works even if weapon scripts change clip/reserve sizes)
	int  m_HudMaxClipObserved = 0;
	int  m_HudMaxReserveObserved = 0;

	float m_ControllerSmoothing = 0.0f;
	bool m_ControllerSmoothingInitialized = false;
	float m_HeadSmoothing = 0.0f;
	bool m_HeadSmoothingInitialized = false;
	Vector m_HmdPosSmoothed = { 0,0,0 };
	QAngle m_HmdAngSmoothed = { 0,0,0 };
	CustomActionBinding m_CustomAction1Binding{};
	CustomActionBinding m_CustomAction2Binding{};
	CustomActionBinding m_CustomAction3Binding{};
	CustomActionBinding m_CustomAction4Binding{};
	CustomActionBinding m_CustomAction5Binding{};

	float m_MotionGestureSwingThreshold = 1.1f;
	float m_MotionGesturePushThreshold = 1.8f;
	float m_MotionGestureDownSwingThreshold = 1.0f;
	float m_MotionGestureJumpThreshold = 1.0f;
	float m_MotionGestureCooldown = 0.8f;
	float m_MotionGestureHoldDuration = 0.2f;

	bool m_MotionGestureInitialized = false;
	std::chrono::steady_clock::time_point m_LastGestureUpdateTime{};
	Vector m_PrevLeftControllerLocalPos = { 0,0,0 };
	Vector m_PrevRightControllerLocalPos = { 0,0,0 };
	Vector m_PrevHmdLocalPos = { 0,0,0 };
	std::chrono::steady_clock::time_point m_SecondaryAttackGestureHoldUntil{};
	std::chrono::steady_clock::time_point m_ReloadGestureHoldUntil{};
	std::chrono::steady_clock::time_point m_JumpGestureHoldUntil{};
	std::chrono::steady_clock::time_point m_SecondaryGestureCooldownEnd{};
	std::chrono::steady_clock::time_point m_ReloadGestureCooldownEnd{};
	std::chrono::steady_clock::time_point m_JumpGestureCooldownEnd{};
	float m_InventoryGestureRange = 0.25f;
	Vector m_InventoryChestOffset = { 0.20f, 0.0f, -0.20f };
	Vector m_InventoryBackOffset = { -0.25f, 0.0f, -0.10f };
	Vector m_InventoryLeftWaistOffset = { 0.05f, -0.25f, -0.45f };
	Vector m_InventoryRightWaistOffset = { 0.05f, 0.25f, -0.45f };

	// Inventory quick-switch (Half-Life: Alyx style): press/hold a bind to spawn 4 zones around the RIGHT hand.
	// When enabled, the legacy body-anchored inventory switching is disabled.
	bool m_InventoryQuickSwitchEnabled = false;
	Vector m_InventoryQuickSwitchOffset = { 0.06f, 0.12f, 0.12f }; // meters (forward,right,up) in quick-switch basis
	float m_InventoryQuickSwitchZoneRadius = 0.10f;               // meters, selection radius per zone

	// Runtime state for quick-switch
	bool m_InventoryQuickSwitchActive = false;
	bool m_InventoryQuickSwitchArmed = false;
	// Stored in *tracking-local* Source units (i.e. rightControllerAbs - (CameraAnchor - (0,0,64))).
	// This keeps selection stable while the player translates in-game (stick movement), while
	// still allowing debug rendering by adding the same anchor back to get world space.
	Vector m_InventoryQuickSwitchOrigin = { 0,0,0 };
	Vector m_InventoryQuickSwitchForward = { 1,0,0 };
	Vector m_InventoryQuickSwitchRight = { 0,1,0 };

	// Legacy inventory: swallow Reload/Crouch until release when consumed by inventory switching.
	bool m_BlockReloadUntilRelease = false;
	bool m_BlockCrouchUntilRelease = false;

	// Inventory anchor basis: apply offsets in a BODY space (yaw-only), not head pitch/roll.
	// This makes anchors stable when you look up/down.
	Vector m_InventoryBodyOriginOffset = { 0.0f, 0.0f, 0.0f }; // meters (forward,right,up) in body space

	// Optional front-of-view debug helper markers (purely visual) so anchors behind/at waist are still discoverable.
	float m_InventoryHudMarkerDistance = 0.45f;   // meters forward from head
	float m_InventoryHudMarkerUpOffset = -0.10f;  // meters up (+) / down (-)
	float m_InventoryHudMarkerSeparation = 0.14f; // meters between markers horizontally
	// Auto mat_queue_mode management for multicore rendering.
	// When enabled, the mod will keep mat_queue_mode=1 in menus/loading/pause/scoreboard,
	// and switch to mat_queue_mode=2 once fully in-game.
	bool m_AutoMatQueueMode = false;
	int  m_AutoMatQueueModeLastRequested = -999;
	std::chrono::steady_clock::time_point m_AutoMatQueueModeLastCmdTime{};

	// Auto fps_max in main menu: set fps_max to match HMD refresh rate when VR is active.
	bool m_MenuFpsMaxSent = false;
	int  m_MenuFpsMaxLastHz = 0;

	bool m_DrawInventoryAnchors = false;
	int m_InventoryAnchorColorR = 0;
	int m_InventoryAnchorColorG = 255;
	int m_InventoryAnchorColorB = 255;
	int m_InventoryAnchorColorA = 64;
	bool m_ServerHookFallbackPending = false;
	int m_ServerHookFallbackDelayMs = 3000;
	std::chrono::steady_clock::time_point m_ServerHookFallbackCheckTime{};
	bool m_ForceNonVRServerMovement = false;
	bool m_Roomscale1To1Movement = true;
	float m_Roomscale1To1MaxStepMeters = 0.35f;

	// Roomscale 1:1 movement (ForceNonVRServerMovement=false):
	// - Camera stays 1:1 with the HMD at render rate (no tick-rate stepping).
	// - The player entity is only pulled/teleported when the camera drifts too far away.
	// - Roomscale is optionally disabled while thumbstick locomotion is active to avoid conflicts.
	bool m_Roomscale1To1DecoupleCamera = true;
	bool m_Roomscale1To1UseCameraDistanceChase = true;
	bool m_Roomscale1To1DisableWhileThumbstick = true;
	float m_Roomscale1To1AllowedCameraDriftMeters = 0.25f;
	float m_Roomscale1To1ChaseHysteresisMeters = 0.05f;
	float m_Roomscale1To1MinApplyMeters = 0.02f;
	bool m_Roomscale1To1ChaseActive = false;
	// After control locomotion stops, keep 1:1 chase/apply paused for a few cmds to avoid stop-time pullback.
	int m_Roomscale1To1LocomotionCooldownCmds = 0;

	Vector m_Roomscale1To1PrevCorrectedAbs = {};
	// Accumulate sub-centimeter HMD deltas so slow walking/leaning still produces movement.
	// This is in *meters* in the same corrected space as m_HmdPosCorrectedPrev.
	Vector m_Roomscale1To1AccumMeters = {};
	bool m_Roomscale1To1PrevValid = false;
	// Debug logging for 1:1 roomscale pipeline (encode -> wire -> server apply).
	bool m_Roomscale1To1DebugLog = false;
	float m_Roomscale1To1DebugLogHz = 4.0f; // max prints per second; 0 disables throttling
	std::chrono::steady_clock::time_point m_Roomscale1To1DebugLastEncode{};
	std::chrono::steady_clock::time_point m_Roomscale1To1DebugLastPredict{};
	std::chrono::steady_clock::time_point m_Roomscale1To1DebugLastServer{};
	bool m_NonVRServerMovementAngleOverride = true;
	// Non-VR server movement: make client-side bullet/muzzle effects originate from controller (visual-only).
	bool m_NonVRServerMovementEffectsFromController = true;
	bool m_NonVRServerMovementEffectsDebugLog = false;
	float m_NonVRServerMovementEffectsDebugLogHz = 4.0f;

	// Non-VR server movement: client-side melee gesture -> IN_ATTACK tuning (ForceNonVRServerMovement=true only)
	// These are intentionally separate from generic MotionGesture* knobs.
	float m_NonVRMeleeSwingThreshold = 1.1f;     // controller linear speed threshold (m/s-ish in tracking space)
	float m_NonVRMeleeSwingCooldown = 0.30f;     // seconds between synthetic swings
	float m_NonVRMeleeHoldTime = 0.06f;          // seconds to hold IN_ATTACK after trigger (reduces "dropped swings")
	float m_NonVRMeleeAttackDelay = 0.04f;     // seconds to wait after gesture before starting IN_ATTACK (adds "wind-up")
	float m_NonVRMeleeAimLockTime = 0.12f;       // seconds to lock viewangles after trigger (stabilizes swing direction)
	float m_NonVRMeleeHysteresis = 0.60f;        // re-arm threshold = threshold * hysteresis
	float m_NonVRMeleeAngVelThreshold = 0.0f;    // deg/s, 0 = disabled (optional wrist-flick trigger)
	float m_NonVRMeleeSwingDirBlend = 0.0f;      // 0..1 blend locked aim toward velocity direction
	bool m_RequireSecondaryAttackForItemSwitch = true;

	// ----------------------------
	// Non-VR server aim consistency (ForceNonVRServerMovement)
	//
	// When playing on a server that does NOT run the VR plugin, the server will
	// calculate bullet traces from the player's eye position using cmd->viewangles.
	// To keep the rendered aim line and actual hit point consistent (both in 1P/3P),
	// we solve an "aim hit point" H each frame:
	//   P = point hit by controller ray (or max distance)
	//   H = point hit by eye ray towards P  (what the server will actually hit)
	//   m_NonVRAimAngles = angles from eye -> H (what we send in cmd->viewangles)
	std::chrono::steady_clock::time_point m_LastNonVRAimSolveTime{};
	float m_NonVRAimSolveMaxHz = 90.0f; // throttle traces; 0 disables throttling
	Vector m_NonVRAimDesiredPoint = { 0.0f, 0.0f, 0.0f }; // P
	Vector m_NonVRAimHitPoint = { 0.0f, 0.0f, 0.0f };     // H
	QAngle m_NonVRAimAngles = { 0.0f, 0.0f, 0.0f };
	bool m_HasNonVRAimSolution = false;

	struct RgbColor
	{
		int r;
		int g;
		int b;
	};

	enum class SpecialInfectedType
	{
		None = -1,
		Boomer,
		Smoker,
		Hunter,
		Spitter,
		Jockey,
		Charger,
		Tank,
		Witch,
		Count
	};

	static constexpr int kZombieClassOffset = 0x1c90;
	static constexpr int kLifeStateOffset = 0x147;
	static constexpr int kTeamNumOffset = 0xE4; // DT_BaseEntity::m_iTeamNum
	static constexpr int kObserverModeOffset = 0x1450; // C_BasePlayer::m_iObserverMode
	static constexpr int kObserverTargetOffset = 0x1454; // C_BasePlayer::m_hObserverTarget

	// Common netvars (from offsets.txt) used by hand HUD overlays
	static constexpr int kHealthOffset = 0xEC;               // DT_BasePlayer::m_iHealth
	static constexpr int kMaxHealthOffset = 0x1FDC;           // DT_TerrorPlayer::m_iMaxHealth (also used by special infected players)
	static constexpr int kAmmoArrayOffset = 0xF24;            // DT_BasePlayer::m_iAmmo (int array)
	static constexpr int kHealthBufferOffset = 0x1FAC;        // DT_TerrorPlayer::m_healthBuffer (temporary HP)
	static constexpr int kHealthBufferTimeOffset = 0x1FB0;    // DT_TerrorPlayer::m_healthBufferTime
	static constexpr int kSurvivorCharacterOffset = 0x1C8C;  // DT_TerrorPlayer::m_survivorCharacter
	static constexpr int kIsOnThirdStrikeOffset = 0x1EC0;     // DT_TerrorPlayer::m_bIsOnThirdStrike
	static constexpr int kIsHangingFromLedgeOffset = 0x25EC;  // DT_TerrorPlayer::m_isHangingFromLedge
	static constexpr int kMissionZombieKillsOffset = 0x24AC;   // DT_TerrorPlayer::m_missionZombieKills[0] (current chapter)
	static constexpr int kCheckpointZombieKillsOffset = 0x2488; // DT_TerrorPlayer::m_checkpointZombieKills[0] (checkpoint stats; some servers only update this)
	static constexpr int kCheckpointHeadshotsOffset = 0x2568;   // DT_TerrorPlayer::m_checkpointHeadshots
	static constexpr int kZombieKillsMaxIndex = 8;             // 0=common, 1..8=special categories (smoker..tank/witch)
	// Weapon netvars (from offsets.txt)
	static constexpr int kClip1Offset = 0x984;                // DT_BaseCombatWeapon::m_iClip1
	static constexpr int kPrimaryAmmoTypeOffset = 0x97C;       // DT_BaseCombatWeapon::m_iPrimaryAmmoType
	static constexpr int kUpgradedPrimaryAmmoLoadedOffset = 0xCB8; // m_nUpgradedPrimaryAmmoLoaded
	static constexpr int kUpgradeBitVecOffset = 0xCF0;         // m_upgradeBitVec (incendiary/explosive/laser bits)


	// Aim-line friendly-fire guard (client-side fire suppression)
	vr::VRActionHandle_t m_ActionFriendlyFireBlockToggle;
	bool m_BlockFireOnFriendlyAimEnabled = false; // toggled by SteamVR binding
	bool m_AimLineHitsFriendly = false;           // updated from a ray trace (aim ray)
	// Extra radius (meters) for the friendly-fire aim guard trace.
	// 0 = legacy thin ray; >0 uses a swept hull (fat ray) to reduce misses from spread/latency.
	float m_BlockFireOnFriendlyAimRadiusMeters = 0.0f;


	// Aim-line teammate HUD hint (left wrist HUD):
	// - When the aim line stays on a teammate for >= 2 seconds, show "Name:XX%".
	// - After aim leaves teammates, keep the last shown target for 5 seconds.
	int m_AimTeammateCandidateIndex = -1;
	std::chrono::steady_clock::time_point m_AimTeammateCandidateSince{};
	int m_AimTeammateDisplayIndex = -1;
	std::chrono::steady_clock::time_point m_AimTeammateDisplayUntil{};
	int m_AimTeammateLastRawIndex = -1;
	std::chrono::steady_clock::time_point m_AimTeammateLastRawTime{};

	struct PendingKillSoundHit
	{
		std::uintptr_t entityTag = 0;
		std::chrono::steady_clock::time_point expiresAt{};
		bool headshot = false;
		Vector impactPos = { 0,0,0 };
	};

	struct ActiveKillIndicator
	{
		Vector worldPos = { 0,0,0 };
		std::chrono::steady_clock::time_point startedAt{};
		bool killConfirmed = true;
		bool headshot = false;
		int overlaySlot = -1;
	};

	struct KillIndicatorOverlayTexture
	{
		IDirect3DTexture9* d3dTexture = nullptr;
		IDirect3DSurface9* d3dSurface = nullptr;
		SharedTextureHolder sharedTexture{};
		int width = 0;
		int height = 0;
		uint32_t uploadedFrameIndex = UINT32_MAX;
		bool uploadedFromDecodedFrames = false;
	};

	struct KillIndicatorOverlaySlot
	{
		vr::VROverlayHandle_t overlayHandle = vr::k_ulOverlayHandleInvalid;
		int materialIndex = -1;
		bool visible = false;
	};

	struct PendingKillSoundEvent
	{
		std::uintptr_t entityTag = 0;
		bool headshot = false;
		std::chrono::steady_clock::time_point receivedAt{};
	};

	struct PendingDamageHapticPulse
	{
		float amplitude = 0.0f;
		float frequency = 0.0f;
		float durationSeconds = 0.0f;
		float rightBias = 0.0f;
		int priority = 1;
		bool hasDirection = false;
		std::chrono::steady_clock::time_point receivedAt{};
	};

	enum class DamageFeedbackType
	{
		CommonHit,
		SpecialHit,
		HeavyHit,
		Explosion,
		Fire,
		Acid
	};

	bool m_HitSoundEnabled = true;
	float m_HitSoundPlaybackCooldownSeconds = 0.03f;
	std::string m_HitSoundSpec = "gamesound:VR_HitMarker";
	float m_HitSoundVolume = 0.80f;
	bool m_HitSoundPending = false;
	uint32_t m_HitSoundPendingMergedCount = 0;
	Vector m_HitSoundPendingWorldPos = { 0,0,0 };
	std::chrono::steady_clock::time_point m_HitSoundPendingQueuedAt{};
	bool m_KillSoundEnabled = true;
	float m_KillSoundDetectionWindowSeconds = 0.75f;
	float m_KillSoundPlaybackCooldownSeconds = 0.04f;
	std::string m_KillSoundNormalSpec = "gamesound:VR_KillMarker";
	std::string m_KillSoundHeadshotSpec = "gamesound:VR_HeadshotMarker";
	float m_KillSoundVolume = 0.95f;
	float m_HeadshotSoundVolume = 1.10f;
	float m_FeedbackSoundSpatialBlend = 0.85f;
	float m_FeedbackSoundSpatialRange = 1400.0f;
	int m_FeedbackSoundDebugForceChannel = 0; // -1 = left only, 1 = right only
	bool m_FeedbackSoundDebugLog = false;
	float m_FeedbackSoundDebugLogHz = 1.0f;
	bool m_HitIndicatorEnabled = false;
	bool m_KillIndicatorEnabled = true;
	bool m_KillIndicatorDebugLog = false;
	float m_KillIndicatorDebugLogHz = 1.0f;
	float m_KillIndicatorLifetimeSeconds = 0.85f;
	float m_KillIndicatorSizePixels = 180.0f;
	float m_KillIndicatorRiseUnits = 18.0f;
	float m_KillIndicatorMaxDistance = 4096.0f;
	std::string m_KillIndicatorMaterialBaseSpec = "overlays/2965700751";
	std::vector<PendingKillSoundHit> m_PendingKillSoundHits;
	std::vector<PendingKillSoundEvent> m_PendingKillSoundEvents;
	std::vector<ActiveKillIndicator> m_ActiveKillIndicators;
	int m_LastKillSoundCommonKills = -1;
	int m_LastKillSoundSpecialKills = -1;
	float m_PredictedHitFeedbackDedupWindowSeconds = 0.015f;
	Vector m_LastPredictedHitFeedbackStart = { 0,0,0 };
	Vector m_LastPredictedHitFeedbackDir = { 0,0,0 };
	std::uintptr_t m_LastPredictedHitFeedbackEntityTag = 0;
	std::chrono::steady_clock::time_point m_LastPredictedHitFeedbackTime{};
	uint32_t m_PredictedHitFeedbackShotSerial = 0;
	uint32_t m_LastPredictedHitSoundShotSerial = 0;
	std::chrono::steady_clock::time_point m_LastPredictedHitFeedbackShotTime{};
	std::chrono::steady_clock::time_point m_LastHitSoundPlaybackTime{};
	std::chrono::steady_clock::time_point m_LastKillSoundPlaybackTime{};
	std::chrono::steady_clock::time_point m_LastKillSoundEventRegisterAttempt{};
	std::chrono::steady_clock::time_point m_LastKillIndicatorTrimTime{};
	std::chrono::steady_clock::time_point m_LastKillIndicatorDebugLogTime{};
	std::chrono::steady_clock::time_point m_LastFeedbackSoundDebugLogTime{};
	uint32_t m_KillIndicatorStatsHitSpawned = 0;
	uint32_t m_KillIndicatorStatsKillSpawned = 0;
	uint32_t m_KillIndicatorStatsHitMerged = 0;
	uint32_t m_KillIndicatorStatsRecycled = 0;
	uint32_t m_KillIndicatorStatsTrimmed = 0;
	uint32_t m_KillIndicatorStatsPeakActive = 0;
	uint32_t m_HitSoundStatsQueued = 0;
	uint32_t m_HitSoundStatsMerged = 0;
	uint32_t m_HitSoundStatsFlushed = 0;
	struct FeedbackSoundWorkerJob
	{
		enum class Type
		{
			PlayFile,
			WarmupFile,
			ResetState
		};

		Type type = Type::PlayFile;
		std::string resolvedPath;
		int leftVolume = 1000;
		int rightVolume = 1000;
		bool preferLoadedPathReuse = true;
	};
	std::mutex m_FeedbackSoundWorkerMutex{};
	std::condition_variable m_FeedbackSoundWorkerCv{};
	std::deque<FeedbackSoundWorkerJob> m_FeedbackSoundWorkerJobs;
	std::atomic<bool> m_FeedbackSoundWorkerStarted{ false };
	std::string m_FeedbackSoundWarmupSignature;
	IMaterial* m_KillIndicatorHitMaterial = nullptr;
	IMaterial* m_KillIndicatorNormalMaterial = nullptr;
	IMaterial* m_KillIndicatorHeadshotMaterial = nullptr;
	std::array<KillIndicatorOverlayTexture, 3> m_KillIndicatorOverlayTextures{};
	std::array<KillIndicatorOverlaySlot, 16> m_KillIndicatorOverlaySlots{};
	uint64_t m_NextKillIndicatorOverlaySerial = 1;
	IGameEventManager2* m_KillSoundEventManager = nullptr;
	IGameEventListener2* m_KillSoundEventListener = nullptr;
	bool m_KillSoundEventListenerRegistered = false;
	IGameEventManager2* m_DamageFeedbackEventManager = nullptr;
	IGameEventListener2* m_DamageFeedbackEventListener = nullptr;
	bool m_DamageFeedbackEventListenerRegistered = false;
	std::deque<PendingDamageHapticPulse> m_PendingDamageHapticPulses;
	std::chrono::steady_clock::time_point m_LastDamageHapticPulseTime{};
	float m_DamagePulseMergeWindowSeconds = 0.035f;
	float m_DamagePulseMinIntervalSeconds = 0.020f;
	bool m_DamageFeedbackEnabled = false;
	bool m_DamageDirectionalEnabled = false;
	bool m_DamageSustainEnabled = false;
	bool m_LandingHapticsEnabled = false;
	bool m_CameraShakeHapticsEnabled = false;
	WeaponHapticsProfile m_DamageCommonHapticsProfile = { 0.016f, 135.0f, 0.24f };
	WeaponHapticsProfile m_DamageSpecialHapticsProfile = { 0.020f, 112.0f, 0.38f };
	WeaponHapticsProfile m_DamageHeavyHapticsProfile = { 0.030f, 82.0f, 0.62f };
	WeaponHapticsProfile m_DamageExplosionHapticsProfile = { 0.036f, 72.0f, 0.74f };
	WeaponHapticsProfile m_DamageFireHapticsProfile = { 0.018f, 106.0f, 0.28f };
	WeaponHapticsProfile m_DamageAcidHapticsProfile = { 0.014f, 156.0f, 0.32f };
	float m_DamageScaleStart = 6.0f;
	float m_DamageScalePerPoint = 0.008f;
	float m_DamageScaleMaxBonus = 0.24f;
	float m_DamageAmplitudeMin = 0.05f;
	float m_DamageAmplitudeMax = 1.0f;
	float m_DamageFireSustainSeconds = 1.4f;
	float m_DamageAcidSustainSeconds = 1.6f;
	WeaponHapticsProfile m_DamageFireSustainPulse = { 0.016f, 110.0f, 0.20f };
	WeaponHapticsProfile m_DamageAcidSustainPulse = { 0.010f, 170.0f, 0.24f };
	float m_DamageFireSustainIntervalSeconds = 0.11f;
	float m_DamageAcidSustainIntervalSeconds = 0.08f;
	std::chrono::steady_clock::time_point m_LastDamageFeedbackEventRegisterAttempt{};
	std::chrono::steady_clock::time_point m_AcidSustainUntil{};
	std::chrono::steady_clock::time_point m_FireSustainUntil{};
	std::chrono::steady_clock::time_point m_NextAcidSustainPulse{};
	std::chrono::steady_clock::time_point m_NextFireSustainPulse{};
	std::chrono::steady_clock::time_point m_LastCameraShakeHapticsPulse{};
	std::chrono::steady_clock::time_point m_LandingAirborneSince{};
	bool m_WasOnGroundForHaptics = true;
	float m_LastVerticalSpeedForHaptics = 0.0f;
	float m_LandingPeakDownwardSpeedForHaptics = 0.0f;
	float m_LandingMinFallSpeed = 60.0f;
	float m_LandingMinAirTime = 0.06f;
	float m_LandingFallSpeedRange = 500.0f;
	float m_LandingAmpMin = 0.25f;
	float m_LandingAmpMax = 0.80f;
	float m_LandingFreqMin = 65.0f;
	float m_LandingFreqMax = 85.0f;
	float m_LandingDurMin = 0.018f;
	float m_LandingDurMax = 0.040f;
	bool m_CameraShakeStateInitialized = false;
	Vector m_LastCameraShakeOrigin = { 0,0,0 };
	QAngle m_LastCameraShakeAngles = { 0,0,0 };
	float m_CameraShakeAngleThreshold = 5.0f;
	float m_CameraShakeAngleRange = 18.0f;
	float m_CameraShakePosThreshold = 8.0f;
	float m_CameraShakePosRange = 64.0f;
	float m_CameraShakeHmdAngVelMax = 120.0f;
	float m_CameraShakePulseIntervalSeconds = 0.12f;
	float m_CameraShakePulseAmpMin = 0.12f;
	float m_CameraShakePulseAmpMax = 0.46f;
	float m_CameraShakePulseFreqMin = 80.0f;
	float m_CameraShakePulseFreqMax = 100.0f;
	float m_CameraShakePulseDurMin = 0.012f;
	float m_CameraShakePulseDurMax = 0.028f;

	// Right ammo HUD: show *aimed* special-infected (and Witch) HP%% (client-side, visual-only).
	// - Updated from the aim-ray trace (same trace used for the teammate aim hint).
	// - Hidden immediately when the aim ray leaves the target.
	std::atomic<long long> m_HudAimTargetTimeTicks{ 0 }; // optional: for debugging/telemetry
	std::atomic<std::uintptr_t> m_HudAimTargetTag{ 0 };
	std::atomic<int> m_HudAimTargetMaxHealth{ 0 };
	std::atomic<int> m_HudAimTargetPct{ 0 };

	inline void ClearAmmoHudAimTarget()
	{
		m_HudAimTargetTimeTicks.store(0, std::memory_order_relaxed);
		m_HudAimTargetTag.store(0, std::memory_order_relaxed);
		m_HudAimTargetMaxHealth.store(0, std::memory_order_relaxed);
		m_HudAimTargetPct.store(0, std::memory_order_relaxed);
	}

	inline void NotifyAmmoHudAimTarget(std::uintptr_t tag, int hp, int maxHealthCandidate)
	{
		if (tag == 0 || hp <= 0)
		{
			ClearAmmoHudAimTarget();
			return;
		}

		// Store timestamp (steady_clock ticks)
		const auto now = std::chrono::steady_clock::now();
		m_HudAimTargetTimeTicks.store((long long)now.time_since_epoch().count(), std::memory_order_relaxed);

		const std::uintptr_t prevTag = m_HudAimTargetTag.load(std::memory_order_relaxed);
		int storedMax = (prevTag == tag) ? m_HudAimTargetMaxHealth.load(std::memory_order_relaxed) : 0;

		int maxHp = maxHealthCandidate;
		// Sanity clamp: avoid bogus reads for non-player NPCs.
		if (maxHp <= 0 || maxHp > 20000)
			maxHp = 0;
		if (storedMax > 0)
			maxHp = (maxHp > 0) ? std::max(maxHp, storedMax) : storedMax;
		if (maxHp <= 0)
			maxHp = std::max(1, hp);
		else
			maxHp = std::max(maxHp, hp);

		m_HudAimTargetTag.store(tag, std::memory_order_relaxed);
		m_HudAimTargetMaxHealth.store(maxHp, std::memory_order_relaxed);

		const long long num = (long long)hp * 100LL + (long long)maxHp / 2LL;
		int pct = (int)(num / (long long)maxHp);
		if (pct < 0) pct = 0;
		if (pct > 100) pct = 100;
		if (hp > 0 && pct == 0) pct = 1;
		m_HudAimTargetPct.store(pct, std::memory_order_relaxed);
	}

	// Back-compat: old name used by previous hit-based implementation.
	inline void NotifyAmmoHudHitTarget(std::uintptr_t tag, int hp, int maxHealthCandidate)
	{
		NotifyAmmoHudAimTarget(tag, hp, maxHealthCandidate);
	}


	std::chrono::steady_clock::time_point m_LastFriendlyFireGuardLogTime{};
	// Latch suppression while attack is held (prevents flicker causing intermittent firing).
	bool m_FriendlyFireGuardLatched = false;

	// Auto-repeat semi-auto / single-shot guns while holding IN_ATTACK (client-side input pulse).
	// Config: AutoRepeatSemiAutoFire (true/false)
	//         AutoRepeatSemiAutoFireHz (float, pulses per second; 0 disables)
	bool m_AutoRepeatSemiAutoFire = false;
	float m_AutoRepeatSemiAutoFireHz = 12.0f;
	bool m_AutoRepeatHoldPrev = false;
	std::chrono::steady_clock::time_point m_AutoRepeatNextPulse{};
	// Pump/chrome shotgun + AWP/scout spray-push while auto-repeat is active.
	// Config:
	//   AutoRepeatSprayPushEnabled
	//   AutoRepeatSprayPushDelayTicks
	//   AutoRepeatSprayPushHoldTicks
	bool m_AutoRepeatSprayPushEnabled = true;
	int m_AutoRepeatSprayPushDelayTicks = 0;
	int m_AutoRepeatSprayPushHoldTicks = 1;

	// Auto fast-melee (client-side hold-to-pulse + optional weapon-switch cancel).
	// Config:
	//   AutoFastMelee
	//   AutoFastMeleeUseWeaponSwitch
	bool m_AutoFastMelee = false;
	bool m_AutoFastMeleeUseWeaponSwitch = true;

	// Auto ResetPosition after a level finishes loading.
	// Config: AutoResetPositionAfterLoadSeconds (0 disables)
	float m_AutoResetPositionAfterLoadSeconds = 5.0f;
	bool m_AutoResetPositionPending = false;
	std::chrono::steady_clock::time_point m_AutoResetPositionDueTime{};
	bool m_AutoResetHadLocalPlayerPrev = false;
	// Spectator/observer camera behavior.
	// When enabled, we try to switch the engine spectator camera to free-roaming
	// (spec_mode 6) once per observer session, instead of default chase cam locked
	// to a teammate.
	// Config: ObserverDefaultFreeCam (true/false)
	bool m_ObserverDefaultFreeCam = true;
	bool m_ObserverWasActivePrev = false;
	bool m_ObserverForcedFreeCamThisObserver = false;
	// Some servers place the client into observer mode immediately on join, but
	// early spec_mode commands can be ignored during connection/spawn. We retry a
	// few times until the engine actually reports roaming (mode 6), then stop so
	// the user can still manually change spectator mode afterwards.
	std::chrono::steady_clock::time_point m_ObserverLastFreeCamAttempt{};
	int m_ObserverFreeCamAttemptCount = 0;
	// Observer in-eye (obsMode 4) target switch: auto ResetPosition to re-align anchors.
	int m_ObserverInEyeTargetPrev = 0;
	bool m_ObserverInEyeWasActivePrev = false;
	bool m_ResetPositionAfterObserverTargetSwitchPending = false;
	// CTerrorPlayer netvars (from offsets.txt). These are used for a special-case in the
	// friendly-fire aim guard: if the aim ray hits a teammate who is currently pinned/
	// controlled, we allow a "see-through" trace to hit the attacker behind them.
	// NOTE: These offsets can change between game builds.
	static constexpr int kIsIncapacitatedOffset = 0x1EA9; // DT_TerrorPlayer::m_isIncapacitated
	static constexpr int kTongueOwnerOffset = 0x1F6C; // DT_TerrorPlayer::m_tongueOwner
	static constexpr int kPummelAttackerOffset = 0x2720; // DT_TerrorPlayer::m_pummelAttacker
	static constexpr int kCarryAttackerOffset = 0x2714; // DT_TerrorPlayer::m_carryAttacker
	static constexpr int kPounceAttackerOffset = 0x272C; // DT_TerrorPlayer::m_pounceAttacker
	static constexpr int kJockeyAttackerOffset = 0x274C; // DT_TerrorPlayer::m_jockeyAttacker
	// Mounted gun (fixed machine-gun / turret) support:
	// When the player is using a mounted gun, their aim ray can intersect the gun/base itself,
	// causing the aim line + third-person convergence point to jitter wildly.
	// We treat the mounted-gun entity as "transparent" for aim traces by skipping the current use-entity.
	// NOTE: These offsets can change between game builds.
	static constexpr int kUsingMountedGunOffset = 0x1EBA;  // DT_TerrorPlayer::m_usingMountedGun
	static constexpr int kUsingMountedWeaponOffset = 0x1EBB;  // DT_TerrorPlayer::m_usingMountedWeapon (minigun/gatling uses this)
	static constexpr int kUseEntityHandleOffset = 0x1480;  // DT_BasePlayer::m_hUseEntity
	bool m_SpecialInfectedArrowEnabled = false;
	bool m_SpecialInfectedDebug = false;
	float m_SpecialInfectedArrowSize = 12.0f;
	float m_SpecialInfectedArrowHeight = 36.0f;
	float m_SpecialInfectedArrowThickness = 0.0f;
	RgbColor m_SpecialInfectedArrowDefaultColor{ 255, 64, 0 };
	std::array<RgbColor, static_cast<size_t>(SpecialInfectedType::Count)> m_SpecialInfectedArrowColors{
		RgbColor{ 120, 220, 80 },   // Boomer
		RgbColor{ 180, 80, 255 },   // Smoker
		RgbColor{ 0, 170, 255 },    // Hunter
		RgbColor{ 60, 220, 120 },   // Spitter
		RgbColor{ 255, 140, 20 },   // Jockey
		RgbColor{ 0, 200, 200 },    // Charger
		RgbColor{ 240, 40, 40 },    // Tank
		RgbColor{ 255, 255, 255 }   // Witch
	};
	float m_SpecialInfectedBlindSpotDistance = 105.0f;
	float m_SpecialInfectedBlindSpotWarningDuration = 0.5f;
	bool m_SpecialInfectedBlindSpotWarningActive = false;
	std::chrono::steady_clock::time_point m_LastSpecialInfectedWarningTime{};
	float m_SpecialInfectedPreWarningEvadeDistance = 260.0f;
	float m_SpecialInfectedPreWarningEvadeCooldown = 0.85f;
	int m_LastSpecialInfectedEvadeEntityIndex = -1;
	bool m_SpecialInfectedPreWarningEvadeArmed = true;
	std::chrono::steady_clock::time_point m_SpecialInfectedPreWarningEvadeCooldownEnd{};
	bool m_SpecialInfectedPreWarningEvadeTriggered = false;
	float m_SpecialInfectedWarningSecondaryHoldDuration = 0.15f;
	float m_SpecialInfectedWarningPostAttackDelay = 0.1f;
	float m_SpecialInfectedWarningJumpHoldDuration = 0.2f;
	bool m_SpecialInfectedWarningActionEnabled = false;
	float m_SpecialInfectedPreWarningDistance = 750.0f;
	float m_SpecialInfectedPreWarningTargetUpdateInterval = 0.1f;
	float m_SpecialInfectedPreWarningAimAngle = 5.0f;
	float m_SpecialInfectedPreWarningAimSnapDistance = 18.0f;
	float m_SpecialInfectedPreWarningAimReleaseDistance = 28.0f;
	bool m_SpecialInfectedPreWarningAutoAimConfigEnabled = false;
	bool m_SpecialInfectedPreWarningAutoAimEnabled = false;
	bool m_SpecialInfectedPreWarningActive = false;
	bool m_SpecialInfectedPreWarningInRange = false;
	Vector m_SpecialInfectedPreWarningTarget = { 0.0f, 0.0f, 0.0f };
	int m_SpecialInfectedPreWarningTargetEntityIndex = -1;
	bool m_SpecialInfectedPreWarningTargetIsPlayer = false;
	float m_SpecialInfectedPreWarningTargetDistanceSq = std::numeric_limits<float>::max();
	Vector m_SpecialInfectedAutoAimDirection = { 0.0f, 0.0f, 0.0f };
	float m_SpecialInfectedAutoAimLerp = 0.4f;
	float m_SpecialInfectedAutoAimCooldown = 0.5f;
	std::chrono::steady_clock::time_point m_SpecialInfectedAutoAimCooldownEnd{};
	std::array<Vector, static_cast<size_t>(SpecialInfectedType::Count)> m_SpecialInfectedPreWarningAimOffsets{
		Vector{ 0.0f, 0.0f, 0.0f }, // Boomer
		Vector{ 0.0f, 0.0f, 0.0f }, // Smoker
		Vector{ 0.0f, 0.0f, 0.0f }, // Hunter
		Vector{ 0.0f, 0.0f, 0.0f }, // Spitter
		Vector{ 0.0f, 0.0f, 0.0f }, // Jockey
		Vector{ 0.0f, 0.0f, 0.0f }, // Charger
		Vector{ 0.0f, 0.0f, 0.0f }, // Tank
		Vector{ 0.0f, 0.0f, 0.0f }  // Witch
	};
	std::chrono::steady_clock::time_point m_LastSpecialInfectedPreWarningSeenTime{};
	std::chrono::steady_clock::time_point m_LastSpecialInfectedPreWarningTargetUpdateTime{};
	Vector m_SpecialInfectedWarningTarget = { 0.0f, 0.0f, 0.0f };
	bool m_SpecialInfectedWarningTargetActive = false;
	bool m_SuppressPlayerInput = false;
	enum class SpecialInfectedWarningActionStep
	{
		None,
		PressSecondaryAttack,
		ReleaseSecondaryAttack,
		PressJump,
		ReleaseJump
	};
	SpecialInfectedWarningActionStep m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::None;
	bool m_SpecialInfectedWarningAttack2CmdOwned = false;
	bool m_SpecialInfectedWarningJumpCmdOwned = false;
	std::chrono::steady_clock::time_point m_SpecialInfectedWarningNextActionTime{};
	int m_AimLineWarningColorR = 255;
	int m_AimLineWarningColorG = 255;
	int m_AimLineWarningColorB = 0;

	// ----------------------------
	// Gun-mounted scope (RTT overlay)
	// ----------------------------
	bool  m_ScopeEnabled = false;
	int   m_ScopeRTTSize = 1024;               // square RTT size in pixels
	float m_ScopeRTTMaxHz = 90.0f;
	std::chrono::steady_clock::time_point m_LastScopeRTTRenderTime{};
	float m_ScopeFov = 20.0f;                  // smaller = more zoom
	float m_ScopeZNear = 2.0f;                 // game units
	std::vector<float> m_ScopeMagnificationOptions{ 20.0f, 15.0f, 10.0f, 5.0f };
	size_t m_ScopeMagnificationIndex = 0;

	// Scope camera pose relative to gun hand (game units, in controller basis fwd/right/up)
	Vector m_ScopeCameraOffset = { 10.0f, 0.0f, 2.0f };
	QAngle m_ScopeCameraAngleOffset = { 0.0f, 0.0f, 0.0f };

	// Overlay placement relative to tracked device (meters, in controller local space)
	float  m_ScopeOverlayWidthMeters = 0.06f;
	float  m_ScopeOverlayXOffset = 0.00f;
	float  m_ScopeOverlayYOffset = 0.00f;
	float  m_ScopeOverlayZOffset = 0.10f;
	QAngle m_ScopeOverlayAngleOffset = { 0.0f, 0.0f, 0.0f };
	// If true, when scoped-in the aim line is rendered only during the scope RTT pass.
	bool  m_ScopeAimLineOnlyInScope = true;
	// If true, hide the local player model while rendering scope RTT (prevents head/body obstruction).
	bool  m_ScopeHideLocalPlayerModelInScope = true;

	// Look-through activation (HMD -> scope camera)
	bool  m_ScopeRequireLookThrough = true;
	float m_ScopeLookThroughDistanceMeters = 0.12f;
	float m_ScopeLookThroughAngleDeg = 12.0f;
	bool  m_ScopeOverlayAlwaysVisible = true;
	float m_ScopeOverlayIdleAlpha = 0.35f;
	// Scope stabilization (visual only): smooth the scope RTT camera pose when scoped-in.
	// This reduces high-magnification jitter without changing shooting / aim direction.
	bool  m_ScopeStabilizationEnabled = true;
	float m_ScopeStabilizationMinCutoff = 1.0f;  // Hz (lower = smoother, more latency)
	float m_ScopeStabilizationBeta = 0.08f;      // responsiveness to fast motion
	float m_ScopeStabilizationDCutoff = 1.0f;    // Hz (derivative low-pass cutoff)

	// Scoped aim sensitivity scaling (mouse-style ADS / zoom sensitivity).
	// Multiplies controller aim delta while scoped-in:
	//  - 1.0 = unchanged
	//  - 0.8 = 80% sensitivity (slower)
	// Supports per-magnification values via config: ScopeAimSensitivityScale=100,85,70,55
	std::vector<float> m_ScopeAimSensitivityScales{ 1.0f };
	bool   m_ScopeAimSensitivityInit = false;
	QAngle m_ScopeAimSensitivityBaseAng = { 0.0f, 0.0f, 0.0f };

	// Runtime state
	Vector m_ScopeCameraPosAbs = { 0.0f, 0.0f, 0.0f };
	QAngle m_ScopeCameraAngAbs = { 0.0f, 0.0f, 0.0f };
	bool   m_ScopeActive = false;

	bool   m_ScopeWeaponIsFirearm = false;

	// Scope stabilization filter state (One Euro filter)
	bool   m_ScopeStabilizationInit = false;
	Vector m_ScopeStabPos = { 0.0f, 0.0f, 0.0f };
	Vector m_ScopeStabPosDeriv = { 0.0f, 0.0f, 0.0f };
	QAngle m_ScopeStabAng = { 0.0f, 0.0f, 0.0f };
	QAngle m_ScopeStabAngDeriv = { 0.0f, 0.0f, 0.0f };
	std::chrono::steady_clock::time_point m_ScopeStabilizationLastTime{};

	Vector GetScopeCameraAbsPos() const { return m_ScopeCameraPosAbs; }
	QAngle GetScopeCameraAbsAngle() const { return m_ScopeCameraAngAbs; }
	bool   IsMouseModeScopeActive() const { return m_MouseModeEnabled && m_ScopeEnabled && m_ScopeWeaponIsFirearm && m_MouseModeScopeToggleActive; }
	float  GetMouseModeScopeSensitivityScale() const
	{
		if (!IsMouseModeScopeActive())
			return 1.0f;
		const int idx = std::max(0, static_cast<int>(m_ScopeMagnificationIndex));
		if (m_MouseModeScopeSensitivityScales.empty())
			return 1.0f;
		const int clamped = std::min(idx, (int)m_MouseModeScopeSensitivityScales.size() - 1);
		return std::clamp(m_MouseModeScopeSensitivityScales[clamped] / 100.0f, 0.05f, 2.0f);
	}
	bool   IsScopeActive() const { return m_ScopeEnabled && (m_ScopeActive || IsMouseModeScopeActive()); }
	bool   ShouldRenderScope() const
	{
		const bool forceScopeForThirdPersonFrontView = m_ThirdPersonFrontViewEnabled && m_IsThirdPersonCamera;
		return m_ScopeEnabled
			&& (m_ScopeWeaponIsFirearm || forceScopeForThirdPersonFrontView)
			&& (m_ScopeOverlayAlwaysVisible || IsScopeActive() || forceScopeForThirdPersonFrontView);
	}
	bool   ShouldUpdateScopeRTT();
	void   ToggleMouseModeScope();
	void   CycleScopeMagnification();
	void   UpdateScopeAimLineState();

	// ----------------------------
	// Rear mirror (off-hand)
	// ----------------------------
	bool  m_RearMirrorEnabled = false;
	// If enabled, the rear mirror overlay/RTT stays hidden most of the time,
	// and only pops up briefly when a special infected is detected behind you.
	bool  m_RearMirrorShowOnlyOnSpecialWarning = false;
	// Seconds to keep the mirror visible after a special infected warning.
	float m_RearMirrorSpecialShowHoldSeconds = 0.50f;
	std::chrono::steady_clock::time_point m_LastRearMirrorAlertTime{};
	int   m_RearMirrorRTTSize = 512;
	float m_RearMirrorRTTMaxHz = 45.0f;
	std::chrono::steady_clock::time_point m_LastRearMirrorRTTRenderTime{};
	float m_RearMirrorFov = 85.0f;
	float m_RearMirrorZNear = 6.0f;

	Vector m_RearMirrorCameraOffset = { 0.0f, 0.0f, 0.0f };
	QAngle m_RearMirrorCameraAngleOffset = { 0.0f, 0.0f, 0.0f };

	float  m_RearMirrorOverlayWidthMeters = 0.10f;
	float  m_RearMirrorOverlayXOffset = -0.01f;
	float  m_RearMirrorOverlayYOffset = 0.02f;
	float  m_RearMirrorOverlayZOffset = 0.08f;
	QAngle m_RearMirrorOverlayAngleOffset = { 0.0f, 180.0f, 0.0f };
	float  m_RearMirrorAlpha = 1.0f;

	// If true, mirror the rear-mirror texture horizontally (left/right).
	bool  m_RearMirrorFlipHorizontal = false;

	// When the aim line/ray intersects the rear-mirror overlay in view, hide the mirror to avoid blocking aim.
	bool  m_RearMirrorHideWhenAimLineHits = true;
	float m_RearMirrorAimLineHideHoldSeconds = 0.08f;
	std::chrono::steady_clock::time_point m_RearMirrorAimLineHideUntil{};

	// Rear mirror hint: when special-infected arrows are visible in the mirror pass,
	// temporarily enlarge the rear-mirror overlay (2x width).
	// Distance is in Source units (same as SpecialInfected* distances). <= 0 disables this hint.
	float  m_RearMirrorSpecialWarningDistance = 180.0f;
	float  m_RearMirrorSpecialEnlargeHoldSeconds = 0.35f;
	bool   m_ScopeRenderingPass = false;
	bool   m_RearMirrorRenderingPass = false;
	bool   m_RearMirrorSawSpecialThisPass = false;	// set from DrawModelExecute during mirror RTT pass
	bool   m_RearMirrorSpecialEnlargeActive = false;
	std::chrono::steady_clock::time_point m_LastRearMirrorSpecialSeenTime{};

	Vector m_RearMirrorCameraPosAbs = { 0.0f, 0.0f, 0.0f };
	QAngle m_RearMirrorCameraAngAbs = { 0.0f, 0.0f, 0.0f };

	Vector GetRearMirrorCameraAbsPos() const { return m_RearMirrorCameraPosAbs; }
	QAngle GetRearMirrorCameraAbsAngle() const { return m_RearMirrorCameraAngAbs; }
	bool   ShouldRenderRearMirror() const;
	bool   ShouldUpdateRearMirrorRTT();
	void   NotifyRearMirrorSpecialWarning();

	VR() {};
	VR(Game* game);
	int SetActionManifest(const char* fileName);
	void InstallApplicationManifest(const char* fileName);
	void Update();
	void UpdateAutoMatQueueMode();
	void CreateVRTextures();
	void EnsureOpticsRTTTextures();
	void LogVAS(const char* tag);
	void HandleMissingRenderContext(const char* location);
	void SubmitVRTextures();
	void LogCompositorError(const char* action, vr::EVRCompositorError error);
	void RepositionOverlays();
	void UpdateRearMirrorOverlayTransform();
	void UpdateScopeOverlayTransform();
	void UpdateHandHudOverlays();
	void DestroyHandHudWorldQuadTextures();
	void GetPoses();
	bool UpdatePosesAndActions();
	void GetViewParameters();
	void ProcessMenuInput();
	void ProcessInput();
	void SendVirtualKey(WORD virtualKey);
	void SendVirtualKeyDown(WORD virtualKey);
	void SendVirtualKeyUp(WORD virtualKey);
	void SendFunctionKey(WORD virtualKey);
	VMatrix VMatrixFromHmdMatrix(const vr::HmdMatrix34_t& hmdMat);
	vr::HmdMatrix34_t VMatrixToHmdMatrix(const VMatrix& vMat);
	vr::HmdMatrix34_t GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole);
	vr::HmdMatrix34_t BuildThirdPersonSubmitPose() const;
	bool CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole);
	QAngle GetLeftControllerAbsAngle();
	Vector GetLeftControllerAbsPos();
	QAngle GetRightControllerAbsAngle();
	Vector GetRightControllerAbsPos();
	Vector GetRecommendedViewmodelAbsPos();
	QAngle GetRecommendedViewmodelAbsAngle();
	// Mouse-mode: compute the eye-center ray used for aiming (mouse pitch+yaw or HMD-based, optionally sensitivity-scaled).
	void GetMouseModeEyeRay(Vector& eyeDirOut, QAngle* eyeAngOut = nullptr);
	void UpdateTracking();
	void UpdateMotionGestures(C_BasePlayer* localPlayer);
	bool UpdateThirdPersonViewState(const Vector& cameraOrigin, const Vector& cameraAngles);
	Vector GetViewAngle();
	// Yaw (degrees) used as the movement basis for the walk axis.
	// Default: HMD yaw. Optional: right-controller yaw (hand-oriented locomotion).
	float GetMovementYawDeg();
	Vector GetViewOriginLeft();
	Vector GetViewOriginRight();
	Vector GetThirdPersonViewOrigin() const { return m_ThirdPersonViewOrigin; }
	QAngle GetThirdPersonViewAngles() const { return m_ThirdPersonViewAngles; }
	bool IsThirdPersonCameraActive() const { return m_IsThirdPersonCamera; }
	// Death flicker guard (see m_DeathFirstPersonLockEnd).
	void RefreshDeathFirstPersonLock(const C_BasePlayer* localPlayer);
	bool IsDeathFirstPersonLockActive() const;
	// Map-load / reconnect cooldown: suppress observer-driven 3P latching for a short window.
	bool IsThirdPersonMapLoadCooldownActive() const;
	bool PressedDigitalAction(vr::VRActionHandle_t& actionHandle, bool checkIfActionChanged = false);
	bool GetDigitalActionData(vr::VRActionHandle_t& actionHandle, vr::InputDigitalActionData_t& digitalDataOut);
	bool GetAnalogActionData(vr::VRActionHandle_t& actionHandle, vr::InputAnalogActionData_t& analogDataOut);
	void ResetPosition();
	void GetPoseData(vr::TrackedDevicePose_t& poseRaw, TrackedDevicePoseData& poseOut);
	void PoseWaiterThreadMain();
	bool ReadPoseWaiterSnapshot(vr::TrackedDevicePose_t* outPoses, uint32_t* outSeq = nullptr) const;
	// leftHand follows the project's gameplay hand ordering after LeftHanded remapping.
	bool IsGameplayHandLeftPhysical(bool leftHand) const;
	vr::TrackedDeviceIndex_t GetPhysicalControllerIndexForHand(bool leftHand) const;
	void TriggerLegacyHapticPulse(vr::TrackedDeviceIndex_t deviceIndex, float durationSeconds, float amplitude) const;
	void TriggerPhysicalHandHapticPulse(bool leftHand, float durationSeconds, float frequency, float amplitude, int priority = 1);
	void FlushHapticMixer();
	WeaponHapticsProfile GetWeaponHapticsProfile(int weaponId) const;
	void TriggerWeaponFireHaptics(int weaponId, bool leftHand = false);
	void TriggerMeleeSwingHaptics(bool leftHand = false);
	void TriggerShoveHaptics(bool leftHand = false);
	void ParseConfigFile();
	void ParseHapticsConfigFile();
	void LoadViewmodelAdjustments();
	void SaveViewmodelAdjustments();
	void RefreshActiveViewmodelAdjustment(C_BasePlayer* localPlayer);
	ViewmodelAdjustment& EnsureViewmodelAdjustment(const std::string& key);
	std::string BuildViewmodelAdjustKey(C_WeaponCSBase* weapon) const;
	std::string WeaponIdToString(int weaponId) const;
	std::string NormalizeViewmodelAdjustKey(const std::string& rawKey) const;
	std::string GetMeleeWeaponName(C_WeaponCSBase* weapon) const;
	void WaitForConfigUpdate();
	bool GetWalkAxis(float& x, float& y);
	void UpdateNonVRAimSolution(C_BasePlayer* localPlayer);
	// Friendly-fire aim guard:
	// - m_AimLineHitsFriendly is computed from a ray trace and may flicker at hitbox edges.
	// - While the attack button is held, flicker can effectively create press/release edges.
	// To make this robust, we compute the friendly-hit from the current aim ray on CreateMove
	// ticks and latch suppression until the user releases attack.
	bool ShouldSuppressPrimaryFire(const CUserCmd* cmd, C_BasePlayer* localPlayer);
	bool UpdateFriendlyFireAimHit(C_BasePlayer* localPlayer);
	void UpdateAimTeammateHudTarget(C_BasePlayer* localPlayer, const Vector& start, const Vector& end, bool aimLineActive);
	bool GetAimTeammateHudInfo(int& outPlayerIndex, int& outPercent, char* outName, size_t outNameSize);
	int GetIncapMaxHealth() const;
	void BeginPredictedHitFeedbackShot();
	void RegisterPotentialKillSoundHit(const Vector& start, const QAngle& angles);
	void UpdateKillSoundFeedback();
	void EnsureKillSoundEventListener();
	void HandleKillSoundGameEvent(IGameEvent* event);
	void EnsureDamageFeedbackEventListener();
	void HandleDamageFeedbackGameEvent(IGameEvent* event);
	void UpdateDamageFeedback();
	DamageFeedbackType ClassifyDamageFeedbackType(const char* weaponName, int damage) const;
	WeaponHapticsProfile GetDamageHapticsProfile(DamageFeedbackType type) const;
	void TriggerImpactHapticsBothHands(float amplitude, float frequency, float durationSeconds, int priority = 1);
	void TriggerDirectionalDamageHaptics(float amplitude, float frequency, float durationSeconds, float rightBias, int priority = 1);
	void QueuePendingKillSoundEvent(std::uintptr_t entityTag, bool headshot);
	bool ConsumePendingKillSoundEvent(std::chrono::steady_clock::time_point now, bool& outHeadshot, std::uintptr_t& outEntityTag);
	bool ReadLocalKillCounters(C_BasePlayer* localPlayer, int& outCommon, int& outSpecial) const;
	bool ReadLocalKillCounters(C_BasePlayer* localPlayer, int& outCommon, int& outSpecial, char* outSource) const;
	bool ReadLocalHeadshotCounter(C_BasePlayer* localPlayer, int& outHeadshots) const;
	bool IsKillSoundTargetEntity(const C_BaseEntity* entity) const;
	bool ConsumePendingKillSoundHit(std::uintptr_t preferredEntityTag, std::chrono::steady_clock::time_point now, Vector* outImpactPos = nullptr);
	bool FindPendingKillSoundHit(std::uintptr_t preferredEntityTag, std::chrono::steady_clock::time_point now, Vector* outImpactPos = nullptr);
	void PlayHitSound(const Vector* worldPos = nullptr);
	void PlayKillSound(bool headshot, const Vector* worldPos = nullptr);
	bool TryPlayKillSoundSpec(const std::string& spec, float baseVolume = 1.0f, const Vector* worldPos = nullptr, bool preferLoadedPathReuse = true);
	void QueueHitSoundPlayback(const Vector* worldPos = nullptr);
	void FlushPendingHitSound(std::chrono::steady_clock::time_point now);
	void ComputeFeedbackSoundStereoVolumes(const Vector* worldPos, float baseVolume, int& outLeftVolume, int& outRightVolume) const;
	void SyncVrmodFeedbackGameSounds() const;
	void EnsureFeedbackSoundWarmup();
	void EnsureFeedbackSoundWorkerThread();
	bool EnqueueFeedbackSoundPlayback(const std::string& resolvedPath, int leftVolume, int rightVolume, bool preferLoadedPathReuse = true);
	void EnqueueFeedbackSoundWarmupPath(const std::string& resolvedPath);
	void ResetFeedbackSoundWorkerState();
	void FeedbackSoundWorkerMain();
	void SpawnHitIndicator(const Vector& worldPos);
	void SpawnKillIndicator(bool headshot, const Vector& worldPos);
	void DrawKillIndicators(IMatRenderContext* renderContext, ITexture* hudTexture);
	void UpdateKillIndicatorOverlays();
	IMaterial* ResolveHitIndicatorMaterial();
	IMaterial* ResolveKillIndicatorMaterial(bool headshot);
	void DestroyKillIndicatorOverlayTextures();
	void DestroyKillIndicatorOverlayTexture(int materialIndex);
	bool EnsureKillIndicatorOverlayTexture(int materialIndex, int width, int height);
	bool UploadKillIndicatorOverlayTexture(int materialIndex, const uint8_t* rgba, int width, int height, uint32_t frameIndex = 0, bool fromDecodedFrames = false);
	void TrimExpiredKillIndicators(std::chrono::steady_clock::time_point now, bool clearAll = false);
	void MaybeTrimExpiredKillIndicators(std::chrono::steady_clock::time_point now, bool force = false);
	void MaybeLogKillIndicatorStats(std::chrono::steady_clock::time_point now);
	void DestroyKillIndicatorOverlay(ActiveKillIndicator& indicator);
	bool EnsureKillIndicatorOverlaySlot(int slotIndex);
	int AcquireKillIndicatorOverlaySlot() const;
	int FindReusableKillIndicatorIndex(bool preferNonKill) const;
	void AddOrRecycleKillIndicator(const Vector& worldPos, bool killConfirmed, bool headshot, std::chrono::steady_clock::time_point now, bool preferNonKill);
	bool BuildKillIndicatorOverlayPixels(IMaterial* material, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight, uint32_t preferredFrameIndex = UINT32_MAX, bool* outUsedDecodedFrames = nullptr);
	bool ComputeKillIndicatorOverlayTransform(const Vector& worldPos, vr::HmdMatrix34_t& outTransform) const;
	// Mounted gun helper: returns the entity the player is currently "using" (turret/mounted gun) if any.
	// Used to skip that entity in aim-related traces so the aim line doesn't collide with the gun platform.
	bool IsUsingMountedGun(const C_BasePlayer* localPlayer) const;
	C_BaseEntity* GetMountedGunUseEntity(C_BasePlayer* localPlayer) const;
	bool m_EncodeVRUsercmd = true;
	void UpdateAimingLaser(C_BasePlayer* localPlayer);
	// In queued (multicore) rendering, the render thread uses a render-frame pose snapshot.
	// Draw the aim line from the render hook (dRenderView) using that snapshot to avoid head-turn ghosting.
	void RenderDrawAimLineQueued(C_BasePlayer* localPlayer);
	bool ShouldShowAimLine(C_WeaponCSBase* weapon) const;
	bool IsThrowableWeapon(C_WeaponCSBase* weapon) const;
	bool ShouldDrawAimLine(C_WeaponCSBase* weapon) const;
	bool IsWeaponLaserSightActive(C_WeaponCSBase* weapon) const;
	float CalculateThrowArcDistance(const Vector& pitchSource, bool* clampedToMax = nullptr) const;
	void DrawAimLine(const Vector& start, const Vector& end);
	void DrawThrowArc(const Vector& origin, const Vector& forward, const Vector& pitchSource);
	void DrawThrowArcFromCache(float duration);
	void DrawLineWithThickness(const Vector& start, const Vector& end, float duration);
	SpecialInfectedType GetSpecialInfectedType(const C_BaseEntity* entity) const;
	SpecialInfectedType GetSpecialInfectedTypeFromModel(const std::string& modelName) const;
	bool IsEntityAlive(const C_BaseEntity* entity) const;
	void DrawSpecialInfectedArrow(const Vector& origin, SpecialInfectedType type);
	void RefreshSpecialInfectedPreWarning(const Vector& infectedOrigin, SpecialInfectedType type, int entityIndex, bool isPlayerClass);
	void RefreshSpecialInfectedBlindSpotWarning(const Vector& infectedOrigin);
	bool HasLineOfSightToSpecialInfected(const Vector& infectedOrigin, int entityIndex) const;
	bool IsSpecialInfectedInBlindSpot(const Vector& infectedOrigin) const;
	void UpdateSpecialInfectedWarningState();
	void UpdateSpecialInfectedPreWarningState();
	void EncodeRoomscale1To1Move(CUserCmd* cmd);
	static bool DecodeRoomscale1To1Delta(int weaponsubtype, Vector& outDeltaMeters);
	void OnPredictionRunCommand(CUserCmd* cmd);
	void OnPrimaryAttackServerDecision(CUserCmd* cmd, bool fromSecondaryPrediction);
	void StartSpecialInfectedWarningAction();
	void UpdateSpecialInfectedWarningAction();
	void ResetSpecialInfectedWarningAction();
	void GetAimLineColor(int& r, int& g, int& b, int& a) const;
	void FinishFrame();
	void ConfigureExplicitTiming();
};
