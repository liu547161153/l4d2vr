#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <cstdarg>
#include <Windows.h>

#include "vector.h"

// === Forward Declarations for Engine Interfaces ===
class IClientEntityList;
class IEngineTrace;
class IEngineClient;
class IMaterialSystem;
class IBaseClientDLL;
class IModelInfo;
class IModelRender;
class IMaterial;
class IInput;
class ISurface;
class C_BaseEntity;
class C_BasePlayer;
struct model_t;
class IVDebugOverlay;

// === Forward Declarations for Internal Systems ===
class Game;
class Offsets;
class VR;
class Hooks;

// === Global Game Instance ===
inline Game* g_Game = nullptr;

// === Per-Player VR State ===
struct Player
{
    C_BasePlayer* pPlayer = nullptr;
    bool isUsingVR = false;

    Vector controllerPos = { 0.f, 0.f, 0.f };
    QAngle controllerAngle = { 0.f, 0.f, 0.f };
    QAngle prevControllerAngle = { 0.f, 0.f, 0.f };

    bool isMeleeing = false;
    bool isNewSwing = false;
};

// === Main Game System ===
class Game
{
public:
    // === Engine Interfaces ===
    IClientEntityList* m_ClientEntityList = nullptr;
    IEngineTrace* m_EngineTrace = nullptr;
    IEngineClient* m_EngineClient = nullptr;
    IMaterialSystem* m_MaterialSystem = nullptr;
    IBaseClientDLL* m_BaseClientDll = nullptr;
    IModelInfo* m_ModelInfo = nullptr;
    IModelRender* m_ModelRender = nullptr;
    IInput* m_VguiInput = nullptr;
    ISurface* m_VguiSurface = nullptr;
    IVDebugOverlay* m_DebugOverlay = nullptr;

    // === Module Base Addresses ===
    uintptr_t m_BaseEngine = 0;
    uintptr_t m_BaseClient = 0;
    uintptr_t m_BaseServer = 0;
    uintptr_t m_BaseMaterialSystem = 0;
    uintptr_t m_BaseVgui2 = 0;

    // === Internal Systems ===
    Offsets* m_Offsets = nullptr;
    VR* m_VR = nullptr;
    Hooks* m_Hooks = nullptr;

    // === State Flags ===
    bool m_Initialized = false;
    bool m_PerformingMelee = false;
    int m_CurrentUsercmdID = -1;

    // === Player VR State (Multiplayer) ===
    // Matches Source's MAX_PLAYERS (65) to cover the full player index range.
    static constexpr size_t kMaxPlayers = 65;
    std::array<Player, kMaxPlayers> m_PlayersVRInfo;

    // === Weapon / Viewmodel State ===
    bool m_IsMeleeWeaponActive = false;
    bool m_SwitchedWeapons = false;
    model_t* m_ArmsModel = nullptr;
    IMaterial* m_ArmsMaterial = nullptr;
    bool m_CachedArmsModel = false;

    // === Constructor ===
    Game();

    // === Interface Utilities ===
    void* GetInterface(const char* dllname, const char* interfacename);
    C_BaseEntity* GetClientEntity(int entityIndex);
    const char* GetClientClassName(C_BaseEntity* entity) const;

    // === Command Execution ===
    void ClientCmd(const char* szCmdString);
    void ClientCmd_Unrestricted(const char* szCmdString);

    // === Logging ===
    static void logMsg(const char* fmt, ...);
    static void errorMsg(const char* msg);

    // === Player Utilities ===
    bool IsValidPlayerIndex(int index) const;
    void ResetAllPlayerVRInfo();
};

// === Logging Macros (Debug Only) ===
#ifdef _DEBUG
#define LOG(fmt, ...) Game::logMsg("[LOG] " fmt, ##__VA_ARGS__)
#define ERR(msg) Game::errorMsg("[ERROR] " msg)
#else
#define LOG(fmt, ...)
#define ERR(msg)
#endif
