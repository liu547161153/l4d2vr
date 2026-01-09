#pragma once
#include <Windows.h>
#include "openvr.h"
#include "vector.h"
#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define MAX_STR_LEN 256

class Game;
class C_BaseEntity;
class C_BasePlayer;
class C_WeaponCSBase;
class IDirect3DTexture9;
class IDirect3DSurface9;
class ITexture;

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
	vr::VRVulkanTextureData_t m_VulkanData;
	vr::Texture_t m_VRTexture;
};

struct CustomActionBinding
{
	std::string command;
	std::string releaseCommand;
	std::optional<WORD> virtualKey;
	bool holdVirtualKey = false;
	bool usePressReleaseCommands = false;
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
	// When a CustomAction is bound to +walk (press/release), we can optionally treat it
	// as a signal that the gameplay camera has been forced into a third-person mode
	// (e.g. slide mods that switch to 3P while +walk is held).
	bool m_CustomWalkHeld = false;
	bool m_ThirdPersonRenderOnCustomWalk = false;
	bool m_ObserverThirdPerson = false;
	int m_ThirdPersonHoldFrames = 0;
	Vector m_ThirdPersonViewOrigin = { 0,0,0 };
	QAngle m_ThirdPersonViewAngles = { 0,0,0 };
	bool m_ThirdPersonPoseInitialized = false;
	float m_ThirdPersonCameraSmoothing = 0.5f;
	float m_ThirdPersonVRCameraOffset = 80.0f;
	Vector m_LeftControllerPosAbs;
	QAngle m_LeftControllerAngAbs;
	Vector m_RightControllerPosAbs;
	QAngle m_RightControllerAngAbs;

	Vector m_ViewmodelPosOffset;
	QAngle m_ViewmodelAngOffset;
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
	bool m_ScopeForcingAimLine = false;
	bool m_MeleeAimLineEnabled = true;
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

	bool m_IsVREnabled = false;
	bool m_IsInitialized = false;
	bool m_RenderedNewFrame = false;
	bool m_RenderedHud = false;
	bool m_CreatedVRTextures = false;
	// Used by extra offscreen passes (scope RTT): prevents HUD hooks from hijacking RT stack
	bool m_SuppressHudCapture = false;
	bool m_CompositorExplicitTiming = false;
	bool m_CompositorNeedsHandoff = false;
	TextureID m_CreatingTextureID = Texture_None;

	bool m_PressedTurn = false;
	bool m_PushingThumbstick = false;
	bool m_CrouchToggleActive = false;
	bool m_VoiceRecordActive = false;
	bool m_QuickTurnTriggered = false;
	bool m_PrimaryAttackDown = false;

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
	float m_VRScale = 43.2;
	float m_IpdScale = 1.0;
	bool m_HideArms = false;
	float m_HudDistance = 1.3;
	float m_FixedHudYOffset = 0.0f;
	float m_FixedHudDistanceOffset = 0.0f;
	float m_HudSize = 1.1;
	float m_ControllerHudSize = 0.5f;
	float m_ControllerHudYOffset = 0.12f;
	float m_ControllerHudZOffset = 0.0f;
	float m_ControllerHudRotation = 0.0f;
	float m_ControllerHudXOffset = 0.0f;
	bool m_ControllerHudCut = true;
	bool m_HudAlwaysVisible = false;
	bool m_HudToggleState = false;
	std::chrono::steady_clock::time_point m_HudChatVisibleUntil{};
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

	// Inventory anchor basis: apply offsets in a BODY space (yaw-only), not head pitch/roll.
	// This makes anchors stable when you look up/down.
	Vector m_InventoryBodyOriginOffset = { 0.0f, 0.0f, 0.0f }; // meters (forward,right,up) in body space

	// Optional front-of-view debug helper markers (purely visual) so anchors behind/at waist are still discoverable.
	float m_InventoryHudMarkerDistance = 0.45f;   // meters forward from head
	float m_InventoryHudMarkerUpOffset = -0.10f;  // meters up (+) / down (-)
	float m_InventoryHudMarkerSeparation = 0.14f; // meters between markers horizontally

	bool m_DrawInventoryAnchors = false;
	int m_InventoryAnchorColorR = 0;
	int m_InventoryAnchorColorG = 255;
	int m_InventoryAnchorColorB = 255;
	int m_InventoryAnchorColorA = 64;

	bool m_ForceNonVRServerMovement = false;
	bool m_NonVRServerMovementAngleOverride = true;

	// Non-VR server movement: client-side melee gesture -> IN_ATTACK tuning (ForceNonVRServerMovement=true only)
	// These are intentionally separate from generic MotionGesture* knobs.
	float m_NonVRMeleeSwingThreshold = 1.1f;     // controller linear speed threshold (m/s-ish in tracking space)
	float m_NonVRMeleeSwingCooldown = 0.30f;     // seconds between synthetic swings
	float m_NonVRMeleeHoldTime = 0.06f;          // seconds to hold IN_ATTACK after trigger (reduces "dropped swings")
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
	std::chrono::steady_clock::time_point m_SpecialInfectedWarningNextActionTime{};
	int m_AimLineWarningColorR = 255;
	int m_AimLineWarningColorG = 255;
	int m_AimLineWarningColorB = 0;

	// ----------------------------
	// Gun-mounted scope (RTT overlay)
	// ----------------------------
	bool  m_ScopeEnabled = false;
	int   m_ScopeRTTSize = 1024;               // square RTT size in pixels
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
	bool   IsScopeActive() const { return m_ScopeEnabled && m_ScopeActive; }
	bool   ShouldRenderScope() const{return m_ScopeEnabled && m_ScopeWeaponIsFirearm&& (m_ScopeOverlayAlwaysVisible || IsScopeActive());}
	void   CycleScopeMagnification();
	void   UpdateScopeAimLineState();

	// ----------------------------
	// Rear mirror (off-hand)
	// ----------------------------
	bool  m_RearMirrorEnabled = false;
	int   m_RearMirrorRTTSize = 512;
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

	Vector m_RearMirrorCameraPosAbs = { 0.0f, 0.0f, 0.0f };
	QAngle m_RearMirrorCameraAngAbs = { 0.0f, 0.0f, 0.0f };

	Vector GetRearMirrorCameraAbsPos() const { return m_RearMirrorCameraPosAbs; }
	QAngle GetRearMirrorCameraAbsAngle() const { return m_RearMirrorCameraAngAbs; }
	bool   ShouldRenderRearMirror() const { return m_RearMirrorEnabled; }

	VR() {};
	VR(Game* game);
	int SetActionManifest(const char* fileName);
	void InstallApplicationManifest(const char* fileName);
	void Update();
	void CreateVRTextures();
	void HandleMissingRenderContext(const char* location);
	void SubmitVRTextures();
	void LogCompositorError(const char* action, vr::EVRCompositorError error);
	void RepositionOverlays(bool attachToControllers = true);
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
	QAngle GetRightControllerAbsAngle();
	Vector GetRightControllerAbsPos();
	Vector GetRecommendedViewmodelAbsPos();
	QAngle GetRecommendedViewmodelAbsAngle();
	void UpdateTracking();
	void UpdateMotionGestures(C_BasePlayer* localPlayer);
	bool UpdateThirdPersonViewState(const Vector& cameraOrigin, const Vector& cameraAngles);
	Vector GetViewAngle();
	Vector GetViewOriginLeft();
	Vector GetViewOriginRight();
	Vector GetThirdPersonViewOrigin() const { return m_ThirdPersonViewOrigin; }
	QAngle GetThirdPersonViewAngles() const { return m_ThirdPersonViewAngles; }
	bool IsThirdPersonCameraActive() const { return m_IsThirdPersonCamera; }
	bool PressedDigitalAction(vr::VRActionHandle_t& actionHandle, bool checkIfActionChanged = false);
	bool GetDigitalActionData(vr::VRActionHandle_t& actionHandle, vr::InputDigitalActionData_t& digitalDataOut);
	bool GetAnalogActionData(vr::VRActionHandle_t& actionHandle, vr::InputAnalogActionData_t& analogDataOut);
	void ResetPosition();
	void GetPoseData(vr::TrackedDevicePose_t& poseRaw, TrackedDevicePoseData& poseOut);
	void ParseConfigFile();
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
	bool m_EncodeVRUsercmd = true;
	void UpdateAimingLaser(C_BasePlayer* localPlayer);
	bool ShouldShowAimLine(C_WeaponCSBase* weapon) const;
	bool IsThrowableWeapon(C_WeaponCSBase* weapon) const;
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
	void StartSpecialInfectedWarningAction();
	void UpdateSpecialInfectedWarningAction();
	void ResetSpecialInfectedWarningAction();
	void GetAimLineColor(int& r, int& g, int& b, int& a) const;
	void FinishFrame();
	void ConfigureExplicitTiming();
};
