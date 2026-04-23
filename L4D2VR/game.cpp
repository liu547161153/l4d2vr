#include "game.h"
#include <Windows.h>
#include <iostream>
#include <unordered_map>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <cstdint>
#include <mutex>
#include <string>

#include "sdk.h"
#include "vr.h"
#include "hooks.h"
#include "offsets.h"
#include "sigscanner.h"
#include "sdk/ivdebugoverlay.h"

static std::mutex logMutex;
using tCreateInterface = void* (__cdecl*)(const char* name, int* returnCode);

namespace
{
    static_assert(sizeof(void*) == 4, "L4D2VR ConVar bridge assumes 32-bit Source DLL layout.");

    static bool NormalizeSuspiciousFloat(float candidate, float& normalized)
    {
        if (std::isfinite(candidate) && std::fabs(candidate) <= 1000000.0f)
        {
            normalized = candidate;
            return true;
        }

        const double rounded = std::nearbyint(static_cast<double>(candidate));
        if (std::isfinite(candidate) &&
            std::fabs(static_cast<double>(candidate) - rounded) <= 0.5 &&
            rounded >= 0.0 &&
            rounded <= static_cast<double>(UINT32_MAX))
        {
            const uint32_t rebits = static_cast<uint32_t>(rounded);
            float rebound = 0.0f;
            std::memcpy(&rebound, &rebits, sizeof(rebound));
            if (std::isfinite(rebound) && std::fabs(rebound) <= 1000000.0f)
            {
                normalized = rebound;
                return true;
            }
        }

        normalized = candidate;
        return false;
    }

    // Source SDK 2007/2009 style ConVar layout used by L4D2.
    // We only depend on the vtable order for SetValue(...) and the cached numeric fields for reads.
    class SourceConCommandBase
    {
    public:
        virtual ~SourceConCommandBase() = default;
        virtual bool IsCommand(void) const = 0;
        virtual bool IsFlagSet(int flag) const = 0;
        virtual void AddFlags(int flags) = 0;
        virtual const char* GetName(void) const = 0;
        virtual const char* GetHelpText(void) const = 0;
        virtual bool IsRegistered(void) const = 0;
        virtual int GetDLLIdentifier() const = 0;

    protected:
        virtual void CreateBase(const char* pName, const char* pHelpString = nullptr, int flags = 0) = 0;
        virtual void Init() = 0;

    public:
        SourceConCommandBase* m_pNext = nullptr;
        bool m_bRegistered = false;
        char m_PaddingRegistered[3] = {};
        const char* m_pszName = nullptr;
        const char* m_pszHelpString = nullptr;
        int m_nFlags = 0;
    };

    class SourceConVar : public SourceConCommandBase
    {
    public:
        virtual ~SourceConVar() = default;
        virtual bool IsFlagSet(int flag) const = 0;
        virtual const char* GetHelpText(void) const = 0;
        virtual bool IsRegistered(void) const = 0;
        virtual const char* GetName(void) const = 0;
        virtual void AddFlags(int flags) = 0;
        virtual bool IsCommand(void) const = 0;
        virtual void SetValue(const char* value) = 0;
        virtual void SetValue(float value) = 0;
        virtual void SetValue(int value) = 0;

    private:
        virtual void InternalSetValue(const char* value) = 0;
        virtual void InternalSetFloatValue(float value) = 0;
        virtual void InternalSetIntValue(int value) = 0;
        virtual bool ClampValue(float& value) = 0;
        virtual void ChangeStringValue(const char* value, float oldValue) = 0;
        virtual void Create_Vtbl(const char* pName, const char* pDefaultValue, int flags = 0,
            const char* pHelpString = nullptr, bool bMin = false, float fMin = 0.0f,
            bool bMax = false, float fMax = 0.0f, void* callback = nullptr) = 0;
        virtual void Init() = 0;
        virtual void InternalSetFloatValue2(float value, bool force = false) = 0;

    public:
        // ConVar derives from both ConCommandBase and IConVar in Source.
        // FindVar() returns the full ConVar object, so after the ConCommandBase
        // base subobject there is a second vptr for the IConVar base.
        void* m_pIConVarVTable = nullptr;
        SourceConVar* m_pParent = nullptr;
        const char* m_pszDefaultValue = nullptr;
        char* m_pszString = nullptr;
        int m_StringLength = 0;
        float m_fValue = 0.0f;
        int m_nValue = 0;
        bool m_bHasMin = false;
        char m_PaddingHasMin[3] = {};
        float m_fMinVal = 0.0f;
        bool m_bHasMax = false;
        char m_PaddingHasMax[3] = {};
        float m_fMaxVal = 0.0f;
        void* m_fnChangeCallback = nullptr;

        int GetIntValue() const
        {
            const SourceConVar* parent = (m_pParent != nullptr) ? m_pParent : this;
            if (parent->m_pszString && *parent->m_pszString)
            {
                char* end = nullptr;
                const long parsed = std::strtol(parent->m_pszString, &end, 10);
                if (end != parent->m_pszString)
                    return static_cast<int>(parsed);
            }

            const uintptr_t key = reinterpret_cast<uintptr_t>(parent);
            return static_cast<int>(parent->m_nValue ^ static_cast<int>(key));
        }

        float GetFloatValue() const
        {
            const SourceConVar* parent = (m_pParent != nullptr) ? m_pParent : this;
            if (parent->m_pszString && *parent->m_pszString)
            {
                char* end = nullptr;
                const float parsed = std::strtof(parent->m_pszString, &end);
                if (end != parent->m_pszString)
                {
                    float normalized = parsed;
                    NormalizeSuspiciousFloat(parsed, normalized);
                    return normalized;
                }
            }

            const uintptr_t key = reinterpret_cast<uintptr_t>(parent);
            const uint32_t encodedBits = *reinterpret_cast<const uint32_t*>(&parent->m_fValue);
            const uint32_t decodedBits = encodedBits ^ static_cast<uint32_t>(key);
            float decoded = 0.0f;
            std::memcpy(&decoded, &decodedBits, sizeof(decoded));
            float normalized = decoded;
            NormalizeSuspiciousFloat(decoded, normalized);
            return normalized;
        }
    };

    class SourceIConVar
    {
    public:
        virtual void SetValue(const char* value) = 0;
        virtual void SetValue(float value) = 0;
        virtual void SetValue(int value) = 0;
        virtual const char* GetName(void) const = 0;
        virtual bool IsFlagSet(int flag) const = 0;
    };

    class SourceICvar
    {
    public:
        virtual bool Connect(void* factory) = 0;
        virtual void Disconnect() = 0;
        virtual void* QueryInterface(const char* pInterfaceName) = 0;
        virtual int Init() = 0;
        virtual void Shutdown() = 0;
        virtual int AllocateDLLIdentifier() = 0;
        virtual void RegisterConCommand(SourceConCommandBase* pCommandBase) = 0;
        virtual void UnregisterConCommand(SourceConCommandBase* pCommandBase) = 0;
        virtual void UnregisterConCommands(int id) = 0;
        virtual const char* GetCommandLineValue(const char* pVariableName) = 0;
        virtual SourceConCommandBase* FindCommandBase(const char* name) = 0;
        virtual const SourceConCommandBase* FindCommandBase(const char* name) const = 0;
        virtual SourceConVar* FindVar(const char* varName) = 0;
    };

    struct SourceRecvTable;

    struct SourceRecvProp
    {
        const char* m_pVarName = nullptr;
        int m_RecvType = 0;
        int m_Flags = 0;
        int m_StringBufferSize = 0;
        bool m_bInsideArray = false;
        const void* m_pExtraData = nullptr;
        SourceRecvProp* m_pArrayProp = nullptr;
        void* m_ArrayLengthProxy = nullptr;
        void* m_ProxyFn = nullptr;
        void* m_DataTableProxyFn = nullptr;
        SourceRecvTable* m_pDataTable = nullptr;
        int m_Offset = 0;
        int m_ElementStride = 0;
        int m_nElements = 0;
        const char* m_pParentArrayPropName = nullptr;
    };

    struct SourceRecvTable
    {
        SourceRecvProp* m_pProps = nullptr;
        int m_nProps = 0;
        void* m_pDecoder = nullptr;
        const char* m_pNetTableName = nullptr;
        bool m_bInitialized = false;
        bool m_bInMainList = false;
    };

    struct SourceClientClass
    {
        void* m_pCreateFn = nullptr;
        void* m_pCreateEventFn = nullptr;
        const char* m_pNetworkName = nullptr;
        SourceRecvTable* m_pRecvTable = nullptr;
        SourceClientClass* m_pNext = nullptr;
        int m_ClassID = 0;
    };

    class SourceBaseClientDLL
    {
    public:
        virtual int Connect(void* appSystemFactory, void* pGlobals) = 0;
        virtual int Disconnect(void) = 0;
        virtual int Init(void* appSystemFactory, void* pGlobals) = 0;
        virtual void PostInit() = 0;
        virtual void Shutdown(void) = 0;
        virtual void LevelInitPreEntity(char const* pMapName) = 0;
        virtual void LevelInitPostEntity() = 0;
        virtual void LevelFastReload(void) = 0;
        virtual void LevelShutdown(void) = 0;
        virtual void* GetAllClasses(void) = 0;
    };
}

static_assert(sizeof(SourceConCommandBase) == 24, "Unexpected ConCommandBase size for Source IConVar bridge.");

static int FindRecvPropOffsetRecursive(const SourceRecvTable* table, const char* propName, int accumulatedOffset);
static int FindRecvPropOffsetSafe(void* baseClientDll, const char* networkName, const char* propName);

// === Utility: Retry module load with logging ===
static HMODULE GetModuleWithRetry(const char* dllname, std::chrono::milliseconds timeout = std::chrono::seconds(30), int delayMs = 50)
{
    const auto start = std::chrono::steady_clock::now();
    int attempt = 0;

    while (true)
    {
        HMODULE handle = GetModuleHandleA(dllname);
        if (handle)
            return handle;

        ++attempt;
        Sleep(delayMs);

        if (timeout.count() >= 0 && std::chrono::steady_clock::now() - start >= timeout)
            break;
    }

    Game::errorMsg(("Failed to load module after retrying: " + std::string(dllname)).c_str());
    return nullptr;
}

// === Utility: Safe interface fetch with static cache ===
static void* GetInterfaceSafe(const char* dllname, const char* interfacename)
{
    static std::unordered_map<std::string, void*> cache;

    std::string key = std::string(dllname) + "::" + interfacename;
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;

    HMODULE mod = GetModuleWithRetry(dllname);
    if (!mod)
        return nullptr;

    auto CreateInterface = reinterpret_cast<tCreateInterface>(GetProcAddress(mod, "CreateInterface"));
    if (!CreateInterface)
    {
        Game::errorMsg(("CreateInterface not found in " + std::string(dllname)).c_str());
        return nullptr;
    }

    int returnCode = 0;
    void* iface = CreateInterface(interfacename, &returnCode);
    if (!iface)
    {
        Game::errorMsg(("Interface not found: " + std::string(interfacename)).c_str());
        return nullptr;
    }

    cache[key] = iface;
    return iface;
}

// === Utility: Attempt interface fetch without error logging ===
static void* TryInterfaceNoError(const char* dllname, const char* interfacename)
{
    HMODULE mod = GetModuleWithRetry(dllname);
    if (!mod)
        return nullptr;

    auto CreateInterface = reinterpret_cast<tCreateInterface>(GetProcAddress(mod, "CreateInterface"));
    if (!CreateInterface)
        return nullptr;

    int returnCode = 0;
    return CreateInterface(interfacename, &returnCode);
}

// === Game Constructor ===
Game::Game()
{
    m_BaseClient = reinterpret_cast<uintptr_t>(GetModuleWithRetry("client.dll"));
    m_BaseEngine = reinterpret_cast<uintptr_t>(GetModuleWithRetry("engine.dll"));
    m_BaseMaterialSystem = reinterpret_cast<uintptr_t>(GetModuleWithRetry("MaterialSystem.dll"));
    m_BaseServer = reinterpret_cast<uintptr_t>(GetModuleWithRetry("server.dll"));
    m_BaseVgui2 = reinterpret_cast<uintptr_t>(GetModuleWithRetry("vgui2.dll"));

    m_BaseClientDll = static_cast<IBaseClientDLL*>(GetInterfaceSafe("client.dll", "VClient016"));
    m_ClientEntityList = static_cast<IClientEntityList*>(GetInterfaceSafe("client.dll", "VClientEntityList003"));
    m_EngineTrace = static_cast<IEngineTrace*>(GetInterfaceSafe("engine.dll", "EngineTraceClient003"));
    m_EngineClient = static_cast<IEngineClient*>(GetInterfaceSafe("engine.dll", "VEngineClient013"));
    m_GameEventManager = static_cast<IGameEventManager2*>(TryInterfaceNoError("engine.dll", "GAMEEVENTSMANAGER002"));
    if (!m_GameEventManager)
        m_GameEventManager = static_cast<IGameEventManager2*>(TryInterfaceNoError("engine.dll", "GAMEEVENTSMANAGER001"));
    m_MaterialSystem = static_cast<IMaterialSystem*>(GetInterfaceSafe("MaterialSystem.dll", "VMaterialSystem080"));
    m_ModelInfo = static_cast<IModelInfo*>(GetInterfaceSafe("engine.dll", "VModelInfoClient004"));
    m_ModelRender = static_cast<IModelRender*>(GetInterfaceSafe("engine.dll", "VEngineModel016"));
    m_VguiInput = static_cast<IInput*>(GetInterfaceSafe("vgui2.dll", "VGUI_InputInternal001"));
    m_VguiSurface = static_cast<ISurface*>(GetInterfaceSafe("vguimatsurface.dll", "VGUI_Surface031"));
    m_DebugOverlay = static_cast<IVDebugOverlay*>(TryInterfaceNoError("engine.dll", "VDebugOverlay004"));
    if (!m_DebugOverlay)
        m_DebugOverlay = static_cast<IVDebugOverlay*>(TryInterfaceNoError("engine.dll", "VDebugOverlay003"));
    m_Cvar = TryInterfaceNoError("vstdlib.dll", "VEngineCvar007");
    if (!m_Cvar)
        m_Cvar = TryInterfaceNoError("vstdlib.dll", "VEngineCvar006");
    if (!m_Cvar)
        m_Cvar = TryInterfaceNoError("vstdlib.dll", "VEngineCvar004");

    m_Offsets = new Offsets();
    m_VR = new VR(this);

    ResetAllPlayerVRInfo();

    m_Hooks = new Hooks(this);

    m_Initialized = true;

    Game::logMsg("Game initialized successfully.");
}

// === Fallback Interface ===
void* Game::GetInterface(const char* dllname, const char* interfacename)
{
    Game::logMsg("Fallback GetInterface called for %s::%s", dllname, interfacename);
    return GetInterfaceSafe(dllname, interfacename);
}

// === Thread-safe Log Message with Timestamp ===
void Game::logMsg(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(logMutex);

    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    char timebuf[20] = {};
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now_c));

    printf("[%s] ", timebuf);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");

    const char* logFiles[] = { "vrmod_log.txt"};
    for (const char* logFile : logFiles)
    {
        FILE* file = fopen(logFile, "a");
        if (!file)
            continue;

        fprintf(file, "[%s] ", timebuf);
        va_list args2;
        va_start(args2, fmt);
        vfprintf(file, fmt, args2);
        va_end(args2);
        fprintf(file, "\n");
        fclose(file);
    }
}

// === Error Message ===
void Game::errorMsg(const char* msg)
{
    logMsg("[ERROR] %s", msg);
    MessageBoxA(nullptr, msg, "L4D2VR Error", MB_ICONERROR | MB_OK);
}

bool Game::IsValidPlayerIndex(int index) const
{
    return index >= 0 && index < static_cast<int>(m_PlayersVRInfo.size());
}

void Game::ResetAllPlayerVRInfo()
{
    m_PlayersVRInfo.fill(Player{});
}

// === Entity Access ===
C_BaseEntity* Game::GetClientEntity(int entityIndex)
{
    if (!m_ClientEntityList)
        return nullptr;

    return static_cast<C_BaseEntity*>(m_ClientEntityList->GetClientEntity(entityIndex));
}

// === Network Name Utility ===
char* Game::getNetworkName(uintptr_t* entity)
{
    __try
    {
        if (!entity)
            return nullptr;

        uintptr_t* vtable = reinterpret_cast<uintptr_t*>(*(entity + 0x8));
        if (!vtable)
            return nullptr;

        uintptr_t* getClientClassFn = reinterpret_cast<uintptr_t*>(*(vtable + 0x8));
        if (!getClientClassFn)
            return nullptr;

        uintptr_t* clientClass = reinterpret_cast<uintptr_t*>(*(getClientClassFn + 0x1));
        if (!clientClass)
            return nullptr;

        return reinterpret_cast<char*>(*(clientClass + 0x8));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

const char* Game::GetNetworkClassName(uintptr_t* entity) const
{
    __try
    {
        if (!entity)
            return nullptr;

        uintptr_t* vtable = reinterpret_cast<uintptr_t*>(*(entity + 0x8));
        if (!vtable)
            return nullptr;

        uintptr_t* getClientClassFn = reinterpret_cast<uintptr_t*>(*(vtable + 0x8));
        if (!getClientClassFn)
            return nullptr;

        uintptr_t* clientClass = reinterpret_cast<uintptr_t*>(*(getClientClassFn + 0x1));
        if (!clientClass)
            return nullptr;

        return reinterpret_cast<const char*>(*(clientClass + 0x8));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

int Game::FindRecvPropOffset(const char* networkName, const char* propName) const
{
    if (!m_BaseClientDll || !networkName || !*networkName || !propName || !*propName)
        return -1;

    static std::unordered_map<std::string, int> cache;
    const std::string key = std::string(networkName) + "::" + propName;
    auto cached = cache.find(key);
    if (cached != cache.end())
        return cached->second;

    const int offset = FindRecvPropOffsetSafe(m_BaseClientDll, networkName, propName);
    if (offset >= 0)
        cache[key] = offset;

    return offset;
}

// === Commands ===
void Game::ClientCmd(const char* szCmdString)
{
    if (m_EngineClient)
        m_EngineClient->ClientCmd(szCmdString);
}

void Game::ClientCmd_Unrestricted(const char* szCmdString)
{
    if (m_EngineClient)
        m_EngineClient->ClientCmd_Unrestricted(szCmdString);
}

static SourceConVar* FindConVarInternal(void* cvarIface, const char* name)
{
    if (!cvarIface || !name || !*name)
        return nullptr;

    return reinterpret_cast<SourceICvar*>(cvarIface)->FindVar(name);
}

static int FindRecvPropOffsetRecursive(const SourceRecvTable* table, const char* propName, int accumulatedOffset)
{
    if (!table || !propName || !*propName || !table->m_pProps || table->m_nProps <= 0)
        return -1;

    for (int i = 0; i < table->m_nProps; ++i)
    {
        const SourceRecvProp& prop = table->m_pProps[i];
        if (prop.m_pVarName && std::strcmp(prop.m_pVarName, propName) == 0)
            return accumulatedOffset + prop.m_Offset;

        if (prop.m_pDataTable && prop.m_pDataTable->m_pProps && prop.m_pDataTable->m_nProps > 0)
        {
            const int nestedOffset =
                FindRecvPropOffsetRecursive(prop.m_pDataTable, propName, accumulatedOffset + prop.m_Offset);
            if (nestedOffset >= 0)
                return nestedOffset;
        }
    }

    return -1;
}

static int FindRecvPropOffsetSafe(void* baseClientDll, const char* networkName, const char* propName)
{
    __try
    {
        auto* clientDll = reinterpret_cast<SourceBaseClientDLL*>(baseClientDll);
        if (!clientDll || !networkName || !*networkName || !propName || !*propName)
            return -1;

        auto* clientClass = reinterpret_cast<SourceClientClass*>(clientDll->GetAllClasses());
        while (clientClass)
        {
            if (clientClass->m_pNetworkName && std::strcmp(clientClass->m_pNetworkName, networkName) == 0)
                return FindRecvPropOffsetRecursive(clientClass->m_pRecvTable, propName, 0);

            clientClass = clientClass->m_pNext;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }

    return -1;
}

static SourceIConVar* AsIConVar(SourceConVar* cvar)
{
    if (!cvar)
        return nullptr;

    return reinterpret_cast<SourceIConVar*>(reinterpret_cast<uint8_t*>(cvar) + sizeof(SourceConCommandBase));
}

int Game::GetConVarInt(const char* name, int fallback) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return fallback;

    __try
    {
        return cvar->GetIntValue();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logMsg("[WARN] GetConVarInt failed for %s", name ? name : "<null>");
        return fallback;
    }
}

float Game::GetConVarFloat(const char* name, float fallback) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return fallback;

    __try
    {
        return cvar->GetFloatValue();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logMsg("[WARN] GetConVarFloat failed for %s", name ? name : "<null>");
        return fallback;
    }
}

bool Game::SetConVarInt(const char* name, int value) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return false;

    SourceIConVar* iconvar = AsIConVar(cvar);
    if (!iconvar)
        return false;

    __try
    {
        iconvar->SetValue(value);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logMsg("[WARN] SetConVarInt failed for %s=%d", name ? name : "<null>", value);
        return false;
    }
}

bool Game::SetConVarFloat(const char* name, float value) const
{
    SourceConVar* cvar = FindConVarInternal(m_Cvar, name);
    if (!cvar)
        return false;

    SourceIConVar* iconvar = AsIConVar(cvar);
    if (!iconvar)
        return false;

    __try
    {
        iconvar->SetValue(value);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logMsg("[WARN] SetConVarFloat failed for %s=%.3f", name ? name : "<null>", value);
        return false;
    }
}

bool Game::SetConVarBool(const char* name, bool value) const
{
    return SetConVarInt(name, value ? 1 : 0);
}

bool Game::SampleLightAtPoint(const Vector& point, int& outR, int& outG, int& outB) const
{
    outR = 0;
    outG = 0;
    outB = 0;

    static bool s_loggedUnavailable = false;
    if (!m_Offsets || !m_Offsets->SampleLightAtPoint.valid || m_Offsets->SampleLightAtPoint.address == 0)
    {
        if (!s_loggedUnavailable)
        {
            logMsg("[WARN] engine.dll SampleLightAtPoint wrapper is unavailable");
            s_loggedUnavailable = true;
        }
        return false;
    }

    struct EngineLightSample
    {
        int r;
        int g;
        int b;
        int valid;
    } sample{};

    using tSampleLightAtPoint = int* (__cdecl*)(int* outRgba, const Vector* point);
    auto fn = reinterpret_cast<tSampleLightAtPoint>(m_Offsets->SampleLightAtPoint.address);
    if (!fn)
        return false;

    __try
    {
        if (!fn(reinterpret_cast<int*>(&sample), &point) || sample.valid == 0)
            return false;

        outR = (std::max)(0, (std::min)(255, sample.r));
        outG = (std::max)(0, (std::min)(255, sample.g));
        outB = (std::max)(0, (std::min)(255, sample.b));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logMsg("[WARN] SampleLightAtPoint failed at (%.1f %.1f %.1f)", point.x, point.y, point.z);
        return false;
    }
}

int Game::GetEntityEffects(const C_BaseEntity* entity, int fallback) const
{
    if (!entity)
        return fallback;

    static int s_effectsOffset = std::numeric_limits<int>::min();
    static bool s_loggedOffset = false;
    if (s_effectsOffset == std::numeric_limits<int>::min())
    {
        s_effectsOffset = FindRecvPropOffset("DT_BaseEntity", "m_fEffects");
        if (s_effectsOffset < 0)
            s_effectsOffset = 0xE0;

        if (!s_loggedOffset)
        {
            logMsg("[Offsets] Using DT_BaseEntity::m_fEffects offset 0x%X", s_effectsOffset);
            s_loggedOffset = true;
        }
    }

    __try
    {
        return *reinterpret_cast<const int*>(reinterpret_cast<const uint8_t*>(entity) + s_effectsOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        logMsg("[WARN] GetEntityEffects failed");
        return fallback;
    }
}

// === Rendering Thread Mode ===
int Game::GetMatQueueMode() const
{
    if (!m_MaterialSystem)
        return 0;

    void** vtbl = *reinterpret_cast<void***>(m_MaterialSystem);
    if (!vtbl)
        return 0;

    // IMaterialSystem::GetThreadMode() is vfunc #11 in this ABI (0-based index).
    using tGetThreadMode = int(__thiscall*)(IMaterialSystem*);
    auto fn = reinterpret_cast<tGetThreadMode>(vtbl[11]);
    if (!fn)
        return 0;

    return fn(m_MaterialSystem);
}
