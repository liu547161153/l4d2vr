
#include "vr.h"
#include <Windows.h>
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
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>
#include <d3d9_vr.h>

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

#include "vr/vr_lifecycle.inl"
#include "vr/vr_process_input.inl"
#include "vr/vr_roomscale_prediction.inl"
#include "vr/vr_tracking.inl"
#include "vr/vr_aiming.inl"
#include "vr/vr_viewmodel_config.inl"
