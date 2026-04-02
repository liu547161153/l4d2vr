
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
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <string>
#include <system_error>
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
    constexpr size_t kMaxActiveKillIndicators = 16;
    constexpr float kKillIndicatorTrimIntervalSeconds = 1.0f / 90.0f;
    constexpr float kHitIndicatorMergeWindowSeconds = 0.12f;
    constexpr float kHitIndicatorMergeDistance = 128.0f;
    constexpr size_t kFeedbackSoundWorkerMaxQueuedJobs = 64;
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
        std::string loadedPath;
        bool isOpen = false;
        bool usesWaveOut = false;
        uint32_t waveSampleRate = 0;
        uint16_t waveSourceChannels = 0;
        std::vector<int16_t> waveSourceSamples;
        std::vector<int16_t> waveStereoSamples;
        HWAVEOUT waveOut = nullptr;
        WAVEHDR waveHeader{};
        bool wavePrepared = false;
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

    class VRDamageFeedbackEventListener final : public IGameEventListener2
    {
    public:
        explicit VRDamageFeedbackEventListener(VR* vr)
            : m_VR(vr)
        {
        }

        void FireGameEvent(IGameEvent* event) override
        {
            if (m_VR)
                m_VR->HandleDamageFeedbackGameEvent(event);
        }

        int GetEventDebugID(void) override
        {
            return kGameEventDebugIdInit;
        }

    private:
        VR* m_VR = nullptr;
    };

    static std::string ToLowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

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

    static bool TryGetFileLastWriteTime(const std::string& path, FILETIME& outLastWriteTime)
    {
        if (path.empty())
            return false;

        WIN32_FILE_ATTRIBUTE_DATA attrs{};
        if (!::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs))
            return false;
        if ((attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return false;

        outLastWriteTime = attrs.ftLastWriteTime;
        return true;
    }

    static bool IsAbsoluteWindowsPath(const std::string& path)
    {
        if (!path.empty() && (path[0] == '\\' || path[0] == '/'))
            return true;

        if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
            return true;

        return path.size() >= 2
            && ((path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/'));
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

        if (s_cachedUserIdOffset != SIZE_MAX)
        {
            const int cachedUserId = readUserIdAtOffset(s_cachedUserIdOffset);
            if (cachedUserId > 0)
                return cachedUserId;

            s_cachedUserIdOffset = SIZE_MAX;
        }

        const size_t declaredOffset = offsetof(player_info_t, userID);
        const int declaredUserId = readUserIdAtOffset(declaredOffset);
        if (declaredUserId > 0)
        {
            s_cachedUserIdOffset = declaredOffset;
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
            return candidateUserId;
        }

        return 0;
    }

    static std::uintptr_t ResolveKillEventEntityTag(Game* game, IGameEvent* event, const std::string& eventName)
    {
        if (!game || !event)
            return 0;

        int entityIndex = 0;
        if (eventName == "infected_death")
        {
            entityIndex = event->GetInt("infected_id", 0);
        }
        else if (eventName == "infected_hurt")
        {
            entityIndex = event->GetInt("entityid", 0);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("entindex", 0);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("infected_id", 0);
        }
        else if (eventName == "witch_killed")
        {
            entityIndex = event->GetInt("witchid", 0);
        }
        else if (eventName == "player_death")
        {
            const int victimUserId = event->GetInt("userid", 0);
            if (victimUserId > 0 && game->m_EngineClient)
                entityIndex = game->m_EngineClient->GetPlayerForUserID(victimUserId);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("entityid", 0);
        }
        else if (eventName == "player_hurt")
        {
            const int victimUserId = event->GetInt("userid", 0);
            if (victimUserId > 0 && game->m_EngineClient)
                entityIndex = game->m_EngineClient->GetPlayerForUserID(victimUserId);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("entityid", 0);
            if (entityIndex <= 0)
                entityIndex = event->GetInt("entindex", 0);
        }

        if (entityIndex <= 0)
            return 0;

        return reinterpret_cast<std::uintptr_t>(game->GetClientEntity(entityIndex));
    }
    static float ReadGameMasterVolumeFromConfig(IEngineClient* engine)
    {
        struct CachedGameVolumeState
        {
            bool wasInGame = false;
            float cachedVolume = 1.0f;
            bool initialized = false;
            std::string cachedConfigPath;
            FILETIME cachedWriteTime{};
            std::chrono::steady_clock::time_point lastRefreshAt{};
        };

        static CachedGameVolumeState state{};
        constexpr float kVolumeConfigRefreshSeconds = 10.0f;

        const bool inGame = engine && engine->IsInGame();
        if (!inGame)
        {
            state = {};
            return 1.0f;
        }

        const auto now = std::chrono::steady_clock::now();
        if (state.initialized && state.wasInGame && state.lastRefreshAt.time_since_epoch().count() != 0)
        {
            const float elapsed = std::chrono::duration<float>(now - state.lastRefreshAt).count();
            if (elapsed < kVolumeConfigRefreshSeconds)
                return state.cachedVolume;
        }

        auto updateCachedVolume = [&](float volume, const std::string& configPath, const FILETIME* writeTime)
            {
                state.wasInGame = true;
                state.cachedVolume = std::clamp(volume, 0.0f, 1.0f);
                state.initialized = true;
                state.cachedConfigPath = configPath;
                state.cachedWriteTime = writeTime ? *writeTime : FILETIME{};
                state.lastRefreshAt = now;
                return state.cachedVolume;
            };

        if (state.initialized && state.wasInGame && !state.cachedConfigPath.empty())
        {
            FILETIME writeTime{};
            if (TryGetFileLastWriteTime(state.cachedConfigPath, writeTime)
                && ::CompareFileTime(&writeTime, &state.cachedWriteTime) == 0)
            {
                state.lastRefreshAt = now;
                return state.cachedVolume;
            }
        }

        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return updateCachedVolume(1.0f, std::string{}, nullptr);

        const std::array<std::string, 2> candidates =
        {
            JoinWindowsPath(JoinWindowsPath(moduleDir, "left4dead2"), "cfg\\config.cfg"),
            JoinWindowsPath(moduleDir, "config.cfg")
        };

        for (const auto& candidate : candidates)
        {
            FILETIME writeTime{};
            if (!TryGetFileLastWriteTime(candidate, writeTime))
                continue;

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

            return updateCachedVolume(parsedVolume, candidate, &writeTime);
        }

        return updateCachedVolume(1.0f, std::string{}, nullptr);
    }

    static std::string ResolveKillSoundFilePath(const std::string& rawPath)
    {
        const std::string path = TrimCopy(rawPath);
        if (path.empty())
            return {};

        if (IsAbsoluteWindowsPath(path))
            return FileExistsPath(path) ? path : std::string{};

        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return {};

        if (StartsWithInsensitive(path, "VR\\") || StartsWithInsensitive(path, "VR/"))
        {
            const std::string fromModule = JoinWindowsPath(moduleDir, path);
            return FileExistsPath(fromModule) ? fromModule : std::string{};
        }

        const std::string fromVrDir = JoinWindowsPath(JoinWindowsPath(moduleDir, "VR"), path);
        if (FileExistsPath(fromVrDir))
            return fromVrDir;

        return {};
    }

    static std::string ResolveGameSoundFilePath(const std::string& rawPath)
    {
        std::string path = TrimCopy(rawPath);
        if (path.empty())
            return {};

        while (!path.empty() && (path.front() == '\\' || path.front() == '/'))
            path.erase(path.begin());

        if (StartsWithInsensitive(path, "sound\\") || StartsWithInsensitive(path, "sound/"))
            path = TrimCopy(path.substr(6));

        if (path.empty())
            return {};

        if (IsAbsoluteWindowsPath(path))
            return FileExistsPath(path) ? path : std::string{};

        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return {};

        const std::string candidate = JoinWindowsPath(JoinWindowsPath(JoinWindowsPath(moduleDir, "left4dead2"), "sound"), path);
        return FileExistsPath(candidate) ? candidate : std::string{};
    }

    static std::string ResolveBuiltinFeedbackGameSoundPath(const std::string& rawSoundName)
    {
        const std::string soundName = TrimCopy(rawSoundName);
        if (soundName.empty())
            return {};

        if (_stricmp(soundName.c_str(), "VR_HitMarker") == 0)
            return ResolveGameSoundFilePath("vrmod/hit.mp3");
        if (_stricmp(soundName.c_str(), "VR_KillMarker") == 0)
            return ResolveGameSoundFilePath("vrmod/kill.mp3");
        if (_stricmp(soundName.c_str(), "VR_HeadshotMarker") == 0)
            return ResolveGameSoundFilePath("vrmod/headshot.mp3");

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

    static std::string FormatFeedbackSoundVolume(float value)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(2) << std::clamp(value, 0.0f, 2.0f);
        return stream.str();
    }

    static std::string ReadWholeTextFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return {};

        std::ostringstream stream;
        stream << file.rdbuf();
        return stream.str();
    }

    static bool WriteWholeTextFileIfChanged(const std::string& path, const std::string& contents)
    {
        if (path.empty())
            return false;

        if (ReadWholeTextFile(path) == contents)
            return true;

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            return false;

        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return file.good();
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

    static uint16_t ReadLe16(const std::vector<uint8_t>& bytes, size_t offset)
    {
        return static_cast<uint16_t>(bytes[offset])
            | (static_cast<uint16_t>(bytes[offset + 1]) << 8);
    }

    static uint32_t ReadLe32(const std::vector<uint8_t>& bytes, size_t offset)
    {
        return static_cast<uint32_t>(bytes[offset])
            | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
            | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    }

    static int16_t ClampFeedbackSoundSample(int32_t sample)
    {
        return static_cast<int16_t>(std::clamp(sample, -32768, 32767));
    }

    static void CleanupFeedbackSoundWavePlayback(FeedbackSoundVoiceState& voice)
    {
        if (voice.waveOut)
        {
            ::waveOutReset(voice.waveOut);
            if (voice.wavePrepared)
            {
                ::waveOutUnprepareHeader(voice.waveOut, &voice.waveHeader, sizeof(voice.waveHeader));
                voice.wavePrepared = false;
            }
            ::waveOutClose(voice.waveOut);
            voice.waveOut = nullptr;
        }

        voice.waveHeader = {};
        voice.waveStereoSamples.clear();
    }

    static bool TryLoadFeedbackSoundWavePcm(const std::string& path, std::vector<int16_t>& outSamples, uint32_t& outSampleRate, uint16_t& outChannels)
    {
        outSamples.clear();
        outSampleRate = 0;
        outChannels = 0;

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return false;

        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.size() < 44)
            return false;

        if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
            return false;

        bool haveFmt = false;
        size_t dataOffset = 0;
        size_t dataSize = 0;
        uint16_t formatTag = 0;
        uint16_t channels = 0;
        uint16_t bitsPerSample = 0;
        uint32_t sampleRate = 0;

        for (size_t pos = 12; pos + 8 <= bytes.size();)
        {
            const uint32_t chunkSize = ReadLe32(bytes, pos + 4);
            const size_t chunkDataOffset = pos + 8;
            if (chunkDataOffset + chunkSize > bytes.size())
                break;

            if (std::memcmp(bytes.data() + pos, "fmt ", 4) == 0)
            {
                if (chunkSize < 16)
                    return false;

                formatTag = ReadLe16(bytes, chunkDataOffset + 0);
                channels = ReadLe16(bytes, chunkDataOffset + 2);
                sampleRate = ReadLe32(bytes, chunkDataOffset + 4);
                bitsPerSample = ReadLe16(bytes, chunkDataOffset + 14);
                haveFmt = true;
            }
            else if (std::memcmp(bytes.data() + pos, "data", 4) == 0)
            {
                dataOffset = chunkDataOffset;
                dataSize = static_cast<size_t>(chunkSize);
            }

            pos = chunkDataOffset + chunkSize;
            if ((chunkSize & 1u) != 0u)
                ++pos;
        }

        if (!haveFmt || dataOffset == 0 || dataSize == 0)
            return false;
        if (formatTag != 1 || (channels != 1 && channels != 2) || bitsPerSample != 16 || sampleRate == 0)
            return false;

        const size_t bytesPerSample = static_cast<size_t>(bitsPerSample / 8);
        const size_t frameSize = bytesPerSample * static_cast<size_t>(channels);
        if (frameSize == 0)
            return false;

        const size_t frameCount = dataSize / frameSize;
        if (frameCount == 0)
            return false;

        outSamples.resize(frameCount * static_cast<size_t>(channels));
        for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            const size_t sampleOffset = dataOffset + frameIndex * frameSize;
            for (uint16_t channelIndex = 0; channelIndex < channels; ++channelIndex)
            {
                const size_t channelOffset = sampleOffset + static_cast<size_t>(channelIndex) * bytesPerSample;
                outSamples[frameIndex * static_cast<size_t>(channels) + static_cast<size_t>(channelIndex)] =
                    static_cast<int16_t>(ReadLe16(bytes, channelOffset));
            }
        }

        outSampleRate = sampleRate;
        outChannels = channels;
        return true;
    }

    static bool TryPlayFeedbackSoundWaveVoice(FeedbackSoundVoiceState& voice, int leftVolume, int rightVolume)
    {
        if (voice.waveSourceSamples.empty() || voice.waveSampleRate == 0 || (voice.waveSourceChannels != 1 && voice.waveSourceChannels != 2))
            return false;

        CleanupFeedbackSoundWavePlayback(voice);

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = voice.waveSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>((format.nChannels * format.wBitsPerSample) / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        if (::waveOutOpen(&voice.waveOut, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
            return false;

        const size_t frameCount = voice.waveSourceSamples.size() / static_cast<size_t>(voice.waveSourceChannels);
        voice.waveStereoSamples.resize(frameCount * 2u);
        for (size_t i = 0; i < frameCount; ++i)
        {
            int32_t sourceLeft = 0;
            int32_t sourceRight = 0;
            if (voice.waveSourceChannels == 1)
            {
                sourceLeft = static_cast<int32_t>(voice.waveSourceSamples[i]);
                sourceRight = sourceLeft;
            }
            else
            {
                sourceLeft = static_cast<int32_t>(voice.waveSourceSamples[i * 2u]);
                sourceRight = static_cast<int32_t>(voice.waveSourceSamples[i * 2u + 1u]);
            }

            const int32_t left = (sourceLeft * leftVolume) / 1000;
            const int32_t right = (sourceRight * rightVolume) / 1000;
            voice.waveStereoSamples[i * 2u] = ClampFeedbackSoundSample(left);
            voice.waveStereoSamples[i * 2u + 1u] = ClampFeedbackSoundSample(right);
        }

        voice.waveHeader = {};
        voice.waveHeader.lpData = reinterpret_cast<LPSTR>(voice.waveStereoSamples.data());
        voice.waveHeader.dwBufferLength = static_cast<DWORD>(voice.waveStereoSamples.size() * sizeof(int16_t));

        if (::waveOutPrepareHeader(voice.waveOut, &voice.waveHeader, sizeof(voice.waveHeader)) != MMSYSERR_NOERROR)
        {
            CleanupFeedbackSoundWavePlayback(voice);
            return false;
        }

        voice.wavePrepared = true;
        if (::waveOutWrite(voice.waveOut, &voice.waveHeader, sizeof(voice.waveHeader)) != MMSYSERR_NOERROR)
        {
            CleanupFeedbackSoundWavePlayback(voice);
            return false;
        }

        return true;
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

    static bool IsSameFeedbackSoundPath(const std::string& lhs, const std::string& rhs)
    {
        return _stricmp(lhs.c_str(), rhs.c_str()) == 0;
    }

    static void CloseFeedbackSoundVoice(FeedbackSoundVoiceState& voice)
    {
        CleanupFeedbackSoundWavePlayback(voice);

        if (!voice.alias.empty())
        {
            const std::string closeCmd = std::string("close ") + voice.alias;
            ::mciSendStringA(closeCmd.c_str(), nullptr, 0, nullptr);
        }

        voice.loadedPath.clear();
        voice.isOpen = false;
        voice.usesWaveOut = false;
        voice.waveSampleRate = 0;
        voice.waveSourceChannels = 0;
        voice.waveSourceSamples.clear();
    }

    static void CloseAllFeedbackSoundVoices()
    {
        auto& voices = GetFeedbackSoundVoices();
        for (FeedbackSoundVoiceState& voice : voices)
            CloseFeedbackSoundVoice(voice);
    }

    static bool EnsureFeedbackSoundVoiceOpen(FeedbackSoundVoiceState& voice, const std::string& resolvedPath)
    {
        if (resolvedPath.empty())
            return false;

        const bool wantsWaveOut = EndsWithInsensitive(resolvedPath, ".wav");
        if (voice.isOpen && !voice.loadedPath.empty() && IsSameFeedbackSoundPath(voice.loadedPath, resolvedPath))
            return true;

        CloseFeedbackSoundVoice(voice);
        if (wantsWaveOut)
        {
            if (!TryLoadFeedbackSoundWavePcm(resolvedPath, voice.waveSourceSamples, voice.waveSampleRate, voice.waveSourceChannels))
                return false;

            voice.loadedPath = resolvedPath;
            voice.isOpen = true;
            voice.usesWaveOut = true;
            return true;
        }

        const std::string escapedPath = EscapeMciString(resolvedPath);
        const std::array<std::string, 2> openCommands =
        {
            std::string("open \"") + escapedPath + "\" type mpegvideo alias " + voice.alias,
            std::string("open \"") + escapedPath + "\" alias " + voice.alias
        };

        bool opened = false;
        for (const std::string& openCmd : openCommands)
        {
            if (::mciSendStringA(openCmd.c_str(), nullptr, 0, nullptr) == 0)
            {
                opened = true;
                break;
            }
        }

        if (!opened)
            return false;

        voice.loadedPath = resolvedPath;
        voice.isOpen = true;
        voice.usesWaveOut = false;
        return true;
    }

    static FeedbackSoundVoiceState& AcquireFeedbackSoundVoice(const std::string* preferredResolvedPath = nullptr)
    {
        auto& voices = GetFeedbackSoundVoices();
        if (preferredResolvedPath && !preferredResolvedPath->empty())
        {
            auto it = std::min_element(
                voices.begin(),
                voices.end(),
                [&](const FeedbackSoundVoiceState& lhs, const FeedbackSoundVoiceState& rhs)
                {
                    const bool lhsMatches = lhs.isOpen && !lhs.loadedPath.empty() && IsSameFeedbackSoundPath(lhs.loadedPath, *preferredResolvedPath);
                    const bool rhsMatches = rhs.isOpen && !rhs.loadedPath.empty() && IsSameFeedbackSoundPath(rhs.loadedPath, *preferredResolvedPath);
                    if (lhsMatches != rhsMatches)
                        return lhsMatches;
                    if (lhsMatches && rhsMatches)
                        return lhs.lastStarted < rhs.lastStarted;
                    if (lhs.isOpen != rhs.isOpen)
                        return !lhs.isOpen;
                    return lhs.lastStarted < rhs.lastStarted;
                });
            return *it;
        }

        auto it = std::min_element(
            voices.begin(),
            voices.end(),
            [](const FeedbackSoundVoiceState& lhs, const FeedbackSoundVoiceState& rhs)
            {
                if (lhs.isOpen != rhs.isOpen)
                    return !lhs.isOpen;
                return lhs.lastStarted < rhs.lastStarted;
            });
        return *it;
    }

    static bool TryResolveFeedbackSoundFileSpec(const std::string& rawSpec, std::string& outResolvedPath)
    {
        const std::string spec = TrimCopy(rawSpec);
        if (spec.empty())
            return false;

        auto getPayload = [&](size_t prefixLen)
            {
                return TrimCopy(spec.substr(prefixLen));
            };

        if (StartsWithInsensitive(spec, "alias:") || StartsWithInsensitive(spec, "cmd:"))
            return false;

        if (StartsWithInsensitive(spec, "file:"))
            outResolvedPath = ResolveKillSoundFilePath(getPayload(5));
        else if (StartsWithInsensitive(spec, "game:"))
            outResolvedPath = ResolveGameSoundFilePath(getPayload(5));
        else if (StartsWithInsensitive(spec, "gamesound:"))
            outResolvedPath = ResolveBuiltinFeedbackGameSoundPath(getPayload(10));
        else
            outResolvedPath = ResolveKillSoundFilePath(spec);

        return !outResolvedPath.empty();
    }

    static void ApplyFeedbackSoundStereoVolumes(const std::string& alias, int leftVolume, int rightVolume)
    {
        if (alias.empty())
            return;

        leftVolume = std::clamp(leftVolume, 0, 1000);
        rightVolume = std::clamp(rightVolume, 0, 1000);

        const int overallVolume = std::clamp((leftVolume + rightVolume) / 2, 0, 1000);
        const std::string volumeCmd = "setaudio " + alias + " volume to " + std::to_string(overallVolume);
        ::mciSendStringA(volumeCmd.c_str(), nullptr, 0, nullptr);

        const std::string leftCmd = "setaudio " + alias + " left volume to " + std::to_string(leftVolume);
        ::mciSendStringA(leftCmd.c_str(), nullptr, 0, nullptr);

        const std::string rightCmd = "setaudio " + alias + " right volume to " + std::to_string(rightVolume);
        ::mciSendStringA(rightCmd.c_str(), nullptr, 0, nullptr);
    }

    static bool TryPlayFeedbackSoundFilePath(const std::string& rawPath, int leftVolume, int rightVolume, bool preferLoadedPathReuse = true)
    {
        const std::string resolvedPath = ResolveKillSoundFilePath(rawPath);
        if (resolvedPath.empty())
            return false;

        leftVolume = std::clamp(leftVolume, 0, 1000);
        rightVolume = std::clamp(rightVolume, 0, 1000);
        if (leftVolume == 0 && rightVolume == 0)
            return true;

        FeedbackSoundVoiceState& voice = preferLoadedPathReuse
            ? AcquireFeedbackSoundVoice(&resolvedPath)
            : AcquireFeedbackSoundVoice();
        if (!EnsureFeedbackSoundVoiceOpen(voice, resolvedPath))
            return false;

        if (voice.usesWaveOut)
        {
            if (!TryPlayFeedbackSoundWaveVoice(voice, leftVolume, rightVolume))
            {
                CloseFeedbackSoundVoice(voice);
                return false;
            }

            voice.lastStarted = std::chrono::steady_clock::now();
            return true;
        }

        const std::string stopCmd = std::string("stop ") + voice.alias;
        ::mciSendStringA(stopCmd.c_str(), nullptr, 0, nullptr);
        const std::string seekCmd = std::string("seek ") + voice.alias + " to start";
        ::mciSendStringA(seekCmd.c_str(), nullptr, 0, nullptr);

        ApplyFeedbackSoundStereoVolumes(voice.alias, leftVolume, rightVolume);

        const std::string playCmd = std::string("play ") + voice.alias + " from 0";
        if (::mciSendStringA(playCmd.c_str(), nullptr, 0, nullptr) != 0)
        {
            CloseFeedbackSoundVoice(voice);
            return false;
        }

        voice.lastStarted = std::chrono::steady_clock::now();
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

    static const char* DescribeFeedbackSoundDebugForceChannel(int forceChannel)
    {
        if (forceChannel < 0)
            return "force_left";
        if (forceChannel > 0)
            return "force_right";
        return "normal";
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

    struct KillIndicatorDecodedFrames
    {
        bool attempted = false;
        bool loaded = false;
        bool additive = false;
        uint32_t width = 0;
        uint32_t height = 0;
        float frameRate = 0.0f;
        std::vector<std::vector<uint8_t>> frames;
    };

    static std::string NormalizeSlashes(std::string value, char slash)
    {
        std::replace(value.begin(), value.end(), '\\', slash);
        std::replace(value.begin(), value.end(), '/', slash);
        return value;
    }

    static std::string TrimQuotesCopy(std::string value)
    {
        value = TrimCopy(value);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        return value;
    }

    static size_t BytesPerBlockForImageFormat(ImageFormat format)
    {
        switch (format)
        {
        case IMAGE_FORMAT_DXT1:
        case IMAGE_FORMAT_DXT1_ONEBITALPHA:
            return 8;
        case IMAGE_FORMAT_DXT3:
        case IMAGE_FORMAT_DXT5:
            return 16;
        default:
            return 0;
        }
    }

    static size_t ComputeImageByteSize(ImageFormat format, uint32_t width, uint32_t height)
    {
        switch (format)
        {
        case IMAGE_FORMAT_RGBA8888:
        case IMAGE_FORMAT_ABGR8888:
        case IMAGE_FORMAT_ARGB8888:
        case IMAGE_FORMAT_BGRA8888:
        case IMAGE_FORMAT_BGRX8888:
            return static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
        case IMAGE_FORMAT_RGB888:
        case IMAGE_FORMAT_BGR888:
            return static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
        case IMAGE_FORMAT_DXT1:
        case IMAGE_FORMAT_DXT1_ONEBITALPHA:
        case IMAGE_FORMAT_DXT3:
        case IMAGE_FORMAT_DXT5:
        {
            const size_t blockWidth = (static_cast<size_t>(width) + 3u) / 4u;
            const size_t blockHeight = (static_cast<size_t>(height) + 3u) / 4u;
            return blockWidth * blockHeight * BytesPerBlockForImageFormat(format);
        }
        default:
            return 0;
        }
    }

    static void DecodeRgb565(uint16_t packed, uint8_t& outR, uint8_t& outG, uint8_t& outB)
    {
        outR = static_cast<uint8_t>(((packed >> 11) & 0x1Fu) * 255u / 31u);
        outG = static_cast<uint8_t>(((packed >> 5) & 0x3Fu) * 255u / 63u);
        outB = static_cast<uint8_t>((packed & 0x1Fu) * 255u / 31u);
    }

    static void DecodeDxt1Block(const uint8_t* block, uint8_t* rgba, uint32_t stride, uint32_t maxX, uint32_t maxY, bool oneBitAlpha)
    {
        const uint16_t color0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
        const uint16_t color1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
        uint8_t palette[4][4] = {};
        DecodeRgb565(color0, palette[0][0], palette[0][1], palette[0][2]);
        DecodeRgb565(color1, palette[1][0], palette[1][1], palette[1][2]);
        palette[0][3] = 255;
        palette[1][3] = 255;

        if (color0 > color1 || !oneBitAlpha)
        {
            for (int c = 0; c < 3; ++c)
            {
                palette[2][c] = static_cast<uint8_t>((2u * palette[0][c] + palette[1][c]) / 3u);
                palette[3][c] = static_cast<uint8_t>((palette[0][c] + 2u * palette[1][c]) / 3u);
            }
            palette[2][3] = 255;
            palette[3][3] = 255;
        }
        else
        {
            for (int c = 0; c < 3; ++c)
                palette[2][c] = static_cast<uint8_t>((palette[0][c] + palette[1][c]) / 2u);
            palette[2][3] = 255;
            palette[3][0] = palette[3][1] = palette[3][2] = 0;
            palette[3][3] = 0;
        }

        uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
        for (uint32_t y = 0; y < maxY; ++y)
        {
            for (uint32_t x = 0; x < maxX; ++x)
            {
                const uint32_t idx = (indices >> (2u * (4u * y + x))) & 0x3u;
                uint8_t* dst = rgba + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
                dst[0] = palette[idx][0];
                dst[1] = palette[idx][1];
                dst[2] = palette[idx][2];
                dst[3] = palette[idx][3];
            }
        }
    }

    static void DecodeDxt5Block(const uint8_t* block, uint8_t* rgba, uint32_t stride, uint32_t maxX, uint32_t maxY)
    {
        uint8_t alphaPalette[8] = {};
        alphaPalette[0] = block[0];
        alphaPalette[1] = block[1];
        if (alphaPalette[0] > alphaPalette[1])
        {
            alphaPalette[2] = static_cast<uint8_t>((6u * alphaPalette[0] + 1u * alphaPalette[1]) / 7u);
            alphaPalette[3] = static_cast<uint8_t>((5u * alphaPalette[0] + 2u * alphaPalette[1]) / 7u);
            alphaPalette[4] = static_cast<uint8_t>((4u * alphaPalette[0] + 3u * alphaPalette[1]) / 7u);
            alphaPalette[5] = static_cast<uint8_t>((3u * alphaPalette[0] + 4u * alphaPalette[1]) / 7u);
            alphaPalette[6] = static_cast<uint8_t>((2u * alphaPalette[0] + 5u * alphaPalette[1]) / 7u);
            alphaPalette[7] = static_cast<uint8_t>((1u * alphaPalette[0] + 6u * alphaPalette[1]) / 7u);
        }
        else
        {
            alphaPalette[2] = static_cast<uint8_t>((4u * alphaPalette[0] + 1u * alphaPalette[1]) / 5u);
            alphaPalette[3] = static_cast<uint8_t>((3u * alphaPalette[0] + 2u * alphaPalette[1]) / 5u);
            alphaPalette[4] = static_cast<uint8_t>((2u * alphaPalette[0] + 3u * alphaPalette[1]) / 5u);
            alphaPalette[5] = static_cast<uint8_t>((1u * alphaPalette[0] + 4u * alphaPalette[1]) / 5u);
            alphaPalette[6] = 0;
            alphaPalette[7] = 255;
        }

        uint64_t alphaIndices = 0;
        for (int i = 0; i < 6; ++i)
            alphaIndices |= static_cast<uint64_t>(block[2 + i]) << (8u * i);

        DecodeDxt1Block(block + 8, rgba, stride, maxX, maxY, false);

        for (uint32_t y = 0; y < maxY; ++y)
        {
            for (uint32_t x = 0; x < maxX; ++x)
            {
                const uint32_t alphaIndex = static_cast<uint32_t>((alphaIndices >> (3u * (4u * y + x))) & 0x7u);
                uint8_t* dst = rgba + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4u;
                dst[3] = alphaPalette[alphaIndex];
            }
        }
    }

    static bool ResizeRgbaNearest(const std::vector<uint8_t>& src, uint32_t srcWidth, uint32_t srcHeight, uint32_t dstWidth, uint32_t dstHeight, std::vector<uint8_t>& dst)
    {
        if (src.empty() || srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0)
            return false;

        dst.resize(static_cast<size_t>(dstWidth) * static_cast<size_t>(dstHeight) * 4u);
        for (uint32_t y = 0; y < dstHeight; ++y)
        {
            const uint32_t srcY = static_cast<uint32_t>((static_cast<uint64_t>(y) * srcHeight) / dstHeight);
            for (uint32_t x = 0; x < dstWidth; ++x)
            {
                const uint32_t srcX = static_cast<uint32_t>((static_cast<uint64_t>(x) * srcWidth) / dstWidth);
                const uint8_t* srcPx = src.data() + (static_cast<size_t>(srcY) * srcWidth + srcX) * 4u;
                uint8_t* dstPx = dst.data() + (static_cast<size_t>(y) * dstWidth + x) * 4u;
                dstPx[0] = srcPx[0];
                dstPx[1] = srcPx[1];
                dstPx[2] = srcPx[2];
                dstPx[3] = srcPx[3];
            }
        }
        return true;
    }

    static bool CropRgba(const std::vector<uint8_t>& src, uint32_t srcWidth, uint32_t srcHeight, uint32_t left, uint32_t top, uint32_t cropWidth, uint32_t cropHeight, std::vector<uint8_t>& dst)
    {
        if (src.empty() || srcWidth == 0 || srcHeight == 0 || cropWidth == 0 || cropHeight == 0)
            return false;
        if (left >= srcWidth || top >= srcHeight)
            return false;
        if (left + cropWidth > srcWidth || top + cropHeight > srcHeight)
            return false;

        dst.resize(static_cast<size_t>(cropWidth) * static_cast<size_t>(cropHeight) * 4u);
        for (uint32_t y = 0; y < cropHeight; ++y)
        {
            const uint8_t* srcRow = src.data() + ((static_cast<size_t>(top + y) * srcWidth) + left) * 4u;
            uint8_t* dstRow = dst.data() + static_cast<size_t>(y) * static_cast<size_t>(cropWidth) * 4u;
            std::memcpy(dstRow, srcRow, static_cast<size_t>(cropWidth) * 4u);
        }
        return true;
    }

    static void ConvertAdditiveRgbaToOverlay(std::vector<uint8_t>& rgba)
    {
        constexpr uint8_t kVisibilityThreshold = 12;

        for (size_t i = 0; i + 3 < rgba.size(); i += 4)
        {
            const uint8_t srcR = rgba[i + 0];
            const uint8_t srcG = rgba[i + 1];
            const uint8_t srcB = rgba[i + 2];
            const uint8_t srcA = rgba[i + 3];
            const uint8_t maxRgb = (std::max)({ srcR, srcG, srcB });

            if (maxRgb <= kVisibilityThreshold || srcA == 0)
            {
                rgba[i + 0] = 0;
                rgba[i + 1] = 0;
                rgba[i + 2] = 0;
                rgba[i + 3] = 0;
                continue;
            }

            const float intensity = static_cast<float>(maxRgb) / 255.0f;
            const uint8_t overlayAlpha = static_cast<uint8_t>(std::clamp(std::lround(std::pow(intensity, 0.75f) * 255.0f), 0l, 255l));
            const float hueScale = 255.0f / static_cast<float>(maxRgb);

            rgba[i + 0] = static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(srcR) * hueScale), 0l, 255l));
            rgba[i + 1] = static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(srcG) * hueScale), 0l, 255l));
            rgba[i + 2] = static_cast<uint8_t>(std::clamp(std::lround(static_cast<float>(srcB) * hueScale), 0l, 255l));
            rgba[i + 3] = overlayAlpha;
        }
    }

    static bool FindVisibleRgbaBounds(const std::vector<std::vector<uint8_t>>& frames, uint32_t width, uint32_t height, uint32_t& outLeft, uint32_t& outTop, uint32_t& outWidth, uint32_t& outHeight)
    {
        if (frames.empty() || width == 0 || height == 0)
            return false;

        constexpr uint8_t kVisibleAlphaThreshold = 8;

        bool foundAny = false;
        uint32_t minX = width;
        uint32_t minY = height;
        uint32_t maxX = 0;
        uint32_t maxY = 0;

        for (const std::vector<uint8_t>& frame : frames)
        {
            if (frame.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4u)
                continue;

            for (uint32_t y = 0; y < height; ++y)
            {
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint8_t* px = frame.data() + ((static_cast<size_t>(y) * width) + x) * 4u;
                    if (px[3] <= kVisibleAlphaThreshold)
                        continue;

                    foundAny = true;
                    minX = (std::min)(minX, x);
                    minY = (std::min)(minY, y);
                    maxX = (std::max)(maxX, x);
                    maxY = (std::max)(maxY, y);
                }
            }
        }

        if (!foundAny)
            return false;

        const uint32_t padding = (std::max)(4u, (std::max)(width, height) / 64u);
        outLeft = (minX > padding) ? (minX - padding) : 0u;
        outTop = (minY > padding) ? (minY - padding) : 0u;
        const uint32_t paddedRight = (std::min)(width - 1u, maxX + padding);
        const uint32_t paddedBottom = (std::min)(height - 1u, maxY + padding);
        outWidth = paddedRight - outLeft + 1u;
        outHeight = paddedBottom - outTop + 1u;
        return outWidth > 0 && outHeight > 0;
    }

    static bool ParseTrailingBoolValue(const std::string& line, const char* key, bool& outValue)
    {
        if (!key || !*key)
            return false;

        const std::string trimmed = TrimCopy(line);
        const std::string lowered = ToLowerCopy(trimmed);
        const std::string loweredKey = ToLowerCopy(key);
        const size_t keyPos = lowered.find(loweredKey);
        if (keyPos == std::string::npos)
            return false;

        std::string tail = ToLowerCopy(TrimQuotesCopy(trimmed.substr(keyPos + loweredKey.size())));
        if (tail.empty())
            return false;

        if (tail == "1" || tail == "true" || tail == "yes")
        {
            outValue = true;
            return true;
        }
        if (tail == "0" || tail == "false" || tail == "no")
        {
            outValue = false;
            return true;
        }

        return false;
    }

    static bool ParseQuotedMaterialValue(const std::string& line, const char* key, std::string& outValue)
    {
        if (!key || !*key)
            return false;

        const std::string trimmed = TrimCopy(line);
        const std::string lowered = ToLowerCopy(trimmed);
        const std::string loweredKey = ToLowerCopy(key);
        const size_t keyPos = lowered.find(loweredKey);
        if (keyPos == std::string::npos)
            return false;

        size_t searchPos = keyPos + loweredKey.size();
        while (searchPos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[searchPos])))
            ++searchPos;

        if (searchPos >= trimmed.size() || trimmed[searchPos] != '"')
            return false;

        size_t firstQuote = searchPos;
        size_t secondQuote = trimmed.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos)
            return false;

        std::string candidate = TrimCopy(trimmed.substr(firstQuote + 1, secondQuote - firstQuote - 1));
        if (!candidate.empty())
        {
            outValue = candidate;
            return true;
        }

        firstQuote = secondQuote;
        secondQuote = trimmed.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos)
            return false;

        outValue = TrimCopy(trimmed.substr(firstQuote + 1, secondQuote - firstQuote - 1));
        return !outValue.empty();
    }

    static bool ParseTrailingFloatValue(const std::string& line, const char* key, float& outValue)
    {
        if (!key || !*key)
            return false;

        const std::string trimmed = TrimCopy(line);
        const std::string lowered = ToLowerCopy(trimmed);
        const std::string loweredKey = ToLowerCopy(key);
        const size_t keyPos = lowered.find(loweredKey);
        if (keyPos == std::string::npos)
            return false;

        std::string tail = TrimQuotesCopy(trimmed.substr(keyPos + loweredKey.size()));
        if (tail.empty())
            return false;

        char* endPtr = nullptr;
        const float value = std::strtof(tail.c_str(), &endPtr);
        if (endPtr == tail.c_str())
            return false;
        outValue = value;
        return true;
    }

    static bool LoadKillIndicatorDecodedFramesFromDisk(const std::string& materialName, KillIndicatorDecodedFrames& outFrames)
    {
        const std::string moduleDir = GetModuleDirectoryA();
        if (moduleDir.empty())
            return false;

        const std::string normalizedMaterial = NormalizeMaterialPathSpec(materialName);
        if (normalizedMaterial.empty())
            return false;

        const std::string materialsDir = JoinWindowsPath(JoinWindowsPath(moduleDir, "left4dead2"), "materials");
        const std::string vmtPath = JoinWindowsPath(materialsDir, NormalizeSlashes(normalizedMaterial, '\\') + ".vmt");

        std::ifstream vmtFile(vmtPath);
        if (!vmtFile.is_open())
            return false;

        std::string baseTexture = normalizedMaterial;
        float frameRate = 0.0f;
        bool additive = false;
        std::string line;
        while (std::getline(vmtFile, line))
        {
            std::string parsedValue;
            if (ParseQuotedMaterialValue(line, "$basetexture", parsedValue))
                baseTexture = NormalizeMaterialPathSpec(parsedValue);

            float parsedRate = 0.0f;
            if (ParseTrailingFloatValue(line, "animatedTextureFrameRate", parsedRate))
                frameRate = (std::max)(0.0f, parsedRate);

            std::string parsedBoolValue;
            if (ParseQuotedMaterialValue(line, "$additive", parsedBoolValue))
            {
                const std::string loweredBool = ToLowerCopy(TrimQuotesCopy(parsedBoolValue));
                if (loweredBool == "1" || loweredBool == "true" || loweredBool == "yes")
                    additive = true;
                else if (loweredBool == "0" || loweredBool == "false" || loweredBool == "no")
                    additive = false;
            }
            else
            {
                bool parsedAdditive = false;
                if (ParseTrailingBoolValue(line, "$additive", parsedAdditive))
                    additive = parsedAdditive;
            }
        }

        const std::string vtfPath = JoinWindowsPath(materialsDir, NormalizeSlashes(baseTexture, '\\') + ".vtf");
        std::ifstream vtfFile(vtfPath, std::ios::binary);
        if (!vtfFile.is_open())
            return false;

        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(vtfFile)), std::istreambuf_iterator<char>());
        if (bytes.size() < 80 || bytes[0] != 'V' || bytes[1] != 'T' || bytes[2] != 'F' || bytes[3] != '\0')
            return false;

        auto readU16 = [&](size_t offset) -> uint16_t
            {
                return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
            };
        auto readU32 = [&](size_t offset) -> uint32_t
            {
                return static_cast<uint32_t>(bytes[offset]
                    | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
                    | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
                    | (static_cast<uint32_t>(bytes[offset + 3]) << 24));
            };

        const uint32_t headerSize = readU32(12);
        const uint32_t width = readU16(16);
        const uint32_t height = readU16(18);
        const uint32_t frameCount = (std::max<uint32_t>)(1u, readU16(24));
        const ImageFormat highResFormat = static_cast<ImageFormat>(readU32(52));
        const uint8_t mipCount = bytes[56];
        const ImageFormat lowResFormat = static_cast<ImageFormat>(readU32(57));
        const uint8_t lowResWidth = bytes[61];
        const uint8_t lowResHeight = bytes[62];
        const uint16_t depth = readU16(63);
        if (headerSize >= bytes.size() || width == 0 || height == 0 || mipCount == 0 || depth == 0)
            return false;

        const size_t lowResBytes = ComputeImageByteSize(lowResFormat, lowResWidth, lowResHeight);
        size_t highResOffset = static_cast<size_t>(headerSize) + lowResBytes;
        for (int mip = static_cast<int>(mipCount) - 1; mip >= 1; --mip)
        {
            const uint32_t mipWidth = (std::max)(1u, width >> mip);
            const uint32_t mipHeight = (std::max)(1u, height >> mip);
            const uint32_t mipDepth = (std::max)(1u, static_cast<uint32_t>(depth) >> mip);
            const size_t mipBytes = ComputeImageByteSize(highResFormat, mipWidth, mipHeight) * static_cast<size_t>(mipDepth) * static_cast<size_t>(frameCount);
            highResOffset += mipBytes;
        }

        const size_t frameBytes = ComputeImageByteSize(highResFormat, width, height);
        if (frameBytes == 0 || highResOffset + frameBytes * static_cast<size_t>(frameCount) > bytes.size())
            return false;

        outFrames.frames.clear();
        outFrames.frames.reserve(frameCount);
        outFrames.additive = additive;
        outFrames.width = width;
        outFrames.height = height;
        outFrames.frameRate = frameRate;

        for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            const uint8_t* frameData = bytes.data() + highResOffset + frameIndex * frameBytes;
            std::vector<uint8_t> rgba(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);

            switch (highResFormat)
            {
            case IMAGE_FORMAT_DXT1:
            case IMAGE_FORMAT_DXT1_ONEBITALPHA:
            {
                const size_t blocksWide = (static_cast<size_t>(width) + 3u) / 4u;
                const size_t blocksHigh = (static_cast<size_t>(height) + 3u) / 4u;
                for (size_t by = 0; by < blocksHigh; ++by)
                {
                    for (size_t bx = 0; bx < blocksWide; ++bx)
                    {
                        const uint8_t* block = frameData + (by * blocksWide + bx) * 8u;
                        uint8_t* dst = rgba.data() + (by * 4u * static_cast<size_t>(width) + bx * 4u) * 4u;
                        const uint32_t maxX = static_cast<uint32_t>((std::min)(4u, width - static_cast<uint32_t>(bx * 4u)));
                        const uint32_t maxY = static_cast<uint32_t>((std::min)(4u, height - static_cast<uint32_t>(by * 4u)));
                        DecodeDxt1Block(block, dst, width * 4u, maxX, maxY, highResFormat == IMAGE_FORMAT_DXT1_ONEBITALPHA);
                    }
                }
                break;
            }
            case IMAGE_FORMAT_DXT5:
            {
                const size_t blocksWide = (static_cast<size_t>(width) + 3u) / 4u;
                const size_t blocksHigh = (static_cast<size_t>(height) + 3u) / 4u;
                for (size_t by = 0; by < blocksHigh; ++by)
                {
                    for (size_t bx = 0; bx < blocksWide; ++bx)
                    {
                        const uint8_t* block = frameData + (by * blocksWide + bx) * 16u;
                        uint8_t* dst = rgba.data() + (by * 4u * static_cast<size_t>(width) + bx * 4u) * 4u;
                        const uint32_t maxX = static_cast<uint32_t>((std::min)(4u, width - static_cast<uint32_t>(bx * 4u)));
                        const uint32_t maxY = static_cast<uint32_t>((std::min)(4u, height - static_cast<uint32_t>(by * 4u)));
                        DecodeDxt5Block(block, dst, width * 4u, maxX, maxY);
                    }
                }
                break;
            }
            case IMAGE_FORMAT_BGRA8888:
            {
                for (uint32_t i = 0; i < width * height; ++i)
                {
                    rgba[i * 4 + 0] = frameData[i * 4 + 2];
                    rgba[i * 4 + 1] = frameData[i * 4 + 1];
                    rgba[i * 4 + 2] = frameData[i * 4 + 0];
                    rgba[i * 4 + 3] = frameData[i * 4 + 3];
                }
                break;
            }
            case IMAGE_FORMAT_RGBA8888:
            {
                std::memcpy(rgba.data(), frameData, rgba.size());
                break;
            }
            default:
                return false;
            }

            outFrames.frames.push_back(std::move(rgba));
        }

        if (outFrames.additive)
        {
            for (std::vector<uint8_t>& frame : outFrames.frames)
                ConvertAdditiveRgbaToOverlay(frame);
        }

        uint32_t visibleLeft = 0;
        uint32_t visibleTop = 0;
        uint32_t visibleWidth = outFrames.width;
        uint32_t visibleHeight = outFrames.height;
        if (FindVisibleRgbaBounds(outFrames.frames, outFrames.width, outFrames.height, visibleLeft, visibleTop, visibleWidth, visibleHeight)
            && (visibleWidth < outFrames.width || visibleHeight < outFrames.height))
        {
            for (std::vector<uint8_t>& frame : outFrames.frames)
            {
                std::vector<uint8_t> cropped;
                if (CropRgba(frame, outFrames.width, outFrames.height, visibleLeft, visibleTop, visibleWidth, visibleHeight, cropped))
                    frame = std::move(cropped);
            }

            outFrames.width = visibleWidth;
            outFrames.height = visibleHeight;
        }

        if (outFrames.width > 256 || outFrames.height > 256)
        {
            const uint32_t dstWidth = (std::min)(256u, outFrames.width);
            const uint32_t dstHeight = (std::min)(256u, outFrames.height);
            for (std::vector<uint8_t>& frame : outFrames.frames)
            {
                std::vector<uint8_t> resized;
                if (ResizeRgbaNearest(frame, outFrames.width, outFrames.height, dstWidth, dstHeight, resized))
                    frame = std::move(resized);
            }
            outFrames.width = dstWidth;
            outFrames.height = dstHeight;
        }

        outFrames.loaded = !outFrames.frames.empty();
        return outFrames.loaded;
    }

    static KillIndicatorDecodedFrames& GetKillIndicatorDecodedFrameCache(const std::string& materialName)
    {
        static std::unordered_map<std::string, KillIndicatorDecodedFrames> cache;
        const std::string key = ToLowerCopy(materialName);
        KillIndicatorDecodedFrames& entry = cache[key];
        if (!entry.attempted)
        {
            entry.attempted = true;
            entry.loaded = LoadKillIndicatorDecodedFramesFromDisk(materialName, entry);
        }
        return entry;
    }

    static float GetActiveIndicatorLifetimeSeconds(const VR::ActiveKillIndicator& indicator, float killLifetimeSeconds)
    {
        if (indicator.killConfirmed)
            return (std::max)(0.10f, killLifetimeSeconds);

        const float scaled = killLifetimeSeconds * 0.42f;
        return std::clamp(scaled, 0.10f, 0.45f);
    }

    enum class KillIndicatorMaterialKind
    {
        Hit = 0,
        Kill = 1,
        Headshot = 2,
    };

    static IDirect3DDevice9* GetKillIndicatorD3DDevice(VR* vr)
    {
        if (!vr)
            return nullptr;

        IDirect3DDevice9* device = nullptr;
        if (vr->m_D9HUDSurface)
            vr->m_D9HUDSurface->GetDevice(&device);
        else if (vr->m_D9LeftEyeSurface)
            vr->m_D9LeftEyeSurface->GetDevice(&device);
        else if (vr->m_D9RightEyeSurface)
            vr->m_D9RightEyeSurface->GetDevice(&device);
        return device;
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
    return ReadLocalKillCounters(localPlayer, outCommon, outSpecial, nullptr);
}

bool VR::ReadLocalKillCounters(C_BasePlayer* localPlayer, int& outCommon, int& outSpecial, char* outSource) const
{
    outCommon = 0;
    outSpecial = 0;
    if (outSource)
        *outSource = 'N';

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

    if (!missionOk && !checkpointOk)
        return false;

    const int missionSum = missionCommon + missionSpecial;
    const int checkpointSum = checkpointCommon + checkpointSpecial;

    // Some servers stop refreshing m_missionZombieKills after map transitions but leave the
    // previous non-zero value in place. The old code only fell back when mission was zero,
    // so a stale mission value could pin the HUD / kill-feedback forever after a few chapters.
    // Prefer whichever valid source currently has the larger non-zero total; equal totals keep
    // mission as the primary source.
    const bool useCheckpoint = checkpointOk && (
        !missionOk ||
        (checkpointSum > 0 && missionSum <= 0) ||
        (checkpointSum > missionSum));

    if (useCheckpoint)
    {
        outCommon = checkpointCommon;
        outSpecial = checkpointSpecial;
        if (outSource)
            *outSource = 'C';
        return true;
    }

    if (missionOk)
    {
        outCommon = missionCommon;
        outSpecial = missionSpecial;
        if (outSource)
            *outSource = 'M';
        return true;
    }

    outCommon = checkpointCommon;
    outSpecial = checkpointSpecial;
    if (outSource)
        *outSource = 'C';
    return true;
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

void VR::TriggerImpactHapticsBothHands(float amplitude, float frequency, float durationSeconds, int priority)
{
    const float amp = std::clamp(amplitude, 0.0f, 1.0f);
    if (amp <= 0.0f)
        return;

    TriggerPhysicalHandHapticPulse(true, durationSeconds, frequency, amp, priority);
    TriggerPhysicalHandHapticPulse(false, durationSeconds, frequency, amp, priority);
}

void VR::TriggerDirectionalDamageHaptics(float amplitude, float frequency, float durationSeconds, float rightBias, int priority)
{
    const float amp = std::clamp(amplitude * std::clamp(m_DamageFeedbackOverallScale, 0.0f, 1.0f), 0.0f, 1.0f);
    if (amp <= 0.0f)
        return;

    float bias = std::clamp(rightBias, -1.0f, 1.0f);
    if (bias != 0.0f)
    {
        // Sharpen mid-strength directional samples so they do not collapse into a near-even
        // two-hand buzz. This keeps the dominant hand clearly stronger without requiring
        // a perfectly side-on attacker position.
        bias = std::copysign(std::pow(std::abs(bias), 0.65f), bias);
    }

    constexpr float kOppositeHandFloor = 0.05f;
    const float rightBlend = (bias + 1.0f) * 0.5f;
    const float leftWeight = std::clamp(1.0f - (1.0f - kOppositeHandFloor) * rightBlend, kOppositeHandFloor, 1.0f);
    const float rightWeight = std::clamp(kOppositeHandFloor + (1.0f - kOppositeHandFloor) * rightBlend, kOppositeHandFloor, 1.0f);
    TriggerPhysicalHandHapticPulse(true, durationSeconds, frequency, amp * leftWeight, priority);
    TriggerPhysicalHandHapticPulse(false, durationSeconds, frequency, amp * rightWeight, priority);
}

VR::DamageFeedbackType VR::ClassifyDamageFeedbackType(const char* weaponName, int damage) const
{
    const std::string weapon = weaponName ? ToLowerCopy(weaponName) : std::string();

    if (weapon.find("spit") != std::string::npos || weapon.find("acid") != std::string::npos || weapon.find("insect_swarm") != std::string::npos)
        return DamageFeedbackType::Acid;
    if (weapon.find("fire") != std::string::npos || weapon.find("burn") != std::string::npos || weapon.find("inferno") != std::string::npos || weapon.find("molotov") != std::string::npos)
        return DamageFeedbackType::Fire;
    if (weapon.find("explosion") != std::string::npos || weapon.find("grenade") != std::string::npos || weapon.find("blast") != std::string::npos || weapon.find("pipe_bomb") != std::string::npos || weapon.find("propane") != std::string::npos)
        return DamageFeedbackType::Explosion;
    if (weapon.find("tank") != std::string::npos || weapon.find("charger") != std::string::npos || weapon.find("rock") != std::string::npos)
        return DamageFeedbackType::HeavyHit;
    if (weapon.find("claw") != std::string::npos || weapon.find("hunter") != std::string::npos || weapon.find("jockey") != std::string::npos || weapon.find("smoker") != std::string::npos || weapon.find("boomer") != std::string::npos || weapon.find("spitter") != std::string::npos)
        return DamageFeedbackType::SpecialHit;

    if (damage >= 22)
        return DamageFeedbackType::HeavyHit;
    if (damage >= 8)
        return DamageFeedbackType::SpecialHit;
    return DamageFeedbackType::CommonHit;
}

WeaponHapticsProfile VR::GetDamageHapticsProfile(DamageFeedbackType type) const
{
    switch (type)
    {
    case DamageFeedbackType::CommonHit: return m_DamageCommonHapticsProfile;
    case DamageFeedbackType::SpecialHit: return m_DamageSpecialHapticsProfile;
    case DamageFeedbackType::HeavyHit: return m_DamageHeavyHapticsProfile;
    case DamageFeedbackType::Explosion: return m_DamageExplosionHapticsProfile;
    case DamageFeedbackType::Fire: return m_DamageFireHapticsProfile;
    case DamageFeedbackType::Acid: return m_DamageAcidHapticsProfile;
    default: return m_DamageCommonHapticsProfile;
    }
}

VR::ControlledDamageState VR::ResolveLocalSpecialControlState(C_BasePlayer* localPlayer, C_BaseEntity** outControllerEntity) const
{
    if (outControllerEntity)
        *outControllerEntity = nullptr;

    if (!localPlayer || !m_Game || !m_Game->m_ClientEntityList)
        return ControlledDamageState::None;

    const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
    auto resolveHandle = [&](int offset) -> C_BaseEntity*
        {
            uint32_t handle = 0u;
            if (!VR_TryReadU32(base, offset, handle) || handle == 0u || handle == 0xFFFFFFFFu)
                return nullptr;
            return reinterpret_cast<C_BaseEntity*>(m_Game->m_ClientEntityList->GetClientEntityFromHandle(handle));
        };

    if (C_BaseEntity* attacker = resolveHandle(kPummelAttackerOffset))
    {
        if (outControllerEntity)
            *outControllerEntity = attacker;
        return ControlledDamageState::ChargerPummel;
    }

    if (C_BaseEntity* attacker = resolveHandle(kCarryAttackerOffset))
    {
        if (outControllerEntity)
            *outControllerEntity = attacker;
        return ControlledDamageState::ChargerCarry;
    }

    if (C_BaseEntity* attacker = resolveHandle(kPounceAttackerOffset))
    {
        if (outControllerEntity)
            *outControllerEntity = attacker;
        return ControlledDamageState::HunterPounce;
    }

    if (C_BaseEntity* attacker = resolveHandle(kJockeyAttackerOffset))
    {
        if (outControllerEntity)
            *outControllerEntity = attacker;
        return ControlledDamageState::JockeyRide;
    }

    if (C_BaseEntity* attacker = resolveHandle(kTongueOwnerOffset))
    {
        if (outControllerEntity)
            *outControllerEntity = attacker;
        return ControlledDamageState::SmokerTongue;
    }

    return ControlledDamageState::None;
}

WeaponHapticsProfile VR::GetControlledDamageHapticsProfile(ControlledDamageState state) const
{
    switch (state)
    {
    case ControlledDamageState::SmokerTongue:  return m_DamageSmokerControlHapticsProfile;
    case ControlledDamageState::HunterPounce:  return m_DamageHunterControlHapticsProfile;
    case ControlledDamageState::JockeyRide:    return m_DamageJockeyControlHapticsProfile;
    case ControlledDamageState::ChargerCarry:  return m_DamageChargerCarryHapticsProfile;
    case ControlledDamageState::ChargerPummel: return m_DamageChargerPummelHapticsProfile;
    default:                                   return m_DamageSpecialHapticsProfile;
    }
}

float VR::GetControlledDamageDirectionalBiasScale(ControlledDamageState state) const
{
    switch (state)
    {
    case ControlledDamageState::SmokerTongue:  return m_DamageSmokerControlDirectionalBiasScale;
    case ControlledDamageState::HunterPounce:  return m_DamageHunterControlDirectionalBiasScale;
    case ControlledDamageState::JockeyRide:    return m_DamageJockeyControlDirectionalBiasScale;
    case ControlledDamageState::ChargerCarry:  return m_DamageChargerCarryDirectionalBiasScale;
    case ControlledDamageState::ChargerPummel: return m_DamageChargerPummelDirectionalBiasScale;
    default:                                   return 1.00f;
    }
}

void VR::ParseHapticsConfigFile()
{
    std::ifstream configStream("VR\\haptics_config.txt");
    if (!configStream)
        return;

    auto trim = [](std::string& s)
        {
            auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char ch) { return !isSpace(ch); }));
            s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char ch) { return !isSpace(ch); }).base(), s.end());
        };

    std::unordered_map<std::string, std::string> userConfig;
    std::string line;
    while (std::getline(configStream, line))
    {
        size_t cut = std::string::npos;
        const size_t p1 = line.find("//");
        const size_t p2 = line.find('#');
        const size_t p3 = line.find(';');
        if (p1 != std::string::npos) cut = p1;
        if (p2 != std::string::npos) cut = (cut == std::string::npos) ? p2 : std::min(cut, p2);
        if (p3 != std::string::npos) cut = (cut == std::string::npos) ? p3 : std::min(cut, p3);
        if (cut != std::string::npos)
            line.erase(cut);

        trim(line);
        if (line.empty())
            continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key);
        trim(value);
        if (key.empty())
            continue;

        userConfig[ToLowerCopy(key)] = value;
    }

    auto getBool = [&](const char* key, bool defVal) -> bool
        {
            auto it = userConfig.find(ToLowerCopy(key));
            if (it == userConfig.end())
                return defVal;
            std::string value = ToLowerCopy(TrimCopy(it->second));
            if (value == "1" || value == "true" || value == "on" || value == "yes")
                return true;
            if (value == "0" || value == "false" || value == "off" || value == "no")
                return false;
            return defVal;
        };

    auto getFloat = [&](const char* key, float defVal) -> float
        {
            auto it = userConfig.find(ToLowerCopy(key));
            if (it == userConfig.end())
                return defVal;
            try
            {
                return std::stof(TrimCopy(it->second));
            }
            catch (...)
            {
                return defVal;
            }
        };

    auto parseProfile = [&](const std::string& key, const WeaponHapticsProfile& defaults) -> WeaponHapticsProfile
        {
            auto it = userConfig.find(ToLowerCopy(key));
            if (it == userConfig.end())
                return defaults;

            WeaponHapticsProfile profile = defaults;
            std::stringstream ss(it->second);
            std::string token;
            float* values[3] = { &profile.durationSeconds, &profile.frequency, &profile.amplitude };
            int index = 0;
            while (std::getline(ss, token, ',') && index < 3)
            {
                token = TrimCopy(token);
                if (!token.empty())
                {
                    try
                    {
                        *values[index] = std::stof(token);
                    }
                    catch (...)
                    {
                    }
                }
                ++index;
            }
            profile.durationSeconds = std::clamp(profile.durationSeconds, 0.0f, 0.5f);
            profile.frequency = std::clamp(profile.frequency, 0.0f, 320.0f);
            profile.amplitude = std::clamp(profile.amplitude, 0.0f, 1.0f);
            return profile;
        };

    auto setWeaponOverride = [&](const char* weaponKey, const WeaponHapticsProfile& defaults)
        {
            const std::string key = std::string("weapon.") + weaponKey;
            m_WeaponHapticsOverrides[weaponKey] = parseProfile(key, defaults);
        };

    m_WeaponHapticsEnabled = getBool("weapon.enabled", m_WeaponHapticsEnabled);
    m_HapticMixMinIntervalSeconds = std::max(0.0f, getFloat("mix.min_interval", m_HapticMixMinIntervalSeconds));
    m_DefaultWeaponHapticsProfile = parseProfile("weapon.default", m_DefaultWeaponHapticsProfile);
    m_MeleeSwingHapticsProfile = parseProfile("melee.swing", m_MeleeSwingHapticsProfile);
    m_ShoveHapticsProfile = parseProfile("melee.shove", m_ShoveHapticsProfile);

    m_DamageFeedbackEnabled = getBool("damage.enabled", true);
    m_DamageDirectionalEnabled = getBool("damage.directional", true);
    m_DamageFeedbackMergeWindowSeconds = std::max(0.0f, getFloat("damage.merge_window", m_DamageFeedbackMergeWindowSeconds));
    m_DamageFeedbackMinTriggerIntervalSeconds = std::max(0.0f, getFloat("damage.min_trigger_interval", m_DamageFeedbackMinTriggerIntervalSeconds));
    m_DamageFeedbackMergedHitBonus = std::max(0.0f, getFloat("damage.merged_hit_bonus", m_DamageFeedbackMergedHitBonus));
    m_DamageScaleStart = std::max(0.0f, getFloat("damage.scale_start", m_DamageScaleStart));
    m_DamageScalePerPoint = std::max(0.0f, getFloat("damage.scale_per_point", m_DamageScalePerPoint));
    m_DamageScaleMaxBonus = std::max(0.0f, getFloat("damage.scale_max_bonus", m_DamageScaleMaxBonus));
    m_DamageAmplitudeMin = std::clamp(getFloat("damage.amplitude_min", m_DamageAmplitudeMin), 0.0f, 1.0f);
    m_DamageAmplitudeMax = std::clamp(getFloat("damage.amplitude_max", m_DamageAmplitudeMax), 0.0f, 1.0f);
    if (m_DamageAmplitudeMax < m_DamageAmplitudeMin)
        std::swap(m_DamageAmplitudeMin, m_DamageAmplitudeMax);

    m_DamageCommonHapticsProfile = parseProfile("damage.common", m_DamageCommonHapticsProfile);
    m_DamageSpecialHapticsProfile = parseProfile("damage.special", m_DamageSpecialHapticsProfile);
    m_DamageHeavyHapticsProfile = parseProfile("damage.heavy", m_DamageHeavyHapticsProfile);
    m_DamageExplosionHapticsProfile = parseProfile("damage.explosion", m_DamageExplosionHapticsProfile);
    m_DamageFireHapticsProfile = parseProfile("damage.fire", m_DamageFireHapticsProfile);
    m_DamageAcidHapticsProfile = parseProfile("damage.acid", m_DamageAcidHapticsProfile);
    m_DamageSmokerControlHapticsProfile = parseProfile("damage.control.smoker", m_DamageSmokerControlHapticsProfile);
    m_DamageHunterControlHapticsProfile = parseProfile("damage.control.hunter", m_DamageHunterControlHapticsProfile);
    m_DamageJockeyControlHapticsProfile = parseProfile("damage.control.jockey", m_DamageJockeyControlHapticsProfile);
    m_DamageChargerCarryHapticsProfile = parseProfile("damage.control.charger_carry", m_DamageChargerCarryHapticsProfile);
    m_DamageChargerPummelHapticsProfile = parseProfile("damage.control.charger_pummel", m_DamageChargerPummelHapticsProfile);

    m_DamageSmokerControlDirectionalBiasScale = std::clamp(getFloat("damage.control.smoker.directional_bias", m_DamageSmokerControlDirectionalBiasScale), 0.0f, 1.0f);
    m_DamageHunterControlDirectionalBiasScale = std::clamp(getFloat("damage.control.hunter.directional_bias", m_DamageHunterControlDirectionalBiasScale), 0.0f, 1.0f);
    m_DamageJockeyControlDirectionalBiasScale = std::clamp(getFloat("damage.control.jockey.directional_bias", m_DamageJockeyControlDirectionalBiasScale), 0.0f, 1.0f);
    m_DamageChargerCarryDirectionalBiasScale = std::clamp(getFloat("damage.control.charger_carry.directional_bias", m_DamageChargerCarryDirectionalBiasScale), 0.0f, 1.0f);
    m_DamageChargerPummelDirectionalBiasScale = std::clamp(getFloat("damage.control.charger_pummel.directional_bias", m_DamageChargerPummelDirectionalBiasScale), 0.0f, 1.0f);

    m_LandingHapticsEnabled = getBool("landing.enabled", m_LandingHapticsEnabled);
    m_LandingMinAirTime = std::max(0.0f, getFloat("landing.min_air_time", m_LandingMinAirTime));
    m_LandingMinDownwardSpeed = std::max(0.0f, getFloat("landing.min_down_speed", m_LandingMinDownwardSpeed));
    m_LandingMediumHapticsProfile = parseProfile("landing.medium", m_LandingMediumHapticsProfile);
    m_LandingDamageHapticsProfile = parseProfile("landing.damage", m_LandingDamageHapticsProfile);

    setWeaponOverride("pistol", { 0.018f, 165.0f, 0.33f });
    setWeaponOverride("magnum", { 0.032f, 85.0f, 0.66f });
    setWeaponOverride("uzi", { 0.012f, 185.0f, 0.23f });
    setWeaponOverride("mac10", { 0.011f, 195.0f, 0.24f });
    setWeaponOverride("mp5", { 0.012f, 190.0f, 0.26f });
    setWeaponOverride("m16a1", { 0.015f, 145.0f, 0.34f });
    setWeaponOverride("ak47", { 0.020f, 120.0f, 0.44f });
    setWeaponOverride("scar", { 0.017f, 135.0f, 0.39f });
    setWeaponOverride("sg552", { 0.018f, 130.0f, 0.40f });
    setWeaponOverride("pumpshotgun", { 0.040f, 72.0f, 0.78f });
    setWeaponOverride("shotgun_chrome", { 0.042f, 70.0f, 0.80f });
    setWeaponOverride("autoshotgun", { 0.030f, 78.0f, 0.65f });
    setWeaponOverride("spas", { 0.029f, 82.0f, 0.62f });
    setWeaponOverride("hunting_rifle", { 0.038f, 88.0f, 0.72f });
    setWeaponOverride("sniper_military", { 0.033f, 92.0f, 0.61f });
    setWeaponOverride("scout", { 0.036f, 96.0f, 0.69f });
    setWeaponOverride("awp", { 0.052f, 62.0f, 0.94f });
    setWeaponOverride("m60", { 0.019f, 115.0f, 0.50f });
    setWeaponOverride("grenade_launcher", { 0.060f, 55.0f, 1.00f });
    setWeaponOverride("melee", { 0.028f, 105.0f, 0.54f });
    setWeaponOverride("chainsaw", { 0.014f, 175.0f, 0.34f });

    // Direct damage-event haptics remain configurable here.
    // Sustained acid/fire and camera-shake haptics still stay off until their old branches are rebuilt,
    // but landing haptics are now active again and use raw netvar offsets to detect the airborne->landed transition.
    m_DamageSustainEnabled = false;
    m_CameraShakeHapticsEnabled = false;
    m_AcidSustainUntil = {};
    m_FireSustainUntil = {};
    m_NextAcidSustainPulse = {};
    m_NextFireSustainPulse = {};
    m_LastDamageFeedbackEventRegisterAttempt = {};
    m_LastDamageFeedbackEventSeenTime = {};
    m_LastObservedDamageHealth = -1;
    m_LastCameraShakeHapticsPulse = {};
    m_LandingAirborneSince = {};
    m_WasOnGroundForHaptics = true;
    m_LastVerticalSpeedForHaptics = 0.0f;
    m_LandingPeakDownwardSpeedForHaptics = 0.0f;
    m_LastAirborneHealthForHaptics = -1;
    m_CameraShakeStateInitialized = false;
    m_LastCameraShakeOrigin = { 0, 0, 0 };
    m_LastCameraShakeAngles = { 0, 0, 0 };
}

void VR::EnsureDamageFeedbackEventListener()
{
    if (m_DamageFeedbackEventListenerRegistered || !m_Game || !m_DamageFeedbackEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_LastDamageFeedbackEventRegisterAttempt.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastDamageFeedbackEventRegisterAttempt).count();
        if (elapsed < 2.0f)
            return;
    }
    m_LastDamageFeedbackEventRegisterAttempt = now;

    if (!m_DamageFeedbackEventManager)
        m_DamageFeedbackEventManager = m_Game->m_GameEventManager;

    if (!m_DamageFeedbackEventManager)
    {
        HMODULE engineModule = GetModuleHandleA("engine.dll");
        if (engineModule)
        {
            using tCreateInterface = void* (__cdecl*)(const char* name, int* returnCode);
            const auto createInterface = reinterpret_cast<tCreateInterface>(GetProcAddress(engineModule, "CreateInterface"));
            if (createInterface)
            {
                int returnCode = 0;
                void* iface = createInterface("GAMEEVENTSMANAGER002", &returnCode);
                if (!iface)
                    iface = createInterface("GAMEEVENTSMANAGER001", &returnCode);
                m_DamageFeedbackEventManager = static_cast<IGameEventManager2*>(iface);
                if (m_DamageFeedbackEventManager && !m_Game->m_GameEventManager)
                    m_Game->m_GameEventManager = m_DamageFeedbackEventManager;
            }
        }
    }

    if (!m_DamageFeedbackEventManager)
    {
        Game::logMsg("[VR][DamageFeedback][listener] missing game event manager");
        return;
    }

    if (!m_DamageFeedbackEventListener)
        m_DamageFeedbackEventListener = new VRDamageFeedbackEventListener(this);

    static constexpr const char* kDamageEvents[] = { "player_hurt" };
    bool registeredAll = true;
    for (const char* eventName : kDamageEvents)
    {
        const bool alreadyRegistered = m_DamageFeedbackEventManager->FindListener(m_DamageFeedbackEventListener, eventName);
        const bool registered = alreadyRegistered || m_DamageFeedbackEventManager->AddListener(m_DamageFeedbackEventListener, eventName, false);
        if (!registered)
            Game::logMsg("[VR][DamageFeedback][listener] failed to register event=%s", eventName);
        registeredAll = registeredAll && registered;
    }

    if (registeredAll)
        Game::logMsg("[VR][DamageFeedback][listener] registered player_hurt");

    m_DamageFeedbackEventListenerRegistered = registeredAll;
}

void VR::HandleDamageFeedbackGameEvent(IGameEvent* event)
{
    if (!event || !m_DamageFeedbackEnabled || !m_Game || !m_Game->m_EngineClient || !m_IsVREnabled)
        return;

    const char* eventNameRaw = event->GetName();
    if (!eventNameRaw || !*eventNameRaw)
        return;

    const std::string eventName = ToLowerCopy(eventNameRaw);
    if (eventName != "player_hurt")
        return;

    const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    if (localPlayerIndex <= 0)
        return;

    const int victimUserId = event->GetInt("userid", 0);
    int victimIndex = 0;
    if (victimUserId > 0)
        victimIndex = m_Game->m_EngineClient->GetPlayerForUserID(victimUserId);
    if (victimIndex <= 0)
        victimIndex = event->GetInt("victimentid", 0);
    if (victimIndex <= 0)
        victimIndex = event->GetInt("entityid", 0);
    if (victimIndex <= 0)
        victimIndex = event->GetInt("entindex", 0);

    bool eventIsForLocalPlayer = victimIndex > 0 && victimIndex == localPlayerIndex;
    if (!eventIsForLocalPlayer)
    {
        const int localUserId = GetLocalPlayerUserId(m_Game);
        eventIsForLocalPlayer = localUserId > 0 && victimUserId > 0 && victimUserId == localUserId;
    }
    if (!eventIsForLocalPlayer)
        return;

    C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(localPlayerIndex);
    if (!localPlayer)
        return;

    m_LastDamageFeedbackEventSeenTime = std::chrono::steady_clock::now();

    const int damage = (std::max)(1, event->GetInt("dmg_health", event->GetInt("amount", 1)));
    const char* weapon = event->GetString("weapon", "");
    const DamageFeedbackType baseType = ClassifyDamageFeedbackType(weapon, damage);

    C_BaseEntity* controllerEntity = nullptr;
    const ControlledDamageState controlState = ResolveLocalSpecialControlState(localPlayer, &controllerEntity);
    const bool useControlledProfile = controlState != ControlledDamageState::None
        && baseType != DamageFeedbackType::Fire
        && baseType != DamageFeedbackType::Acid
        && baseType != DamageFeedbackType::Explosion;

    float directionalBias = 0.0f;
    bool hasDirectionalBias = false;

    if (m_DamageDirectionalEnabled)
    {
        C_BaseEntity* directionSource = nullptr;
        if (useControlledProfile)
        {
            directionSource = controllerEntity;
        }
        else
        {
            int attackerIndex = 0;
            const int attackerUserId = event->GetInt("attacker", 0);
            if (attackerUserId > 0)
                attackerIndex = m_Game->m_EngineClient->GetPlayerForUserID(attackerUserId);
            if (attackerIndex <= 0)
                attackerIndex = event->GetInt("attackerentid", 0);
            if (attackerIndex > 0)
                directionSource = m_Game->GetClientEntity(attackerIndex);
        }

        if (directionSource)
        {
            Vector localPos = localPlayer->GetAbsOrigin();
            Vector attackerPos = directionSource->GetAbsOrigin();
            Vector toAttacker = attackerPos - localPos;
            toAttacker.z = 0.0f;

            Vector right = m_HmdRight;
            right.z = 0.0f;

            if (!toAttacker.IsZero() && !right.IsZero())
            {
                VectorNormalize(toAttacker);
                VectorNormalize(right);
                directionalBias = std::clamp(DotProduct(toAttacker, right), -1.0f, 1.0f);
                hasDirectionalBias = true;
            }
        }
    }

    const auto now = std::chrono::steady_clock::now();
    PendingDamageFeedback& pending = m_PendingDamageFeedback;
    const ControlledDamageState pendingControlState = useControlledProfile ? controlState : ControlledDamageState::None;

    const bool sameBurst = pending.active
        && pending.queuedAt.time_since_epoch().count() != 0
        && pending.controlState == pendingControlState
        && std::chrono::duration<float>(now - pending.queuedAt).count() <= (std::max)(0.0f, m_DamageFeedbackMergeWindowSeconds);

    auto damageSeverity = [](DamageFeedbackType feedbackType)
        {
            switch (feedbackType)
            {
            case DamageFeedbackType::CommonHit: return 0;
            case DamageFeedbackType::SpecialHit: return 1;
            case DamageFeedbackType::Fire:      return 2;
            case DamageFeedbackType::Acid:      return 3;
            case DamageFeedbackType::HeavyHit:  return 4;
            case DamageFeedbackType::Explosion: return 5;
            default:                            return 0;
            }
        };

    if (!sameBurst)
    {
        pending = {};
        pending.active = true;
        pending.type = baseType;
        pending.controlState = pendingControlState;
        pending.maxDamage = damage;
        pending.mergedCount = 1;
        pending.queuedAt = now;
    }
    else
    {
        pending.maxDamage = (std::max)(pending.maxDamage, damage);
        pending.mergedCount = (std::min)(pending.mergedCount + 1, 8);
        if (pending.controlState == ControlledDamageState::None && damageSeverity(baseType) > damageSeverity(pending.type))
            pending.type = baseType;
    }

    if (hasDirectionalBias)
    {
        pending.directionalBiasSum += directionalBias;
        pending.directionalSampleCount += 1;
        if (std::abs(directionalBias) > std::abs(pending.directionalBiasPeak))
            pending.directionalBiasPeak = directionalBias;
    }
}

void VR::UpdateDamageFeedback()
{
    if (!m_DamageFeedbackEnabled || !m_IsVREnabled || !m_Game || !m_Game->m_EngineClient)
    {
        m_LastObservedDamageHealth = -1;
        return;
    }

    EnsureDamageFeedbackEventListener();

    const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(localPlayerIndex);
    if (!localPlayer)
    {
        m_LastObservedDamageHealth = -1;
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    const int currentHealth = (std::max)(0, *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(localPlayer) + kHealthOffset));
    if (m_LastObservedDamageHealth < 0)
    {
        m_LastObservedDamageHealth = currentHealth;
    }
    else
    {
        if (currentHealth < m_LastObservedDamageHealth)
        {
            const int healthDelta = m_LastObservedDamageHealth - currentHealth;
            const bool recentDamageEvent = m_LastDamageFeedbackEventSeenTime.time_since_epoch().count() != 0
                && std::chrono::duration<float>(now - m_LastDamageFeedbackEventSeenTime).count() <= 0.20f;

            if (!recentDamageEvent && healthDelta > 0)
            {
                PendingDamageFeedback& pending = m_PendingDamageFeedback;
                pending = {};
                pending.active = true;
                pending.type = ClassifyDamageFeedbackType("", healthDelta);
                pending.controlState = ResolveLocalSpecialControlState(localPlayer, nullptr);
                pending.maxDamage = healthDelta;
                pending.mergedCount = 1;
                pending.queuedAt = now;
                Game::logMsg("[VR][DamageFeedback][fallback] health drop %d -> %d (delta=%d)", m_LastObservedDamageHealth, currentHealth, healthDelta);
            }
        }

        m_LastObservedDamageHealth = currentHealth;
    }

    if (m_PendingDamageFeedback.active)
    {
        const bool mergeWindowElapsed = m_PendingDamageFeedback.queuedAt.time_since_epoch().count() == 0
            || std::chrono::duration<float>(now - m_PendingDamageFeedback.queuedAt).count() >= (std::max)(0.0f, m_DamageFeedbackMergeWindowSeconds);

        const bool triggerIntervalElapsed = m_LastDamageFeedbackTriggerTime.time_since_epoch().count() == 0
            || std::chrono::duration<float>(now - m_LastDamageFeedbackTriggerTime).count() >= (std::max)(0.0f, m_DamageFeedbackMinTriggerIntervalSeconds);

        if (mergeWindowElapsed && triggerIntervalElapsed)
        {
            const bool controlled = m_PendingDamageFeedback.controlState != ControlledDamageState::None;
            const WeaponHapticsProfile damageProfile = controlled
                ? GetControlledDamageHapticsProfile(m_PendingDamageFeedback.controlState)
                : GetDamageHapticsProfile(m_PendingDamageFeedback.type);
            float amplitude = damageProfile.amplitude;
            const float frequency = damageProfile.frequency;
            const float duration = damageProfile.durationSeconds;
            const float damageBonus = std::clamp((m_PendingDamageFeedback.maxDamage - m_DamageScaleStart) * m_DamageScalePerPoint, 0.0f, m_DamageScaleMaxBonus);
            const float mergedBonus = (std::max)(0, m_PendingDamageFeedback.mergedCount - 1) * (std::max)(0.0f, m_DamageFeedbackMergedHitBonus);
            amplitude = std::clamp(amplitude + damageBonus + mergedBonus, m_DamageAmplitudeMin, m_DamageAmplitudeMax);

            int damagePriority = 2;
            if (controlled)
            {
                switch (m_PendingDamageFeedback.controlState)
                {
                case ControlledDamageState::SmokerTongue:
                case ControlledDamageState::HunterPounce:
                case ControlledDamageState::JockeyRide:
                    damagePriority = 3;
                    break;
                case ControlledDamageState::ChargerCarry:
                    damagePriority = 4;
                    break;
                case ControlledDamageState::ChargerPummel:
                    damagePriority = 5;
                    break;
                default:
                    break;
                }
            }
            else
            {
                if (m_PendingDamageFeedback.type == DamageFeedbackType::SpecialHit)
                    damagePriority = 3;
                else if (m_PendingDamageFeedback.type == DamageFeedbackType::HeavyHit || m_PendingDamageFeedback.type == DamageFeedbackType::Explosion)
                    damagePriority = 4;
            }

            float rightBias = 0.0f;
            if (m_PendingDamageFeedback.directionalSampleCount > 0)
            {
                const float averageBias = std::clamp(
                    m_PendingDamageFeedback.directionalBiasSum / (float)m_PendingDamageFeedback.directionalSampleCount,
                    -1.0f,
                    1.0f);
                const float dominantBias = std::clamp(m_PendingDamageFeedback.directionalBiasPeak, -1.0f, 1.0f);
                // Keep the strongest sample in the burst dominant so rapid multi-hit merges
                // do not wash the direction back toward center.
                rightBias = std::clamp(averageBias * 0.35f + dominantBias * 0.65f, -1.0f, 1.0f);
            }
            if (controlled)
                rightBias *= std::clamp(GetControlledDamageDirectionalBiasScale(m_PendingDamageFeedback.controlState), 0.0f, 1.0f);

            TriggerDirectionalDamageHaptics(amplitude, frequency, duration, rightBias, damagePriority);
            m_LastDamageFeedbackTriggerTime = now;
            m_PendingDamageFeedback = {};
        }
    }

    // Sustained damage envelopes (acid / fire).
    if (m_DamageSustainEnabled)
    {
        if (now < m_AcidSustainUntil && now >= m_NextAcidSustainPulse)
        {
            TriggerImpactHapticsBothHands(
                m_DamageAcidSustainPulse.amplitude,
                m_DamageAcidSustainPulse.frequency,
                m_DamageAcidSustainPulse.durationSeconds,
                1);
            m_NextAcidSustainPulse = now + std::chrono::milliseconds((int)std::round(m_DamageAcidSustainIntervalSeconds * 1000.0f));
        }
        if (now < m_FireSustainUntil && now >= m_NextFireSustainPulse)
        {
            TriggerImpactHapticsBothHands(
                m_DamageFireSustainPulse.amplitude,
                m_DamageFireSustainPulse.frequency,
                m_DamageFireSustainPulse.durationSeconds,
                1);
            m_NextFireSustainPulse = now + std::chrono::milliseconds((int)std::round(m_DamageFireSustainIntervalSeconds * 1000.0f));
        }
    }

    // Landing impact. Detect the airborne->landed transition from raw netvar offsets instead of
    // relying on C_BasePlayer field layouts directly, then split the response into:
    //   - landing.medium: landed without losing health
    //   - landing.damage: landed and health dropped on the landing frame
    if (m_LandingHapticsEnabled)
    {
        auto resetLandingState = [&]()
            {
                m_LandingAirborneSince = {};
                m_WasOnGroundForHaptics = true;
                m_LastVerticalSpeedForHaptics = 0.0f;
                m_LandingPeakDownwardSpeedForHaptics = 0.0f;
                m_LastAirborneHealthForHaptics = -1;
            };

        const unsigned char* playerBase = reinterpret_cast<const unsigned char*>(localPlayer);
        unsigned char lifeState = 0;
        int groundEntity = -1;
        int flags = 0;
        float verticalSpeed = 0.0f;
        int health = 0;

        const bool validLandingRead =
            SafeReadU8(playerBase, kLifeStateOffset, lifeState) &&
            SafeReadInt(playerBase, kGroundEntityOffset, groundEntity) &&
            SafeReadInt(playerBase, kFlagsOffset, flags) &&
            SafeReadFloat(playerBase, kVelocityZOffset, verticalSpeed) &&
            SafeReadInt(playerBase, kHealthOffset, health);

        if (!validLandingRead || lifeState != 0 || health <= 0)
        {
            resetLandingState();
        }
        else
        {
            const bool onGround = (groundEntity != -1) || ((flags & kOnGroundFlag) != 0);
            if (onGround)
            {
                if (!m_WasOnGroundForHaptics)
                {
                    const float airTime = (m_LandingAirborneSince.time_since_epoch().count() == 0)
                        ? 0.0f
                        : std::chrono::duration<float>(now - m_LandingAirborneSince).count();
                    const bool hardEnoughLanding = airTime >= m_LandingMinAirTime
                        && m_LandingPeakDownwardSpeedForHaptics >= m_LandingMinDownwardSpeed;

                    if (hardEnoughLanding)
                    {
                        const bool landedWithDamage = m_LastAirborneHealthForHaptics > 0 && health < m_LastAirborneHealthForHaptics;
                        const WeaponHapticsProfile& landingProfile = landedWithDamage
                            ? m_LandingDamageHapticsProfile
                            : m_LandingMediumHapticsProfile;
                        TriggerImpactHapticsBothHands(
                            landingProfile.amplitude,
                            landingProfile.frequency,
                            landingProfile.durationSeconds,
                            landedWithDamage ? 4 : 3);
                    }
                }

                m_LandingAirborneSince = {};
                m_LandingPeakDownwardSpeedForHaptics = 0.0f;
                m_LastAirborneHealthForHaptics = health;
            }
            else
            {
                const float downwardSpeed = std::max(0.0f, -verticalSpeed);
                if (m_WasOnGroundForHaptics)
                {
                    m_LandingAirborneSince = now;
                    m_LandingPeakDownwardSpeedForHaptics = downwardSpeed;
                }
                else
                {
                    m_LandingPeakDownwardSpeedForHaptics = std::max(m_LandingPeakDownwardSpeedForHaptics, downwardSpeed);
                }

                // Keep the last airborne HP sample so the first grounded frame can tell whether
                // the landing itself caused damage. Mid-air damage that already replicated earlier
                // will naturally update this baseline and won't be misclassified as fall damage.
                m_LastAirborneHealthForHaptics = health;
            }

            m_WasOnGroundForHaptics = onGround;
            m_LastVerticalSpeedForHaptics = verticalSpeed;
        }
    }

    // Camera shake -> haptics (explosions, tank stomps, etc.).
    if (m_CameraShakeHapticsEnabled)
    {
        const auto normalizeAngleDelta = [](float current, float previous)
            {
                return std::fabs(std::remainderf(current - previous, 360.0f));
            };

        if (!m_CameraShakeStateInitialized)
        {
            m_LastCameraShakeOrigin = m_SetupOrigin;
            m_LastCameraShakeAngles = m_SetupAngles;
            m_CameraShakeStateInitialized = true;
        }
        else
        {
            vr::InputAnalogActionData_t turnActionData{};
            const bool turnActionActive = GetAnalogActionData(m_ActionTurn, turnActionData)
                && std::fabs(turnActionData.x) > 0.2f;
            const float angDelta =
                normalizeAngleDelta(m_SetupAngles.x, m_LastCameraShakeAngles.x) +
                normalizeAngleDelta(m_SetupAngles.z, m_LastCameraShakeAngles.z);
            const float posDelta = (m_SetupOrigin - m_LastCameraShakeOrigin).Length();
            const float hmdAngVel = m_HmdPose.TrackedDeviceAngVel.Length();

            m_LastCameraShakeAngles = m_SetupAngles;
            m_LastCameraShakeOrigin = m_SetupOrigin;

            if (!turnActionActive && hmdAngVel < m_CameraShakeHmdAngVelMax)
            {
                const float shakeScore = (std::max)(
                    std::clamp((angDelta - m_CameraShakeAngleThreshold) / m_CameraShakeAngleRange, 0.0f, 1.0f),
                    std::clamp((posDelta - m_CameraShakePosThreshold) / m_CameraShakePosRange, 0.0f, 1.0f));

                if (shakeScore > 0.0f
                    && (m_LastCameraShakeHapticsPulse.time_since_epoch().count() == 0
                        || std::chrono::duration<float>(now - m_LastCameraShakeHapticsPulse).count() >= m_CameraShakePulseIntervalSeconds))
                {
                    const float amp = m_CameraShakePulseAmpMin + (m_CameraShakePulseAmpMax - m_CameraShakePulseAmpMin) * shakeScore;
                    const float freq = m_CameraShakePulseFreqMax + (m_CameraShakePulseFreqMin - m_CameraShakePulseFreqMax) * shakeScore;
                    const float dur = m_CameraShakePulseDurMin + (m_CameraShakePulseDurMax - m_CameraShakePulseDurMin) * shakeScore;
                    TriggerImpactHapticsBothHands(amp, freq, dur, 2);
                    m_LastCameraShakeHapticsPulse = now;
                }
            }
        }
    }
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

    static constexpr const char* kKillSoundEvents[] = { "player_hurt", "infected_hurt", "player_death", "infected_death", "witch_killed" };
    bool registeredAll = true;
    for (const char* eventName : kKillSoundEvents)
    {
        const bool alreadyRegistered = m_KillSoundEventManager->FindListener(m_KillSoundEventListener, eventName);
        const bool registered = alreadyRegistered || m_KillSoundEventManager->AddListener(m_KillSoundEventListener, eventName, false);
        if (!registered)
            Game::logMsg("[VR][KillSound][listener] failed to register event=%s", eventName);
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
    const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
    if (localPlayerIndex <= 0)
        return;

    if (eventName == "player_hurt" || eventName == "infected_hurt")
    {
        const int attackerUserId = event->GetInt("attacker", 0);
        int attackerIndex = 0;
        if (attackerUserId > 0)
            attackerIndex = m_Game->m_EngineClient->GetPlayerForUserID(attackerUserId);
        if (attackerIndex <= 0)
            attackerIndex = event->GetInt("attackerentid", 0);

        const bool attackerMatchesLocalUser = localUserId > 0 && attackerUserId == localUserId;
        const bool attackerMatchesLocalIndex = attackerIndex > 0 && attackerIndex == localPlayerIndex;

        const auto now = std::chrono::steady_clock::now();
        const std::uintptr_t entityTag = ResolveKillEventEntityTag(m_Game, event, eventName);
        Vector impactPos{};
        bool matchedImpact = false;
        if (entityTag != 0)
            matchedImpact = FindPendingKillSoundHit(entityTag, now, &impactPos);
        if (!matchedImpact)
            matchedImpact = FindPendingKillSoundHit(0, now, &impactPos);

        const bool attackerMatchesLocal = attackerMatchesLocalUser || attackerMatchesLocalIndex;
        const bool recentShotWindowActive =
            m_LastPredictedHitFeedbackShotTime.time_since_epoch().count() != 0
            && std::chrono::duration<float>(now - m_LastPredictedHitFeedbackShotTime).count() <= 0.35f;

        // Hurt events alone are not a reliable hit-confirm source here:
        // they can arrive without a usable impact match, or be unrelated to the
        // current shot (DOT / lingering damage / other local-side quirks). Only
        // emit hit feedback when the event can still be tied back to a recent
        // predicted shot impact.
        if (!attackerMatchesLocal || !matchedImpact || !recentShotWindowActive)
            return;

        if (m_HitSoundEnabled)
            QueueHitSoundPlayback(&impactPos);
        if (m_HitIndicatorEnabled)
            SpawnHitIndicator(impactPos);
        return;
    }

    int attackerUserId = 0;
    int attackerIndex = 0;
    bool headshot = false;
    if (eventName == "player_death")
    {
        attackerUserId = event->GetInt("attacker", 0);
        if (attackerUserId > 0)
            attackerIndex = m_Game->m_EngineClient->GetPlayerForUserID(attackerUserId);
        if (attackerIndex <= 0)
            attackerIndex = event->GetInt("attackerentid", 0);
        headshot = event->GetBool("headshot", false);
    }
    else if (eventName == "infected_death")
    {
        attackerUserId = event->GetInt("attacker", 0);
        if (attackerUserId > 0)
            attackerIndex = m_Game->m_EngineClient->GetPlayerForUserID(attackerUserId);
        if (attackerIndex <= 0)
            attackerIndex = event->GetInt("attackerentid", 0);
        headshot = event->GetBool("headshot", false);
    }
    else if (eventName == "witch_killed")
    {
        attackerUserId = event->GetInt("userid", 0);
        if (attackerUserId > 0)
            attackerIndex = m_Game->m_EngineClient->GetPlayerForUserID(attackerUserId);
        headshot = false;
    }
    else
    {
        return;
    }

    const bool attackerMatchesLocalUser = localUserId > 0 && attackerUserId == localUserId;
    const bool attackerMatchesLocalIndex = attackerIndex > 0 && attackerIndex == localPlayerIndex;
    const std::uintptr_t entityTag = ResolveKillEventEntityTag(m_Game, event, eventName);

    if (!attackerMatchesLocalUser && !attackerMatchesLocalIndex)
    {
        const auto now = std::chrono::steady_clock::now();
        bool matchedImpact = false;
        if (entityTag != 0)
            matchedImpact = FindPendingKillSoundHit(entityTag, now, nullptr);
        if (!matchedImpact)
            matchedImpact = FindPendingKillSoundHit(0, now, nullptr);
        if (!matchedImpact)
            return;
    }

    QueuePendingKillSoundEvent(entityTag, headshot);
}

void VR::QueuePendingKillSoundEvent(std::uintptr_t entityTag, bool headshot)
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
    m_PendingKillSoundEvents.push_back(std::move(pendingEvent));
    if (m_PendingKillSoundEvents.size() > 32)
        m_PendingKillSoundEvents.erase(m_PendingKillSoundEvents.begin(), m_PendingKillSoundEvents.begin() + (m_PendingKillSoundEvents.size() - 32));
}

bool VR::ConsumePendingKillSoundEvent(std::chrono::steady_clock::time_point now, bool& outHeadshot, std::uintptr_t& outEntityTag)
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

void VR::BeginPredictedHitFeedbackShot()
{
    const auto now = std::chrono::steady_clock::now();
    ++m_PredictedHitFeedbackShotSerial;
    if (m_PredictedHitFeedbackShotSerial == 0)
        m_PredictedHitFeedbackShotSerial = 1;
    m_LastPredictedHitFeedbackShotTime = now;
}

void VR::RegisterPotentialKillSoundHit(const Vector& start, const QAngle& angles)
{
    const bool wantsHitFeedback = m_HitSoundEnabled || m_HitIndicatorEnabled;
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

    const auto now = std::chrono::steady_clock::now();
    bool triggerDirectHitFeedback = true;
    if (m_PredictedHitFeedbackDedupWindowSeconds > 0.0f)
    {
        const auto elapsed = std::chrono::duration<float>(now - m_LastPredictedHitFeedbackTime).count();
        if (m_LastPredictedHitFeedbackTime.time_since_epoch().count() != 0
            && elapsed >= 0.0f
            && elapsed <= m_PredictedHitFeedbackDedupWindowSeconds)
        {
            const Vector deltaStart = start - m_LastPredictedHitFeedbackStart;
            const float startDistSq = deltaStart.LengthSqr();
            const float aimDot = DotProduct(forward, m_LastPredictedHitFeedbackDir);
            if (startDistSq <= 16.0f && aimDot >= 0.9995f)
                triggerDirectHitFeedback = false;
        }
    }

    if (triggerDirectHitFeedback)
    {
        const uint32_t shotSerial = m_PredictedHitFeedbackShotSerial;
        const bool canUseShotGate = shotSerial != 0
            && m_LastPredictedHitFeedbackShotTime.time_since_epoch().count() != 0
            && std::chrono::duration<float>(now - m_LastPredictedHitFeedbackShotTime).count() <= 0.35f;

        if (m_HitSoundEnabled)
        {
            const bool shouldQueueHitSound = !canUseShotGate || m_LastPredictedHitSoundShotSerial != shotSerial;
            if (shouldQueueHitSound)
            {
                QueueHitSoundPlayback(&trace.endpos);
                if (canUseShotGate)
                    m_LastPredictedHitSoundShotSerial = shotSerial;
            }
        }
        if (m_HitIndicatorEnabled)
            SpawnHitIndicator(trace.endpos);

        m_LastPredictedHitFeedbackStart = start;
        m_LastPredictedHitFeedbackDir = forward;
        m_LastPredictedHitFeedbackEntityTag = reinterpret_cast<std::uintptr_t>(entity);
        m_LastPredictedHitFeedbackTime = now;
    }

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

bool VR::FindPendingKillSoundHit(std::uintptr_t preferredEntityTag, std::chrono::steady_clock::time_point now, Vector* outImpactPos)
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

    auto matchesTag = [&](const PendingKillSoundHit& hit)
        {
            return preferredEntityTag == 0 || hit.entityTag == preferredEntityTag;
        };

    for (auto it = m_PendingKillSoundHits.rbegin(); it != m_PendingKillSoundHits.rend(); ++it)
    {
        if (!matchesTag(*it))
            continue;

        if (outImpactPos)
            *outImpactPos = it->impactPos;
        return true;
    }

    return false;
}

bool VR::TryPlayKillSoundSpec(const std::string& rawSpec, float baseVolume, const Vector* worldPos, bool preferLoadedPathReuse)
{
    const std::string spec = TrimCopy(rawSpec);
    if (spec.empty())
        return false;

    auto computeStereoVolumes = [&](int& leftVolume, int& rightVolume)
        {
            ComputeFeedbackSoundStereoVolumes(worldPos, baseVolume, leftVolume, rightVolume);
            if (!m_FeedbackSoundDebugLog)
                return;
            if (ShouldThrottle(m_LastFeedbackSoundDebugLogTime, m_FeedbackSoundDebugLogHz))
                return;

            if (worldPos)
            {
                Game::logMsg(
                    "[VR][FeedbackSound] mode=%s spec=%s base=%.3f blend=%.3f L=%d R=%d src=(%.1f %.1f %.1f) hmd=(%.1f %.1f %.1f) right=(%.3f %.3f %.3f)",
                    DescribeFeedbackSoundDebugForceChannel(m_FeedbackSoundDebugForceChannel),
                    spec.c_str(),
                    baseVolume,
                    m_FeedbackSoundSpatialBlend,
                    leftVolume,
                    rightVolume,
                    worldPos->x,
                    worldPos->y,
                    worldPos->z,
                    m_HmdPosAbs.x,
                    m_HmdPosAbs.y,
                    m_HmdPosAbs.z,
                    m_HmdRight.x,
                    m_HmdRight.y,
                    m_HmdRight.z);
                return;
            }

            Game::logMsg(
                "[VR][FeedbackSound] mode=%s spec=%s base=%.3f blend=%.3f L=%d R=%d src=(none) hmd=(%.1f %.1f %.1f) right=(%.3f %.3f %.3f)",
                DescribeFeedbackSoundDebugForceChannel(m_FeedbackSoundDebugForceChannel),
                spec.c_str(),
                baseVolume,
                m_FeedbackSoundSpatialBlend,
                leftVolume,
                rightVolume,
                m_HmdPosAbs.x,
                m_HmdPosAbs.y,
                m_HmdPosAbs.z,
                m_HmdRight.x,
                m_HmdRight.y,
                m_HmdRight.z);
        };

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
        std::string resolvedPath;
        if (!TryResolveFeedbackSoundFileSpec(spec, resolvedPath))
            return false;

        int leftVolume = 1000;
        int rightVolume = 1000;
        computeStereoVolumes(leftVolume, rightVolume);
        return EnqueueFeedbackSoundPlayback(resolvedPath, leftVolume, rightVolume, preferLoadedPathReuse);
    }

    if (StartsWithInsensitive(spec, "game:"))
    {
        const std::string soundPath = getPayload(5);
        if (soundPath.empty())
            return false;

        const std::string resolvedPath = ResolveGameSoundFilePath(soundPath);
        if (!resolvedPath.empty())
        {
            int leftVolume = 1000;
            int rightVolume = 1000;
            computeStereoVolumes(leftVolume, rightVolume);
            if (EnqueueFeedbackSoundPlayback(resolvedPath, leftVolume, rightVolume, false))
                return true;
        }

        if (!m_Game)
            return false;

        const std::string cmd = "play " + soundPath;
        m_Game->ClientCmd_Unrestricted(cmd.c_str());
        return true;
    }

    if (StartsWithInsensitive(spec, "gamesound:"))
    {
        const std::string soundName = getPayload(10);
        if (soundName.empty())
            return false;

        // Route the built-in VR marker sounds through the local voice pool so repeated hits can overlap.
        const std::string resolvedPath = ResolveBuiltinFeedbackGameSoundPath(soundName);
        if (!resolvedPath.empty())
        {
            int leftVolume = 1000;
            int rightVolume = 1000;
            computeStereoVolumes(leftVolume, rightVolume);
            if (EnqueueFeedbackSoundPlayback(resolvedPath, leftVolume, rightVolume, false))
                return true;
        }

        if (!m_Game)
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
        std::string resolvedPath;
        if (!TryResolveFeedbackSoundFileSpec(spec, resolvedPath))
            return false;

        int leftVolume = 1000;
        int rightVolume = 1000;
        computeStereoVolumes(leftVolume, rightVolume);
        return EnqueueFeedbackSoundPlayback(resolvedPath, leftVolume, rightVolume, preferLoadedPathReuse);
    }

    return ::PlaySoundA(spec.c_str(), nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT) != FALSE;
}

void VR::EnsureFeedbackSoundWorkerThread()
{
    bool expected = false;
    if (!m_FeedbackSoundWorkerStarted.compare_exchange_strong(expected, true))
        return;

    try
    {
        std::thread worker(&VR::FeedbackSoundWorkerMain, this);
        worker.detach();
    }
    catch (const std::system_error&)
    {
        m_FeedbackSoundWorkerStarted.store(false);
        Game::logMsg("[VR][FeedbackSound] failed to start worker thread");
    }
}

bool VR::EnqueueFeedbackSoundPlayback(const std::string& resolvedPath, int leftVolume, int rightVolume, bool preferLoadedPathReuse)
{
    if (resolvedPath.empty())
        return false;
    if (leftVolume <= 0 && rightVolume <= 0)
        return true;

    EnsureFeedbackSoundWorkerThread();
    if (!m_FeedbackSoundWorkerStarted.load())
        return false;

    FeedbackSoundWorkerJob job{};
    job.type = FeedbackSoundWorkerJob::Type::PlayFile;
    job.resolvedPath = resolvedPath;
    job.leftVolume = std::clamp(leftVolume, 0, 1000);
    job.rightVolume = std::clamp(rightVolume, 0, 1000);
    job.preferLoadedPathReuse = preferLoadedPathReuse;

    {
        std::lock_guard<std::mutex> lock(m_FeedbackSoundWorkerMutex);
        if (m_FeedbackSoundWorkerJobs.size() >= kFeedbackSoundWorkerMaxQueuedJobs)
        {
            auto warmupIt = std::find_if(
                m_FeedbackSoundWorkerJobs.begin(),
                m_FeedbackSoundWorkerJobs.end(),
                [](const FeedbackSoundWorkerJob& queuedJob)
                {
                    return queuedJob.type == FeedbackSoundWorkerJob::Type::WarmupFile;
                });
            if (warmupIt != m_FeedbackSoundWorkerJobs.end())
                m_FeedbackSoundWorkerJobs.erase(warmupIt);
            else
                m_FeedbackSoundWorkerJobs.pop_front();
        }

        m_FeedbackSoundWorkerJobs.push_back(std::move(job));
    }

    m_FeedbackSoundWorkerCv.notify_one();
    return true;
}

void VR::EnqueueFeedbackSoundWarmupPath(const std::string& resolvedPath)
{
    if (resolvedPath.empty())
        return;

    EnsureFeedbackSoundWorkerThread();
    if (!m_FeedbackSoundWorkerStarted.load())
        return;

    FeedbackSoundWorkerJob job{};
    job.type = FeedbackSoundWorkerJob::Type::WarmupFile;
    job.resolvedPath = resolvedPath;

    {
        std::lock_guard<std::mutex> lock(m_FeedbackSoundWorkerMutex);
        const auto duplicateIt = std::find_if(
            m_FeedbackSoundWorkerJobs.begin(),
            m_FeedbackSoundWorkerJobs.end(),
            [&](const FeedbackSoundWorkerJob& queuedJob)
            {
                return queuedJob.type == FeedbackSoundWorkerJob::Type::WarmupFile
                    && IsSameFeedbackSoundPath(queuedJob.resolvedPath, resolvedPath);
            });
        if (duplicateIt != m_FeedbackSoundWorkerJobs.end())
            return;

        if (m_FeedbackSoundWorkerJobs.size() >= kFeedbackSoundWorkerMaxQueuedJobs)
        {
            auto warmupIt = std::find_if(
                m_FeedbackSoundWorkerJobs.begin(),
                m_FeedbackSoundWorkerJobs.end(),
                [](const FeedbackSoundWorkerJob& queuedJob)
                {
                    return queuedJob.type == FeedbackSoundWorkerJob::Type::WarmupFile;
                });
            if (warmupIt != m_FeedbackSoundWorkerJobs.end())
                m_FeedbackSoundWorkerJobs.erase(warmupIt);
            else
                m_FeedbackSoundWorkerJobs.pop_front();
        }

        m_FeedbackSoundWorkerJobs.push_back(std::move(job));
    }

    m_FeedbackSoundWorkerCv.notify_one();
}

void VR::ResetFeedbackSoundWorkerState()
{
    if (!m_FeedbackSoundWorkerStarted.load())
        return;

    FeedbackSoundWorkerJob job{};
    job.type = FeedbackSoundWorkerJob::Type::ResetState;

    {
        std::lock_guard<std::mutex> lock(m_FeedbackSoundWorkerMutex);
        m_FeedbackSoundWorkerJobs.clear();
        m_FeedbackSoundWorkerJobs.push_back(std::move(job));
    }

    m_FeedbackSoundWorkerCv.notify_one();
}

void VR::FeedbackSoundWorkerMain()
{
    for (;;)
    {
        FeedbackSoundWorkerJob job{};
        {
            std::unique_lock<std::mutex> lock(m_FeedbackSoundWorkerMutex);
            m_FeedbackSoundWorkerCv.wait(lock, [&]()
                {
                    return !m_FeedbackSoundWorkerJobs.empty();
                });

            job = std::move(m_FeedbackSoundWorkerJobs.front());
            m_FeedbackSoundWorkerJobs.pop_front();
        }

        switch (job.type)
        {
        case FeedbackSoundWorkerJob::Type::PlayFile:
            if (!job.resolvedPath.empty())
                TryPlayFeedbackSoundFilePath(job.resolvedPath, job.leftVolume, job.rightVolume, job.preferLoadedPathReuse);
            break;
        case FeedbackSoundWorkerJob::Type::WarmupFile:
            if (!job.resolvedPath.empty())
            {
                FeedbackSoundVoiceState& voice = AcquireFeedbackSoundVoice(&job.resolvedPath);
                EnsureFeedbackSoundVoiceOpen(voice, job.resolvedPath);
            }
            break;
        case FeedbackSoundWorkerJob::Type::ResetState:
            CloseAllFeedbackSoundVoices();
            break;
        }
    }
}

void VR::ComputeFeedbackSoundStereoVolumes(const Vector* worldPos, float baseVolume, int& outLeftVolume, int& outRightVolume) const
{
    const float gameMasterVolume = ReadGameMasterVolumeFromConfig(m_Game ? m_Game->m_EngineClient : nullptr);
    const float clampedBaseVolume = std::clamp(baseVolume * std::clamp(gameMasterVolume, 0.0f, 1.0f), 0.0f, 2.0f);
    float leftGain = clampedBaseVolume;
    float rightGain = clampedBaseVolume;

    if (worldPos && m_FeedbackSoundSpatialBlend > 0.0f)
    {
        Vector listenerForward{};
        Vector listenerRight{};
        Vector listenerUp{};
        Vector listenerOrigin = m_SetupOrigin;
        if (!m_HmdForward.IsZero() && !m_HmdRight.IsZero() && !m_HmdUp.IsZero())
        {
            listenerForward = m_HmdForward;
            listenerRight = m_HmdRight;
            listenerUp = m_HmdUp;
        }
        else
        {
            QAngle::AngleVectors(m_SetupAngles, &listenerForward, &listenerRight, &listenerUp);
        }

        if (!listenerForward.IsZero() && !listenerRight.IsZero())
        {
            VectorNormalize(listenerForward);
            VectorNormalize(listenerRight);
            if (!m_HmdForward.IsZero() && !m_HmdRight.IsZero() && !m_HmdUp.IsZero())
                listenerOrigin = m_HmdPosAbs;

            // Feedback sounds should follow the player's actual head pose, not the render camera basis.
            const Vector delta = *worldPos - listenerOrigin;
            const float distance = delta.Length();
            if (distance > 0.001f)
            {
                Vector dir = delta;
                VectorNormalize(dir);

                const float spatialBlend = Clamp01(m_FeedbackSoundSpatialBlend);
                const float pan = std::clamp(-DotProduct(dir, listenerRight), -1.0f, 1.0f) * spatialBlend;
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

    if (m_FeedbackSoundDebugForceChannel != 0)
    {
        const int dominantVolume = (std::max)(outLeftVolume, outRightVolume);
        if (m_FeedbackSoundDebugForceChannel < 0)
        {
            outLeftVolume = dominantVolume;
            outRightVolume = 0;
        }
        else
        {
            outLeftVolume = 0;
            outRightVolume = dominantVolume;
        }
    }
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

    bool played = TryPlayKillSoundSpec(m_HitSoundSpec, m_HitSoundVolume, worldPos, false);
    if (!played)
    {
        EnsureFeedbackSoundWarmup();
        played = TryPlayKillSoundSpec(m_HitSoundSpec, m_HitSoundVolume, worldPos, false);
    }
    if (!played)
        MessageBeep(MB_ICONQUESTION);

    m_LastHitSoundPlaybackTime = now;
}

void VR::QueueHitSoundPlayback(const Vector* worldPos)
{
    const auto now = std::chrono::steady_clock::now();
    if (!m_HitSoundPending)
    {
        m_HitSoundPending = true;
        ++m_HitSoundStatsQueued;
        m_HitSoundPendingMergedCount = 1;
        m_HitSoundPendingQueuedAt = now;
        m_HitSoundPendingWorldPos = worldPos ? *worldPos : Vector{};
        return;
    }

    ++m_HitSoundStatsMerged;
    ++m_HitSoundPendingMergedCount;
    if (worldPos)
        m_HitSoundPendingWorldPos = *worldPos;
}

void VR::FlushPendingHitSound(std::chrono::steady_clock::time_point now)
{
    if (!m_HitSoundPending || !m_HitSoundEnabled)
        return;

    if (m_HitSoundPlaybackCooldownSeconds > 0.0f && m_LastHitSoundPlaybackTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastHitSoundPlaybackTime).count();
        if (elapsed < m_HitSoundPlaybackCooldownSeconds)
            return;
    }

    const Vector* worldPos = m_HitSoundPendingWorldPos.IsZero() ? nullptr : &m_HitSoundPendingWorldPos;
    PlayHitSound(worldPos);
    ++m_HitSoundStatsFlushed;
    m_HitSoundPending = false;
    m_HitSoundPendingMergedCount = 0;
    m_HitSoundPendingWorldPos = Vector{};
    m_HitSoundPendingQueuedAt = {};
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

    auto tryPlayConfiguredKillSounds = [&]() -> bool
        {
            bool result = TryPlayKillSoundSpec(preferredSpec, preferredVolume, worldPos);
            if (!result && headshot && !m_KillSoundNormalSpec.empty())
                result = TryPlayKillSoundSpec(m_KillSoundNormalSpec, m_KillSoundVolume, worldPos);
            return result;
        };

    bool played = tryPlayConfiguredKillSounds();
    if (!played)
    {
        EnsureFeedbackSoundWarmup();
        played = tryPlayConfiguredKillSounds();
    }

    if (!played)
        MessageBeep(headshot ? MB_ICONEXCLAMATION : MB_ICONASTERISK);

    m_LastKillSoundPlaybackTime = now;
}

void VR::EnsureFeedbackSoundWarmup()
{
    const std::string signature = m_HitSoundSpec + "\n" + m_KillSoundNormalSpec + "\n" + m_KillSoundHeadshotSpec;
    if (signature == m_FeedbackSoundWarmupSignature)
        return;

    m_FeedbackSoundWarmupSignature = signature;

    const std::array<std::string, 3> specs =
    {
        m_HitSoundEnabled ? m_HitSoundSpec : std::string{},
        m_KillSoundEnabled ? m_KillSoundNormalSpec : std::string{},
        m_KillSoundEnabled ? m_KillSoundHeadshotSpec : std::string{}
    };

    std::vector<std::string> warmedPaths;
    warmedPaths.reserve(specs.size());

    for (const std::string& spec : specs)
    {
        std::string resolvedPath;
        if (!TryResolveFeedbackSoundFileSpec(spec, resolvedPath))
            continue;

        bool alreadyWarmed = false;
        for (const std::string& existingPath : warmedPaths)
        {
            if (IsSameFeedbackSoundPath(existingPath, resolvedPath))
            {
                alreadyWarmed = true;
                break;
            }
        }
        if (alreadyWarmed)
            continue;

        EnqueueFeedbackSoundWarmupPath(resolvedPath);
        warmedPaths.push_back(resolvedPath);
    }
}

void VR::SyncVrmodFeedbackGameSounds() const
{
    const std::string moduleDir = GetModuleDirectoryA();
    if (moduleDir.empty())
        return;

    const std::string scriptsDir = JoinWindowsPath(JoinWindowsPath(moduleDir, "left4dead2"), "scripts");
    ::CreateDirectoryA(scriptsDir.c_str(), nullptr);

    const std::string scriptPath = JoinWindowsPath(scriptsDir, "game_sounds_vrmod.txt");
    std::ostringstream script;
    script
        << "\"VR_HitMarker\"\r\n"
        << "{\r\n"
        << "\t\"channel\"\t\t\"CHAN_AUTO\"\r\n"
        << "\t\"volume\"\t\t\"" << FormatFeedbackSoundVolume(m_HitSoundVolume) << "\"\r\n"
        << "\t\"soundlevel\"\t\t\"SNDLVL_NONE\"\r\n"
        << "\t\"pitch\"\t\t\t\"100\"\r\n"
        << "\t\"wave\"\t\t\t\"vrmod/hit.mp3\"\r\n"
        << "}\r\n\r\n"
        << "\"VR_KillMarker\"\r\n"
        << "{\r\n"
        << "\t\"channel\"\t\t\"CHAN_AUTO\"\r\n"
        << "\t\"volume\"\t\t\"" << FormatFeedbackSoundVolume(m_KillSoundVolume) << "\"\r\n"
        << "\t\"soundlevel\"\t\t\"SNDLVL_NONE\"\r\n"
        << "\t\"pitch\"\t\t\t\"100\"\r\n"
        << "\t\"wave\"\t\t\t\"vrmod/kill.mp3\"\r\n"
        << "}\r\n\r\n"
        << "\"VR_HeadshotMarker\"\r\n"
        << "{\r\n"
        << "\t\"channel\"\t\t\"CHAN_AUTO\"\r\n"
        << "\t\"volume\"\t\t\"" << FormatFeedbackSoundVolume(m_HeadshotSoundVolume) << "\"\r\n"
        << "\t\"soundlevel\"\t\t\"SNDLVL_NONE\"\r\n"
        << "\t\"pitch\"\t\t\t\"100\"\r\n"
        << "\t\"wave\"\t\t\t\"vrmod/headshot.mp3\"\r\n"
        << "}\r\n";

    if (!WriteWholeTextFileIfChanged(scriptPath, script.str()) && m_Game)
        Game::logMsg("[VR][FeedbackSound] failed to write soundscript path=%s", scriptPath.c_str());
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

void VR::DestroyHandHudWorldQuadTextures()
{
    auto SafeReleaseD3D = [](auto*& ptr)
        {
            if (!ptr)
                return;
            ptr->Release();
            ptr = nullptr;
        };

    SafeReleaseD3D(m_D9LeftWristHudDynSurface);
    SafeReleaseD3D(m_D9LeftWristHudDynTex);
    SafeReleaseD3D(m_D9RightAmmoHudDynSurface);
    SafeReleaseD3D(m_D9RightAmmoHudDynTex);
    m_D9LeftWristHudDynW = m_D9LeftWristHudDynH = 0;
    m_D9RightAmmoHudDynW = m_D9RightAmmoHudDynH = 0;
    std::memset(&m_VKLeftWristHudDyn, 0, sizeof(m_VKLeftWristHudDyn));
    std::memset(&m_VKRightAmmoHudDyn, 0, sizeof(m_VKRightAmmoHudDyn));
}

void VR::DestroyKillIndicatorOverlayTextures()
{
    for (int materialIndex = 0; materialIndex < static_cast<int>(m_KillIndicatorOverlayTextures.size()); ++materialIndex)
        DestroyKillIndicatorOverlayTexture(materialIndex);
}

void VR::DestroyKillIndicatorOverlayTexture(int materialIndex)
{
    if (materialIndex < 0 || materialIndex >= static_cast<int>(m_KillIndicatorOverlayTextures.size()))
        return;

    auto SafeReleaseD3D = [](auto*& ptr)
        {
            if (!ptr)
                return;
            ptr->Release();
            ptr = nullptr;
        };

    KillIndicatorOverlayTexture& texture = m_KillIndicatorOverlayTextures[materialIndex];
    SafeReleaseD3D(texture.d3dSurface);
    SafeReleaseD3D(texture.d3dTexture);
    texture.width = 0;
    texture.height = 0;
    std::memset(&texture.sharedTexture, 0, sizeof(texture.sharedTexture));
    texture.uploadedFrameIndex = UINT32_MAX;
    texture.uploadedFromDecodedFrames = false;

    for (KillIndicatorOverlaySlot& slot : m_KillIndicatorOverlaySlots)
    {
        if (slot.materialIndex == materialIndex)
        {
            slot.materialIndex = -1;
            slot.visible = false;
        }
    }
}

bool VR::EnsureKillIndicatorOverlayTexture(int materialIndex, int width, int height)
{
    if (materialIndex < 0 || materialIndex >= static_cast<int>(m_KillIndicatorOverlayTextures.size()) || width <= 0 || height <= 0)
        return false;
    if (!g_D3DVR9)
        return false;

    KillIndicatorOverlayTexture& texture = m_KillIndicatorOverlayTextures[materialIndex];
    if (texture.d3dTexture && (texture.width != width || texture.height != height))
        DestroyKillIndicatorOverlayTexture(materialIndex);
    if (texture.d3dTexture && texture.d3dSurface)
        return true;

    IDirect3DDevice9* device = GetKillIndicatorD3DDevice(this);
    if (!device)
        return false;

    g_D3DVR9->LockDevice();

    HRESULT hr = device->CreateTexture(
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        1,
        D3DUSAGE_DYNAMIC,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &texture.d3dTexture,
        nullptr);

    if (SUCCEEDED(hr) && texture.d3dTexture)
    {
        texture.d3dTexture->GetSurfaceLevel(0, &texture.d3dSurface);
        if (texture.d3dSurface)
        {
            D3D9_TEXTURE_VR_DESC desc{};
            if (SUCCEEDED(g_D3DVR9->GetVRDesc(texture.d3dSurface, &desc)))
            {
                std::memcpy(&texture.sharedTexture.m_VulkanData, &desc, sizeof(vr::VRVulkanTextureData_t));
                texture.sharedTexture.m_VRTexture.handle = &texture.sharedTexture.m_VulkanData;
                texture.sharedTexture.m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
                texture.sharedTexture.m_VRTexture.eType = vr::TextureType_Vulkan;
                texture.width = width;
                texture.height = height;
            }
            else
            {
                DestroyKillIndicatorOverlayTexture(materialIndex);
            }
        }
        else
        {
            DestroyKillIndicatorOverlayTexture(materialIndex);
        }
    }

    g_D3DVR9->UnlockDevice();
    device->Release();

    return texture.d3dTexture != nullptr && texture.d3dSurface != nullptr;
}

bool VR::UploadKillIndicatorOverlayTexture(int materialIndex, const uint8_t* rgba, int width, int height, uint32_t frameIndex, bool fromDecodedFrames)
{
    if (!rgba || width <= 0 || height <= 0)
        return false;
    if (!EnsureKillIndicatorOverlayTexture(materialIndex, width, height))
        return false;
    if (!g_D3DVR9)
        return false;

    KillIndicatorOverlayTexture& texture = m_KillIndicatorOverlayTextures[materialIndex];
    if (!texture.d3dTexture || !texture.d3dSurface)
        return false;

    g_D3DVR9->LockDevice();

    D3DLOCKED_RECT lockedRect{};
    const HRESULT hr = texture.d3dTexture->LockRect(0, &lockedRect, nullptr, D3DLOCK_DISCARD);
    if (FAILED(hr) || !lockedRect.pBits)
    {
        g_D3DVR9->UnlockDevice();
        return false;
    }

    uint8_t* dst0 = reinterpret_cast<uint8_t*>(lockedRect.pBits);
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* srcRow = rgba + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
        uint8_t* dstRow = dst0 + static_cast<size_t>(y) * static_cast<size_t>(lockedRect.Pitch);
        for (int x = 0; x < width; ++x)
        {
            const uint8_t r = srcRow[x * 4 + 0];
            const uint8_t g = srcRow[x * 4 + 1];
            const uint8_t b = srcRow[x * 4 + 2];
            const uint8_t a = srcRow[x * 4 + 3];
            dstRow[x * 4 + 0] = b;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = r;
            dstRow[x * 4 + 3] = a;
        }
    }

    texture.d3dTexture->UnlockRect(0);
    const HRESULT transferHr = g_D3DVR9->TransferSurface(texture.d3dSurface, FALSE);
    g_D3DVR9->UnlockDevice();
    if (FAILED(transferHr))
    {
        DestroyKillIndicatorOverlayTexture(materialIndex);
        return false;
    }
    texture.uploadedFrameIndex = frameIndex;
    texture.uploadedFromDecodedFrames = fromDecodedFrames;
    return true;
}

void VR::DestroyKillIndicatorOverlay(ActiveKillIndicator& indicator)
{
    if (indicator.overlaySlot < 0 || indicator.overlaySlot >= static_cast<int>(m_KillIndicatorOverlaySlots.size()))
    {
        indicator.overlaySlot = -1;
        return;
    }

    KillIndicatorOverlaySlot& slot = m_KillIndicatorOverlaySlots[indicator.overlaySlot];
    if (slot.overlayHandle != vr::k_ulOverlayHandleInvalid)
    {
        vr::IVROverlay* overlay = m_Overlay ? m_Overlay : vr::VROverlay();
        if (overlay)
        {
            std::lock_guard<std::mutex> lock(m_VROverlayMutex);
            overlay->HideOverlay(slot.overlayHandle);
        }
    }

    slot.visible = false;
    slot.materialIndex = -1;
    indicator.overlaySlot = -1;
}

bool VR::EnsureKillIndicatorOverlaySlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= static_cast<int>(m_KillIndicatorOverlaySlots.size()))
        return false;

    KillIndicatorOverlaySlot& slot = m_KillIndicatorOverlaySlots[slotIndex];
    if (slot.overlayHandle != vr::k_ulOverlayHandleInvalid)
        return true;

    vr::IVROverlay* overlay = m_Overlay ? m_Overlay : vr::VROverlay();
    if (!overlay)
        return false;

    const std::string key = "KillIndicatorOverlayKey_" + std::to_string(m_NextKillIndicatorOverlaySerial++);
    const std::string name = "KillIndicatorOverlay_" + std::to_string(slotIndex);

    std::lock_guard<std::mutex> lock(m_VROverlayMutex);
    const vr::EVROverlayError createError = overlay->CreateOverlay(key.c_str(), name.c_str(), &slot.overlayHandle);
    if (createError != vr::VROverlayError_None)
    {
        slot.overlayHandle = vr::k_ulOverlayHandleInvalid;
        Game::logMsg("[VR][KillIndicator] CreateOverlay failed err=%d key=%s", (int)createError, key.c_str());
        return false;
    }

    static const vr::VRTextureBounds_t fullTextureBounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    overlay->SetOverlayTexelAspect(slot.overlayHandle, 1.0f);
    overlay->SetOverlayFlag(slot.overlayHandle, vr::VROverlayFlags_IgnoreTextureAlpha, false);
    overlay->SetOverlayTextureBounds(slot.overlayHandle, &fullTextureBounds);
    slot.materialIndex = -1;
    slot.visible = false;
    return true;
}

int VR::AcquireKillIndicatorOverlaySlot() const
{
    std::array<bool, 16> used{};
    for (const ActiveKillIndicator& active : m_ActiveKillIndicators)
    {
        if (active.overlaySlot >= 0 && active.overlaySlot < static_cast<int>(used.size()))
            used[active.overlaySlot] = true;
    }

    for (int slotIndex = 0; slotIndex < static_cast<int>(used.size()); ++slotIndex)
    {
        if (!used[slotIndex])
            return slotIndex;
    }

    return -1;
}

void VR::TrimExpiredKillIndicators(std::chrono::steady_clock::time_point now, bool clearAll)
{
    size_t writeIndex = 0;
    const size_t originalCount = m_ActiveKillIndicators.size();
    for (size_t readIndex = 0; readIndex < originalCount; ++readIndex)
    {
        ActiveKillIndicator& indicator = m_ActiveKillIndicators[readIndex];
        const bool expired = clearAll
            || std::chrono::duration<float>(now - indicator.startedAt).count() >= GetActiveIndicatorLifetimeSeconds(indicator, m_KillIndicatorLifetimeSeconds);
        if (!expired)
        {
            if (writeIndex != readIndex)
                m_ActiveKillIndicators[writeIndex] = std::move(indicator);
            ++writeIndex;
            continue;
        }

        DestroyKillIndicatorOverlay(indicator);
        ++m_KillIndicatorStatsTrimmed;
    }

    if (writeIndex < originalCount)
        m_ActiveKillIndicators.resize(writeIndex);
}

void VR::MaybeTrimExpiredKillIndicators(std::chrono::steady_clock::time_point now, bool force)
{
    if (!force && m_LastKillIndicatorTrimTime.time_since_epoch().count() != 0)
    {
        const float elapsed = std::chrono::duration<float>(now - m_LastKillIndicatorTrimTime).count();
        if (elapsed < kKillIndicatorTrimIntervalSeconds)
            return;
    }

    TrimExpiredKillIndicators(now, false);
    m_LastKillIndicatorTrimTime = now;
}

void VR::MaybeLogKillIndicatorStats(std::chrono::steady_clock::time_point now)
{
    if (!m_KillIndicatorDebugLog)
        return;

    if (ShouldThrottle(m_LastKillIndicatorDebugLogTime, m_KillIndicatorDebugLogHz))
        return;

    Game::logMsg(
        "[VR][KillIndicator][stats] active=%zu peak=%u hit_spawned=%u kill_spawned=%u hit_merged=%u recycled=%u trimmed=%u hit_sound_queued=%u hit_sound_merged=%u hit_sound_flushed=%u",
        m_ActiveKillIndicators.size(),
        m_KillIndicatorStatsPeakActive,
        m_KillIndicatorStatsHitSpawned,
        m_KillIndicatorStatsKillSpawned,
        m_KillIndicatorStatsHitMerged,
        m_KillIndicatorStatsRecycled,
        m_KillIndicatorStatsTrimmed,
        m_HitSoundStatsQueued,
        m_HitSoundStatsMerged,
        m_HitSoundStatsFlushed);

    m_KillIndicatorStatsHitSpawned = 0;
    m_KillIndicatorStatsKillSpawned = 0;
    m_KillIndicatorStatsHitMerged = 0;
    m_KillIndicatorStatsRecycled = 0;
    m_KillIndicatorStatsTrimmed = 0;
    m_KillIndicatorStatsPeakActive = static_cast<uint32_t>(m_ActiveKillIndicators.size());
    m_HitSoundStatsQueued = 0;
    m_HitSoundStatsMerged = 0;
    m_HitSoundStatsFlushed = 0;
}

int VR::FindReusableKillIndicatorIndex(bool preferNonKill) const
{
    if (m_ActiveKillIndicators.empty())
        return -1;

    int fallbackIndex = 0;
    auto fallbackStartedAt = m_ActiveKillIndicators[0].startedAt;
    int preferredIndex = -1;
    auto preferredStartedAt = std::chrono::steady_clock::time_point::max();

    for (int i = 0; i < static_cast<int>(m_ActiveKillIndicators.size()); ++i)
    {
        const ActiveKillIndicator& indicator = m_ActiveKillIndicators[i];
        if (indicator.startedAt < fallbackStartedAt)
        {
            fallbackStartedAt = indicator.startedAt;
            fallbackIndex = i;
        }

        if (preferNonKill && indicator.killConfirmed)
            continue;

        if (preferredIndex < 0 || indicator.startedAt < preferredStartedAt)
        {
            preferredStartedAt = indicator.startedAt;
            preferredIndex = i;
        }
    }

    return preferredIndex >= 0 ? preferredIndex : fallbackIndex;
}

void VR::AddOrRecycleKillIndicator(const Vector& worldPos, bool killConfirmed, bool headshot, std::chrono::steady_clock::time_point now, bool preferNonKill)
{
    if (m_ActiveKillIndicators.capacity() < kMaxActiveKillIndicators)
        m_ActiveKillIndicators.reserve(kMaxActiveKillIndicators);

    if (m_ActiveKillIndicators.size() < kMaxActiveKillIndicators)
    {
        ActiveKillIndicator indicator{};
        indicator.worldPos = worldPos;
        indicator.startedAt = now;
        indicator.killConfirmed = killConfirmed;
        indicator.headshot = headshot;
        m_ActiveKillIndicators.push_back(indicator);
        m_KillIndicatorStatsPeakActive = (std::max)(m_KillIndicatorStatsPeakActive, static_cast<uint32_t>(m_ActiveKillIndicators.size()));
        return;
    }

    const int reuseIndex = FindReusableKillIndicatorIndex(preferNonKill);
    if (reuseIndex < 0)
        return;

    ActiveKillIndicator& indicator = m_ActiveKillIndicators[reuseIndex];
    DestroyKillIndicatorOverlay(indicator);
    ++m_KillIndicatorStatsRecycled;
    indicator.worldPos = worldPos;
    indicator.startedAt = now;
    indicator.killConfirmed = killConfirmed;
    indicator.headshot = headshot;
    indicator.overlaySlot = -1;
}

bool VR::BuildKillIndicatorOverlayPixels(IMaterial* material, std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight, uint32_t preferredFrameIndex, bool* outUsedDecodedFrames)
{
    outPixels.clear();
    outWidth = 0;
    outHeight = 0;
    if (outUsedDecodedFrames)
        *outUsedDecodedFrames = false;

    if (!material || material->IsErrorMaterial())
        return false;

    KillIndicatorDecodedFrames& decoded = GetKillIndicatorDecodedFrameCache(material->GetName());
    if (decoded.loaded && !decoded.frames.empty())
    {
        size_t frameIndex = 0;
        if (decoded.frames.size() > 1)
        {
            if (preferredFrameIndex != UINT32_MAX)
            {
                frameIndex = static_cast<size_t>(preferredFrameIndex) % decoded.frames.size();
            }
            else if (decoded.frameRate > 0.01f)
            {
                const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
                frameIndex = static_cast<size_t>(std::floor(nowSeconds * decoded.frameRate)) % decoded.frames.size();
            }
        }

        outPixels = decoded.frames[frameIndex];
        outWidth = decoded.width;
        outHeight = decoded.height;
        if (outUsedDecodedFrames)
            *outUsedDecodedFrames = true;
        return !outPixels.empty();
    }

    int previewWidth = 0;
    int previewHeight = 0;
    ImageFormat previewFormat = IMAGE_FORMAT_RGBA8888;
    bool isTranslucent = false;
    material->Refresh();
    material->GetPreviewImageProperties(&previewWidth, &previewHeight, &previewFormat, &isTranslucent);

    if (previewWidth <= 0 || previewHeight <= 0)
    {
        previewWidth = material->GetMappingWidth();
        previewHeight = material->GetMappingHeight();
    }

    previewWidth = std::clamp(previewWidth, 1, 256);
    previewHeight = std::clamp(previewHeight, 1, 256);

    outPixels.resize(static_cast<size_t>(previewWidth) * static_cast<size_t>(previewHeight) * 4u, 0);
    material->GetPreviewImage(outPixels.data(), previewWidth, previewHeight, IMAGE_FORMAT_RGBA8888);

    outWidth = static_cast<uint32_t>(previewWidth);
    outHeight = static_cast<uint32_t>(previewHeight);
    return !outPixels.empty();
}

bool VR::ComputeKillIndicatorOverlayTransform(const Vector& worldPos, vr::HmdMatrix34_t& outTransform) const
{
    Vector srcRight = m_HmdRight;
    Vector srcUp = m_HmdUp;
    Vector srcForward = m_HmdForward;
    if (VectorNormalize(srcRight) == 0.0f || VectorNormalize(srcUp) == 0.0f || VectorNormalize(srcForward) == 0.0f)
        return false;

    const float unitsPerMeter = (std::max)(1.0f, m_VRScale);
    const Vector deltaSource = worldPos - m_HmdPosAbs;
    const float deltaRightMeters = DotProduct(deltaSource, srcRight) / unitsPerMeter;
    const float deltaUpMeters = DotProduct(deltaSource, srcUp) / unitsPerMeter;
    const float deltaForwardMeters = DotProduct(deltaSource, srcForward) / unitsPerMeter;

    outTransform = {
        1.0f, 0.0f, 0.0f, deltaRightMeters,
        0.0f, 1.0f, 0.0f, deltaUpMeters,
        0.0f, 0.0f, 1.0f, -deltaForwardMeters
    };
    return true;
}

void VR::UpdateKillIndicatorOverlays()
{
    const auto now = std::chrono::steady_clock::now();
    MaybeTrimExpiredKillIndicators(now, true);
    MaybeLogKillIndicatorStats(now);

    if (!m_KillIndicatorEnabled || m_ActiveKillIndicators.empty())
        return;

    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
        return;

    {
        std::lock_guard<std::mutex> lock(m_VROverlayMutex);
        if (vr::IVROverlay* current = vr::VROverlay())
            m_Overlay = current;
    }

    vr::IVROverlay* overlay = m_Overlay ? m_Overlay : vr::VROverlay();
    if (!overlay || !m_Compositor)
        return;

    IMaterial* materials[3] = {
        ResolveHitIndicatorMaterial(),
        ResolveKillIndicatorMaterial(false),
        ResolveKillIndicatorMaterial(true)
    };
    bool textureReady[3] = {};
    const float baseSizePixels = (std::max)(16.0f, m_KillIndicatorSizePixels);

    for (int materialIndex = 0; materialIndex < 3; ++materialIndex)
    {
        IMaterial* material = materials[materialIndex];
        if (!material)
            continue;

        uint32_t desiredFrameIndex = 0;
        bool usesDecodedFrames = false;
        KillIndicatorDecodedFrames& decoded = GetKillIndicatorDecodedFrameCache(material->GetName());
        if (decoded.loaded && !decoded.frames.empty())
        {
            usesDecodedFrames = true;
            if (decoded.frames.size() > 1 && decoded.frameRate > 0.01f)
            {
                const double nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
                desiredFrameIndex = static_cast<uint32_t>(std::floor(nowSeconds * decoded.frameRate)) % static_cast<uint32_t>(decoded.frames.size());
            }
        }

        KillIndicatorOverlayTexture& texture = m_KillIndicatorOverlayTextures[materialIndex];
        const bool needsUpload = texture.sharedTexture.m_VRTexture.handle == nullptr
            || texture.uploadedFrameIndex != desiredFrameIndex
            || texture.uploadedFromDecodedFrames != usesDecodedFrames;
        if (needsUpload)
        {
            std::vector<uint8_t> pixels;
            uint32_t pixelWidth = 0;
            uint32_t pixelHeight = 0;
            bool builtFromDecodedFrames = false;
            if (!BuildKillIndicatorOverlayPixels(material, pixels, pixelWidth, pixelHeight, desiredFrameIndex, &builtFromDecodedFrames))
            {
                Game::logMsg("[VR][KillIndicator] preview build failed material=%s", material->GetName());
                continue;
            }

            if (!UploadKillIndicatorOverlayTexture(materialIndex, pixels.data(), static_cast<int>(pixelWidth), static_cast<int>(pixelHeight), desiredFrameIndex, builtFromDecodedFrames))
            {
                Game::logMsg("[VR][KillIndicator] world texture upload failed material=%s size=%ux%u", material->GetName(), pixelWidth, pixelHeight);
                continue;
            }
        }

        textureReady[materialIndex] = m_KillIndicatorOverlayTextures[materialIndex].sharedTexture.m_VRTexture.handle != nullptr;
    }

    for (ActiveKillIndicator& indicator : m_ActiveKillIndicators)
    {
        const KillIndicatorMaterialKind kind = !indicator.killConfirmed
            ? KillIndicatorMaterialKind::Hit
            : (indicator.headshot ? KillIndicatorMaterialKind::Headshot : KillIndicatorMaterialKind::Kill);
        const int materialIndex = static_cast<int>(kind);
        IMaterial* material = materials[materialIndex];
        if (!material || !textureReady[materialIndex])
            continue;

        if (indicator.overlaySlot < 0)
        {
            indicator.overlaySlot = AcquireKillIndicatorOverlaySlot();
            if (indicator.overlaySlot < 0)
                continue;
        }

        if (!EnsureKillIndicatorOverlaySlot(indicator.overlaySlot))
        {
            indicator.overlaySlot = -1;
            continue;
        }

        KillIndicatorOverlaySlot& slot = m_KillIndicatorOverlaySlots[indicator.overlaySlot];
        if (slot.overlayHandle == vr::k_ulOverlayHandleInvalid)
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
        if ((drawPos - m_HmdPosAbs).Length() > m_KillIndicatorMaxDistance)
        {
            if (slot.visible)
            {
                std::lock_guard<std::mutex> lock(m_VROverlayMutex);
                overlay->HideOverlay(slot.overlayHandle);
                slot.visible = false;
            }
            continue;
        }

        vr::HmdMatrix34_t transform{};
        if (!ComputeKillIndicatorOverlayTransform(drawPos, transform))
            continue;

        float scale = indicator.killConfirmed ? (0.78f + 0.34f * pulse) : (0.56f + 0.24f * pulse);
        if (indicator.killConfirmed && indicator.headshot)
            scale *= 1.10f;

        const float alphaBase = indicator.killConfirmed ? 0.72f : 0.60f;
        const float alpha = Clamp01((alphaBase + (1.0f - alphaBase) * intro) * fade);
        const float widthMeters = std::clamp((baseSizePixels / 640.0f) * scale, 0.10f, 0.45f);

        std::lock_guard<std::mutex> lock(m_VROverlayMutex);
        if (slot.materialIndex != materialIndex)
        {
            const vr::EVROverlayError textureError = overlay->SetOverlayTexture(slot.overlayHandle, &m_KillIndicatorOverlayTextures[materialIndex].sharedTexture.m_VRTexture);
            if (textureError != vr::VROverlayError_None)
            {
                Game::logMsg("[VR][KillIndicator] SetOverlayTexture failed err=%d material=%s", (int)textureError, material->GetName());
                slot.materialIndex = -1;
                slot.visible = false;
                if (textureError == vr::VROverlayError_InvalidHandle)
                    slot.overlayHandle = vr::k_ulOverlayHandleInvalid;
                DestroyKillIndicatorOverlayTexture(materialIndex);
                continue;
            }
            slot.materialIndex = materialIndex;
        }

        overlay->SetOverlayTransformTrackedDeviceRelative(slot.overlayHandle, vr::k_unTrackedDeviceIndex_Hmd, &transform);
        overlay->SetOverlayWidthInMeters(slot.overlayHandle, widthMeters);
        overlay->SetOverlayAlpha(slot.overlayHandle, alpha);
        if (!slot.visible)
        {
            const vr::EVROverlayError showError = overlay->ShowOverlay(slot.overlayHandle);
            if (showError != vr::VROverlayError_None)
            {
                Game::logMsg("[VR][KillIndicator] ShowOverlay failed err=%d material=%s", (int)showError, material->GetName());
                slot.visible = false;
                if (showError == vr::VROverlayError_InvalidHandle)
                    slot.overlayHandle = vr::k_ulOverlayHandleInvalid;
                continue;
            }
            slot.visible = true;
        }
    }
}

void VR::SpawnHitIndicator(const Vector& worldPos)
{
    if (!m_HitIndicatorEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    MaybeTrimExpiredKillIndicators(now, false);
    ++m_KillIndicatorStatsHitSpawned;

    const float mergeDistanceSqr = kHitIndicatorMergeDistance * kHitIndicatorMergeDistance;
    for (ActiveKillIndicator& indicator : m_ActiveKillIndicators)
    {
        if (indicator.killConfirmed)
            continue;

        const float ageSeconds = std::chrono::duration<float>(now - indicator.startedAt).count();
        if (ageSeconds > kHitIndicatorMergeWindowSeconds)
            continue;

        if (indicator.worldPos.DistToSqr(worldPos) > mergeDistanceSqr)
            continue;

        indicator.worldPos = worldPos;
        indicator.startedAt = now;
        ++m_KillIndicatorStatsHitMerged;
        return;
    }

    AddOrRecycleKillIndicator(worldPos, false, false, now, true);
}

void VR::SpawnKillIndicator(bool headshot, const Vector& worldPos)
{
    if (!m_KillIndicatorEnabled)
        return;

    const auto now = std::chrono::steady_clock::now();
    MaybeTrimExpiredKillIndicators(now, false);
    ++m_KillIndicatorStatsKillSpawned;

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

    AddOrRecycleKillIndicator(indicatorPos, true, headshot, now, true);
}

void VR::DrawKillIndicators(IMatRenderContext* renderContext, ITexture* hudTexture)
{
    if (!renderContext)
        return;

    const auto now = std::chrono::steady_clock::now();
    MaybeTrimExpiredKillIndicators(now, true);

    if (!m_KillIndicatorEnabled || m_ActiveKillIndicators.empty() || !hudTexture)
        return;

    if (m_IsVREnabled && (m_Overlay || vr::VROverlay()))
        return;

    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_EngineClient->IsInGame())
    {
        TrimExpiredKillIndicators(now, true);
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

        int screenX = 0;
        int screenY = 0;
        if (!ProjectKillIndicatorToHud(this, drawPos, screenWidth, screenHeight, m_KillIndicatorMaxDistance, screenX, screenY))
            continue;

        float scale = indicator.killConfirmed ? (0.78f + 0.34f * pulse) : (0.56f + 0.24f * pulse);
        if (indicator.killConfirmed && indicator.headshot)
            scale *= 1.10f;

        const float alphaBase = indicator.killConfirmed ? 0.72f : 0.60f;
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
            const bool hadFeedbackState = m_HitSoundPending
                || !m_PendingKillSoundHits.empty()
                || !m_PendingKillSoundEvents.empty()
                || !m_FeedbackSoundWarmupSignature.empty();
            if (hadFeedbackState)
                ResetFeedbackSoundWorkerState();

            TrimExpiredKillIndicators(std::chrono::steady_clock::now(), true);
            m_PendingKillSoundHits.clear();
            m_PendingKillSoundEvents.clear();
            m_HitSoundPending = false;
            m_HitSoundPendingMergedCount = 0;
            m_HitSoundPendingWorldPos = Vector{};
            m_HitSoundPendingQueuedAt = {};
            m_LastKillSoundCommonKills = -1;
            m_LastKillSoundSpecialKills = -1;
            m_PredictedHitFeedbackShotSerial = 0;
            m_LastPredictedHitSoundShotSerial = 0;
            m_LastPredictedHitFeedbackShotTime = {};
            m_FeedbackSoundWarmupSignature.clear();
        };

    const bool wantsHitFeedback = m_HitSoundEnabled || m_HitIndicatorEnabled;
    const bool wantsKillFeedback = m_KillSoundEnabled || m_KillIndicatorEnabled;
    const bool wantsAnyFeedback = wantsHitFeedback || wantsKillFeedback;
    if (!wantsAnyFeedback || !m_Game || !m_Game->m_EngineClient)
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

    EnsureFeedbackSoundWarmup();

    const auto now = std::chrono::steady_clock::now();
    FlushPendingHitSound(now);
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

    if (!wantsKillFeedback)
        return;

    int commonKills = 0;
    int specialKills = 0;
    if (!ReadLocalKillCounters(localPlayer, commonKills, specialKills))
    {
        m_LastKillSoundCommonKills = -1;
        m_LastKillSoundSpecialKills = -1;
        return;
    }

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

    for (int i = 0; i < totalDelta; ++i)
    {
        bool headshot = false;
        std::uintptr_t entityTag = 0;
        const bool hasEvent = ConsumePendingKillSoundEvent(now, headshot, entityTag);
        Vector impactPos{};
        bool matched = false;
        if (entityTag != 0)
            matched = ConsumePendingKillSoundHit(entityTag, now, &impactPos);
        if (!matched && hasEvent)
            matched = ConsumePendingKillSoundHit(0, now, &impactPos);

        if (matched)
        {
            PlayKillSound(headshot, &impactPos);
            SpawnKillIndicator(headshot, impactPos);
        }
        else
        {
            const bool unresolvedHeadshot = hasEvent && headshot;
            PlayKillSound(unresolvedHeadshot, nullptr);
            SpawnKillIndicator(unresolvedHeadshot, Vector{});
        }
    }

    m_LastKillSoundCommonKills = commonKills;
    m_LastKillSoundSpecialKills = specialKills;
}
