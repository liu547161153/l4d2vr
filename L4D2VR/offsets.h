#pragma once
#include "sigscanner.h"
#include "game.h"


struct Offset
{
    std::string moduleName;
    int offset;
    int address;
    std::string signature;
    int sigOffset;
    bool optional;
    bool valid;

    Offset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0, bool optional = false)
    {
        this->moduleName = moduleName;
        this->offset = currentOffset;
        this->address = 0;      // Always initialize to safe value
        this->signature = signature;
        this->sigOffset = sigOffset;
        this->optional = optional;
        this->valid = false;

        int newOffset = SigScanner::VerifyOffset(moduleName, currentOffset, signature, sigOffset);
        if (newOffset > 0)
        {
            this->offset = newOffset;
        }
        else if (newOffset == 0)
        {
            Game::logMsg("[Offsets] Signature verified: module=%s offset=0x%X sigOffset=%d sig=\"%s\"",
                moduleName.c_str(), this->offset, sigOffset, signature.c_str());
        }

        if (newOffset == -1)
        {
            // Keep address=0 and valid=false so hook setup can safely skip.
            if (optional)
            {
                Game::logMsg("[Offsets] Optional signature not found: module=%s sig=\"%s\"",
                    moduleName.c_str(), signature.c_str());
            }
            else
            {
                Game::errorMsg(("Signature not found: " + signature).c_str());
            }
            return;
        }

        HMODULE mod = GetModuleHandle(moduleName.c_str());
        if (!mod)
        {
            if (!optional)
                Game::errorMsg(("Module not loaded: " + moduleName).c_str());
            return;
        }

        this->address = (uintptr_t)mod + this->offset;
        this->valid = (this->address != 0);
    }
};

class Offsets
{
public:
    Offset RenderView =                  { "client.dll", 0x1D6C30, "55 8B EC 81 EC ? ? ? ? 53 56 57 8B D9" };
    Offset g_pClientMode =               { "client.dll", 0x228351, "89 04 B5 ? ? ? ? E8", 3 };
    Offset CalcViewModelView =           { "client.dll", 0x287270, "55 8B EC 83 EC 48 A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 8B 10" };
    Offset ClientFireTerrorBullets =     { "client.dll", 0x2F4350, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10"};
    Offset WriteUsercmdDeltaToBuffer =   { "client.dll", 0x134790, "55 8B EC 83 EC 60 0F 57 C0 8B 55 0C" };
    Offset WriteUsercmd =                { "client.dll", 0x1AAD50, "55 8B EC A1 ? ? ? ? 83 78 30 00 53 8B 5D 10 56 57" };
    Offset g_pppInput =                  { "client.dll", 0xA8A22, "8B 0D ? ? ? ? 8B 01 8B 50 58 FF E2", 2 };
    Offset AdjustEngineViewport =        { "client.dll", 0x31A890, "55 8B EC 8B 0D ? ? ? ? 85 C9 74 17" };
    Offset TestMeleeSwingClient =        { "client.dll", 0x30C040, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 53 56 8B 75 08 57 8B D9 E8 ? ? ? ? 8B" };
    Offset GetMeleeWeaponInfoClient =    { "client.dll", 0x30B570, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3" };
    Offset IsSplitScreen =               { "client.dll", 0x1B2A60, "33 C0 83 3D ? ? ? ? ? 0F 9D C0" };
    Offset PrePushRenderTarget =         { "client.dll", 0xA8C80, "55 8B EC 8B C1 56 8B 75 08 8B 0E 89 08 8B 56 04 89" };
    Offset UpdateLaserSight =            { "client.dll", 0x25DD80, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 28 02 00 00 A1 ? ? ? ? 33 C5 89 45 FC 56 57 8B F1 E8 ? ? ? ? 8B F8 83 FF FF 0F 84 ? ? ? ?", 0, true };
    Offset ParticleSetControlPointPosition = { "client.dll", 0x15BD10, "55 8B EC 53 56 8B 75 0C 57 8B F9 BB 01 00 00 00 84 9F B1 03 00 00 0F 84 ? ? ? ? 83 BF B4 03 00 00 FF", 0, true };
    Offset CreateParticleEffect =        { "client.dll", 0x152200, "55 8B EC 56 57 8B 7D 08 8B F1 8B 0D ? ? ? ? 57 E8 ? ? ? ? 85 C0 75 17 57 68 ? ? ? ? FF 15 ? ? ? ? 83 C4 08 33 C0 5F 5E 5D C2 1C 00 8B 4D 20 8B 55 14 51 83 EC 0C 8B CC 89 11 8B 55 18 89 51 04 8B 55 1C 89 51 08", 0, true };
    Offset StopParticleEffect =          { "client.dll", 0x1523C0, "55 8B EC 51 53 56 8B 75 08 57 8B F9 8B 5F 14 85 F6 74 61 33 C0 85 DB 0F 8E ? ? ? ? 8B 57 08 83 C2 14 39 32 74 11 40 83 C2 18 3B C3 7C F4 5F 5E 5B 8B E5 5D C2 04 00", 0, true };
    Offset ParticleSetControlPointForwardVector = { "client.dll", 0x15C030, "55 8B EC 8B 45 0C 56 57 8B 7D 08 8B F1 50 57 8D 4E 10 E8 ? ? ? ? 57 8B CE E8 ? ? ? ? 5F 5E 5D C2 08 00", 0, true };
    Offset DrawLaserBeam =               { "client.dll", 0x0EF660, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC C8 04 00 00 A1 ? ? ? ? 33 C5 89 45 FC A1 ? ? ? ? D9 BD BA FB FF FF D9 40 0C 8B 4B 0C 0F B7 85 BA FB FF FF D8 0D ? ? ? ? 0D 00 0C 00 00", 0, true };

    Offset ServerFireTerrorBullets =     { "server.dll", 0x3C3FC0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 8B 4D 10" };
    Offset ReadUserCmd =                 { "server.dll", 0x205100, "55 8B EC 53 8B 5D 10 56 57 8B 7D 0C 53" };
    Offset ProcessUsercmds =             { "server.dll", 0xEF710, "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08" };
    Offset CBaseEntity_entindex =        { "server.dll", 0x25390, "8B 41 28 85 C0 75 01 C3 8B 0D ? ? ? ? 2B 41 58 C1 F8 04 C3 CC CC CC CC CC CC CC CC CC CC CC 55"};
    Offset TestMeleeSwingServer =        { "server.dll", 0x3E79E0, "24 FF D2 5B 5F 5E C3", 20};
    Offset DoMeleeSwingServer =          { "server.dll", 0x3E84C0, "55 8B EC 83 EC 3C 53 56 8B F1 E8 ? ? ? ? 8B D8 85" };
    Offset StartMeleeSwingServer =       { "server.dll", 0x3E8780, "55 8B EC 53 56 8B F1 8B 86 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? 8B" };
    Offset PrimaryAttackServer =         { "server.dll", 0x3E8AB0, "56 57 8B F1 E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 8B 87 ? ? ? ? 83 F8 FF" };
    Offset ItemPostFrameServer =         { "server.dll", 0x3E8BA0, "56 57 8B F1 E8 ? ? ? ? 8B CE E8 ? ? ? ? 8B F8 85 FF 0F 84 ? ? ? ? 53" };
    Offset GetPrimaryAttackActivity =    { "server.dll", 0x3E7630, "55 8B EC 53 8B 5D 08 56 57 8B BB ? ? ? ?" };
    Offset GetActiveWeapon =             { "server.dll", 0x464F0, "55 8B EC 8B 45 0C 56 8B 75 08 50 56 E8 ? ? ? ? 84 C0 74 47 8B", -64 };
    Offset GetMeleeWeaponInfo =          { "server.dll", 0x3E67D0, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3" };
    Offset EyePosition =                 { "server.dll", 0x6D610, "55 8B EC 56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8B 45 08 F3" };
    // CBaseEntity::SetAbsOrigin (server/client) - used for roomscale 1:1 movement
    Offset CBaseEntity_SetAbsOrigin_Server = { "server.dll", 0,
        "?? ?? ?? ?? ?? ?? A1 ?? ?? ?? ?? 33 ?? 89 ?? ?? 56 57 8B ?? ?? 8B ?? E8 ?? ?? ?? ?? F3 ?? ?? ?? 0F 2E ?? ?? ?? ?? ?? 9F F6 ?? ?? 7A ?? F3 ?? ?? ?? ?? 0F 2E ?? ?? ?? ?? ?? 9F F6 ?? ?? 7A ?? F3 ?? ?? ?? ?? 0F 2E ?? ?? ?? ?? ?? 9F F6 ?? ?? 0F 8B ?? ?? ?? ?? 6A",
        0
    };

    Offset CBaseEntity_SetAbsOrigin_Client = { "client.dll", 0,
        "55 8B EC 56 57 8B F1 E8 ?? ?? ?? ?? 8B 7D 08 F3 0F 10 07",
        0
    };

    Offset GetRenderTarget =             { "materialsystem.dll", 0x2CD30, "83 79 4C 00" };
    Offset Viewport =                    { "materialsystem.dll", 0x2E010, "55 8B EC 83 EC 28 8B C1" };
    Offset GetViewport =                 { "materialsystem.dll", 0x2D240, "55 8B EC 8B 41 4C 8B 49 40 8D 04 C0 83 7C 81 ? ?" };
    Offset PushRenderTargetAndViewport = { "materialsystem.dll", 0x2D5F0, "55 8B EC 83 EC 24 8B 45 08 8B 55 10 89" };
    Offset PopRenderTargetAndViewport =  { "materialsystem.dll", 0x2CE80, "56 8B F1 83 7E 4C 00" };

    Offset DrawModelExecute =            { "engine.dll", 0xE05E0, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 56 8B 75 08 57 8B" };
    Offset VGui_Paint =                  { "engine.dll", 0x115CE0, "55 8B EC E8 ? ? ? ? 8B 10 8B C8 8B 52 38" };
    Offset SampleLightAtPoint =          { "engine.dll", 0xC1FB0, "55 8B EC 83 EC 18 8B 45 0C F3 0F 10 00 56 33 F6 56 56 56 56 8D 4D ? 51 F3 0F 11 45 ? F3 0F 10 40 04 6A 01 8D 55 ? F3 0F 11 45 ? F3 0F 10 40 08 F3 0F 5C 05 ? ? ? ? 52 50 F3 0F 11 45 ? E8 ? ? ? ? 83 C4 20 85 C0", 0, true };
};
