
#include "vr.h"
#include <Windows.h>
#include <mmsystem.h>
#include "sdk.h"
#include "game.h"
#include "hooks.h"
#include "usercmd.h"
#include "trace.h"
#include "sdk/ivdebugoverlay.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iterator>
#include <vector>
#include <d3d9_vr.h>

#pragma comment(lib, "Winmm.lib")

namespace
{
    // NOTE: “被控放行”要宁可保守：只在「确实是控制者本人」且「目标非常贴近队友」时才放行。
    // Used by VR::UpdateFriendlyFireAimHit().
    constexpr float kAllowThroughControlledTeammateMaxDist = 64.0f; // units (conservative)
    // Returns true if the call should be skipped because we ran it too recently.
    inline bool ShouldThrottle(std::chrono::steady_clock::time_point& last, float maxHz)
    {
        if (maxHz <= 0.0f)
            return false;

        const float minInterval = 1.0f / std::max(1.0f, maxHz);
        const auto now = std::chrono::steady_clock::now();
        if (last.time_since_epoch().count() != 0)
        {
            const float elapsed = std::chrono::duration<float>(now - last).count();
            if (elapsed < minInterval)
                return true;
        }
        last = now;
        return false;
    }

    inline float MinIntervalSeconds(float maxHz)
    {
        if (maxHz <= 0.0f)
            return 0.0f;
        return 1.0f / std::max(1.0f, maxHz);
    }

    // Roomscale 1:1 delta packing.
    //
    // We originally used a 32-bit payload stored in CUserCmd::weaponsubtype. In practice,
    // some builds/routes appear to truncate this field during usercmd serialization.
    // To be robust, prefer a compact 15-bit format (1 marker + 7-bit signed X + 7-bit signed Y)
    // that survives 16-bit truncation, while still accepting the legacy 32-bit format.
    static constexpr uint32_t kRSMarker32 = 0x80000000u;

    static constexpr uint32_t kRSMarker15 = 0x00004000u; // bit 14
    static constexpr int kAxisBits15 = 7;
    static constexpr int kAxisMask15 = (1 << kAxisBits15) - 1; // 0x7F
    static constexpr int kAxisBias15 = 1 << (kAxisBits15 - 1); // 64

    static constexpr int kAxisBits32 = 15;
    static constexpr int kAxisMask32 = (1 << kAxisBits32) - 1;
    static constexpr int kAxisBias32 = 1 << (kAxisBits32 - 1);

    static inline int QuantizeCm(float meters)
    {
        int cm = (int)roundf(meters * 100.0f);
        if (cm < -kAxisBias32) cm = -kAxisBias32;
        if (cm > (kAxisBias32 - 1)) cm = (kAxisBias32 - 1);
        return cm;
    }

    static inline uint32_t PackDeltaCm(int dx, int dy)
    {
        // Compact 15-bit format: marker + 7-bit signed dx + 7-bit signed dy (centimeters).
        // Clamp to +/-63cm per command to guarantee it fits.
        if (dx < -(kAxisBias15 - 1)) dx = -(kAxisBias15 - 1);
        if (dx > (kAxisBias15 - 1))  dx = (kAxisBias15 - 1);
        if (dy < -(kAxisBias15 - 1)) dy = -(kAxisBias15 - 1);
        if (dy > (kAxisBias15 - 1))  dy = (kAxisBias15 - 1);

        uint32_t ux = (uint32_t)((dx + kAxisBias15) & kAxisMask15);
        uint32_t uy = (uint32_t)((dy + kAxisBias15) & kAxisMask15);
        return kRSMarker15 | ux | (uy << kAxisBits15);
    }

    struct VASStats
    {
        size_t freeTotal = 0;
        size_t freeLargest = 0;
        size_t reserved = 0;
        size_t committed = 0;
    };

    inline double BytesToMiB(size_t bytes)
    {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

    // Best-effort Virtual Address Space snapshot for 32-bit processes.
    // This is what typical "av" tools visualize: free vs reserved/committed address ranges.
    inline VASStats QueryVASStats()
    {
        VASStats out{};

        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);

        MEMORY_BASIC_INFORMATION mbi{};
        while (addr < maxAddr)
        {
            SIZE_T queried = VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi));
            if (queried == 0)
                break;

            const size_t regionSize = static_cast<size_t>(mbi.RegionSize);
            switch (mbi.State)
            {
            case MEM_FREE:
                out.freeTotal += regionSize;
                out.freeLargest = std::max(out.freeLargest, regionSize);
                break;
            case MEM_RESERVE:
                out.reserved += regionSize;
                break;
            case MEM_COMMIT:
                out.committed += regionSize;
                break;
            default:
                break;
            }

            // Advance to the next region.
            addr += regionSize ? regionSize : 4096;
        }

        return out;
    }

    // Normalize Source-style view angles:
    // - Bring pitch/yaw into [-180, 180] first (avoid -30 becoming 330 then clamped to 89).
    // - Then clamp pitch to [-89, 89].
    inline void NormalizeAndClampViewAngles(QAngle& a)
    {
        while (a.x > 180.f) a.x -= 360.f;
        while (a.x < -180.f) a.x += 360.f;
        while (a.y > 180.f) a.y -= 360.f;
        while (a.y < -180.f) a.y += 360.f;
        a.z = 0.f;
        if (a.x > 89.f) a.x = 89.f;
        if (a.x < -89.f) a.x = -89.f;
    }
    inline bool IsFirearmWeaponId(C_WeaponCSBase::WeaponID id)
    {
        switch (id)
        {
        case C_WeaponCSBase::WeaponID::PISTOL:
        case C_WeaponCSBase::WeaponID::MAGNUM:
        case C_WeaponCSBase::WeaponID::UZI:
        case C_WeaponCSBase::WeaponID::MAC10:
        case C_WeaponCSBase::WeaponID::MP5:

        case C_WeaponCSBase::WeaponID::PUMPSHOTGUN:
        case C_WeaponCSBase::WeaponID::SHOTGUN_CHROME:
        case C_WeaponCSBase::WeaponID::AUTOSHOTGUN:
        case C_WeaponCSBase::WeaponID::SPAS:

        case C_WeaponCSBase::WeaponID::M16A1:
        case C_WeaponCSBase::WeaponID::AK47:
        case C_WeaponCSBase::WeaponID::SCAR:
        case C_WeaponCSBase::WeaponID::SG552:

        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE:
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
        case C_WeaponCSBase::WeaponID::AWP:
        case C_WeaponCSBase::WeaponID::SCOUT:

        case C_WeaponCSBase::WeaponID::GRENADE_LAUNCHER:
        case C_WeaponCSBase::WeaponID::M60:
        case C_WeaponCSBase::WeaponID::MACHINEGUN:
            return true;

        default:
            return false;
        }
    }
}

// ----------------------------
// One Euro filter helpers (for scope stabilization)
// ----------------------------
constexpr float kPi = 3.14159265358979323846f;

inline float OneEuroAlpha(float cutoffHz, float dt)
{
    cutoffHz = std::max(0.0001f, cutoffHz);
    dt = std::max(0.000001f, dt);
    const float tau = 1.0f / (2.0f * kPi * cutoffHz);
    return 1.0f / (1.0f + tau / dt);
}

inline float AngleDeltaDeg(float a, float b)
{
    float d = a - b;
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    return d;
}

inline Vector OneEuroFilterVec3(
    const Vector& x,
    Vector& xHat,
    Vector& dxHat,
    bool& initialized,
    float dt,
    float minCutoff,
    float beta,
    float dCutoff)
{
    if (!initialized)
    {
        initialized = true;
        xHat = x;
        dxHat = { 0.0f, 0.0f, 0.0f };
        return xHat;
    }

    const Vector dx = (x - xHat) * (1.0f / std::max(0.000001f, dt));
    const float aD = OneEuroAlpha(dCutoff, dt);
    dxHat = dxHat + (dx - dxHat) * aD;

    const float speed = VectorLength(dxHat);
    const float cutoff = std::max(0.0001f, minCutoff + beta * speed);
    const float a = OneEuroAlpha(cutoff, dt);
    xHat = xHat + (x - xHat) * a;
    return xHat;
}

inline QAngle OneEuroFilterAngles(
    const QAngle& x,
    QAngle& xHat,
    QAngle& dxHat,
    bool& initialized,
    float dt,
    float minCutoff,
    float beta,
    float dCutoff)
{
    if (!initialized)
    {
        initialized = true;
        xHat = x;
        dxHat = { 0.0f, 0.0f, 0.0f };
        return xHat;
    }

    const float invDt = 1.0f / std::max(0.000001f, dt);
    const QAngle dx = {
        AngleDeltaDeg(x.x, xHat.x) * invDt,
        AngleDeltaDeg(x.y, xHat.y) * invDt,
        AngleDeltaDeg(x.z, xHat.z) * invDt
    };

    const float aD = OneEuroAlpha(dCutoff, dt);
    dxHat.x = dxHat.x + (dx.x - dxHat.x) * aD;
    dxHat.y = dxHat.y + (dx.y - dxHat.y) * aD;
    dxHat.z = dxHat.z + (dx.z - dxHat.z) * aD;

    const float speed = std::sqrt(dxHat.x * dxHat.x + dxHat.y * dxHat.y + dxHat.z * dxHat.z);
    const float cutoff = std::max(0.0001f, minCutoff + beta * speed);
    const float a = OneEuroAlpha(cutoff, dt);

    xHat.x = xHat.x + AngleDeltaDeg(x.x, xHat.x) * a;
    xHat.y = xHat.y + AngleDeltaDeg(x.y, xHat.y) * a;
    xHat.z = xHat.z + AngleDeltaDeg(x.z, xHat.z) * a;

    // Keep angles in a sane range.
    while (xHat.x > 180.f) xHat.x -= 360.f;
    while (xHat.x < -180.f) xHat.x += 360.f;
    while (xHat.y > 180.f) xHat.y -= 360.f;
    while (xHat.y < -180.f) xHat.y += 360.f;
    while (xHat.z > 180.f) xHat.z -= 360.f;
    while (xHat.z < -180.f) xHat.z += 360.f;
    return xHat;
}

// ----------------------------
// Player name extraction (robust across engine struct variants)
// ----------------------------

static inline size_t BoundedStrLen(const char* s, size_t maxLen)
{
    if (!s) return 0;
    size_t n = 0;
    for (; n < maxLen; ++n)
    {
        if (s[n] == '\0')
            break;
    }
    return n;
}

static inline bool LooksLikeSteamGuidOrJunk(const char* s)
{
    if (!s || !*s) return true;
    // Reject obvious non-name payloads that could be misread if the struct layout is off.
    // (Examples: STEAM_*, BOT, GUID-like tokens)
    if (std::strncmp(s, "STEAM_", 6) == 0) return true;
    if (std::strncmp(s, "BOT", 3) == 0) return true;
    if (std::strncmp(s, "STEAM_ID_", 9) == 0) return true;

    // Reject GUID-like or machine-token payloads (mostly hex + separators).
    const size_t n = std::strlen(s);
    if (n >= 16)
    {
        size_t hexish = 0;
        size_t separators = 0;
        size_t other = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const unsigned char c = (unsigned char)s[i];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                ++hexish;
            else if (c == '-' || c == '_' || c == ':' || c == '{' || c == '}')
                ++separators;
            else
                ++other;
        }
        if (other == 0 && hexish >= 12)
            return true;
    }

    return false;
}

static inline bool CopyNormalizedNameCandidate(const char* src, size_t srcMax, char* dst, size_t dstSize)
{
    if (!dst || dstSize == 0) return false;
    dst[0] = 0;
    if (!src) return false;

    // Require a terminating NUL within srcMax.
    const size_t rawLen = BoundedStrLen(src, srcMax);
    if (rawLen == 0 || rawLen >= srcMax)
        return false;

    // Trim leading ASCII whitespace/control chars.
    size_t b = 0;
    while (b < rawLen)
    {
        const unsigned char c = (unsigned char)src[b];
        if (c > 0x20) break;
        ++b;
    }
    if (b >= rawLen)
        return false;

    // Trim trailing ASCII whitespace.
    size_t e = rawLen;
    while (e > b)
    {
        const unsigned char c = (unsigned char)src[e - 1];
        if (c > 0x20) break;
        --e;
    }
    if (e <= b)
        return false;

    // Heuristic: reject payloads that are clearly not player names.
    if (LooksLikeSteamGuidOrJunk(src + b))
        return false;

    // Heuristic: reject strings with control chars in the trimmed region.
    int printable = 0;
    int controls = 0;
    for (size_t i = b; i < e; ++i)
    {
        const unsigned char c = (unsigned char)src[i];
        if (c == 0) break;
        if (c < 0x20) ++controls;
        else ++printable;
    }
    if (printable <= 0 || controls > 0)
        return false;

    const size_t want = e - b;
    const size_t ncopy = (want < (dstSize - 1)) ? want : (dstSize - 1);
    std::memcpy(dst, src + b, ncopy);
    dst[ncopy] = 0;
    return dst[0] != 0;
}

static inline int ScoreNameCandidate(const char* s)
{
    if (!s || !*s)
        return (std::numeric_limits<int>::min)();

    size_t len = 0;
    int alnum = 0;
    int printable = 0;
    int punctuation = 0;
    int highBytes = 0;

    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
    {
        const unsigned char c = *p;
        ++len;
        if (c < 0x20 || c == 0x7F)
            return (std::numeric_limits<int>::min)();
        if (c < 0x80)
        {
            ++printable;
            if (std::isalnum(c))
                ++alnum;
            else if (std::ispunct(c))
                ++punctuation;
        }
        else
        {
            ++printable;
            ++highBytes;
        }
    }

    if (len == 0 || printable == 0)
        return (std::numeric_limits<int>::min)();

    int score = 0;
    score += (int)len * 6;          // Prefer fuller names over accidental 1-2 byte junk.
    score += alnum * 2;
    score += highBytes;             // Keep non-ASCII names competitive.
    score -= punctuation;

    // Penalize very short payloads to avoid false positives from misaligned struct probes.
    if (len <= 2) score -= 40;
    else if (len <= 4) score -= 12;

    return score;
}

// Best-effort UTF-8 player name. Works even if our player_info_t struct is slightly mismatched
// by probing multiple plausible locations for the name string.
static inline bool GetPlayerNameUtf8Safe(IEngineClient* engine, int entIndex, char* outName, size_t outNameSize)
{
    if (!outName || outNameSize == 0)
        return false;
    outName[0] = 0;
    if (!engine || entIndex <= 0)
        return false;

    player_info_t info{};
    if (!engine->GetPlayerInfo(entIndex, &info))
        return false;

    char bestName[128] = { 0 };
    int bestScore = (std::numeric_limits<int>::min)();

    auto consider = [&](const char* src, size_t srcMax)
        {
            char cand[128] = { 0 };
            if (!CopyNormalizedNameCandidate(src, srcMax, cand, sizeof(cand)))
                return;

            const int score = ScoreNameCandidate(cand);
            if (score > bestScore)
            {
                bestScore = score;
                const size_t n = (std::min)(std::strlen(cand), sizeof(bestName) - 1);
                if (n > 0)
                    std::memcpy(bestName, cand, n);
                bestName[n] = 0;
            }
        };

    // 1) Preferred fields.
    consider(info.name, sizeof(info.name));
    consider(info.friendsName, sizeof(info.friendsName));

    // 2) Some builds place name at the beginning or after an 8-byte XUID.
    // Probe a few common offsets and pick the most plausible candidate.
    const char* blob = reinterpret_cast<const char*>(&info);
    consider(blob + 16, 128);
    consider(blob + 8, 128);
    consider(blob + 0, 128);

    if (bestScore == (std::numeric_limits<int>::min)() || !bestName[0])
        return false;

    const size_t n = (std::min)(std::strlen(bestName), outNameSize - 1);
    if (n > 0)
        std::memcpy(outName, bestName, n);
    outName[n] = 0;
    return outName[0] != 0;
}

#include "vr/vr_lifecycle_init.inl"
#include "vr/vr_lifecycle_update.inl"
#include "vr/vr_lifecycle_pose_hud.inl"
#include "vr/vr_process_input.inl"
#include "vr/vr_roomscale_prediction.inl"
#include "vr/vr_tracking.inl"
#include "vr/vr_aiming.inl"
#include "vr/vr_viewmodel_config.inl"

namespace
{
    constexpr float kKillSoundTraceDistance = 8192.0f;
    constexpr int kFeedbackSoundVoiceCount = 8;
    constexpr int kGameEventDebugIdInit = 42;

    struct FeedbackSoundVoiceState
    {
        std::string alias;
        std::chrono::steady_clock::time_point lastStarted{};
    };

    class VRKillSoundEventListener final : public IGameEventListener2
    {
    public:
        explicit VRKillSoundEventListener(VR* vr)
            : m_VR(vr)
        {
        }

        void FireGameEvent(IGameEvent* event) override
        {
            if (m_VR)
                m_VR->HandleKillSoundGameEvent(event);
        }

        int GetEventDebugID(void) override
        {
            return kGameEventDebugIdInit;
        }

    private:
        VR* m_VR = nullptr;
    };

    static std::string TrimCopy(std::string value)
    {
        auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) { return !isSpace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) { return !isSpace(ch); }).base(), value.end());
        return value;
    }

    static bool StartsWithInsensitive(const std::string& value, const char* prefix)
    {
        if (!prefix)
            return false;

        const size_t prefixLen = std::strlen(prefix);
        if (value.size() < prefixLen)
            return false;

        for (size_t i = 0; i < prefixLen; ++i)
        {
            if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        }

        return true;
    }

    static bool EndsWithInsensitive(const std::string& value, const char* suffix)
    {
        if (!suffix)
            return false;

        const size_t suffixLen = std::strlen(suffix);
        if (value.size() < suffixLen)
            return false;

        const size_t start = value.size() - suffixLen;
        for (size_t i = 0; i < suffixLen; ++i)
        {
            if (std::tolower(static_cast<unsigned char>(value[start + i])) != std::tolower(static_cast<unsigned char>(suffix[i])))
                return false;
        }

        return true;
    }

    static bool FileExistsPath(const std::string& path)
    {
        if (path.empty())
            return false;

        DWORD attrs = ::GetFileAttributesA(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    static std::string JoinWindowsPath(const std::string& base, const std::string& child)
    {
        if (base.empty())
            return child;
        if (child.empty())
            return base;

        const char tail = base.back();
        if (tail == '\\' || tail == '/')
            return base + child;

        return base + "\\" + child;
    }

    static std::string GetModuleDirectoryA()
    {
        char modulePath[MAX_PATH] = {};
        const DWORD len = ::GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return {};

        std::string path(modulePath, len);
        const size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos)
            return {};

        return path.substr(0, slash);
    }

    static bool TryParseConfigFloatValue(const std::string& line, const char* key, float& outValue)
    {
        if (!key || !*key)
            return false;

        const std::string trimmed = TrimCopy(line);
        if (trimmed.empty())
            return false;
        if (trimmed[0] == '/' || trimmed[0] == '#' || StartsWithInsensitive(trimmed, "//"))
            return false;
        if (!StartsWithInsensitive(trimmed, key))
            return false;

        const size_t keyLen = std::strlen(key);
        if (trimmed.size() > keyLen)
        {
            const char next = trimmed[keyLen];
            if (!std::isspace(static_cast<unsigned char>(next)) && next != '=')
                return false;
        }

        std::string value = TrimCopy(trimmed.substr(keyLen));
        if (!value.empty() && value.front() == '=')
            value = TrimCopy(value.substr(1));
        if (value.empty())
            return false;

        if (value.front() == '"')
        {
            const size_t closingQuote = value.find('"', 1);
            if (closingQuote != std::string::npos)
                value = value.substr(1, closingQuote - 1);
            else
                value.erase(value.begin());
        }

        char* endPtr = nullptr;
        const float parsed = std::strtof(value.c_str(), &endPtr);
        if (!endPtr || endPtr == value.c_str())
            return false;

        outValue = parsed;
        return true;
    }

    static int GetLocalPlayerUserId(Game* game)
    {
        if (!game || !game->m_EngineClient)
            return 0;

        const int localPlayerIndex = game->m_EngineClient->GetLocalPlayer();
        if (localPlayerIndex <= 0)
            return 0;

        player_info_t playerInfo{};
        if (!game->m_EngineClient->GetPlayerInfo(localPlayerIndex, &playerInfo))
            return 0;

        auto isValidUserId = [&](int candidateUserId)
            {
                return candidateUserId > 0
                    && candidateUserId <= 0x7FFF
                    && game->m_EngineClient->GetPlayerForUserID(candidateUserId) == localPlayerIndex;
            };

        auto readUserIdAtOffset = [&](size_t offset)
            {
                if (offset + sizeof(int) > sizeof(player_info_t))
                    return 0;

                int candidateUserId = 0;
                std::memcpy(&candidateUserId, reinterpret_cast<const unsigned char*>(&playerInfo) + offset, sizeof(candidateUserId));
                return isValidUserId(candidateUserId) ? candidateUserId : 0;
            };

        static size_t s_cachedUserIdOffset = SIZE_MAX;
        static bool s_loggedCompatOffset = false;
        static bool s_loggedResolveFailure = false;

        if (s_cachedUserIdOffset != SIZE_MAX)
        {
            const int cachedUserId = readUserIdAtOffset(s_cachedUserIdOffset);
            if (cachedUserId > 0)
                return cachedUserId;

            s_cachedUserIdOffset = SIZE_MAX;
            s_loggedCompatOffset = false;
        }

        const size_t declaredOffset = offsetof(player_info_t, userID);
        const int declaredUserId = readUserIdAtOffset(declaredOffset);
        if (declaredUserId > 0)
        {
            s_cachedUserIdOffset = declaredOffset;
            s_loggedResolveFailure = false;
            return declaredUserId;
        }

        const size_t scanLimit = (std::min)(sizeof(player_info_t), static_cast<size_t>(256));
        for (size_t offset = 0; offset + sizeof(int) <= scanLimit; offset += sizeof(int))
        {
            if (offset == declaredOffset)
                continue;

            const int candidateUserId = readUserIdAtOffset(offset);
            if (candidateUserId <= 0)
                continue;

            s_cachedUserIdOffset = offset;
            s_loggedResolveFailure = false;
            if (!s_loggedCompatOffset)
            {
                Game::logMsg(
                    "[VR][KillSound][userid] localPlayer=%d userId=%d offset=0x%zX (compat)",
                    localPlayerIndex,
                    candidateUserId,
                    offset);
                s_loggedCompatOffset = true;
            }
            return candidateUserId;
        }

        if (!s_loggedResolveFailure)
        {
            Game::logMsg(
                "[VR][KillSound][userid] failed localPlayer=%d declaredOffset=0x%zX declaredValue=%d",
                localPlayerIndex,
                declaredOffset,
                playerInfo.userID);
            s_loggedResolveFailure = true;
        }

        return 0;
    }

    static std::uintptr_t ResolveKillEventEntityTag(Game* game, IGameEvent* event, const std::string& eventName, int& outEntityIndex)
    {
        outEntityIndex = 0;
        if (!game || !event)
            return 0;

        if (eventName == "infected_death")
        {
            outEntityIndex = event->GetInt("infected_id", 0);
        }
        else if (eventName == "witch_killed")
        {
            outEntityIndex = event->GetInt("witchid", 0);
        }
        else if (eventName == "player_death")
        {
            const int victimUserId = event->GetInt("userid", 0);
            if (victimUserId > 0 && game->m_EngineClient)
                outEntityIndex = game->m_EngineClient->GetPlayerForUserID(victimUserId);
            if (outEntityIndex <= 0)
                outEntityIndex = event->GetInt("entityid", 0);
        }

        if (outEntityIndex <= 0)
            return 0;

        return reinterpret_cast<std::uintptr_t>(game->GetClientEntity(outEntityIndex));
    }

    static float ReadGameMasterVolumeFromConfig(IEngineClient* engine)
    {
        struct CachedGameVolumeState
        {
            bool wasInGame = false;
            float cachedVolume = 1.0f;
            bool initialized = false;
        };

        static CachedGameVolumeState state{};

        const bool inGame = engine && engine->IsInGame();
        if (!inGame)
        {
            state = {};
            return 1.0f;
        }

        if (state.initialized && state.wasInGame)
            return state.cachedVolume;

        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
        {
            state.wasInGame = true;
            state.cachedVolume = 1.0f;
            state.initialized = true;
            return state.cachedVolume;
        }

        const std::array<std::string, 2> candidates =
        {
            JoinWindowsPath(JoinWindowsPath(moduleDir, "left4dead2"), "cfg\\config.cfg"),
            JoinWindowsPath(moduleDir, "config.cfg")
        };

        for (const auto& candidate : candidates)
        {
            std::ifstream file(candidate);
            if (!file.is_open())
                continue;

            float parsedVolume = 1.0f;
            std::string line;
            while (std::getline(file, line))
            {
                float value = 0.0f;
                if (TryParseConfigFloatValue(line, "volume", value))
                    parsedVolume = value;
            }

            state.wasInGame = true;
            state.cachedVolume = std::clamp(parsedVolume, 0.0f, 1.0f);
            state.initialized = true;
            return state.cachedVolume;
        }

        state.wasInGame = true;
        state.cachedVolume = 1.0f;
        state.initialized = true;
        return state.cachedVolume;
    }

    static std::string ResolveKillSoundFilePath(const std::string& rawPath)
    {
        const std::string path = TrimCopy(rawPath);
        if (path.empty())
            return {};

        if (FileExistsPath(path))
            return path;

        const std::string moduleDir = GetModuleDirectoryA();
        if (!moduleDir.empty())
        {
            const std::string fromModule = JoinWindowsPath(moduleDir, path);
            if (FileExistsPath(fromModule))
                return fromModule;

            const std::string fromVrDir = JoinWindowsPath(JoinWindowsPath(moduleDir, "VR"), path);
            if (FileExistsPath(fromVrDir))
                return fromVrDir;
        }

        return {};
    }

    static bool LooksLikeAudioFilePath(const std::string& value)
    {
        return value.find('\\') != std::string::npos
            || value.find('/') != std::string::npos
            || value.find(':') != std::string::npos
            || EndsWithInsensitive(value, ".wav")
            || EndsWithInsensitive(value, ".mp3")
            || EndsWithInsensitive(value, ".ogg")
            || EndsWithInsensitive(value, ".m4a")
            || EndsWithInsensitive(value, ".aac")
            || EndsWithInsensitive(value, ".wma")
            || EndsWithInsensitive(value, ".flac");
    }

    static std::string EscapeMciString(std::string value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (char ch : value)
        {
            if (ch != '"')
                escaped.push_back(ch);
        }
        return escaped;
    }

    static std::array<FeedbackSoundVoiceState, kFeedbackSoundVoiceCount>& GetFeedbackSoundVoices()
    {
        static std::array<FeedbackSoundVoiceState, kFeedbackSoundVoiceCount> voices{};
        static bool initialized = false;
        if (!initialized)
        {
            for (int i = 0; i < kFeedbackSoundVoiceCount; ++i)
                voices[static_cast<size_t>(i)].alias = "l4d2vr_feedback_" + std::to_string(i);

            initialized = true;
        }

        return voices;
    }

    static void CloseFeedbackSoundVoice(const std::string& alias)
    {
        if (alias.empty())
            return;

        const std::string closeCmd = std::string("close ") + alias;
        ::mciSendStringA(closeCmd.c_str(), nullptr, 0, nullptr);
    }

    static FeedbackSoundVoiceState& AcquireFeedbackSoundVoice()
    {
        auto& voices = GetFeedbackSoundVoices();
        auto it = std::min_element(
            voices.begin(),
            voices.end(),
            [](const FeedbackSoundVoiceState& lhs, const FeedbackSoundVoiceState& rhs)
            {
                return lhs.lastStarted < rhs.lastStarted;
            });

        CloseFeedbackSoundVoice(it->alias);
        it->lastStarted = std::chrono::steady_clock::now();
        return *it;
    }

    static void ApplyFeedbackSoundStereoVolumes(const std::string& alias, int leftVolume, int rightVolume)
    {
        if (alias.empty())
            return;

        leftVolume = std::clamp(leftVolume, 0, 1000);
        rightVolume = std::clamp(rightVolume, 0, 1000);

        const std::string leftCmd = "setaudio " + alias + " left volume to " + std::to_string(leftVolume);
        ::mciSendStringA(leftCmd.c_str(), nullptr, 0, nullptr);

        const std::string rightCmd = "setaudio " + alias + " right volume to " + std::to_string(rightVolume);
        ::mciSendStringA(rightCmd.c_str(), nullptr, 0, nullptr);
    }

    static bool TryPlayFeedbackSoundFilePath(const std::string& rawPath, int leftVolume, int rightVolume)
    {
        const std::string resolvedPath = ResolveKillSoundFilePath(rawPath);
        if (resolvedPath.empty())
            return false;

        FeedbackSoundVoiceState& voice = AcquireFeedbackSoundVoice();
        const std::string openCmd = std::string("open \"") + EscapeMciString(resolvedPath) + "\" alias " + voice.alias;
        if (::mciSendStringA(openCmd.c_str(), nullptr, 0, nullptr) != 0)
            return false;

        ApplyFeedbackSoundStereoVolumes(voice.alias, leftVolume, rightVolume);

        const std::string playCmd = std::string("play ") + voice.alias + " from 0";
        if (::mciSendStringA(playCmd.c_str(), nullptr, 0, nullptr) != 0)
        {
            CloseFeedbackSoundVoice(voice.alias);
            return false;
        }

        return true;
    }

    static float Clamp01(float value)
    {
        return std::clamp(value, 0.0f, 1.0f);
    }

    static float Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    static float EaseOutCubic(float t)
    {
        const float clamped = Clamp01(t);
        const float inv = 1.0f - clamped;
        return 1.0f - inv * inv * inv;
    }

    static std::string NormalizeMaterialPathSpec(std::string rawSpec)
    {
        std::string spec = TrimCopy(rawSpec);
        if (spec.empty())
            return {};

        std::replace(spec.begin(), spec.end(), '\\', '/');

        std::string lowered = spec;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        const size_t materialMarker = lowered.find("/materials/");
        if (materialMarker != std::string::npos)
            spec = spec.substr(materialMarker + 11);
        else if (lowered.rfind("materials/", 0) == 0)
            spec = spec.substr(10);

        while (!spec.empty() && spec.front() == '/')
            spec.erase(spec.begin());

        if (EndsWithInsensitive(spec, ".vmt"))
            spec.erase(spec.size() - 4);

        while (!spec.empty() && spec.back() == '/')
            spec.pop_back();

        return TrimCopy(spec);
    }

    static std::string BuildKillIndicatorMaterialName(const std::string& rawBaseSpec, const char* desiredLeaf)
    {
        std::string spec = NormalizeMaterialPathSpec(rawBaseSpec);
        if (spec.empty() || !desiredLeaf || !*desiredLeaf)
            return {};

        const size_t slash = spec.find_last_of('/');
        const std::string leaf = (slash == std::string::npos) ? spec : spec.substr(slash + 1);

        if (_stricmp(leaf.c_str(), "kill") == 0 || _stricmp(leaf.c_str(), "headshot") == 0 || _stricmp(leaf.c_str(), "hit") == 0)
        {
            if (slash == std::string::npos)
                return desiredLeaf;

            return spec.substr(0, slash + 1) + desiredLeaf;
        }

        return spec + "/" + desiredLeaf;
    }

    static float GetActiveIndicatorLifetimeSeconds(const VR::ActiveKillIndicator& indicator, float killLifetimeSeconds)
    {
        if (indicator.killConfirmed)
            return (std::max)(0.10f, killLifetimeSeconds);

        const float scaled = killLifetimeSeconds * 0.42f;
        return std::clamp(scaled, 0.10f, 0.45f);
    }

    static bool ProjectKillIndicatorToHud(const VR* vr, const Vector& worldPos, int screenWidth, int screenHeight, float maxDistance, int& outX, int& outY)
    {
        if (!vr || screenWidth <= 0 || screenHeight <= 0)
            return false;

        Vector camForward{};
        Vector camRight{};
        Vector camUp{};
        QAngle::AngleVectors(vr->m_SetupAngles, &camForward, &camRight, &camUp);
        if (camForward.IsZero() || camRight.IsZero() || camUp.IsZero())
        {
            camForward = vr->m_HmdForward;
            camRight = vr->m_HmdRight;
            camUp = vr->m_HmdUp;
        }

        if (camForward.IsZero() || camRight.IsZero() || camUp.IsZero())
            return false;

        VectorNormalize(camForward);
        VectorNormalize(camRight);
        VectorNormalize(camUp);

        const Vector delta = worldPos - vr->m_SetupOrigin;
        if (maxDistance > 0.0f && delta.LengthSqr() > (maxDistance * maxDistance))
            return false;

        const float depth = DotProduct(delta, camForward);
        if (depth <= 4.0f)
            return false;

        constexpr float kPi = 3.14159265358979323846f;
        const float fovXDeg = std::clamp(vr->m_Fov, 10.0f, 170.0f);
        const float tanHalfFovX = std::tan((fovXDeg * 0.5f) * (kPi / 180.0f));
        const float aspect = std::max(0.25f, static_cast<float>(screenWidth) / static_cast<float>(screenHeight));
        const float tanHalfFovY = tanHalfFovX / aspect;
        if (tanHalfFovX <= 0.0f || tanHalfFovY <= 0.0f)
            return false;

        const float ndcX = (DotProduct(delta, camRight) / depth) / tanHalfFovX;
        const float ndcY = (DotProduct(delta, camUp) / depth) / tanHalfFovY;
        if (std::fabs(ndcX) > 1.2f || std::fabs(ndcY) > 1.2f)
            return false;

        const float clampedX = std::clamp(ndcX, -0.96f, 0.96f);
        const float clampedY = std::clamp(ndcY, -0.96f, 0.96f);

        outX = static_cast<int>(std::lround((clampedX * 0.5f + 0.5f) * static_cast<float>(screenWidth)));
        outY = static_cast<int>(std::lround((0.5f - clampedY * 0.5f) * static_cast<float>(screenHeight)));
        return true;
    }
}

bool VR::ReadLocalKillCounters(C_BasePlayer* localPlayer, int& outCommon, int& outSpecial) const
{
    outCommon = 0;
    outSpecial = 0;

    if (!localPlayer)
        return false;

    const auto* base = reinterpret_cast<const unsigned char*>(localPlayer);
    int localTeam = 0;
    if (!VR_TryReadI32(base, kTeamNumOffset, localTeam) || localTeam != 2)
        return false;

    auto readKillsArray = [&](int baseOff, int& common, int& special) -> bool
        {
            int value = 0;
            bool okAny = false;

            common = 0;
            special = 0;

            if (VR_TryReadI32(base, baseOff + 0 * 4, value))
            {
                common = (std::max)(0, value);
                okAny = true;
            }

            int specialSum = 0;
            for (int i = 1; i <= kZombieKillsMaxIndex; ++i)
            {
                if (VR_TryReadI32(base, baseOff + i * 4, value))
                {
                    specialSum += (std::max)(0, value);
                    okAny = true;
                }
            }

            special = specialSum;
            return okAny;
        };

    int missionCommon = 0;
    int missionSpecial = 0;
    int checkpointCommon = 0;
    int checkpointSpecial = 0;
    const bool missionOk = readKillsArray(kMissionZombieKillsOffset, missionCommon, missionSpecial);
    const bool checkpointOk = readKillsArray(kCheckpointZombieKillsOffset, checkpointCommon, checkpointSpecial);

    const int missionSum = missionCommon + missionSpecial;
    const int checkpointSum = checkpointCommon + checkpointSpecial;

    if ((!missionOk || missionSum == 0) && checkpointOk && checkpointSum > 0)
    {
        outCommon = checkpointCommon;
        outSpecial = checkpointSpecial;
        return true;
    }

    if (missionOk)
    {
        outCommon = missionCommon;
        outSpecial = missionSpecial;
        return true;
    }

    if (checkpointOk)
    {
        outCommon = checkpointCommon;
        outSpecial = checkpointSpecial;
        return true;
    }

    return false;
}

bool VR::ReadLocalHeadshotCounter(C_BasePlayer* localPlayer, int& outHeadshots) const
{
    outHeadshots = 0;

    if (!localPlayer)
        return false;

    const auto* base = reinterpret_cast<const unsigned char*>(localPlayer);
    int localTeam = 0;
    if (!VR_TryReadI32(base, kTeamNumOffset, localTeam) || localTeam != 2)
        return false;

    int value = 0;
    if (!VR_TryReadI32(base, kCheckpointHeadshotsOffset, value))
        return false;

    outHeadshots = (std::max)(0, value);
    return true;
}

void VR::EnsureKillSoundEventListener()
{
    if (m_KillSoundEventListenerRegistered || !m_Game)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_LastKillSoundEventRegisterAttempt.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastKillSoundEventRegisterAttempt).count();
        if (elapsed < 2.0f)
            return;
    }
    m_LastKillSoundEventRegisterAttempt = now;

    if (!m_KillSoundEventManager)
        m_KillSoundEventManager = m_Game->m_GameEventManager;
    if (!m_KillSoundEventManager)
    {
        Game::logMsg("[VR][KillSound][listener] missing game event manager");
        return;
    }

    if (!m_KillSoundEventListener)
        m_KillSoundEventListener = new VRKillSoundEventListener(this);

    static constexpr const char* kKillSoundEvents[] = { "player_death", "infected_death", "witch_killed" };
    bool registeredAll = true;
    for (const char* eventName : kKillSoundEvents)
    {
        const bool alreadyRegistered = m_KillSoundEventManager->FindListener(m_KillSoundEventListener, eventName);
        const bool registered = alreadyRegistered || m_KillSoundEventManager->AddListener(m_KillSoundEventListener, eventName, false);
        Game::logMsg("[VR][KillSound][listener] event=%s registered=%d", eventName, registered ? 1 : 0);
        registeredAll = registeredAll && registered;
    }

    m_KillSoundEventListenerRegistered = registeredAll;
}

void VR::HandleKillSoundGameEvent(IGameEvent* event)
{
    if (!event || !m_Game || !m_Game->m_EngineClient)
        return;

    const char* rawEventName = event->GetName();
    if (!rawEventName || !*rawEventName)
        return;

    const std::string eventName(rawEventName);
    const int localUserId = GetLocalPlayerUserId(m_Game);
    Game::logMsg("[VR][KillSound][event-raw] name=%s localUserId=%d", eventName.c_str(), localUserId);
    if (localUserId <= 0)
        return;

    int attackerUserId = 0;
    bool headshot = false;
    if (eventName == "player_death")
    {
        attackerUserId = event->GetInt("attacker", 0);
        headshot = event->GetBool("headshot", false);
    }
    else if (eventName == "infected_death")
    {
        attackerUserId = event->GetInt("attacker", 0);
        headshot = event->GetBool("headshot", false);
    }
    else if (eventName == "witch_killed")
    {
        attackerUserId = event->GetInt("userid", 0);
        headshot = false;
    }
    else
    {
        return;
    }

    if (attackerUserId != localUserId)
    {
        Game::logMsg(
            "[VR][KillSound][event-skip] name=%s attacker=%d local=%d",
            eventName.c_str(),
            attackerUserId,
            localUserId);
        return;
    }

    int resolvedEntityIndex = 0;
    const std::uintptr_t entityTag = ResolveKillEventEntityTag(m_Game, event, eventName, resolvedEntityIndex);
    const int victimUserId = event->GetInt("userid", 0);
    const int victimEntityId = event->GetInt("entityid", 0);
    const int infectedId = event->GetInt("infected_id", 0);
    const int witchId = event->GetInt("witchid", 0);
    const char* weapon = event->GetString("weapon", "");

    Game::logMsg(
        "[VR][KillSound][event] name=%s attacker=%d userid=%d entityid=%d infected_id=%d witchid=%d headshot=%d weapon=%s resolvedEntity=%d tag=%p",
        eventName.c_str(),
        attackerUserId,
        victimUserId,
        victimEntityId,
        infectedId,
        witchId,
        headshot ? 1 : 0,
        weapon ? weapon : "",
        resolvedEntityIndex,
        reinterpret_cast<void*>(entityTag));

    QueuePendingKillSoundEvent(entityTag, headshot, eventName.c_str());
}

void VR::QueuePendingKillSoundEvent(std::uintptr_t entityTag, bool headshot, const char* eventName)
{
    const auto now = std::chrono::steady_clock::now();
    const float eventLifetimeSeconds = (std::max)(0.5f, m_KillSoundDetectionWindowSeconds + 0.35f);
    m_PendingKillSoundEvents.erase(
        std::remove_if(
            m_PendingKillSoundEvents.begin(),
            m_PendingKillSoundEvents.end(),
            [&](const PendingKillSoundEvent& pendingEvent)
            {
                return std::chrono::duration<float>(now - pendingEvent.receivedAt).count() >= eventLifetimeSeconds;
            }),
        m_PendingKillSoundEvents.end());

    PendingKillSoundEvent pendingEvent{};
    pendingEvent.entityTag = entityTag;
    pendingEvent.headshot = headshot;
    pendingEvent.receivedAt = now;
    pendingEvent.eventName = eventName ? eventName : "";
    m_PendingKillSoundEvents.push_back(std::move(pendingEvent));
    if (m_PendingKillSoundEvents.size() > 32)
        m_PendingKillSoundEvents.erase(m_PendingKillSoundEvents.begin(), m_PendingKillSoundEvents.begin() + (m_PendingKillSoundEvents.size() - 32));
}

bool VR::ConsumePendingKillSoundEvent(std::chrono::steady_clock::time_point now, bool& outHeadshot, std::uintptr_t& outEntityTag, std::string* outEventName)
{
    const float eventLifetimeSeconds = (std::max)(0.5f, m_KillSoundDetectionWindowSeconds + 0.35f);
    m_PendingKillSoundEvents.erase(
        std::remove_if(
            m_PendingKillSoundEvents.begin(),
            m_PendingKillSoundEvents.end(),
            [&](const PendingKillSoundEvent& pendingEvent)
            {
                return std::chrono::duration<float>(now - pendingEvent.receivedAt).count() >= eventLifetimeSeconds;
            }),
        m_PendingKillSoundEvents.end());

    if (m_PendingKillSoundEvents.empty())
        return false;

    const PendingKillSoundEvent pendingEvent = m_PendingKillSoundEvents.front();
    m_PendingKillSoundEvents.erase(m_PendingKillSoundEvents.begin());

    outHeadshot = pendingEvent.headshot;
    outEntityTag = pendingEvent.entityTag;
    if (outEventName)
        *outEventName = pendingEvent.eventName;
    return true;
}

bool VR::IsKillSoundTargetEntity(const C_BaseEntity* entity) const
{
    if (!entity)
        return false;

    const auto* base = reinterpret_cast<const unsigned char*>(entity);
    unsigned char lifeState = 1;
    int team = 0;
    if (VR_TryReadU8(base, kLifeStateOffset, lifeState) && VR_TryReadI32(base, kTeamNumOffset, team))
    {
        if (lifeState == 0 && team == 3)
            return true;
    }

    if (GetSpecialInfectedType(entity) != SpecialInfectedType::None)
        return true;

    if (m_Game)
    {
        const char* className = m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(const_cast<C_BaseEntity*>(entity)));
        if (className)
        {
            std::string lowered = className;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (lowered.find("infected") != std::string::npos || lowered.find("witch") != std::string::npos)
                return true;
        }
    }

    return false;
}

void VR::RegisterPotentialKillSoundHit(const Vector& start, const QAngle& angles)
{
    const bool wantsHitFeedback = m_HitSoundEnabled || m_KillIndicatorEnabled;
    const bool wantsKillFeedback = m_KillSoundEnabled || m_KillIndicatorEnabled;
    if ((!wantsHitFeedback && !wantsKillFeedback) || !m_Game || !m_Game->m_EngineTrace || !m_Game->m_EngineClient)
        return;

    const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* localPlayer = reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(localPlayerIndex));
    if (!localPlayer)
        return;

    Vector forward{};
    Vector right{};
    Vector up{};
    QAngle::AngleVectors(angles, &forward, &right, &up);
    if (forward.IsZero())
        return;
    VectorNormalize(forward);

    Ray_t ray;
    ray.Init(start, start + forward * kKillSoundTraceDistance);

    CTraceFilterSkipSelf traceFilter(reinterpret_cast<IHandleEntity*>(localPlayer), 0);
    CGameTrace trace{};
    m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT, &traceFilter, &trace);

    C_BaseEntity* entity = reinterpret_cast<C_BaseEntity*>(trace.m_pEnt);
    if (!entity || !IsKillSoundTargetEntity(entity))
        return;

    if (m_HitSoundEnabled)
        PlayHitSound(&trace.endpos);

    if (m_KillIndicatorEnabled)
        SpawnHitIndicator(trace.endpos);

    if (!wantsKillFeedback)
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto expiresAt = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(m_KillSoundDetectionWindowSeconds));
    const std::uintptr_t entityTag = reinterpret_cast<std::uintptr_t>(entity);

    m_PendingKillSoundHits.erase(
        std::remove_if(
            m_PendingKillSoundHits.begin(),
            m_PendingKillSoundHits.end(),
            [&](const PendingKillSoundHit& hit)
            {
                return hit.entityTag == 0 || hit.expiresAt < now;
            }),
        m_PendingKillSoundHits.end());

    for (auto& hit : m_PendingKillSoundHits)
    {
        if (hit.entityTag == entityTag)
        {
            hit.expiresAt = expiresAt;
            hit.impactPos = trace.endpos;
            return;
        }
    }

    PendingKillSoundHit hit{};
    hit.entityTag = entityTag;
    hit.expiresAt = expiresAt;
    hit.impactPos = trace.endpos;
    m_PendingKillSoundHits.push_back(hit);
    if (m_PendingKillSoundHits.size() > 24)
        m_PendingKillSoundHits.erase(m_PendingKillSoundHits.begin(), m_PendingKillSoundHits.begin() + (m_PendingKillSoundHits.size() - 24));
}

bool VR::ConsumePendingKillSoundHit(std::uintptr_t preferredEntityTag, std::chrono::steady_clock::time_point now, Vector* outImpactPos)
{
    m_PendingKillSoundHits.erase(
        std::remove_if(
            m_PendingKillSoundHits.begin(),
            m_PendingKillSoundHits.end(),
            [&](const PendingKillSoundHit& hit)
            {
                return hit.entityTag == 0 || hit.expiresAt < now;
            }),
        m_PendingKillSoundHits.end());

    if (preferredEntityTag != 0)
    {
        for (auto it = m_PendingKillSoundHits.rbegin(); it != m_PendingKillSoundHits.rend(); ++it)
        {
            if (it->entityTag != preferredEntityTag)
                continue;

            if (outImpactPos)
                *outImpactPos = it->impactPos;
            m_PendingKillSoundHits.erase(std::next(it).base());
            return true;
        }
    }

    for (auto it = m_PendingKillSoundHits.rbegin(); it != m_PendingKillSoundHits.rend(); ++it)
    {
        const auto* entity = reinterpret_cast<const C_BaseEntity*>(it->entityTag);
        if (entity && IsEntityAlive(entity))
            continue;
        if (outImpactPos)
            *outImpactPos = it->impactPos;
        m_PendingKillSoundHits.erase(std::next(it).base());
        return true;
    }

    if (!m_PendingKillSoundHits.empty())
    {
        if (outImpactPos)
            *outImpactPos = m_PendingKillSoundHits.back().impactPos;
        m_PendingKillSoundHits.pop_back();
        return true;
    }

    return false;
}

bool VR::TryPlayKillSoundSpec(const std::string& rawSpec, float baseVolume, const Vector* worldPos)
{
    const std::string spec = TrimCopy(rawSpec);
    if (spec.empty())
        return false;

    auto getPayload = [&](size_t prefixLen)
        {
            return TrimCopy(spec.substr(prefixLen));
        };

    if (StartsWithInsensitive(spec, "alias:"))
    {
        const std::string alias = getPayload(6);
        return !alias.empty()
            && ::PlaySoundA(alias.c_str(), nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT) != FALSE;
    }

    if (StartsWithInsensitive(spec, "file:"))
    {
        const std::string path = getPayload(5);
        if (path.empty())
            return false;

        int leftVolume = 1000;
        int rightVolume = 1000;
        ComputeFeedbackSoundStereoVolumes(worldPos, baseVolume, leftVolume, rightVolume);
        return TryPlayFeedbackSoundFilePath(path, leftVolume, rightVolume);
    }

    if (StartsWithInsensitive(spec, "game:"))
    {
        const std::string soundPath = getPayload(5);
        if (soundPath.empty() || !m_Game)
            return false;

        const std::string cmd = "play " + soundPath;
        m_Game->ClientCmd_Unrestricted(cmd.c_str());
        return true;
    }

    if (StartsWithInsensitive(spec, "gamesound:"))
    {
        const std::string soundName = getPayload(10);
        if (soundName.empty() || !m_Game)
            return false;

        const std::string cmd = "playgamesound " + soundName;
        m_Game->ClientCmd_Unrestricted(cmd.c_str());
        return true;
    }

    if (StartsWithInsensitive(spec, "cmd:"))
    {
        const std::string cmd = getPayload(4);
        if (cmd.empty() || !m_Game)
            return false;

        m_Game->ClientCmd_Unrestricted(cmd.c_str());
        return true;
    }

    if (LooksLikeAudioFilePath(spec))
    {
        int leftVolume = 1000;
        int rightVolume = 1000;
        ComputeFeedbackSoundStereoVolumes(worldPos, baseVolume, leftVolume, rightVolume);
        return TryPlayFeedbackSoundFilePath(spec, leftVolume, rightVolume);
    }

    return ::PlaySoundA(spec.c_str(), nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT) != FALSE;
}

void VR::ComputeFeedbackSoundStereoVolumes(const Vector* worldPos, float baseVolume, int& outLeftVolume, int& outRightVolume) const
{
    const float gameMasterVolume = ReadGameMasterVolumeFromConfig(m_Game ? m_Game->m_EngineClient : nullptr);
    const float clampedBaseVolume = std::clamp(baseVolume * gameMasterVolume, 0.0f, 2.0f);
    float leftGain = clampedBaseVolume;
    float rightGain = clampedBaseVolume;

    if (worldPos && m_FeedbackSoundSpatialBlend > 0.0f)
    {
        Vector listenerForward{};
        Vector listenerRight{};
        Vector listenerUp{};
        QAngle::AngleVectors(m_SetupAngles, &listenerForward, &listenerRight, &listenerUp);
        if (listenerForward.IsZero() || listenerRight.IsZero() || listenerUp.IsZero())
        {
            listenerForward = m_HmdForward;
            listenerRight = m_HmdRight;
            listenerUp = m_HmdUp;
        }

        if (!listenerForward.IsZero() && !listenerRight.IsZero())
        {
            VectorNormalize(listenerForward);
            VectorNormalize(listenerRight);

            const Vector delta = *worldPos - m_SetupOrigin;
            const float distance = delta.Length();
            if (distance > 0.001f)
            {
                Vector dir = delta;
                VectorNormalize(dir);

                const float spatialBlend = Clamp01(m_FeedbackSoundSpatialBlend);
                const float pan = std::clamp(DotProduct(dir, listenerRight), -1.0f, 1.0f) * spatialBlend;
                const float normalizedPan = pan * 0.5f + 0.5f;
                const float panAngle = normalizedPan * (kPi * 0.5f);
                const float leftPan = std::cos(panAngle) * 1.41421356f;
                const float rightPan = std::sin(panAngle) * 1.41421356f;

                const float nearDistance = 96.0f;
                const float farDistance = (std::max)(nearDistance + 1.0f, m_FeedbackSoundSpatialRange);
                const float distanceT = Clamp01((distance - nearDistance) / (farDistance - nearDistance));
                const float distanceGain = 1.0f - 0.65f * spatialBlend * std::sqrt(distanceT);

                leftGain *= distanceGain * Lerp(1.0f, leftPan, spatialBlend);
                rightGain *= distanceGain * Lerp(1.0f, rightPan, spatialBlend);
            }
        }
    }

    outLeftVolume = std::clamp(static_cast<int>(std::lround(leftGain * 1000.0f)), 0, 1000);
    outRightVolume = std::clamp(static_cast<int>(std::lround(rightGain * 1000.0f)), 0, 1000);
}

void VR::PlayHitSound(const Vector* worldPos)
{
    if (!m_HitSoundEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_HitSoundPlaybackCooldownSeconds > 0.0f && m_LastHitSoundPlaybackTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastHitSoundPlaybackTime).count();
        if (elapsed < m_HitSoundPlaybackCooldownSeconds)
            return;
    }

    bool played = TryPlayKillSoundSpec(m_HitSoundSpec, m_HitSoundVolume, worldPos);
    if (!played)
        MessageBeep(MB_ICONQUESTION);

    m_LastHitSoundPlaybackTime = now;
}

void VR::PlayKillSound(bool headshot, const Vector* worldPos)
{
    if (!m_KillSoundEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_KillSoundPlaybackCooldownSeconds > 0.0f && m_LastKillSoundPlaybackTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastKillSoundPlaybackTime).count();
        if (elapsed < m_KillSoundPlaybackCooldownSeconds)
            return;
    }

    const std::string& preferredSpec = headshot && !m_KillSoundHeadshotSpec.empty()
        ? m_KillSoundHeadshotSpec
        : m_KillSoundNormalSpec;
    const float preferredVolume = headshot ? m_HeadshotSoundVolume : m_KillSoundVolume;

    bool played = TryPlayKillSoundSpec(preferredSpec, preferredVolume, worldPos);
    if (!played && headshot && !m_KillSoundNormalSpec.empty())
        played = TryPlayKillSoundSpec(m_KillSoundNormalSpec, m_KillSoundVolume, worldPos);

    if (!played)
        MessageBeep(headshot ? MB_ICONEXCLAMATION : MB_ICONASTERISK);

    m_LastKillSoundPlaybackTime = now;
}

IMaterial* VR::ResolveHitIndicatorMaterial()
{
    if (m_KillIndicatorHitMaterial && !m_KillIndicatorHitMaterial->IsErrorMaterial())
        return m_KillIndicatorHitMaterial;

    if (!m_Game || !m_Game->m_MaterialSystem)
        return nullptr;

    const std::string materialName = BuildKillIndicatorMaterialName(m_KillIndicatorMaterialBaseSpec, "hit");
    if (materialName.empty())
        return nullptr;

    m_KillIndicatorHitMaterial = m_Game->m_MaterialSystem->FindMaterial(materialName.c_str(), "Other textures", false, nullptr);
    if (!m_KillIndicatorHitMaterial || m_KillIndicatorHitMaterial->IsErrorMaterial())
    {
        m_KillIndicatorHitMaterial = nullptr;
        return nullptr;
    }

    return m_KillIndicatorHitMaterial;
}

IMaterial* VR::ResolveKillIndicatorMaterial(bool headshot)
{
    IMaterial*& cachedMaterial = headshot ? m_KillIndicatorHeadshotMaterial : m_KillIndicatorNormalMaterial;
    if (cachedMaterial && !cachedMaterial->IsErrorMaterial())
        return cachedMaterial;

    if (!m_Game || !m_Game->m_MaterialSystem)
        return nullptr;

    const std::string materialName = BuildKillIndicatorMaterialName(m_KillIndicatorMaterialBaseSpec, headshot ? "headshot" : "kill");
    if (materialName.empty())
        return nullptr;

    cachedMaterial = m_Game->m_MaterialSystem->FindMaterial(materialName.c_str(), "Other textures", false, nullptr);
    if (!cachedMaterial || cachedMaterial->IsErrorMaterial())
    {
        cachedMaterial = nullptr;
        return nullptr;
    }

    return cachedMaterial;
}

void VR::SpawnHitIndicator(const Vector& worldPos)
{
    if (!m_KillIndicatorEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    m_ActiveKillIndicators.erase(
        std::remove_if(
            m_ActiveKillIndicators.begin(),
            m_ActiveKillIndicators.end(),
            [&](const ActiveKillIndicator& indicator)
            {
                const float lifetime = GetActiveIndicatorLifetimeSeconds(indicator, m_KillIndicatorLifetimeSeconds);
                return std::chrono::duration<float>(now - indicator.startedAt).count() >= lifetime;
            }),
        m_ActiveKillIndicators.end());

    ActiveKillIndicator indicator{};
    indicator.worldPos = worldPos;
    indicator.startedAt = now;
    indicator.killConfirmed = false;
    indicator.headshot = false;
    m_ActiveKillIndicators.push_back(indicator);
    if (m_ActiveKillIndicators.size() > 16)
        m_ActiveKillIndicators.erase(m_ActiveKillIndicators.begin(), m_ActiveKillIndicators.begin() + (m_ActiveKillIndicators.size() - 16));
}

void VR::SpawnKillIndicator(bool headshot, const Vector& worldPos)
{
    if (!m_KillIndicatorEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    m_ActiveKillIndicators.erase(
        std::remove_if(
            m_ActiveKillIndicators.begin(),
            m_ActiveKillIndicators.end(),
            [&](const ActiveKillIndicator& indicator)
            {
                const float lifetime = GetActiveIndicatorLifetimeSeconds(indicator, m_KillIndicatorLifetimeSeconds);
                return std::chrono::duration<float>(now - indicator.startedAt).count() >= lifetime;
            }),
        m_ActiveKillIndicators.end());

    Vector indicatorPos = worldPos;
    if (indicatorPos.IsZero())
    {
        Vector fallbackForward{};
        Vector fallbackRight{};
        Vector fallbackUp{};
        QAngle::AngleVectors(m_SetupAngles, &fallbackForward, &fallbackRight, &fallbackUp);
        if (fallbackForward.IsZero())
            fallbackForward = m_HmdForward;
        if (fallbackForward.IsZero())
            fallbackForward = { 1.0f, 0.0f, 0.0f };

        VectorNormalize(fallbackForward);
        indicatorPos = m_SetupOrigin + fallbackForward * 196.0f;
    }

    ActiveKillIndicator indicator{};
    indicator.worldPos = indicatorPos;
    indicator.startedAt = now;
    indicator.killConfirmed = true;
    indicator.headshot = headshot;
    m_ActiveKillIndicators.push_back(indicator);
    if (m_ActiveKillIndicators.size() > 16)
        m_ActiveKillIndicators.erase(m_ActiveKillIndicators.begin(), m_ActiveKillIndicators.begin() + (m_ActiveKillIndicators.size() - 16));
}

void VR::DrawKillIndicators(IMatRenderContext* renderContext, ITexture* hudTexture)
{
    if (!renderContext)
        return;

    const auto now = std::chrono::steady_clock::now();
    m_ActiveKillIndicators.erase(
        std::remove_if(
            m_ActiveKillIndicators.begin(),
            m_ActiveKillIndicators.end(),
            [&](const ActiveKillIndicator& indicator)
            {
                const float lifetime = GetActiveIndicatorLifetimeSeconds(indicator, m_KillIndicatorLifetimeSeconds);
                return std::chrono::duration<float>(now - indicator.startedAt).count() >= lifetime;
            }),
        m_ActiveKillIndicators.end());

    if (!m_KillIndicatorEnabled || m_ActiveKillIndicators.empty() || !hudTexture)
        return;

    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
    {
        m_ActiveKillIndicators.clear();
        return;
    }

    IMaterial* hitMaterial = ResolveHitIndicatorMaterial();
    IMaterial* normalMaterial = ResolveKillIndicatorMaterial(false);
    IMaterial* headshotMaterial = ResolveKillIndicatorMaterial(true);
    if (!hitMaterial && !normalMaterial && !headshotMaterial)
        return;

    int screenWidth = hudTexture->GetMappingWidth();
    int screenHeight = hudTexture->GetMappingHeight();
    if (screenWidth <= 0 || screenHeight <= 0)
        renderContext->GetWindowSize(screenWidth, screenHeight);
    if (screenWidth <= 0 || screenHeight <= 0)
        return;

    const float baseSizePixels = (std::max)(16.0f, m_KillIndicatorSizePixels);
    for (const ActiveKillIndicator& indicator : m_ActiveKillIndicators)
    {
        IMaterial* material = nullptr;
        if (!indicator.killConfirmed)
            material = hitMaterial;
        else
            material = indicator.headshot ? headshotMaterial : normalMaterial;
        if (!material)
            material = normalMaterial ? normalMaterial : (headshotMaterial ? headshotMaterial : hitMaterial);
        if (!material)
            continue;

        const float lifetime = GetActiveIndicatorLifetimeSeconds(indicator, m_KillIndicatorLifetimeSeconds);
        const float ageSeconds = std::chrono::duration<float>(now - indicator.startedAt).count();
        const float progress = Clamp01(ageSeconds / lifetime);
        const float introWindow = indicator.killConfirmed ? 0.22f : 0.18f;
        const float intro = Clamp01(progress / introWindow);
        const float fadeStart = indicator.killConfirmed ? 0.72f : 0.58f;
        const float fadeWidth = indicator.killConfirmed ? 0.28f : 0.42f;
        const float fade = 1.0f - Clamp01((progress - fadeStart) / fadeWidth);
        const float pulse = std::sin(intro * 1.57079632679f);

        Vector drawPos = indicator.worldPos;
        const float riseScale = indicator.killConfirmed ? 1.0f : 0.38f;
        drawPos.z += m_KillIndicatorRiseUnits * riseScale * (0.15f + 0.85f * EaseOutCubic(progress));

        int screenX = 0;
        int screenY = 0;
        if (!ProjectKillIndicatorToHud(this, drawPos, screenWidth, screenHeight, m_KillIndicatorMaxDistance, screenX, screenY))
            continue;

        float scale = indicator.killConfirmed ? (0.78f + 0.34f * pulse) : (0.56f + 0.24f * pulse);
        if (indicator.killConfirmed && indicator.headshot)
            scale *= 1.10f;

        const float alphaBase = indicator.killConfirmed ? 0.55f : 0.42f;
        const float alpha = Clamp01((alphaBase + (1.0f - alphaBase) * intro) * fade);
        const int texWidth = (std::max)(1, material->GetMappingWidth());
        const int texHeight = (std::max)(1, material->GetMappingHeight());
        const float texAspect = static_cast<float>(texWidth) / static_cast<float>(texHeight);
        const int drawHeight = (std::max)(8, static_cast<int>(std::lround(baseSizePixels * scale)));
        const int drawWidth = (std::max)(8, static_cast<int>(std::lround(static_cast<float>(drawHeight) * texAspect)));

        const float colorG = (!indicator.killConfirmed || indicator.headshot) ? 0.94f : 1.0f;
        const float colorB = (!indicator.killConfirmed || indicator.headshot) ? 0.94f : 1.0f;
        material->ColorModulate(1.0f, colorG, colorB);
        material->AlphaModulate(alpha);
        renderContext->DrawScreenSpaceRectangle(
            material,
            screenX - drawWidth / 2,
            screenY - drawHeight / 2,
            drawWidth,
            drawHeight,
            0.0f,
            0.0f,
            static_cast<float>(texWidth),
            static_cast<float>(texHeight),
            texWidth,
            texHeight);
    }

    if (hitMaterial)
    {
        hitMaterial->ColorModulate(1.0f, 1.0f, 1.0f);
        hitMaterial->AlphaModulate(1.0f);
    }
    if (normalMaterial)
    {
        normalMaterial->ColorModulate(1.0f, 1.0f, 1.0f);
        normalMaterial->AlphaModulate(1.0f);
    }
    if (headshotMaterial && headshotMaterial != normalMaterial)
    {
        headshotMaterial->ColorModulate(1.0f, 1.0f, 1.0f);
        headshotMaterial->AlphaModulate(1.0f);
    }
}

void VR::UpdateKillSoundFeedback()
{
    auto resetState = [&]()
        {
            m_PendingKillSoundHits.clear();
            m_PendingKillSoundEvents.clear();
            m_ActiveKillIndicators.clear();
            m_LastKillSoundCommonKills = -1;
            m_LastKillSoundSpecialKills = -1;
            m_LastKillSoundHeadshots = -1;
        };

    if ((!m_KillSoundEnabled && !m_KillIndicatorEnabled) || !m_Game || !m_Game->m_EngineClient)
    {
        resetState();
        return;
    }

    if (!m_Game->m_EngineClient->IsInGame())
    {
        resetState();
        return;
    }

    const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* localPlayer = reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(localPlayerIndex));
    if (!localPlayer)
    {
        resetState();
        return;
    }

    int commonKills = 0;
    int specialKills = 0;
    if (!ReadLocalKillCounters(localPlayer, commonKills, specialKills))
    {
        resetState();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    EnsureKillSoundEventListener();
    m_PendingKillSoundHits.erase(
        std::remove_if(
            m_PendingKillSoundHits.begin(),
            m_PendingKillSoundHits.end(),
            [&](const PendingKillSoundHit& hit)
            {
                return hit.entityTag == 0 || hit.expiresAt < now;
            }),
        m_PendingKillSoundHits.end());

    if (m_LastKillSoundCommonKills < 0 || m_LastKillSoundSpecialKills < 0)
    {
        m_LastKillSoundCommonKills = commonKills;
        m_LastKillSoundSpecialKills = specialKills;
        return;
    }

    const int deltaCommon = commonKills - m_LastKillSoundCommonKills;
    const int deltaSpecial = specialKills - m_LastKillSoundSpecialKills;
    if (deltaCommon < 0 || deltaSpecial < 0)
    {
        m_LastKillSoundCommonKills = commonKills;
        m_LastKillSoundSpecialKills = specialKills;
        m_PendingKillSoundHits.clear();
        m_PendingKillSoundEvents.clear();
        return;
    }

    const int totalDelta = deltaCommon + deltaSpecial;
    if (totalDelta > 0)
    {
        Game::logMsg(
            "[VR][KillSound][resolve] totalDelta=%d commonDelta=%d specialDelta=%d pendingHits=%zu pendingEvents=%zu",
            totalDelta,
            deltaCommon,
            deltaSpecial,
            m_PendingKillSoundHits.size(),
            m_PendingKillSoundEvents.size());
    }

    for (int i = 0; i < totalDelta; ++i)
    {
        bool headshot = false;
        std::uintptr_t entityTag = 0;
        std::string eventName;
        const bool hasEvent = ConsumePendingKillSoundEvent(now, headshot, entityTag, &eventName);
        Vector impactPos{};
        const bool matched = ConsumePendingKillSoundHit(entityTag, now, &impactPos);

        Game::logMsg(
            "[VR][KillSound][kill] idx=%d/%d event=%s eventHeadshot=%d entityTag=%p matched=%d impact=(%.1f %.1f %.1f)",
            i + 1,
            totalDelta,
            hasEvent ? eventName.c_str() : "",
            headshot ? 1 : 0,
            reinterpret_cast<void*>(entityTag),
            matched ? 1 : 0,
            impactPos.x,
            impactPos.y,
            impactPos.z);

        if (matched)
        {
            PlayKillSound(headshot, &impactPos);
            SpawnKillIndicator(headshot, impactPos);
        }
        else
        {
            PlayKillSound(false, nullptr);
            SpawnKillIndicator(false, Vector{});
        }
    }

    m_LastKillSoundCommonKills = commonKills;
    m_LastKillSoundSpecialKills = specialKills;
}
