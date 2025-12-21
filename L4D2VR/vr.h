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
	std::optional<WORD> virtualKey;
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

	float m_HorizontalOffsetLeft;
	float m_VerticalOffsetLeft;
	float m_HorizontalOffsetRight;
	float m_VerticalOffsetRight;

	uint32_t m_RenderWidth;
	uint32_t m_RenderHeight;
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
	bool m_ObserverThirdPerson = false;
	int m_ThirdPersonHoldFrames = 0;
	Vector m_ThirdPersonViewOrigin = { 0,0,0 };
	QAngle m_ThirdPersonViewAngles = { 0,0,0 };
	bool m_ThirdPersonPoseInitialized = false;
	float m_ThirdPersonCameraSmoothing = 0.5f;

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
	Vector m_LastAimDirection = { 0,0,0 };
	bool m_HasAimLine = false;
	float m_AimLineThickness = 2.0f;
	bool m_AimLineEnabled = true;
	bool m_MeleeAimLineEnabled = true;
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

	float m_Ipd;
	float m_EyeZ;

	Vector m_IntendedPositionOffset = { 0,0,0 };

	enum TextureID
	{
		Texture_None = -1,
		Texture_LeftEye,
		Texture_RightEye,
		Texture_HUD,
		Texture_Blank
	};

	ITexture* m_LeftEyeTexture;
	ITexture* m_RightEyeTexture;
	ITexture* m_HUDTexture;
	ITexture* m_BlankTexture = nullptr;

	IDirect3DSurface9* m_D9LeftEyeSurface;
	IDirect3DSurface9* m_D9RightEyeSurface;
	IDirect3DSurface9* m_D9HUDSurface;
	IDirect3DSurface9* m_D9BlankSurface;

	SharedTextureHolder m_VKLeftEye;
	SharedTextureHolder m_VKRightEye;
	SharedTextureHolder m_VKBackBuffer;
	SharedTextureHolder m_VKHUD;
	SharedTextureHolder m_VKBlankTexture;

	bool m_IsVREnabled = false;
	bool m_IsInitialized = false;
	bool m_RenderedNewFrame = false;
	bool m_RenderedHud = false;
	bool m_CreatedVRTextures = false;
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
	vr::VRActionHandle_t m_ShowHUD;
	vr::VRActionHandle_t m_Pause;
	vr::VRActionHandle_t m_NonVRServerMovementAngleToggle;
	vr::VRActionHandle_t m_CustomAction1;
	vr::VRActionHandle_t m_CustomAction2;
	vr::VRActionHandle_t m_CustomAction3;
	vr::VRActionHandle_t m_CustomAction4;
	vr::VRActionHandle_t m_CustomAction5;

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
	float m_HudSize = 1.1;
	float m_ControllerHudSize = 0.5f;
	float m_ControllerHudYOffset = 0.12f;
	float m_ControllerHudZOffset = 0.0f;
	float m_ControllerHudRotation = 0.0f;
	float m_ControllerHudXOffset = 0.0f;
	bool m_HudAlwaysVisible = false;
	float m_ControllerSmoothing = 0.0f;
	bool m_ControllerSmoothingInitialized = false;
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
	bool m_DrawInventoryAnchors = false;
	int m_InventoryAnchorColorR = 0;
	int m_InventoryAnchorColorG = 255;
	int m_InventoryAnchorColorB = 255;
	int m_InventoryAnchorColorA = 64;

	bool m_ForceNonVRServerMovement = false;
	bool m_NonVRServerMovementAngleOverride = false;
	bool m_RequireSecondaryAttackForItemSwitch = true;
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
	float m_SpecialInfectedBlindSpotDistance = 300.0f;
	float m_SpecialInfectedBlindSpotWarningDuration = 0.5f;
	bool m_SpecialInfectedBlindSpotWarningActive = false;
	std::chrono::steady_clock::time_point m_LastSpecialInfectedWarningTime{};
	float m_SpecialInfectedWarningSecondaryHoldDuration = 0.15f;
	float m_SpecialInfectedWarningPostAttackDelay = 0.1f;
	float m_SpecialInfectedWarningJumpHoldDuration = 0.2f;
	bool m_SpecialInfectedWarningActionEnabled = false;
	float m_SpecialInfectedPreWarningDistance = 450.0f;
	float m_SpecialInfectedPreWarningTargetUpdateInterval = 0.2f;
	float m_SpecialInfectedPreWarningAimAngle = 30.0f;
	bool m_SpecialInfectedPreWarningAutoAimConfigEnabled = false;
	bool m_SpecialInfectedPreWarningAutoAimEnabled = false;
	bool m_SpecialInfectedPreWarningActive = false;
	bool m_SpecialInfectedPreWarningInRange = false;
	Vector m_SpecialInfectedPreWarningTarget = { 0.0f, 0.0f, 0.0f };
	int m_SpecialInfectedPreWarningTargetEntityIndex = -1;
	bool m_SpecialInfectedPreWarningTargetIsPlayer = false;
	float m_SpecialInfectedPreWarningTargetDistanceSq = std::numeric_limits<float>::max();
	Vector m_SpecialInfectedAutoAimDirection = { 0.0f, 0.0f, 0.0f };
	float m_SpecialInfectedAutoAimLerp = 0.2f;
	float m_SpecialInfectedAutoAimCooldown = 0.0f;
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
	bool HasLineOfSightToSpecialInfected(const Vector& infectedOrigin) const;
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
