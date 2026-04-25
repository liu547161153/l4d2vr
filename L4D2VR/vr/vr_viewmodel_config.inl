void VR::GetAimLineColor(int& r, int& g, int& b, int& a) const
{
    if (m_SpecialInfectedBlindSpotWarningActive)
    {
        r = m_AimLineWarningColorR;
        g = m_AimLineWarningColorG;
        b = m_AimLineWarningColorB;
    }
    else if (m_EffectiveAttackRangeIndicatorEnabled && m_AimLineEffectiveAttackRangeActive)
    {
        r = m_EffectiveAttackRangeColorR;
        g = m_EffectiveAttackRangeColorG;
        b = m_EffectiveAttackRangeColorB;
    }
    else if (m_SpecialInfectedPreWarningActive)
    {
        r = m_AimLineWarningColorR;
        g = m_AimLineWarningColorG;
        b = m_AimLineWarningColorB;
    }
    else
    {
        r = m_AimLineColorR;
        g = m_AimLineColorG;
        b = m_AimLineColorB;
    }

	a = m_AimLineColorA;
	const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
	if (m_ScopeAimLineOnlyInScope
		&& m_ThirdPersonFrontViewEnabled
		&& m_IsThirdPersonCamera
		&& m_ScopeWeaponIsFirearm
		&& queueMode == 0
		&& !m_ScopeRenderingPass)
		a = 0;
}


namespace vr_render_snapshot_cache_viewmodel
{
    struct ViewCache
    {
        uint32_t seq = 0;
        Vector viewAng{ 0.0f, 0.0f, 0.0f };
        Vector viewLeft{ 0.0f, 0.0f, 0.0f };
        Vector viewRight{ 0.0f, 0.0f, 0.0f };
    };

    inline ViewCache& TLS()
    {
        static thread_local ViewCache cache{};
        return cache;
    }

    inline bool Refresh(const VR* vr, ViewCache& c)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const uint32_t s1 = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
            if (s1 == 0 || (s1 & 1u))
                continue;

            const float ax = vr->m_RenderViewAngX.load(std::memory_order_relaxed);
            const float ay = vr->m_RenderViewAngY.load(std::memory_order_relaxed);
            const float az = vr->m_RenderViewAngZ.load(std::memory_order_relaxed);

            const float lx = vr->m_RenderViewOriginLeftX.load(std::memory_order_relaxed);
            const float ly = vr->m_RenderViewOriginLeftY.load(std::memory_order_relaxed);
            const float lz = vr->m_RenderViewOriginLeftZ.load(std::memory_order_relaxed);

            const float rx = vr->m_RenderViewOriginRightX.load(std::memory_order_relaxed);
            const float ry = vr->m_RenderViewOriginRightY.load(std::memory_order_relaxed);
            const float rz = vr->m_RenderViewOriginRightZ.load(std::memory_order_relaxed);

            const uint32_t s2 = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
            if (s1 == s2 && !(s2 & 1u))
            {
                c.seq = s2;
                c.viewAng = Vector(ax, ay, az);
                c.viewLeft = Vector(lx, ly, lz);
                c.viewRight = Vector(rx, ry, rz);
                return true;
            }
        }
        return false;
    }

    inline bool Get(const VR* vr, ViewCache& c)
    {
        if (!VR::t_UseRenderFrameSnapshot)
            return false;

        const uint32_t s = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
        if (s != 0 && !(s & 1u) && s == c.seq)
            return true;

        return Refresh(vr, c);
    }
}

Vector VR::GetViewAngle()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_viewmodel::TLS();
        if (vr_render_snapshot_cache_viewmodel::Get(this, cache))
            return cache.viewAng;
    }

    return Vector(m_HmdAngAbs.x, m_HmdAngAbs.y, m_HmdAngAbs.z);
}


float VR::GetMovementYawDeg()
{
    if (!m_MoveDirectionFromController)
    {
        if (m_MouseModeEnabled)
        {
            // In mouse mode, locomotion follows the body yaw (turning), not head yaw.
            float yaw = m_RotationOffset;
            // Wrap to [-180, 180]
            yaw -= 360.0f * std::floor((yaw + 180.0f) / 360.0f);
            return yaw;
        }

        Vector hmdAng = GetViewAngle();
        return hmdAng.y;
    }

    // Use the dominant (right) controller yaw as the movement basis.
    // This is intentionally yaw-only; pitch/roll should not affect locomotion.
    QAngle ctrlAng = GetRightControllerAbsAngle();
    return ctrlAng.y;
}

Vector VR::GetViewOriginLeft()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_viewmodel::TLS();
        if (vr_render_snapshot_cache_viewmodel::Get(this, cache))
            return cache.viewLeft;
    }

    Vector viewOriginLeft;

    viewOriginLeft = m_HmdPosAbs + (m_HmdForward * (-(m_EyeZ * m_VRScale)));
    viewOriginLeft = viewOriginLeft + (m_HmdRight * (-((m_Ipd * m_IpdScale * m_VRScale) / 2)));

    return viewOriginLeft;
}


Vector VR::GetViewOriginRight()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_viewmodel::TLS();
        if (vr_render_snapshot_cache_viewmodel::Get(this, cache))
            return cache.viewRight;
    }

    Vector viewOriginRight;

    viewOriginRight = m_HmdPosAbs + (m_HmdForward * (-(m_EyeZ * m_VRScale)));
    viewOriginRight = viewOriginRight + (m_HmdRight * (m_Ipd * m_IpdScale * m_VRScale) / 2);

    return viewOriginRight;
}



void VR::ResetPosition()
{
    m_CameraAnchor += m_SetupOrigin - m_HmdPosAbs;
    m_HeightOffset += m_SetupOrigin.z - m_HmdPosAbs.z;
    m_Roomscale1To1PrevValid = false;
    m_Roomscale1To1PrevCorrectedAbs = {};
}

std::string VR::GetMeleeWeaponName(C_WeaponCSBase* weapon) const
{
    if (!weapon)
        return std::string();

    if (!m_Game || !m_Game->m_Offsets)
    {
        Game::logMsg("[VR] Missing offsets while resolving melee weapon name.");
        return std::string();
    }

    typedef CMeleeWeaponInfoStore* (__thiscall* tGetMeleeWepInfo)(void* thisptr);
    static tGetMeleeWepInfo oGetMeleeWepInfo = nullptr;

    if (!oGetMeleeWepInfo)
        oGetMeleeWepInfo = (tGetMeleeWepInfo)(m_Game->m_Offsets->GetMeleeWeaponInfoClient.address);

    if (!oGetMeleeWepInfo)
        return std::string();

    CMeleeWeaponInfoStore* meleeWepInfo = oGetMeleeWepInfo(weapon);
    if (!meleeWepInfo)
        return std::string();

    std::string meleeName(meleeWepInfo->meleeWeaponName);
    std::transform(meleeName.begin(), meleeName.end(), meleeName.begin(), ::tolower);
    return meleeName;
}

std::string VR::WeaponIdToString(int weaponId) const
{
    static const std::vector<std::string> weaponNames =
    {
        "none",
        "pistol",
        "uzi",
        "pumpshotgun",
        "autoshotgun",
        "m16a1",
        "hunting_rifle",
        "mac10",
        "shotgun_chrome",
        "scar",
        "sniper_military",
        "spas",
        "first_aid_kit",
        "molotov",
        "pipe_bomb",
        "pain_pills",
        "gascan",
        "propane_tank",
        "oxygen_tank",
        "melee",
        "chainsaw",
        "grenade_launcher",
        "ammo_pack",
        "adrenaline",
        "defibrillator",
        "vomitjar",
        "ak47",
        "gnome_chompski",
        "cola_bottles",
        "fireworks_box",
        "incendiary_ammo",
        "frag_ammo",
        "magnum",
        "mp5",
        "sg552",
        "awp",
        "scout",
        "m60",
        "tank_claw",
        "hunter_claw",
        "charger_claw",
        "boomer_claw",
        "smoker_claw",
        "spitter_claw",
        "jockey_claw",
        "machinegun",
        "vomit",
        "splat",
        "pounce",
        "lounge",
        "pull",
        "choke",
        "rock",
        "physics",
        "ammo",
        "upgrade_item"
    };

    size_t index = static_cast<size_t>(weaponId);
    if (index < weaponNames.size())
        return weaponNames[index];

    return std::string();
}

std::string VR::NormalizeViewmodelAdjustKey(const std::string& rawKey) const
{
    const std::string weaponPrefix = "weapon:";
    if (rawKey.rfind(weaponPrefix, 0) == 0)
    {
        std::string weaponIdString = rawKey.substr(weaponPrefix.size());
        if (!weaponIdString.empty() &&
            std::all_of(weaponIdString.begin(), weaponIdString.end(), [](unsigned char c) { return std::isdigit(c); }))
        {
            try
            {
                int weaponId = std::stoi(weaponIdString);
                std::string weaponName = WeaponIdToString(weaponId);
                if (!weaponName.empty())
                    return weaponPrefix + weaponName;
            }
            catch (...)
            {
            }
        }
    }

    return rawKey;
}

std::string VR::BuildViewmodelAdjustKey(C_WeaponCSBase* weapon) const
{
    if (!weapon)
        return "weapon:none";

    C_WeaponCSBase::WeaponID weaponId = weapon->GetWeaponID();

    if (weaponId == C_WeaponCSBase::WeaponID::MELEE)
    {
        std::string meleeName = GetMeleeWeaponName(weapon);
        if (!meleeName.empty())
            return "melee:" + meleeName;

        Game::logMsg("[VR] Failed to resolve melee name, using generic key.");
        return "melee:unknown";
    }

    std::string weaponName = WeaponIdToString(static_cast<int>(weaponId));
    if (!weaponName.empty())
        return "weapon:" + weaponName;

    return "weapon:" + std::to_string(static_cast<int>(weaponId));
}

ViewmodelAdjustment& VR::EnsureViewmodelAdjustment(const std::string& key)
{
    auto [it, inserted] = m_ViewmodelAdjustments.emplace(key, m_DefaultViewmodelAdjust);
    return it->second;
}

void VR::RefreshActiveViewmodelAdjustment(C_BasePlayer* localPlayer)
{
    C_WeaponCSBase* activeWeapon = localPlayer ? (C_WeaponCSBase*)localPlayer->GetActiveWeapon() : nullptr;
    std::string adjustKey = BuildViewmodelAdjustKey(activeWeapon);

    m_CurrentViewmodelKey = adjustKey;

    if (m_LastLoggedViewmodelKey != m_CurrentViewmodelKey)
    {
        m_LastLoggedViewmodelKey = m_CurrentViewmodelKey;
    }

    ViewmodelAdjustment& adjustment = EnsureViewmodelAdjustment(adjustKey);
    m_ViewmodelPosAdjust = adjustment.position;
    m_ViewmodelAngAdjust = adjustment.angle;
}

void VR::LoadViewmodelAdjustments()
{
    m_ViewmodelAdjustments.clear();

    if (m_ViewmodelAdjustmentSavePath.empty())
    {
        Game::logMsg("[VR] No viewmodel adjustment save path configured.");
        ParseHapticsConfigFile();
        return;
    }

    std::ifstream adjustmentStream(m_ViewmodelAdjustmentSavePath);
    if (!adjustmentStream)
    {
        Game::logMsg("[VR] Viewmodel adjustment file missing, will create on save: %s", m_ViewmodelAdjustmentSavePath.c_str());
        return;
    }

    auto ltrim = [](std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        };
    auto rtrim = [](std::string& s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        };
    auto trim = [&](std::string& s) { ltrim(s); rtrim(s); };

    auto parseVector3 = [&](const std::string& raw, const Vector& defaults)->Vector
        {
            Vector result = defaults;
            std::stringstream ss(raw);
            std::string token;
            float* components[3] = { &result.x, &result.y, &result.z };
            int index = 0;

            while (std::getline(ss, token, ',') && index < 3)
            {
                trim(token);
                if (!token.empty())
                {
                    try
                    {
                        *components[index] = std::stof(token);
                    }
                    catch (...)
                    {
                    }
                }
                ++index;
            }

            return result;
        };

    std::string line;
    while (std::getline(adjustmentStream, line))
    {
        trim(line);
        if (line.empty())
            continue;

        size_t eq = line.find('=');
        size_t separator = line.find(';', eq == std::string::npos ? 0 : eq + 1);

        if (eq == std::string::npos || separator == std::string::npos || separator <= eq)
            continue;

        std::string key = line.substr(0, eq);
        trim(key);
        if (key.empty())
            continue;

        std::string posStr = line.substr(eq + 1, separator - eq - 1);
        std::string angStr = line.substr(separator + 1);

        std::string normalizedKey = NormalizeViewmodelAdjustKey(key);
        if (normalizedKey != key)
            Game::logMsg("[VR] Normalized viewmodel adjust key '%s' -> '%s'", key.c_str(), normalizedKey.c_str());

        Vector posAdjust = parseVector3(posStr, m_DefaultViewmodelAdjust.position);
        Vector angAdjustVec = parseVector3(angStr, Vector{ m_DefaultViewmodelAdjust.angle.x, m_DefaultViewmodelAdjust.angle.y, m_DefaultViewmodelAdjust.angle.z });

        m_ViewmodelAdjustments[normalizedKey] = { posAdjust, { angAdjustVec.x, angAdjustVec.y, angAdjustVec.z } };
    }

    Game::logMsg("[VR] Loaded %zu viewmodel adjustment entries from %s", m_ViewmodelAdjustments.size(), m_ViewmodelAdjustmentSavePath.c_str());
    m_ViewmodelAdjustmentsDirty = false;
}

void VR::SaveViewmodelAdjustments()
{
    if (m_ViewmodelAdjustmentSavePath.empty())
    {
        Game::logMsg("[VR] Cannot save viewmodel adjustments: missing path.");
        return;
    }

    std::ofstream adjustmentStream(m_ViewmodelAdjustmentSavePath, std::ios::trunc);
    if (!adjustmentStream)
    {
        Game::logMsg("[VR] Failed to open %s for saving viewmodel adjustments", m_ViewmodelAdjustmentSavePath.c_str());
        return;
    }

    for (const auto& [key, adjustment] : m_ViewmodelAdjustments)
    {
        adjustmentStream << key << '=' << adjustment.position.x << ',' << adjustment.position.y << ',' << adjustment.position.z;
        adjustmentStream << ';' << adjustment.angle.x << ',' << adjustment.angle.y << ',' << adjustment.angle.z << "\n";
    }

    Game::logMsg("[VR] Saved %zu viewmodel adjustment entries to %s", m_ViewmodelAdjustments.size(), m_ViewmodelAdjustmentSavePath.c_str());
    m_ViewmodelAdjustmentsDirty = false;
}

void VR::ParseConfigFile()
{
    std::ifstream configStream("VR\\config.txt");
    if (!configStream) {
        //  Ҳ    ͱ  ֹ   ʱ  Ĭ  ֵ
        return;
    }

    //  򵥵  trim
    auto ltrim = [](std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(),
            [](unsigned char ch) { return !std::isspace(ch); }));
        };
    auto rtrim = [](std::string& s) {
        s.erase(std::find_if(s.rbegin(), s.rend(),
            [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        };
    auto trim = [&](std::string& s) { ltrim(s); rtrim(s); };

    std::unordered_map<std::string, std::string> userConfig;
    std::string line;
    while (std::getline(configStream, line))
    {
        // ȥ  ע  
        size_t cut = std::string::npos;
        size_t p1 = line.find("//");
        size_t p2 = line.find('#');
        size_t p3 = line.find(';');
        if (p1 != std::string::npos) cut = p1;
        if (p2 != std::string::npos) cut = (cut == std::string::npos) ? p2 : std::min(cut, p2);
        if (p3 != std::string::npos) cut = (cut == std::string::npos) ? p3 : std::min(cut, p3);
        if (cut != std::string::npos) line.erase(cut);

        trim(line);
        if (line.empty()) continue;

        //      key=value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key); trim(value);
        if (!key.empty())
            userConfig[key] = value;
    }

    // С   ߣ   Ĭ  ֵ İ ȫ  ȡ
    auto getBool = [&](const char* k, bool defVal)->bool {
        auto it = userConfig.find(k);
        if (it == userConfig.end()) return defVal;
        std::string v = it->second;
        // תСд
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
        if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
        if (v == "0" || v == "false" || v == "off" || v == "no")  return false;
        return defVal;
        };
    auto getFloat = [&](const char* k, float defVal)->float {
        auto it = userConfig.find(k);
        if (it == userConfig.end() || it->second.empty()) return defVal;
        std::string v = it->second;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
        if (v == "true" || v == "on" || v == "yes") return 1.0f;
        if (v == "false" || v == "off" || v == "no") return 0.0f;
        try { return std::stof(it->second); }
        catch (...) { return defVal; }
        };
    auto getInt = [&](const char* k, int defVal)->int {
        auto it = userConfig.find(k);
        if (it == userConfig.end() || it->second.empty()) return defVal;
        std::string v = it->second;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
        if (v == "true" || v == "on" || v == "yes") return 1;
        if (v == "false" || v == "off" || v == "no") return 0;
        try { return std::stoi(it->second); }
        catch (...) { return defVal; }
        };
    auto getColor = [&](const char* k, int defR, int defG, int defB, int defA)->std::array<int, 4> {
        std::array<int, 4> defaults{ defR, defG, defB, defA };
        auto it = userConfig.find(k);
        if (it == userConfig.end())
            return defaults;

        std::array<int, 4> color = defaults;
        std::stringstream ss(it->second);
        std::string token;
        int index = 0;
        while (std::getline(ss, token, ',') && index < 4)
        {
            trim(token);
            if (!token.empty())
            {
                try
                {
                    color[index] = std::clamp(std::stoi(token), 0, 255);
                }
                catch (...)
                {
                    color[index] = defaults[index];
                }
            }
            ++index;
        }

        for (int& component : color)
        {
            component = std::clamp(component, 0, 255);
        }

        return color;
        };
    auto getVector3 = [&](const char* k, const Vector& defVal)->Vector {
        auto it = userConfig.find(k);
        if (it == userConfig.end())
            return defVal;

        Vector result = defVal;
        std::stringstream ss(it->second);
        std::string token;
        float* components[3] = { &result.x, &result.y, &result.z };
        int index = 0;

        while (std::getline(ss, token, ',') && index < 3)
        {
            trim(token);
            if (!token.empty())
            {
                try
                {
                    *components[index] = std::stof(token);
                }
                catch (...)
                {
                }
            }
            ++index;
        }

        return result;
        };

    auto getFloatList = [&](const char* k, const char* defVal = nullptr) -> std::vector<float>
        {
            std::vector<float> values;
            const auto it = userConfig.find(k);
            if (it == userConfig.end() && !defVal)
                return values;

            const std::string source = (it == userConfig.end()) ? defVal : it->second;
            std::stringstream ss(source);
            std::string token;
            while (std::getline(ss, token, ','))
            {
                trim(token);
                if (token.empty())
                    continue;

                try
                {
                    values.push_back(std::stof(token));
                }
                catch (...)
                {
                }
            }

            return values;
        };

    auto getString = [&](const char* k, const std::string& defVal)->std::string {
        auto it = userConfig.find(k);
        if (it == userConfig.end())
            return defVal;

        std::string value = it->second;
        trim(value);

        return value.empty() ? defVal : value;
        };

    const std::string injectedCmd = getString("cmd", getString("Cmd", ""));
    if (!injectedCmd.empty())
    {
        m_Game->ClientCmd_Unrestricted(injectedCmd.c_str());
        Game::logMsg("[VR] Executed config cmd: %s", injectedCmd.c_str());
    }

    auto parseVirtualKey = [&](const std::string& rawValue)->std::optional<WORD>
        {
            std::string value = rawValue;
            trim(value);
            if (value.size() < 5) // key:<x>
                return std::nullopt;

            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });

            const std::string prefix = "key:";
            if (value.rfind(prefix, 0) != 0)
                return std::nullopt;

            std::string keyToken = value.substr(prefix.size());
            trim(keyToken);
            if (keyToken.empty())
                return std::nullopt;

            static const std::unordered_map<std::string, WORD> keyLookup = {
                { "space", VK_SPACE }, { "enter", VK_RETURN }, { "return", VK_RETURN },
                { "tab", VK_TAB }, { "escape", VK_ESCAPE }, { "esc", VK_ESCAPE },
                { "shift", VK_SHIFT }, { "lshift", VK_LSHIFT }, { "rshift", VK_RSHIFT },
                { "ctrl", VK_CONTROL }, { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL },
                { "alt", VK_MENU }, { "lalt", VK_LMENU }, { "ralt", VK_RMENU },
                { "backspace", VK_BACK }, { "delete", VK_DELETE }, { "insert", VK_INSERT },
                { "home", VK_HOME }, { "end", VK_END }, { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT },
                { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT }
            };

            auto lookupIt = keyLookup.find(keyToken);
            if (lookupIt != keyLookup.end())
                return lookupIt->second;

            if (keyToken.size() == 1)
            {
                char ch = keyToken[0];
                if (ch >= 'a' && ch <= 'z')
                    return static_cast<WORD>('A' + (ch - 'a'));
                if (ch >= '0' && ch <= '9')
                    return static_cast<WORD>(ch);
            }

            if (keyToken.size() > 1 && keyToken[0] == 'f')
            {
                try
                {
                    int functionIndex = std::stoi(keyToken.substr(1));
                    if (functionIndex >= 1 && functionIndex <= 24)
                        return static_cast<WORD>(VK_F1 + functionIndex - 1);
                }
                catch (...)
                {
                }
            }

            return std::nullopt;
        };

    auto parseCustomActionBinding = [&](const char* key, CustomActionBinding& binding)
        {
            binding.command = getString(key, binding.command);
            binding.holdVirtualKey = false;
            binding.releaseCommand.clear();
            binding.usePressReleaseCommands = false;

            std::string trimmedCommand = binding.command;
            trim(trimmedCommand);
            std::string normalized = trimmedCommand;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return std::tolower(c); });

            const std::string holdPrefix = "hold:";
            if (normalized.rfind(holdPrefix, 0) == 0)
            {
                binding.holdVirtualKey = true;
                trimmedCommand = trimmedCommand.substr(holdPrefix.size());
                trim(trimmedCommand);
                binding.command = trimmedCommand;
            }
            // Custom alias helper: CustomActionXCommand=alias:aliasName:+speed|wait 30|-speed
            // The section after the alias name uses '|' as a stand-in for ';' to avoid config comment stripping.
            const std::string aliasPrefix = "alias:";
            if (normalized.rfind(aliasPrefix, 0) == 0)
            {
                std::string aliasDefinition = trimmedCommand.substr(aliasPrefix.size());
                trim(aliasDefinition);

                size_t separator = aliasDefinition.find(':');
                if (separator != std::string::npos)
                {
                    std::string aliasName = aliasDefinition.substr(0, separator);
                    std::string aliasBody = aliasDefinition.substr(separator + 1);
                    trim(aliasName);
                    trim(aliasBody);

                    if (!aliasName.empty() && !aliasBody.empty())
                    {
                        std::replace(aliasBody.begin(), aliasBody.end(), '|', ';');
                        std::string aliasCommand = "alias " + aliasName + " \"" + aliasBody + "\"";
                        m_Game->ClientCmd_Unrestricted(aliasCommand.c_str());
                        Game::logMsg("[VR] %s defined alias '%s' = %s", key, aliasName.c_str(), aliasBody.c_str());

                        binding.command = aliasName;
                        normalized = aliasName;
                    }
                }
            }

            binding.virtualKey = parseVirtualKey(binding.command);
            if (!binding.command.empty() && binding.virtualKey.has_value())
            {
                Game::logMsg("[VR] %s mapped to virtual key 0x%02X%s", key, *binding.virtualKey,
                    binding.holdVirtualKey ? " (hold)" : "");
            }
            else if (!trimmedCommand.empty() && trimmedCommand[0] == '+' && trimmedCommand.size() > 1)
            {
                binding.usePressReleaseCommands = true;
                binding.releaseCommand = "-" + trimmedCommand.substr(1);
                binding.command = trimmedCommand;
                Game::logMsg("[VR] %s mapped to command press/release: %s / %s", key, binding.command.c_str(), binding.releaseCommand.c_str());
            }
        };


    m_SnapTurnAngle = getFloat("SnapTurnAngle", m_SnapTurnAngle);
    m_TurnSpeed = getFloat("TurnSpeed", m_TurnSpeed);
    // Locomotion direction: default is HMD-yaw-based. Optional hand-yaw-based.
    m_MoveDirectionFromController = getBool("MoveDirectionFromController", m_MoveDirectionFromController);
    m_MoveDirectionFromController = getBool("MovementDirectionFromController", m_MoveDirectionFromController);

    m_InventoryGestureRange = getFloat("InventoryGestureRange", m_InventoryGestureRange);
    m_InventoryChestOffset = getVector3("InventoryChestOffset", m_InventoryChestOffset);
    m_InventoryBackOffset = getVector3("InventoryBackOffset", m_InventoryBackOffset);
    m_InventoryLeftWaistOffset = getVector3("InventoryLeftWaistOffset", m_InventoryLeftWaistOffset);
    m_InventoryRightWaistOffset = getVector3("InventoryRightWaistOffset", m_InventoryRightWaistOffset);
    m_InventoryBodyOriginOffset = getVector3("InventoryBodyOriginOffset", m_InventoryBodyOriginOffset);
    m_InventoryHudMarkerDistance = getFloat("InventoryHudMarkerDistance", m_InventoryHudMarkerDistance);
    m_InventoryHudMarkerUpOffset = getFloat("InventoryHudMarkerUpOffset", m_InventoryHudMarkerUpOffset);
    m_InventoryHudMarkerSeparation = getFloat("InventoryHudMarkerSeparation", m_InventoryHudMarkerSeparation);

    m_InventoryQuickSwitchEnabled = getBool("InventoryQuickSwitchEnabled", m_InventoryQuickSwitchEnabled);
    m_InventoryQuickSwitchOffset = getVector3("InventoryQuickSwitchOffset", m_InventoryQuickSwitchOffset);
    m_InventoryQuickSwitchZoneRadius = getFloat("InventoryQuickSwitchZoneRadius", m_InventoryQuickSwitchZoneRadius);
    if (!m_InventoryQuickSwitchEnabled)
    {
        m_InventoryQuickSwitchActive = false;
        m_InventoryQuickSwitchArmed = false;
    }
    m_DrawInventoryAnchors = getBool("ShowInventoryAnchors", m_DrawInventoryAnchors);
    const auto inventoryColor = getColor("InventoryAnchorColor", m_InventoryAnchorColorR, m_InventoryAnchorColorG, m_InventoryAnchorColorB, m_InventoryAnchorColorA);
    m_InventoryAnchorColorR = inventoryColor[0];
    m_InventoryAnchorColorG = inventoryColor[1];
    m_InventoryAnchorColorB = inventoryColor[2];
    m_InventoryAnchorColorA = inventoryColor[3];
    const std::unordered_map<std::string, vr::VRActionHandle_t*> actionLookup =
    {
        {"jump", &m_ActionJump},
        {"primaryattack", &m_ActionPrimaryAttack},
        {"secondaryattack", &m_ActionSecondaryAttack},
        {"reload", &m_ActionReload},
        {"walk", &m_ActionWalk},
        {"turn", &m_ActionTurn},
        {"use", &m_ActionUse},
        {"nextitem", &m_ActionNextItem},
        {"previtem", &m_ActionPrevItem},
        {"resetposition", &m_ActionResetPosition},
        {"crouch", &m_ActionCrouch},
        {"flashlight", &m_ActionFlashlight},
        {"activatevr", &m_ActionActivateVR},
        {"menuselect", &m_MenuSelect},
        {"menuback", &m_MenuBack},
        {"menuup", &m_MenuUp},
        {"menudown", &m_MenuDown},
        {"menuleft", &m_MenuLeft},
        {"menuright", &m_MenuRight},
        {"spray", &m_Spray},
        {"scoreboard", &m_Scoreboard},
        {"togglehud", &m_ToggleHUD},
        {"pause", &m_Pause}
    };

    auto parseActionCombo = [&](const char* key, const ActionCombo& defaultCombo) -> ActionCombo
        {
            auto it = userConfig.find(key);
            if (it == userConfig.end())
                return defaultCombo;

            std::string rawValue = it->second;
            trim(rawValue);
            std::transform(rawValue.begin(), rawValue.end(), rawValue.begin(), [](unsigned char c) { return std::tolower(c); });

            // Allow disabling a combo by setting it to "false" in the config file.
            if (rawValue == "false")
            {
                Game::logMsg("[VR] %s combo disabled via config", key);
                return ActionCombo{};
            }

            ActionCombo combo{};
            std::stringstream ss(rawValue);
            std::string token;
            int index = 0;
            while (std::getline(ss, token, '+') && index < 2)
            {
                trim(token);
                std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return std::tolower(c); });
                if (token.empty())
                {
                    ++index;
                    continue;
                }

                auto actionIt = actionLookup.find(token);
                if (actionIt != actionLookup.end())
                {
                    if (index == 0)
                        combo.primary = actionIt->second;
                    else
                        combo.secondary = actionIt->second;
                }
                ++index;
            }

            if (!combo.primary || !combo.secondary)
            {
                Game::logMsg("[VR] Invalid %s combo '%s', using defaults.", key, it->second.c_str());
                return defaultCombo;
            }

            return combo;
        };

    m_VoiceRecordCombo = parseActionCombo("VoiceRecordCombo", m_VoiceRecordCombo);
    m_QuickTurnCombo = parseActionCombo("QuickTurnCombo", m_QuickTurnCombo);
    m_ViewmodelAdjustCombo = parseActionCombo("ViewmodelAdjustCombo", m_ViewmodelAdjustCombo);
    parseCustomActionBinding("CustomAction1Command", m_CustomAction1Binding);
    parseCustomActionBinding("CustomAction2Command", m_CustomAction2Binding);
    parseCustomActionBinding("CustomAction3Command", m_CustomAction3Binding);
    parseCustomActionBinding("CustomAction4Command", m_CustomAction4Binding);
    parseCustomActionBinding("CustomAction5Command", m_CustomAction5Binding);

    m_LeftHanded = getBool("LeftHanded", m_LeftHanded);
    m_VRScale = getFloat("VRScale", m_VRScale);
    m_IpdScale = getFloat("IPDScale", m_IpdScale);
    m_ThirdPersonVRCameraOffset = std::max(0.0f, getFloat("ThirdPersonVRCameraOffset", m_ThirdPersonVRCameraOffset));
    {
        // Backward-compatible parsing:
        // - old form: ThirdPersonFrontVRCameraOffset=80
        // - new form: ThirdPersonFrontVRCameraOffset=80,0,0
        const float legacyForward = getFloat("ThirdPersonFrontVRCameraOffset", m_ThirdPersonVRCameraOffset);
        const Vector frontDefault = { legacyForward, 0.0f, 0.0f };
        m_ThirdPersonFrontVRCameraOffset = getVector3("ThirdPersonFrontVRCameraOffset", frontDefault);
        // Optional per-axis overrides for easier live tuning.
        m_ThirdPersonFrontVRCameraOffset.x = getFloat("ThirdPersonFrontVRCameraOffsetX", m_ThirdPersonFrontVRCameraOffset.x);
        m_ThirdPersonFrontVRCameraOffset.y = getFloat("ThirdPersonFrontVRCameraOffsetY", m_ThirdPersonFrontVRCameraOffset.y);
        m_ThirdPersonFrontVRCameraOffset.z = getFloat("ThirdPersonFrontVRCameraOffsetZ", m_ThirdPersonFrontVRCameraOffset.z);
    }
    m_ThirdPersonCameraSmoothing = std::clamp(getFloat("ThirdPersonCameraSmoothing", m_ThirdPersonCameraSmoothing), 0.0f, 0.99f);
    m_ThirdPersonMapLoadCooldownMs = std::max(0, getInt("ThirdPersonMapLoadCooldownMs", m_ThirdPersonMapLoadCooldownMs));
    m_ThirdPersonRenderOnCustomWalk = getBool("ThirdPersonRenderOnCustomWalk", m_ThirdPersonRenderOnCustomWalk);
    m_ThirdPersonDefault = getBool("ThirdPersonDefault", m_ThirdPersonDefault);
    m_ThirdPersonCameraFollowHmd = getBool("ThirdPersonCameraFollowHmd", m_ThirdPersonCameraFollowHmd);
    m_ThirdPersonFrontViewEnabled = getBool("ThirdPersonFrontViewEnabled", m_ThirdPersonFrontViewEnabled);
    m_ThirdPersonFrontScopeFromEye = getBool("ThirdPersonFrontScopeFromEye", m_ThirdPersonFrontScopeFromEye);
    m_ThirdPersonScopeOverlayOffset = getVector3("ThirdPersonScopeOverlayOffset", m_ThirdPersonScopeOverlayOffset);
    m_ThirdPersonScopeOverlayOffset.x = getFloat("ThirdPersonScopeOverlayOffsetX", m_ThirdPersonScopeOverlayOffset.x);
    m_ThirdPersonScopeOverlayOffset.y = getFloat("ThirdPersonScopeOverlayOffsetY", m_ThirdPersonScopeOverlayOffset.y);
    m_ThirdPersonScopeOverlayOffset.z = getFloat("ThirdPersonScopeOverlayOffsetZ", m_ThirdPersonScopeOverlayOffset.z);
    m_HideArms = getBool("HideArms", m_HideArms);
    m_SplitArmsToControllers = getBool("SplitArmsToControllers", m_SplitArmsToControllers);
    m_HudDistance = getFloat("HudDistance", m_HudDistance);
    m_HudSize = getFloat("HudSize", m_HudSize);
    m_TopHudCurvature = std::clamp(getFloat("TopHudCurvature", m_TopHudCurvature), 0.0f, 1.0f);
    m_HudAlwaysVisible = getBool("HudAlwaysVisible", m_HudAlwaysVisible);
    m_HudToggleState = m_HudAlwaysVisible;

    // Hand HUD background opacity (0..1)
    m_LeftWristHudBgAlpha = std::clamp(getFloat("LeftWristHudBgAlpha", m_LeftWristHudBgAlpha), 0.0f, 1.0f);
    m_RightAmmoHudBgAlpha = std::clamp(getFloat("RightAmmoHudBgAlpha", m_RightAmmoHudBgAlpha), 0.0f, 1.0f);

    // Right ammo HUD: crop ratio (U max). Removes unused blank area on the right.
    m_RightAmmoHudUVMaxU = std::clamp(getFloat("RightAmmoHudUVMaxU", m_RightAmmoHudUVMaxU), 0.05f, 1.0f);

    // Hand HUD alpha (legacy): used as an extra BACKGROUND opacity multiplier (0..1).
    // We no longer apply IVROverlay::SetOverlayAlpha to the whole overlay because it makes text/bars fade too.
    const float leftBgMul = std::clamp(getFloat("LeftWristHudAlpha", m_LeftWristHudAlpha), 0.0f, 1.0f);
    const float rightBgMul = std::clamp(getFloat("RightAmmoHudAlpha", m_RightAmmoHudAlpha), 0.0f, 1.0f);
    m_LeftWristHudBgAlpha = std::clamp(m_LeftWristHudBgAlpha * leftBgMul, 0.0f, 1.0f);
    m_RightAmmoHudBgAlpha = std::clamp(m_RightAmmoHudBgAlpha * rightBgMul, 0.0f, 1.0f);
    m_LeftWristHudAlpha = 1.0f;
    m_RightAmmoHudAlpha = 1.0f;

    // Left wrist HUD: battery label font scale (1..4)
    m_LeftWristHudBatteryTextScale = std::clamp(getInt("LeftWristHudBatteryTextScale", m_LeftWristHudBatteryTextScale), 1, 4);

    // Hand HUD temp health decay (HP per second)
    m_HandHudTempHealthDecayRate = std::max(0.0f, getFloat("HandHudTempHealthDecayRate", m_HandHudTempHealthDecayRate));

    // Hand HUD overlays (SteamVR overlay, raw)
    m_LeftWristHudEnabled = getBool("LeftWristHudEnabled", m_LeftWristHudEnabled);
    m_LeftWristHudWidthMeters = std::clamp(getFloat("LeftWristHudWidthMeters", m_LeftWristHudWidthMeters), 0.01f, 1.0f);
    m_LeftWristHudXOffset = getFloat("LeftWristHudXOffset", m_LeftWristHudXOffset);
    m_LeftWristHudYOffset = getFloat("LeftWristHudYOffset", m_LeftWristHudYOffset);
    m_LeftWristHudZOffset = getFloat("LeftWristHudZOffset", m_LeftWristHudZOffset);
    {
        const Vector def = { m_LeftWristHudAngleOffset.x, m_LeftWristHudAngleOffset.y, m_LeftWristHudAngleOffset.z };
        const Vector ang = getVector3("LeftWristHudAngleOffset", def);
        m_LeftWristHudAngleOffset = { ang.x, ang.y, ang.z };
    }

    m_LeftWristHudCurvature = std::clamp(getFloat("LeftWristHudCurvature", m_LeftWristHudCurvature), 0.0f, 1.0f);
    m_LeftWristHudShowBattery = getBool("LeftWristHudShowBattery", m_LeftWristHudShowBattery);
    m_LeftWristHudShowTeammates = getBool("LeftWristHudShowTeammates", m_LeftWristHudShowTeammates);

    m_RightAmmoHudEnabled = getBool("RightAmmoHudEnabled", m_RightAmmoHudEnabled);
    m_RightAmmoHudWidthMeters = std::clamp(getFloat("RightAmmoHudWidthMeters", m_RightAmmoHudWidthMeters), 0.01f, 1.0f);
    m_RightAmmoHudXOffset = getFloat("RightAmmoHudXOffset", m_RightAmmoHudXOffset);
    m_RightAmmoHudYOffset = getFloat("RightAmmoHudYOffset", m_RightAmmoHudYOffset);
    m_RightAmmoHudZOffset = getFloat("RightAmmoHudZOffset", m_RightAmmoHudZOffset);
    {
        const Vector def = { m_RightAmmoHudAngleOffset.x, m_RightAmmoHudAngleOffset.y, m_RightAmmoHudAngleOffset.z };
        const Vector ang = getVector3("RightAmmoHudAngleOffset", def);
        m_RightAmmoHudAngleOffset = { ang.x, ang.y, ang.z };
    }


    // Hand HUD: world-quad mode (HMD-relative overlays using GPU textures)
    m_HandHudWorldQuadEnabled = getBool("HandHudWorldQuadEnabled", m_HandHudWorldQuadEnabled);
    m_HandHudWorldQuadAttachToControllers = getBool("HandHudWorldQuadAttachToControllers", m_HandHudWorldQuadAttachToControllers);
    m_HandHudWorldQuadDistanceMeters = std::clamp(getFloat("HandHudWorldQuadDistanceMeters", m_HandHudWorldQuadDistanceMeters), 0.05f, 5.0f);
    m_HandHudWorldQuadYOffsetMeters = std::clamp(getFloat("HandHudWorldQuadYOffsetMeters", m_HandHudWorldQuadYOffsetMeters), -2.0f, 2.0f);
    m_HandHudWorldQuadXGapMeters = std::clamp(getFloat("HandHudWorldQuadXGapMeters", m_HandHudWorldQuadXGapMeters), 0.0f, 0.5f);
    {
        const Vector def = { m_HandHudWorldQuadAngleOffset.x, m_HandHudWorldQuadAngleOffset.y, m_HandHudWorldQuadAngleOffset.z };
        const Vector ang = getVector3("HandHudWorldQuadAngleOffset", def);
        m_HandHudWorldQuadAngleOffset = { ang.x, ang.y, ang.z };
    }

    // Debug: hand HUD update diagnostics (prints periodic state + overlay errors).
    m_HandHudDebugLog = getBool("HandHudDebugLog", m_HandHudDebugLog);
    m_HandHudDebugLogHz = std::clamp(getFloat("HandHudDebugLogHz", m_HandHudDebugLogHz), 0.0f, 240.0f);

    m_AntiAliasing = std::stol(userConfig["AntiAliasing"]);
    m_FixedHudYOffset = getFloat("FixedHudYOffset", m_FixedHudYOffset);
    m_FixedHudDistanceOffset = getFloat("FixedHudDistanceOffset", m_FixedHudDistanceOffset);
    float controllerSmoothingValue = m_ControllerSmoothing;
    const bool hasControllerSmoothing = userConfig.find("ControllerSmoothing") != userConfig.end();
    const bool hasHeadSmoothing = userConfig.find("HeadSmoothing") != userConfig.end();
    if (hasControllerSmoothing)
        controllerSmoothingValue = getFloat("ControllerSmoothing", controllerSmoothingValue);
    else if (hasHeadSmoothing) // Backward compatibility: old configs used HeadSmoothing
        controllerSmoothingValue = getFloat("HeadSmoothing", controllerSmoothingValue);
    m_ControllerSmoothing = std::clamp(controllerSmoothingValue, 0.0f, 0.99f);

    float headSmoothingValue = m_HeadSmoothing;
    if (hasHeadSmoothing)
        headSmoothingValue = getFloat("HeadSmoothing", headSmoothingValue);
    else
        headSmoothingValue = controllerSmoothingValue; // Match controller smoothing by default
    m_HeadSmoothing = std::clamp(headSmoothingValue, 0.0f, 0.99f);
    m_MotionGestureSwingThreshold = std::max(0.0f, getFloat("MotionGestureSwingThreshold", m_MotionGestureSwingThreshold));
    m_MotionGesturePushThreshold = std::max(0.0f, getFloat("MotionGesturePushThreshold", m_MotionGesturePushThreshold));
    m_MotionGestureDownSwingThreshold = std::max(0.0f, getFloat("MotionGestureDownSwingThreshold", m_MotionGestureDownSwingThreshold));
    m_MotionGestureJumpThreshold = std::max(0.0f, getFloat("MotionGestureJumpThreshold", m_MotionGestureJumpThreshold));
    m_MotionGestureCooldown = std::max(0.0f, getFloat("MotionGestureCooldown", m_MotionGestureCooldown));
    m_MotionGestureHoldDuration = std::max(0.0f, getFloat("MotionGestureHoldDuration", m_MotionGestureHoldDuration));
    m_ViewmodelAdjustEnabled = getBool("ViewmodelAdjustEnabled", m_ViewmodelAdjustEnabled);
    m_AimLineThickness = std::max(0.0f, getFloat("AimLineThickness", m_AimLineThickness));
    m_AimLineEnabled = getBool("AimLineEnabled", m_AimLineEnabled);
    m_AimLineOnlyWhenLaserSight = getBool("AimLineOnlyWhenLaserSight", m_AimLineOnlyWhenLaserSight);
    m_AimLineConfigEnabled = m_AimLineEnabled;
    m_BlockFireOnFriendlyAimEnabled = getBool("BlockFireOnFriendlyAimEnabled", m_BlockFireOnFriendlyAimEnabled);
    m_BlockFireOnFriendlyAimRadiusMeters = std::clamp(getFloat("BlockFireOnFriendlyAimRadiusMeters", m_BlockFireOnFriendlyAimRadiusMeters), 0.0f, 0.5f);
    m_AutoRepeatSemiAutoFire = getBool("AutoRepeatSemiAutoFire", m_AutoRepeatSemiAutoFire);
    m_AutoRepeatSemiAutoFireHz = std::max(0.0f, getFloat("AutoRepeatSemiAutoFireHz", m_AutoRepeatSemiAutoFireHz));
    m_AutoRepeatSprayPushEnabled = getBool("AutoRepeatSprayPushEnabled", m_AutoRepeatSprayPushEnabled);
    m_AutoRepeatSprayPushDelayTicks = std::clamp(getInt("AutoRepeatSprayPushDelayTicks", m_AutoRepeatSprayPushDelayTicks), 0, 8);
    m_AutoRepeatSprayPushHoldTicks = std::clamp(getInt("AutoRepeatSprayPushHoldTicks", m_AutoRepeatSprayPushHoldTicks), 1, 8);

    m_AutoFastMelee = getBool("AutoFastMelee", m_AutoFastMelee);
    m_AutoFastMeleeUseWeaponSwitch = getBool("AutoFastMeleeUseWeaponSwitch", getBool("AutoFastMeleeUseShove", m_AutoFastMeleeUseWeaponSwitch));
    m_HitSoundEnabled = getBool("HitSoundEnabled", m_HitSoundEnabled);
    m_HitSoundPlaybackCooldownSeconds = std::clamp(getFloat("HitSoundPlaybackCooldownSeconds", m_HitSoundPlaybackCooldownSeconds), 0.0f, 0.25f);
    m_HitSoundSpec = getString("HitSoundSpec", m_HitSoundSpec);
    m_HitSoundVolume = std::clamp(getFloat("HitSoundVolume", m_HitSoundVolume), 0.0f, 2.0f);
    m_KillSoundEnabled = getBool("KillSoundEnabled", m_KillSoundEnabled);
    m_KillSoundDetectionWindowSeconds = std::clamp(getFloat("KillSoundDetectionWindowSeconds", m_KillSoundDetectionWindowSeconds), 0.05f, 1.0f);
    m_KillSoundPlaybackCooldownSeconds = std::clamp(getFloat("KillSoundPlaybackCooldownSeconds", m_KillSoundPlaybackCooldownSeconds), 0.0f, 0.25f);
    m_KillSoundNormalSpec = getString("KillSoundNormalSpec", m_KillSoundNormalSpec);
    m_KillSoundHeadshotSpec = getString("KillSoundHeadshotSpec", m_KillSoundHeadshotSpec);
    m_KillSoundVolume = std::clamp(getFloat("KillSoundVolume", m_KillSoundVolume), 0.0f, 2.0f);
    m_HeadshotSoundVolume = std::clamp(getFloat("HeadshotSoundVolume", m_HeadshotSoundVolume), 0.0f, 2.0f);
    m_FeedbackSoundSpatialBlend = std::clamp(getFloat("FeedbackSoundSpatialBlend", m_FeedbackSoundSpatialBlend), 0.0f, 1.0f);
    m_FeedbackSoundSpatialRange = std::clamp(getFloat("FeedbackSoundSpatialRange", m_FeedbackSoundSpatialRange), 64.0f, 8192.0f);
    m_FeedbackSoundDebugForceChannel = std::clamp(getInt("FeedbackSoundDebugForceChannel", m_FeedbackSoundDebugForceChannel), -1, 1);
    m_FeedbackSoundDebugLog = getBool("FeedbackSoundDebugLog", m_FeedbackSoundDebugLog);
    m_FeedbackSoundDebugLogHz = std::clamp(getFloat("FeedbackSoundDebugLogHz", m_FeedbackSoundDebugLogHz), 0.0f, 20.0f);
    SyncVrmodFeedbackGameSounds();
    m_HitIndicatorEnabled = getBool("HitIndicatorEnabled", m_HitIndicatorEnabled);
    m_KillIndicatorEnabled = getBool("KillIndicatorEnabled", m_KillIndicatorEnabled);
    m_KillIndicatorDebugLog = getBool("KillIndicatorDebugLog", m_KillIndicatorDebugLog);
    m_KillIndicatorDebugLogHz = std::clamp(getFloat("KillIndicatorDebugLogHz", m_KillIndicatorDebugLogHz), 0.0f, 20.0f);
    m_KillIndicatorLifetimeSeconds = std::clamp(getFloat("KillIndicatorLifetimeSeconds", m_KillIndicatorLifetimeSeconds), 0.10f, 3.0f);
    m_KillIndicatorSizePixels = std::clamp(getFloat("KillIndicatorSizePixels", m_KillIndicatorSizePixels), 16.0f, 512.0f);
    m_KillIndicatorRiseUnits = std::clamp(getFloat("KillIndicatorRiseUnits", m_KillIndicatorRiseUnits), 0.0f, 128.0f);
    m_KillIndicatorMaxDistance = std::clamp(getFloat("KillIndicatorMaxDistance", m_KillIndicatorMaxDistance), 128.0f, 16384.0f);
    m_KillIndicatorMaterialBaseSpec = getString("KillIndicatorMaterialBaseSpec", m_KillIndicatorMaterialBaseSpec);
    m_KillIndicatorHitMaterial = nullptr;
    m_KillIndicatorNormalMaterial = nullptr;
    m_KillIndicatorHeadshotMaterial = nullptr;
    m_MeleeAimLineEnabled = getBool("MeleeAimLineEnabled", m_MeleeAimLineEnabled);
    auto aimColor = getColor("AimLineColor", m_AimLineColorR, m_AimLineColorG, m_AimLineColorB, m_AimLineColorA);
    m_AimLineColorR = aimColor[0];
    m_AimLineColorG = aimColor[1];
    m_AimLineColorB = aimColor[2];
    m_AimLineColorA = aimColor[3];
    m_EffectiveAttackRangeIndicatorEnabled = getBool("EffectiveAttackRangeIndicatorEnabled", m_EffectiveAttackRangeIndicatorEnabled);
    m_EffectiveAttackRangeAutoFireEnabled = getBool("EffectiveAttackRangeAutoFireEnabled", m_EffectiveAttackRangeAutoFireEnabled);
    auto effectiveRangeColor = getColor("EffectiveAttackRangeColor",
        m_EffectiveAttackRangeColorR, m_EffectiveAttackRangeColorG, m_EffectiveAttackRangeColorB, m_AimLineColorA);
    m_EffectiveAttackRangeColorR = effectiveRangeColor[0];
    m_EffectiveAttackRangeColorG = effectiveRangeColor[1];
    m_EffectiveAttackRangeColorB = effectiveRangeColor[2];
    m_EffectiveAttackRangeDebugLog = getBool("EffectiveAttackRangeDebugLog", m_EffectiveAttackRangeDebugLog);
    m_EffectiveAttackRangeDebugLogHz = std::clamp(getFloat("EffectiveAttackRangeDebugLogHz", m_EffectiveAttackRangeDebugLogHz), 0.0f, 20.0f);
    m_EffectiveAttackRangeHoldSeconds = std::clamp(getFloat("EffectiveAttackRangeHoldSeconds", m_EffectiveAttackRangeHoldSeconds), 0.0f, 1.0f);
    m_EffectiveAttackRangeCacheSeconds = std::clamp(getFloat("EffectiveAttackRangeCacheSeconds", m_EffectiveAttackRangeCacheSeconds), 0.0f, 1.0f);
    m_EffectiveAttackRangeCacheDistanceTolerance = std::clamp(getFloat("EffectiveAttackRangeCacheDistanceTolerance", m_EffectiveAttackRangeCacheDistanceTolerance), 0.0f, 256.0f);
    m_EffectiveAttackRangeCacheSpreadTolerance = std::clamp(getFloat("EffectiveAttackRangeCacheSpreadTolerance", m_EffectiveAttackRangeCacheSpreadTolerance), 0.0f, 10.0f);
    m_EffectiveAttackRangeCacheDirectionDot = std::clamp(getFloat("EffectiveAttackRangeCacheDirectionDot", m_EffectiveAttackRangeCacheDirectionDot), 0.0f, 1.0f);
    m_EffectiveAttackRangeMeleeDistance = std::clamp(getFloat("EffectiveAttackRangeMeleeDistance", m_EffectiveAttackRangeMeleeDistance), 1.0f, 512.0f);
    m_EffectiveAttackRangeMeleeFanAngle = std::clamp(getFloat("EffectiveAttackRangeMeleeFanAngle", m_EffectiveAttackRangeMeleeFanAngle), 0.0f, 180.0f);
    m_EffectiveAttackRangeMeleeAutoFastMeleeIntervalSeconds = std::clamp(getFloat("EffectiveAttackRangeMeleeAutoFastMeleeIntervalSeconds", m_EffectiveAttackRangeMeleeAutoFastMeleeIntervalSeconds), 0.05f, 5.0f);
    m_EffectiveAttackRangeHitPointTolerance = std::clamp(getFloat("EffectiveAttackRangeHitPointTolerance", m_EffectiveAttackRangeHitPointTolerance), 0.0f, 64.0f);
    m_EffectiveAttackRangeHitPointSpreadScale = std::clamp(getFloat("EffectiveAttackRangeHitPointSpreadScale", m_EffectiveAttackRangeHitPointSpreadScale), 0.0f, 2.0f);
    m_EffectiveAttackRangeHitPointMaxTolerance = std::clamp(getFloat("EffectiveAttackRangeHitPointMaxTolerance", m_EffectiveAttackRangeHitPointMaxTolerance), m_EffectiveAttackRangeHitPointTolerance, 64.0f);
    m_D3DAimLineOverlayEnabled = getBool("D3DAimLineOverlayEnabled", m_D3DAimLineOverlayEnabled);
    m_D3DAimLineOverlaySyncAimLineColor = getBool("D3DAimLineOverlaySyncAimLineColor", m_D3DAimLineOverlaySyncAimLineColor);
    m_D3DAimLineOverlayWidthPixels = std::clamp(getFloat("D3DAimLineOverlayWidthPixels", m_D3DAimLineOverlayWidthPixels), 0.0f, 64.0f);
    m_D3DAimLineOverlayOutlinePixels = std::clamp(getFloat("D3DAimLineOverlayOutlinePixels", m_D3DAimLineOverlayOutlinePixels), 0.0f, 64.0f);
    m_D3DAimLineOverlayEndpointPixels = std::clamp(getFloat("D3DAimLineOverlayEndpointPixels", m_D3DAimLineOverlayEndpointPixels), 0.0f, 128.0f);
    auto d3dAimColor = getColor("D3DAimLineOverlayColor",
        m_D3DAimLineOverlayColorR, m_D3DAimLineOverlayColorG, m_D3DAimLineOverlayColorB, m_D3DAimLineOverlayColorA);
    m_D3DAimLineOverlayColorR = d3dAimColor[0];
    m_D3DAimLineOverlayColorG = d3dAimColor[1];
    m_D3DAimLineOverlayColorB = d3dAimColor[2];
    m_D3DAimLineOverlayColorA = d3dAimColor[3];
    auto d3dAimOutlineColor = getColor("D3DAimLineOverlayOutlineColor",
        m_D3DAimLineOverlayOutlineColorR, m_D3DAimLineOverlayOutlineColorG, m_D3DAimLineOverlayOutlineColorB, m_D3DAimLineOverlayOutlineColorA);
    m_D3DAimLineOverlayOutlineColorR = d3dAimOutlineColor[0];
    m_D3DAimLineOverlayOutlineColorG = d3dAimOutlineColor[1];
    m_D3DAimLineOverlayOutlineColorB = d3dAimOutlineColor[2];
    m_D3DAimLineOverlayOutlineColorA = d3dAimOutlineColor[3];
    m_AimLinePersistence = std::max(0.0f, getFloat("AimLinePersistence", m_AimLinePersistence));
    m_AimLineFrameDurationMultiplier = std::max(0.0f, getFloat("AimLineFrameDurationMultiplier", m_AimLineFrameDurationMultiplier));
    m_AimLineMaxHz = std::max(0.0f, getFloat("AimLineMaxHz", m_AimLineMaxHz));
    m_GameLaserSightBeamEnabled = getBool("GameLaserSightBeamEnabled", m_GameLaserSightBeamEnabled);
    m_GameLaserSightReplaceParticle = getBool("GameLaserSightReplaceParticle", m_GameLaserSightReplaceParticle);
    m_GameLaserSightThickness = std::clamp(getFloat("GameLaserSightThickness", m_GameLaserSightThickness), 0.0f, 8.0f);
    auto gameLaserColor = getColor("GameLaserSightColor", m_GameLaserSightColorR, m_GameLaserSightColorG, m_GameLaserSightColorB, m_GameLaserSightColorA);
    m_GameLaserSightColorR = gameLaserColor[0];
    m_GameLaserSightColorG = gameLaserColor[1];
    m_GameLaserSightColorB = gameLaserColor[2];
    m_GameLaserSightColorA = gameLaserColor[3];
    m_GameLaserSightEndOffset = getVector3("GameLaserSightEndOffset", m_GameLaserSightEndOffset);
    m_GameLaserSightEndOffset.x = std::clamp(m_GameLaserSightEndOffset.x, -256.0f, 256.0f);
    m_GameLaserSightEndOffset.y = std::clamp(m_GameLaserSightEndOffset.y, -256.0f, 256.0f);
    m_GameLaserSightEndOffset.z = std::clamp(m_GameLaserSightEndOffset.z, -256.0f, 256.0f);
    m_ThrowArcLandingOffset = std::max(-10000.0f, std::min(10000.0f, getFloat("ThrowArcLandingOffset", m_ThrowArcLandingOffset)));
    m_ThrowArcMaxHz = std::max(0.0f, getFloat("ThrowArcMaxHz", m_ThrowArcMaxHz));
    // Debug / memory
    const bool prevVASLog = m_DebugVASLog;
    m_DebugVASLog = getBool("DebugVASLog", m_DebugVASLog);
    m_LazyScopeRearMirrorRTT = getBool("LazyScopeRearMirrorRTT", m_LazyScopeRearMirrorRTT);
    if (!prevVASLog && m_DebugVASLog)
        LogVAS("DebugVASLog enabled");
    const bool prevAutoMatQueueMode = m_AutoMatQueueMode;
    m_AutoMatQueueMode = getBool("AutoMatQueueMode", m_AutoMatQueueMode);
    if (m_AutoMatQueueMode != prevAutoMatQueueMode)
    {
        m_AutoMatQueueModeLastRequested = -999;
        m_AutoMatQueueModeLastCmdTime = {};
        m_MenuFpsMaxSent = false;
        m_MenuFpsMaxLastHz = 0;
    }

    // Multicore rendering: explicit minimum render-thread wait budget (ms) for a fresher
    // WaitGetPoses() snapshot in queued mode. 0 disables fixed waiting, but the render hook can
    // still auto-wait during real HMD motion to avoid reusing the same pose sample.
    // 1~3 = light wait, 5+ = prioritize stability, -1 = strong sync (wait up to ~50ms).
    m_QueuedRenderPoseWaitMs = std::clamp(getInt("QueuedRenderPoseWaitMs", m_QueuedRenderPoseWaitMs), -1, 20);
    // Multicore rendering: present-side wait budget (ms) for a fresh dRenderView frame before submit.
    // 0 = no wait (max FPS, can increase stale-frame submits), 1~3 = usually best balance.
    m_QueuedSubmitWaitMs = std::clamp(getInt("QueuedSubmitWaitMs", m_QueuedSubmitWaitMs), 0, 20);

    // Queued rendering: optional render-thread FPS cap as % of HMD refresh.
    // 0 = unlimited, 100 = match HMD refresh.
    m_QueuedRenderMaxFps = std::clamp(getInt("QueuedRenderMaxFps", m_QueuedRenderMaxFps), 0, 240);
    // Queued rendering: smart FPS cap engagement. When enabled, the FPS cap is only applied when
    // the render thread is detected to be outrunning pose updates during body motion or HMD motion.
    m_QueuedRenderMaxFpsSmart = getBool("QueuedRenderMaxFpsSmart", m_QueuedRenderMaxFpsSmart);
    // Queued rendering: limit how many extra render frames may reuse the same WaitGetPoses() snapshot.
    // -1 = disabled, 0 = never reuse (most stable), 1 = allow 1 reuse (2 frames per pose), etc.
    m_QueuedRenderMaxFramesAhead = std::clamp(getInt("QueuedRenderMaxFramesAhead", m_QueuedRenderMaxFramesAhead), -1, 6);

    // Queued rendering: render-thread smoothing time constant (ms) for cameraAnchor/rotationOffset.
    // 0 = off, 20~80 typical, higher = smoother but more latency.
    m_QueuedRenderViewSmoothMs = std::clamp(getInt("QueuedRenderViewSmoothMs", m_QueuedRenderViewSmoothMs), 0, 250);

    // Queued rendering: HMD pose smoothing time constant (ms) for visual stability.
    // 0 = off. Higher values can soften stale-pose stepping a bit, but they do not produce fresher
    // poses, so they are not a full fix for queued-render ghosting during real head motion.
    m_QueuedRenderHmdSmoothMs = std::clamp(getInt("QueuedRenderHmdSmoothMs", m_QueuedRenderHmdSmoothMs), 0, 250);
    // Queued rendering: feed HMD yaw through the same rotationOffset path used by thumbstick turns.
    m_QueuedRenderHmdYawUsesTurnPath = getBool("QueuedRenderHmdYawUsesTurnPath", m_QueuedRenderHmdYawUsesTurnPath);

    // Queued rendering: stabilize first-person viewmodel (disable engine bob/lag in queued mode).
    m_QueuedViewmodelStabilize = getBool("QueuedViewmodelStabilize", m_QueuedViewmodelStabilize);
    // Global: hard-lock first-person viewmodel pose after engine calc (all queue modes).
    m_ViewmodelDisableMoveBob = getBool("ViewmodelDisableMoveBob", m_ViewmodelDisableMoveBob);
    Game::logMsg("[VR][Config] ViewmodelDisableMoveBob=%s", m_ViewmodelDisableMoveBob ? "true" : "false");
    m_QueuedViewmodelStabilizeDebugLog = getBool("QueuedViewmodelStabilizeDebugLog", m_QueuedViewmodelStabilizeDebugLog);
    m_QueuedViewmodelStabilizeDebugLogHz = std::max(0.0f, getFloat("QueuedViewmodelStabilizeDebugLogHz", m_QueuedViewmodelStabilizeDebugLogHz));
    m_RenderPipelineDebugLog = getBool("RenderPipelineDebugLog", m_RenderPipelineDebugLog);
    m_RenderPipelineDebugLogHz = std::clamp(getFloat("RenderPipelineDebugLogHz", m_RenderPipelineDebugLogHz), 0.0f, 60.0f);

    // Bullet FX alignment: fine-tune client-side tracer/impact visuals.
    // Units: meters in aim-ray space (X=forward, Y=right, Z=up). Visual-only.
    const bool hasBulletFxOff = (userConfig.find("BulletVisualHitOffset") != userConfig.end());
    const bool hasQueuedFxOff = (userConfig.find("QueuedBulletVisualHitOffset") != userConfig.end());

    m_BulletVisualHitOffset = getVector3("BulletVisualHitOffset", m_BulletVisualHitOffset);
    m_BulletVisualHitOffset.x = std::clamp(m_BulletVisualHitOffset.x, -1.0f, 1.0f);
    m_BulletVisualHitOffset.y = std::clamp(m_BulletVisualHitOffset.y, -1.0f, 1.0f);
    m_BulletVisualHitOffset.z = std::clamp(m_BulletVisualHitOffset.z, -1.0f, 1.0f);

    // Additional correction only when queued rendering is enabled (mat_queue_mode!=0).
    m_QueuedBulletVisualHitOffset = getVector3("QueuedBulletVisualHitOffset", m_QueuedBulletVisualHitOffset);
    m_QueuedBulletVisualHitOffset.x = std::clamp(m_QueuedBulletVisualHitOffset.x, -1.0f, 1.0f);
    m_QueuedBulletVisualHitOffset.y = std::clamp(m_QueuedBulletVisualHitOffset.y, -1.0f, 1.0f);
    m_QueuedBulletVisualHitOffset.z = std::clamp(m_QueuedBulletVisualHitOffset.z, -1.0f, 1.0f);

    // Backward compatibility: older configs used only QueuedBulletVisualHitOffset.
    // If BulletVisualHitOffset is not set but QueuedBulletVisualHitOffset is set,
    // treat the queued value as the base offset (applies in both single/queued render).
    if (!hasBulletFxOff && hasQueuedFxOff)
    {
        m_BulletVisualHitOffset = m_QueuedBulletVisualHitOffset;
        m_QueuedBulletVisualHitOffset = { 0.0f, 0.0f, 0.0f };
        Game::logMsg("[VR][Config] QueuedBulletVisualHitOffset provided without BulletVisualHitOffset -> using it as BulletVisualHitOffset (compat)");
    }

    if (hasBulletFxOff)
    {
        Game::logMsg("[VR][Config] BulletVisualHitOffset=(%.4f, %.4f, %.4f)",
            m_BulletVisualHitOffset.x, m_BulletVisualHitOffset.y, m_BulletVisualHitOffset.z);
    }
    if (hasQueuedFxOff)
    {
        Game::logMsg("[VR][Config] QueuedBulletVisualHitOffset=(%.4f, %.4f, %.4f)",
            m_QueuedBulletVisualHitOffset.x, m_QueuedBulletVisualHitOffset.y, m_QueuedBulletVisualHitOffset.z);
    }

    // Gun-mounted scope
    m_ScopeEnabled = getBool("ScopeEnabled", m_ScopeEnabled);
    m_ScopeRTTSize = std::clamp(getInt("ScopeRTTSize", m_ScopeRTTSize), 128, 4096);
    m_ScopeRTTMaxHz = std::clamp(getFloat("ScopeRTTMaxHz", m_ScopeRTTMaxHz), 0.0f, 240.0f);
    m_ScopeFov = std::clamp(getFloat("ScopeFov", m_ScopeFov), 1.0f, 179.0f);
    m_ScopeZNear = std::clamp(getFloat("ScopeZNear", m_ScopeZNear), 0.1f, 64.0f);
    {
        const float configuredScopeFov = m_ScopeFov;
        const auto magnifications = getFloatList("ScopeMagnification");
        if (!magnifications.empty())
        {
            m_ScopeMagnificationOptions.clear();
            for (float mag : magnifications)
            {
                if (std::isfinite(mag))
                    m_ScopeMagnificationOptions.push_back(std::clamp(mag, 1.0f, 179.0f));
            }
        }

        if (m_ScopeMagnificationOptions.empty())
            m_ScopeMagnificationOptions.push_back(m_ScopeFov);

        m_ScopeMagnificationIndex = 0;
        for (size_t i = 0; i < m_ScopeMagnificationOptions.size(); ++i)
        {
            if (fabs(m_ScopeMagnificationOptions[i] - configuredScopeFov) < 0.01f)
            {
                m_ScopeMagnificationIndex = i;
                break;
            }
        }
        m_ScopeFov = std::clamp(m_ScopeMagnificationOptions[m_ScopeMagnificationIndex], 1.0f, 179.0f);
    }


    // Scoped aim sensitivity scaling (mouse-style ADS / zoom sensitivity).
    // Accepts either:
    //   ScopeAimSensitivityScale=80              (80% for all magnifications)
    //   ScopeAimSensitivityScale=100,85,70,55    (per ScopeMagnification index)
    //   ScopeAimSensitivityScale=120,140,160,180 (faster than base when magnified)
    {
        const auto scalesRaw = getFloatList("ScopeAimSensitivityScale");
        if (!scalesRaw.empty())
        {
            m_ScopeAimSensitivityScales.clear();
            for (float s : scalesRaw)
            {
                if (!std::isfinite(s))
                    continue;

                // Allow both [0..1] and [0..100] styles.
                if (s > 1.5f)
                    s *= 0.01f;

                m_ScopeAimSensitivityScales.push_back(std::clamp(s, 0.05f, 4.0f));
            }
        }

        // Ensure the table matches magnification count.
        const size_t n = m_ScopeMagnificationOptions.size();
        if (n > 0)
        {
            if (m_ScopeAimSensitivityScales.empty())
                m_ScopeAimSensitivityScales.assign(n, 1.0f);
            else if (m_ScopeAimSensitivityScales.size() < n)
                m_ScopeAimSensitivityScales.resize(n, m_ScopeAimSensitivityScales.back());
            else if (m_ScopeAimSensitivityScales.size() > n)
                m_ScopeAimSensitivityScales.resize(n);
        }
    }

    // Changing config values should not cause a sudden jump mid-scope.
    m_ScopeAimSensitivityInit = false;
    m_ScopeCameraOffset = getVector3("ScopeCameraOffset", m_ScopeCameraOffset);
    { Vector tmp = getVector3("ScopeCameraAngleOffset", Vector{ m_ScopeCameraAngleOffset.x, m_ScopeCameraAngleOffset.y, m_ScopeCameraAngleOffset.z }); m_ScopeCameraAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z }; }

    m_ScopeOverlayWidthMeters = std::max(0.001f, getFloat("ScopeOverlayWidthMeters", m_ScopeOverlayWidthMeters));
    m_ScopeOverlayXOffset = getFloat("ScopeOverlayXOffset", m_ScopeOverlayXOffset);
    m_ScopeOverlayYOffset = getFloat("ScopeOverlayYOffset", m_ScopeOverlayYOffset);
    m_ScopeOverlayZOffset = getFloat("ScopeOverlayZOffset", m_ScopeOverlayZOffset);
    { Vector tmp = getVector3("ScopeOverlayAngleOffset", Vector{ m_ScopeOverlayAngleOffset.x, m_ScopeOverlayAngleOffset.y, m_ScopeOverlayAngleOffset.z }); m_ScopeOverlayAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z }; }
    m_ScopeAimLineOnlyInScope = getBool("ScopeAimLineOnlyInScope", m_ScopeAimLineOnlyInScope);
    m_ScopeHideLocalPlayerModelInScope = getBool("ScopeHideLocalPlayerModelInScope", m_ScopeHideLocalPlayerModelInScope);

    m_ScopeOverlayAlwaysVisible = getBool("ScopeOverlayAlwaysVisible", m_ScopeOverlayAlwaysVisible);
    m_ScopeOverlayIdleAlpha = std::clamp(getFloat("ScopeOverlayIdleAlpha", m_ScopeOverlayIdleAlpha), 0.0f, 1.0f);
    m_ScopeRequireLookThrough = getBool("ScopeRequireLookThrough", m_ScopeRequireLookThrough);
    m_ScopeLookThroughDistanceMeters = std::clamp(getFloat("ScopeLookThroughDistanceMeters", m_ScopeLookThroughDistanceMeters), 0.01f, 2.0f);
    m_ScopeLookThroughAngleDeg = std::clamp(getFloat("ScopeLookThroughAngleDeg", m_ScopeLookThroughAngleDeg), 1.0f, 89.0f);

    // Scope stabilization (visual only)
    m_ScopeStabilizationEnabled = getBool("ScopeStabilizationEnabled", m_ScopeStabilizationEnabled);
    m_ScopeStabilizationMinCutoff = std::clamp(getFloat("ScopeStabilizationMinCutoff", m_ScopeStabilizationMinCutoff), 0.05f, 30.0f);
    m_ScopeStabilizationBeta = std::clamp(getFloat("ScopeStabilizationBeta", m_ScopeStabilizationBeta), 0.0f, 5.0f);
    m_ScopeStabilizationDCutoff = std::clamp(getFloat("ScopeStabilizationDCutoff", m_ScopeStabilizationDCutoff), 0.05f, 30.0f);
    if (!m_ScopeStabilizationEnabled)
    {
        m_ScopeStabilizationInit = false;
        m_ScopeStabilizationLastTime = {};
    }

    // Rear mirror
    m_RearMirrorEnabled = getBool("RearMirrorEnabled", m_RearMirrorEnabled);
    m_RearMirrorShowOnlyOnSpecialWarning = getBool("RearMirrorShowOnlyOnSpecialWarning", m_RearMirrorShowOnlyOnSpecialWarning);
    m_RearMirrorSpecialShowHoldSeconds = std::max(0.0f, getFloat("RearMirrorSpecialShowHoldSeconds", m_RearMirrorSpecialShowHoldSeconds));
    m_RearMirrorRTTSize = std::clamp(getInt("RearMirrorRTTSize", m_RearMirrorRTTSize), 128, 4096);
    m_RearMirrorRTTMaxHz = std::clamp(getFloat("RearMirrorRTTMaxHz", m_RearMirrorRTTMaxHz), 0.0f, 240.0f);
    m_RearMirrorFov = std::clamp(getFloat("RearMirrorFov", m_RearMirrorFov), 1.0f, 179.0f);
    m_RearMirrorZNear = std::clamp(getFloat("RearMirrorZNear", m_RearMirrorZNear), 0.1f, 64.0f);

    m_RearMirrorCameraOffset = getVector3("RearMirrorCameraOffset", m_RearMirrorCameraOffset);
    {
        Vector tmp = getVector3(
            "RearMirrorCameraAngleOffset",
            Vector{ m_RearMirrorCameraAngleOffset.x, m_RearMirrorCameraAngleOffset.y, m_RearMirrorCameraAngleOffset.z });
        m_RearMirrorCameraAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z };
    }

    m_RearMirrorOverlayWidthMeters = std::max(0.01f, getFloat("RearMirrorOverlayWidthMeters", m_RearMirrorOverlayWidthMeters));
    m_RearMirrorOverlayXOffset = getFloat("RearMirrorOverlayXOffset", m_RearMirrorOverlayXOffset);
    m_RearMirrorOverlayYOffset = getFloat("RearMirrorOverlayYOffset", m_RearMirrorOverlayYOffset);
    m_RearMirrorOverlayZOffset = getFloat("RearMirrorOverlayZOffset", m_RearMirrorOverlayZOffset);
    {
        Vector tmp = getVector3(
            "RearMirrorOverlayAngleOffset",
            Vector{ m_RearMirrorOverlayAngleOffset.x, m_RearMirrorOverlayAngleOffset.y, m_RearMirrorOverlayAngleOffset.z });
        m_RearMirrorOverlayAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z };
    }
    m_RearMirrorAlpha = std::clamp(getFloat("RearMirrorAlpha", m_RearMirrorAlpha), 0.0f, 1.0f);
    m_RearMirrorFlipHorizontal = getBool("RearMirrorFlipHorizontal", m_RearMirrorFlipHorizontal);

    // Hide the rear mirror when the aim line/ray intersects it (prevents blocking your view while aiming).
    m_RearMirrorHideWhenAimLineHits = getBool("RearMirrorHideWhenAimLineHits", m_RearMirrorHideWhenAimLineHits);
    m_RearMirrorAimLineHideHoldSeconds = std::clamp(getFloat("RearMirrorAimLineHideHoldSeconds", m_RearMirrorAimLineHideHoldSeconds), 0.0f, 1.0f);
    if (!m_RearMirrorHideWhenAimLineHits)
        m_RearMirrorAimLineHideUntil = {};

    // Rear mirror hint: enlarge overlay when special-infected arrows are visible in the mirror render pass
    m_RearMirrorSpecialWarningDistance = std::max(0.0f, getFloat("RearMirrorSpecialWarningDistance", m_RearMirrorSpecialWarningDistance));
    if (m_RearMirrorSpecialWarningDistance <= 0.0f)
        m_RearMirrorSpecialEnlargeActive = false;

    // Auto reset tracking origin after a level loads (0 disables)
    m_AutoResetPositionAfterLoadSeconds = std::clamp(
        getFloat("AutoResetPositionAfterLoadSeconds", m_AutoResetPositionAfterLoadSeconds),
        0.0f, 60.0f);

    // Damage feedback overall amplitude scale (0..1).
    // Applies after profile/merge/damage scaling so you can globally tame or mute hurt haptics
    // without re-tuning each individual damage profile.
    m_DamageFeedbackOverallScale = std::clamp(
        getFloat("DamageFeedbackOverallScale", m_DamageFeedbackOverallScale),
        0.0f, 1.0f);

    m_ForceNonVRServerMovement = getBool("ForceNonVRServerMovement", m_ForceNonVRServerMovement);

    // Non-VR server movement: make client-side bullet/muzzle effects originate from controller (visual-only).
    m_NonVRServerMovementEffectsFromController = getBool("NonVRServerMovementEffectsFromController", m_NonVRServerMovementEffectsFromController);
    m_NonVRServerMovementEffectsDebugLog = getBool("NonVRServerMovementEffectsDebugLog", m_NonVRServerMovementEffectsDebugLog);
    m_NonVRServerMovementEffectsDebugLogHz = std::max(0.0f, getFloat("NonVRServerMovementEffectsDebugLogHz", m_NonVRServerMovementEffectsDebugLogHz));
    m_Roomscale1To1Movement = getBool("Roomscale1To1Movement", m_Roomscale1To1Movement);
    m_Roomscale1To1MaxStepMeters = getFloat("Roomscale1To1MaxStepMeters", m_Roomscale1To1MaxStepMeters);
    m_Roomscale1To1DebugLog = getBool("Roomscale1To1DebugLog", m_Roomscale1To1DebugLog);
    m_Roomscale1To1DebugLogHz = getFloat("Roomscale1To1DebugLogHz", m_Roomscale1To1DebugLogHz);

    // 1:1 roomscale camera decoupling / chase
    m_Roomscale1To1DecoupleCamera = getBool("Roomscale1To1DecoupleCamera", m_Roomscale1To1DecoupleCamera);
    m_Roomscale1To1UseCameraDistanceChase = getBool("Roomscale1To1UseCameraDistanceChase", m_Roomscale1To1UseCameraDistanceChase);
    m_Roomscale1To1DisableWhileThumbstick = getBool("Roomscale1To1DisableWhileThumbstick", m_Roomscale1To1DisableWhileThumbstick);
    m_Roomscale1To1AllowedCameraDriftMeters = std::max(0.0f, getFloat("Roomscale1To1AllowedCameraDriftMeters", m_Roomscale1To1AllowedCameraDriftMeters));
    m_Roomscale1To1ChaseHysteresisMeters = std::max(0.0f, getFloat("Roomscale1To1ChaseHysteresisMeters", m_Roomscale1To1ChaseHysteresisMeters));
    m_Roomscale1To1MinApplyMeters = std::max(0.0f, getFloat("Roomscale1To1MinApplyMeters", m_Roomscale1To1MinApplyMeters));
    m_Roomscale1To1ChaseActive = false;

    // Mouse mode (desktop-style aiming while staying in VR rendering)
    m_MouseModeEnabled = getBool("MouseModeEnabled", m_MouseModeEnabled);
    m_MouseModeAimFromHmd = getBool("MouseModeAimFromHmd", m_MouseModeAimFromHmd);
    m_MouseModeHmdAimSensitivity = std::clamp(getFloat("MouseModeHmdAimSensitivity", m_MouseModeHmdAimSensitivity), 0.0f, 3.0f);
    m_MouseModeYawSensitivity = getFloat("MouseModeYawSensitivity", m_MouseModeYawSensitivity);
    m_MouseModePitchSensitivity = getFloat("MouseModePitchSensitivity", m_MouseModePitchSensitivity);
    m_MouseModePitchAffectsView = getBool("MouseModePitchAffectsView", m_MouseModePitchAffectsView);
    m_MouseModeTurnSmoothing = getFloat("MouseModeTurnSmoothing", m_MouseModeTurnSmoothing);
    m_MouseModePitchSmoothing = getFloat("MouseModePitchSmoothing", m_MouseModePitchSmoothing);
    m_MouseModeViewmodelAnchorOffset = getVector3("MouseModeViewmodelAnchorOffset", m_MouseModeViewmodelAnchorOffset);
    m_MouseModeScopedViewmodelAnchorOffset = getVector3("MouseModeScopedViewmodelAnchorOffset", m_MouseModeViewmodelAnchorOffset);
    // Mouse-mode: if non-zero, place the scope overlay using the OpenVR HMD tracking pose
    // (meters in tracking space), so the overlay can't disappear due to unit mismatches.
    m_MouseModeScopeOverlayOffset = getVector3("MouseModeScopeOverlayOffset", m_MouseModeScopeOverlayOffset);
    m_MouseModeScopeOverlayAngleOffsetSet = (userConfig.find("MouseModeScopeOverlayAngleOffset") != userConfig.end());
    if (m_MouseModeScopeOverlayAngleOffsetSet)
    {
        Vector tmp = getVector3("MouseModeScopeOverlayAngleOffset", Vector{ m_MouseModeScopeOverlayAngleOffset.x, m_MouseModeScopeOverlayAngleOffset.y, m_MouseModeScopeOverlayAngleOffset.z });
        m_MouseModeScopeOverlayAngleOffset = QAngle{ tmp.x, tmp.y, tmp.z };
    }
    m_MouseModeScopeToggleKey = parseVirtualKey(getString("MouseModeScopeToggleKey", "key:f9"));
    m_MouseModeScopeMagnificationKey = parseVirtualKey(getString("MouseModeScopeMagnificationKey", "key:f10"));

    // Optional bindable impulses for mouse-mode scope control.
    // Using impulses avoids GetAsyncKeyState issues and allows normal Source binds.
    auto mouseModeScopeSensitivityList = getFloatList("MouseModeScopeSensitivityScale", "100");
    if (mouseModeScopeSensitivityList.empty())
        mouseModeScopeSensitivityList.push_back(100.0f);
    for (auto& v : mouseModeScopeSensitivityList)
        v = std::clamp(v, 1.0f, 200.0f);
    if (mouseModeScopeSensitivityList.size() < m_ScopeMagnificationOptions.size())
        mouseModeScopeSensitivityList.resize(m_ScopeMagnificationOptions.size(), mouseModeScopeSensitivityList.back());
    m_MouseModeScopeSensitivityScales = mouseModeScopeSensitivityList;
    m_MouseModeAimConvergeDistance = getFloat("MouseModeAimConvergeDistance", m_MouseModeAimConvergeDistance);

    // Non-VR server melee feel tuning (ForceNonVRServerMovement=true only)
    m_NonVRMeleeSwingThreshold = std::max(0.0f, getFloat("NonVRMeleeSwingThreshold", m_NonVRMeleeSwingThreshold));
    m_NonVRMeleeSwingCooldown = std::max(0.0f, getFloat("NonVRMeleeSwingCooldown", m_NonVRMeleeSwingCooldown));
    m_NonVRMeleeHoldTime = std::max(0.0f, getFloat("NonVRMeleeHoldTime", m_NonVRMeleeHoldTime));
    m_SnapTurning = getBool("SnapTurning", m_SnapTurning);
    m_NonVRMeleeAttackDelay = std::max(0.0f, getFloat("NonVRMeleeAttackDelay", m_NonVRMeleeAttackDelay));
    m_NonVRMeleeAimLockTime = std::max(0.0f, getFloat("NonVRMeleeAimLockTime", m_NonVRMeleeAimLockTime));
    m_NonVRMeleeHysteresis = std::clamp(getFloat("NonVRMeleeHysteresis", m_NonVRMeleeHysteresis), 0.1f, 0.95f);
    m_NonVRMeleeAngVelThreshold = std::max(0.0f, getFloat("NonVRMeleeAngVelThreshold", m_NonVRMeleeAngVelThreshold));
    m_NonVRMeleeSwingDirBlend = std::clamp(getFloat("NonVRMeleeSwingDirBlend", m_NonVRMeleeSwingDirBlend), 0.0f, 1.0f);
    m_RequireSecondaryAttackForItemSwitch = getBool("RequireSecondaryAttackForItemSwitch", m_RequireSecondaryAttackForItemSwitch);
    m_SpecialInfectedWarningActionEnabled = getBool("SpecialInfectedAutoEvade", m_SpecialInfectedWarningActionEnabled);
    m_SpecialInfectedArrowEnabled = getBool("SpecialInfectedArrowEnabled", m_SpecialInfectedArrowEnabled);
    m_SpecialInfectedDebug = getBool("SpecialInfectedDebug", m_SpecialInfectedDebug);
    m_SpecialInfectedArrowDebugLog = getBool("SpecialInfectedArrowDebugLog", m_SpecialInfectedArrowDebugLog);
    m_SpecialInfectedArrowDebugLogHz = std::clamp(getFloat("SpecialInfectedArrowDebugLogHz", m_SpecialInfectedArrowDebugLogHz), 0.0f, 20.0f);
    m_SpecialInfectedArrowSize = std::max(0.0f, getFloat("SpecialInfectedArrowSize", m_SpecialInfectedArrowSize));
    m_SpecialInfectedArrowHeight = std::max(0.0f, getFloat("SpecialInfectedArrowHeight", m_SpecialInfectedArrowHeight));
    m_SpecialInfectedArrowThickness = std::max(0.0f, getFloat("SpecialInfectedArrowThickness", m_SpecialInfectedArrowThickness));
    m_SpecialInfectedBlindSpotDistance = std::max(0.0f, getFloat("SpecialInfectedBlindSpotDistance", m_SpecialInfectedBlindSpotDistance));
    m_SpecialInfectedPreWarningAutoAimConfigEnabled = getBool("SpecialInfectedPreWarningAutoAimEnabled", m_SpecialInfectedPreWarningAutoAimConfigEnabled);
    m_SpecialInfectedPreWarningEvadeDistance = std::max(0.0f, getFloat("SpecialInfectedPreWarningEvadeDistance", m_SpecialInfectedPreWarningEvadeDistance));
    m_SpecialInfectedPreWarningEvadeCooldown = std::max(0.0f, getFloat("SpecialInfectedPreWarningEvadeCooldown", m_SpecialInfectedPreWarningEvadeCooldown));
    if (!m_SpecialInfectedPreWarningAutoAimConfigEnabled)
        m_SpecialInfectedPreWarningAutoAimEnabled = false;
    m_SpecialInfectedPreWarningDistance = std::max(0.0f, getFloat("SpecialInfectedPreWarningDistance", m_SpecialInfectedPreWarningDistance));
    m_SpecialInfectedPreWarningTargetUpdateInterval = std::max(0.0f, getFloat("SpecialInfectedPreWarningTargetUpdateInterval", m_SpecialInfectedPreWarningTargetUpdateInterval));
    m_SpecialInfectedOverlayMaxHz = std::max(0.0f, getFloat("SpecialInfectedOverlayMaxHz", m_SpecialInfectedOverlayMaxHz));
    m_SpecialInfectedTraceMaxHz = std::max(0.0f, getFloat("SpecialInfectedTraceMaxHz", m_SpecialInfectedTraceMaxHz));
    const float preWarningAimAngle = getFloat("SpecialInfectedPreWarningAimAngle", m_SpecialInfectedPreWarningAimAngle);
    m_SpecialInfectedPreWarningAimAngle = m_SpecialInfectedDebug
        ? std::max(0.0f, preWarningAimAngle)
        : std::clamp(preWarningAimAngle, 0.0f, 10.0f);

    const float aimSnapDistance = getFloat("SpecialInfectedPreWarningAimSnapDistance", m_SpecialInfectedPreWarningAimSnapDistance);
    m_SpecialInfectedPreWarningAimSnapDistance = m_SpecialInfectedDebug
        ? std::max(0.0f, aimSnapDistance)
        : std::clamp(aimSnapDistance, 0.0f, 30.0f);

    const float releaseDistance = getFloat("SpecialInfectedPreWarningAimReleaseDistance", m_SpecialInfectedPreWarningAimReleaseDistance);
    m_SpecialInfectedPreWarningAimReleaseDistance = m_SpecialInfectedDebug
        ? std::max(m_SpecialInfectedPreWarningAimSnapDistance, std::max(0.0f, releaseDistance))
        : std::clamp(std::max(m_SpecialInfectedPreWarningAimSnapDistance, std::max(0.0f, releaseDistance)), 0.0f, 40.0f);

    const float autoAimLerp = getFloat("SpecialInfectedAutoAimLerp", m_SpecialInfectedAutoAimLerp);
    m_SpecialInfectedAutoAimLerp = m_SpecialInfectedDebug
        ? std::max(0.0f, autoAimLerp)
        : std::clamp(autoAimLerp, 0.0f, 0.3f);

    const float autoAimCooldown = getFloat("SpecialInfectedAutoAimCooldown", m_SpecialInfectedAutoAimCooldown);
    m_SpecialInfectedAutoAimCooldown = m_SpecialInfectedDebug
        ? std::max(0.0f, autoAimCooldown)
        : std::max(0.5f, autoAimCooldown);
    m_SpecialInfectedWarningSecondaryHoldDuration = std::max(0.0f, getFloat("SpecialInfectedWarningSecondaryHoldDuration", m_SpecialInfectedWarningSecondaryHoldDuration));
    m_SpecialInfectedWarningPostAttackDelay = std::max(0.0f, getFloat("SpecialInfectedWarningPostAttackDelay", m_SpecialInfectedWarningPostAttackDelay));
    m_SpecialInfectedWarningJumpHoldDuration = std::max(0.0f, getFloat("SpecialInfectedWarningJumpHoldDuration", m_SpecialInfectedWarningJumpHoldDuration));
    auto specialInfectedArrowColor = getColor("SpecialInfectedArrowColor", m_SpecialInfectedArrowDefaultColor.r, m_SpecialInfectedArrowDefaultColor.g, m_SpecialInfectedArrowDefaultColor.b, 255);
    const bool hasGlobalArrowColor = userConfig.find("SpecialInfectedArrowColor") != userConfig.end();
    m_SpecialInfectedArrowDefaultColor.r = specialInfectedArrowColor[0];
    m_SpecialInfectedArrowDefaultColor.g = specialInfectedArrowColor[1];
    m_SpecialInfectedArrowDefaultColor.b = specialInfectedArrowColor[2];

    if (hasGlobalArrowColor)
    {
        for (auto& color : m_SpecialInfectedArrowColors)
        {
            color.r = m_SpecialInfectedArrowDefaultColor.r;
            color.g = m_SpecialInfectedArrowDefaultColor.g;
            color.b = m_SpecialInfectedArrowDefaultColor.b;
        }
    }

    static const std::array<std::pair<SpecialInfectedType, const char*>, 8> arrowColorConfigKeys =
    {
        std::make_pair(SpecialInfectedType::Boomer, "Boomer"),
        std::make_pair(SpecialInfectedType::Smoker, "Smoker"),
        std::make_pair(SpecialInfectedType::Hunter, "Hunter"),
        std::make_pair(SpecialInfectedType::Spitter, "Spitter"),
        std::make_pair(SpecialInfectedType::Jockey, "Jockey"),
        std::make_pair(SpecialInfectedType::Charger, "Charger"),
        std::make_pair(SpecialInfectedType::Tank, "Tank"),
        std::make_pair(SpecialInfectedType::Witch, "Witch")
    };

    for (const auto& [type, suffix] : arrowColorConfigKeys)
    {
        const size_t typeIndex = static_cast<size_t>(type);
        if (typeIndex >= m_SpecialInfectedArrowColors.size())
            continue;

        std::string key = std::string("SpecialInfectedArrowColor") + suffix;
        auto color = getColor(key.c_str(), m_SpecialInfectedArrowColors[typeIndex].r, m_SpecialInfectedArrowColors[typeIndex].g, m_SpecialInfectedArrowColors[typeIndex].b, 255);
        m_SpecialInfectedArrowColors[typeIndex].r = color[0];
        m_SpecialInfectedArrowColors[typeIndex].g = color[1];
        m_SpecialInfectedArrowColors[typeIndex].b = color[2];
    }

    static const std::array<std::pair<SpecialInfectedType, const char*>, 8> preWarningOffsetConfigKeys =
    {
        std::make_pair(SpecialInfectedType::Boomer, "Boomer"),
        std::make_pair(SpecialInfectedType::Smoker, "Smoker"),
        std::make_pair(SpecialInfectedType::Hunter, "Hunter"),
        std::make_pair(SpecialInfectedType::Spitter, "Spitter"),
        std::make_pair(SpecialInfectedType::Jockey, "Jockey"),
        std::make_pair(SpecialInfectedType::Charger, "Charger"),
        std::make_pair(SpecialInfectedType::Tank, "Tank"),
        std::make_pair(SpecialInfectedType::Witch, "Witch")
    };

    for (const auto& [type, suffix] : preWarningOffsetConfigKeys)
    {
        const size_t typeIndex = static_cast<size_t>(type);
        if (typeIndex >= m_SpecialInfectedPreWarningAimOffsets.size())
            continue;

        std::string key = std::string("SpecialInfectedPreWarningAimOffset") + suffix;
        m_SpecialInfectedPreWarningAimOffsets[typeIndex] = getVector3(key.c_str(), m_SpecialInfectedPreWarningAimOffsets[typeIndex]);
    }

    // Client shadow manager / flashlight shadow controls discovered from client.dll.
    m_ShadowTweaksEnabled = getBool("ShadowTweaksEnabled", m_ShadowTweaksEnabled);
    m_ShadowCvarShadows = std::clamp(getInt("r_shadows", m_ShadowCvarShadows), 0, 1);
    m_ShadowCvarRenderToTexture = std::clamp(getInt("r_shadowrendertotexture", m_ShadowCvarRenderToTexture), 0, 1);
    m_ShadowCvarFlashlightDepthTexture = std::clamp(getInt("r_flashlightdepthtexture", m_ShadowCvarFlashlightDepthTexture), 0, 1);
    m_ShadowCvarFlashlightDepthRes = std::clamp(getInt("r_flashlightdepthres", m_ShadowCvarFlashlightDepthRes), 64, 2048);
    m_ShadowCvarHalfUpdateRate = std::clamp(getInt("r_shadow_half_update_rate", m_ShadowCvarHalfUpdateRate), 0, 1);
    m_ShadowCvarMaxRendered = std::clamp(getInt("r_shadowmaxrendered", m_ShadowCvarMaxRendered), 0, 32);
    m_ShadowCvarMaxRenderableDist = std::clamp(getFloat("cl_max_shadow_renderable_dist", m_ShadowCvarMaxRenderableDist), 0.0f, 8192.0f);
    m_ShadowCvarFlashlightDetailProps = std::clamp(getInt("r_FlashlightDetailProps", m_ShadowCvarFlashlightDetailProps), 0, 2);
    m_ShadowCvarMobSimpleShadows = std::clamp(getInt("z_mob_simple_shadows", m_ShadowCvarMobSimpleShadows), 0, 2);
    m_ShadowCvarWorldLightShadows = std::clamp(getInt("r_shadowfromworldlights", m_ShadowCvarWorldLightShadows), 0, 1);
    m_ShadowCvarFlashlightModels = std::clamp(getInt("r_flashlightmodels", m_ShadowCvarFlashlightModels), 0, 1);
    m_ShadowCvarShadowsOnRenderables = std::clamp(getInt("r_shadows_on_renderables_enable", m_ShadowCvarShadowsOnRenderables), 0, 1);
    m_ShadowCvarFlashlightRenderModels = std::clamp(getInt("r_flashlightrendermodels", m_ShadowCvarFlashlightRenderModels), 0, 1);
    m_ShadowCvarPlayerShadowDist = std::clamp(getFloat("cl_player_shadow_dist", m_ShadowCvarPlayerShadowDist), 0.0f, 8192.0f);
    m_ShadowCvarInfectedShadows = std::clamp(getInt("z_infected_shadows", m_ShadowCvarInfectedShadows), 0, 1);
    m_ShadowCvarNbShadowBlobbyDist = std::clamp(getFloat("nb_shadow_blobby_dist", m_ShadowCvarNbShadowBlobbyDist), 0.0f, 8192.0f);
    m_ShadowCvarNbShadowCullDist = std::clamp(getFloat("nb_shadow_cull_dist", m_ShadowCvarNbShadowCullDist), 0.0f, 8192.0f);
    m_ShadowCvarFlashlightInfectedShadows = std::clamp(getInt("r_flashlightinfectedshadows", m_ShadowCvarFlashlightInfectedShadows), 0, 1);
    m_ShadowEntityTweaksEnabled = getBool("ShadowEntityTweaksEnabled", m_ShadowEntityTweaksEnabled);
    m_ShadowEntityDisableShadows = getBool("ShadowControlDisableShadows", m_ShadowEntityDisableShadows);
    m_ShadowEntityMaxDist = std::clamp(getFloat("ShadowControlMaxDist", m_ShadowEntityMaxDist), 0.0f, 8192.0f);
    m_ShadowEntityLocalLightShadows = getBool("ShadowControlLocalLightShadows", m_ShadowEntityLocalLightShadows);
    m_ShadowProjectedTextureEnableShadows = getBool("ProjectedTextureEnableShadows", m_ShadowProjectedTextureEnableShadows);
    m_ShadowProjectedTextureQuality = std::clamp(getInt("ProjectedTextureShadowQuality", m_ShadowProjectedTextureQuality), 0, 3);
    if (m_ShadowEntityTweaksEnabled)
    {
        Game::logMsg("[VR][ShadowTweaks] Entity shadow overrides are temporarily disabled for stability.");
        m_ShadowEntityTweaksEnabled = false;
        ResetShadowEntityOverrideTracking();
    }
    m_ShadowSettingsDirty.store(true, std::memory_order_release);

    m_WriteOnlyPerformanceTweaksEnabled =
        getBool("WriteOnlyPerformanceTweaksEnabled", m_WriteOnlyPerformanceTweaksEnabled);
    m_WriteOnlyCvarThreadedBoneSetup =
        std::clamp(getInt("cl_threaded_bone_setup", m_WriteOnlyCvarThreadedBoneSetup), 0, 1);
    m_WriteOnlyCvarThreadedParticles =
        std::clamp(getInt("r_threaded_particles", m_WriteOnlyCvarThreadedParticles), 0, 1);
    m_WriteOnlyCvarQueuedDecals =
        std::clamp(getInt("r_queued_decals", m_WriteOnlyCvarQueuedDecals), 0, 1);
    m_WriteOnlyCvarQueuedPostProcessing =
        std::clamp(getInt("r_queued_post_processing", m_WriteOnlyCvarQueuedPostProcessing), 0, 1);
    m_WriteOnlyCvarRootLod =
        std::clamp(getInt("r_rootlod", m_WriteOnlyCvarRootLod), -1, 2);
    m_WriteOnlyCvarPropsMaxDist =
        std::clamp(getFloat("r_propsmaxdist", m_WriteOnlyCvarPropsMaxDist), 0.0f, 32768.0f);
    m_WriteOnlyCvarDetailDist =
        std::clamp(getFloat("cl_detaildist", m_WriteOnlyCvarDetailDist), 0.0f, 32768.0f);
    m_WriteOnlyCvarDetailFade =
        std::clamp(getFloat("cl_detailfade", m_WriteOnlyCvarDetailFade), 0.0f, 32768.0f);
    m_WriteOnlyPerformanceSettingsDirty.store(true, std::memory_order_release);
    m_LocalVScriptConvarsEnabled =
        getBool("LocalVScriptConvarsEnabled", m_LocalVScriptConvarsEnabled);
    m_LocalVScriptConvarsBlockExternalWrites =
        getBool("LocalVScriptConvarsBlockExternalWrites", m_LocalVScriptConvarsBlockExternalWrites);
    m_LocalVScriptConvarsPath =
        getString("LocalVScriptConvarsPath", m_LocalVScriptConvarsPath);
    if (m_LocalVScriptConvarsPath.empty())
        m_LocalVScriptConvarsPath = "VR\\local_client_convars.nut";
    m_LocalVScriptConvarsDirty.store(true, std::memory_order_release);
    m_AutoFlashlightEnabled = getBool("AutoFlashlightEnabled", m_AutoFlashlightEnabled);
    m_AutoFlashlightDarkThreshold =
        std::clamp(getFloat("AutoFlashlightDarkThreshold", m_AutoFlashlightDarkThreshold), 0.0f, 255.0f);
    m_AutoFlashlightBrightThreshold =
        std::clamp(getFloat("AutoFlashlightBrightThreshold", m_AutoFlashlightBrightThreshold), 0.0f, 255.0f);
    if (m_AutoFlashlightBrightThreshold < m_AutoFlashlightDarkThreshold)
        m_AutoFlashlightBrightThreshold = m_AutoFlashlightDarkThreshold;
    m_AutoFlashlightSampleInterval =
        std::clamp(getFloat("AutoFlashlightSampleInterval", m_AutoFlashlightSampleInterval), 0.05f, 2.0f);
    m_AutoFlashlightForwardNearDistance =
        std::clamp(getFloat("AutoFlashlightForwardNearDistance", m_AutoFlashlightForwardNearDistance), 0.0f, 512.0f);
    m_AutoFlashlightForwardFarDistance =
        std::clamp(getFloat("AutoFlashlightForwardFarDistance", m_AutoFlashlightForwardFarDistance), 0.0f, 512.0f);
    if (m_AutoFlashlightForwardFarDistance < m_AutoFlashlightForwardNearDistance)
        m_AutoFlashlightForwardFarDistance = m_AutoFlashlightForwardNearDistance;
    m_AutoFlashlightLateralOffset =
        std::clamp(getFloat("AutoFlashlightLateralOffset", m_AutoFlashlightLateralOffset), 0.0f, 256.0f);
    m_AutoFlashlightVerticalOffset =
        std::clamp(getFloat("AutoFlashlightVerticalOffset", m_AutoFlashlightVerticalOffset), -128.0f, 128.0f);
    m_AutoFlashlightSmoothingTime =
        std::clamp(getFloat("AutoFlashlightSmoothingTime", m_AutoFlashlightSmoothingTime), 0.0f, 5.0f);
    m_AutoFlashlightMinOnTime =
        std::clamp(getFloat("AutoFlashlightMinOnTime", m_AutoFlashlightMinOnTime), 0.0f, 30.0f);
    m_AutoFlashlightMinOffTime =
        std::clamp(getFloat("AutoFlashlightMinOffTime", m_AutoFlashlightMinOffTime), 0.0f, 30.0f);
    m_AutoFlashlightManualOverrideSeconds =
        std::clamp(getFloat("AutoFlashlightManualOverrideSeconds", m_AutoFlashlightManualOverrideSeconds), 0.0f, 60.0f);
    m_AutoFlashlightDebugLog = getBool("AutoFlashlightDebugLog", m_AutoFlashlightDebugLog);
    m_AutoFlashlightDebugLogHz =
        std::clamp(getFloat("AutoFlashlightDebugLogHz", m_AutoFlashlightDebugLogHz), 0.0f, 20.0f);

    ParseHapticsConfigFile();
}

template <typename T>
static inline T VR_ReadShadowEntityField(const void* entity, int offset, T fallback)
{
    if (!entity || offset < 0)
        return fallback;

    __try
    {
        return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(entity) + offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return fallback;
    }
}

template <typename T>
static inline void VR_WriteShadowEntityField(void* entity, int offset, T value)
{
    if (!entity || offset < 0)
        return;

    __try
    {
        *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(entity) + offset) = value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void VR::CaptureShadowCvarDefaults()
{
    if (!m_Game)
        return;

    m_ShadowOrigShadows = m_Game->GetConVarInt("r_shadows", m_ShadowOrigShadows);
    m_ShadowOrigRenderToTexture = m_Game->GetConVarInt("r_shadowrendertotexture", m_ShadowOrigRenderToTexture);
    m_ShadowOrigFlashlightDepthTexture = m_Game->GetConVarInt("r_flashlightdepthtexture", m_ShadowOrigFlashlightDepthTexture);
    m_ShadowOrigFlashlightDepthRes = m_Game->GetConVarInt("r_flashlightdepthres", m_ShadowOrigFlashlightDepthRes);
    m_ShadowOrigHalfUpdateRate = m_Game->GetConVarInt("r_shadow_half_update_rate", m_ShadowOrigHalfUpdateRate);
    m_ShadowOrigMaxRendered = m_Game->GetConVarInt("r_shadowmaxrendered", m_ShadowOrigMaxRendered);
    m_ShadowOrigMaxRenderableDist = m_Game->GetConVarFloat("cl_max_shadow_renderable_dist", m_ShadowOrigMaxRenderableDist);
    m_ShadowOrigFlashlightDetailProps = m_Game->GetConVarInt("r_FlashlightDetailProps", m_ShadowOrigFlashlightDetailProps);
    m_ShadowOrigMobSimpleShadows = m_Game->GetConVarInt("z_mob_simple_shadows", m_ShadowOrigMobSimpleShadows);
    m_ShadowOrigWorldLightShadows = m_Game->GetConVarInt("r_shadowfromworldlights", m_ShadowOrigWorldLightShadows);
    m_ShadowOrigFlashlightModels = m_Game->GetConVarInt("r_flashlightmodels", m_ShadowOrigFlashlightModels);
    m_ShadowOrigShadowsOnRenderables = m_Game->GetConVarInt("r_shadows_on_renderables_enable", m_ShadowOrigShadowsOnRenderables);
    m_ShadowOrigFlashlightRenderModels = m_Game->GetConVarInt("r_flashlightrendermodels", m_ShadowOrigFlashlightRenderModels);
    m_ShadowOrigPlayerShadowDist = m_Game->GetConVarFloat("cl_player_shadow_dist", m_ShadowOrigPlayerShadowDist);
    m_ShadowOrigInfectedShadows = m_Game->GetConVarInt("z_infected_shadows", m_ShadowOrigInfectedShadows);
    m_ShadowOrigNbShadowBlobbyDist = m_Game->GetConVarFloat("nb_shadow_blobby_dist", m_ShadowOrigNbShadowBlobbyDist);
    m_ShadowOrigNbShadowCullDist = m_Game->GetConVarFloat("nb_shadow_cull_dist", m_ShadowOrigNbShadowCullDist);
    m_ShadowOrigFlashlightInfectedShadows = m_Game->GetConVarInt("r_flashlightinfectedshadows", m_ShadowOrigFlashlightInfectedShadows);
    m_ShadowOriginalsCaptured = true;
}

void VR::RestoreShadowCvarDefaults()
{
    if (!m_Game || !m_ShadowOriginalsCaptured)
        return;

    m_Game->SetConVarInt("r_shadows", m_ShadowOrigShadows);
    m_Game->SetConVarInt("r_shadowrendertotexture", m_ShadowOrigRenderToTexture);
    m_Game->SetConVarInt("r_flashlightdepthtexture", m_ShadowOrigFlashlightDepthTexture);
    m_Game->SetConVarInt("r_flashlightdepthres", m_ShadowOrigFlashlightDepthRes);
    m_Game->SetConVarInt("r_shadow_half_update_rate", m_ShadowOrigHalfUpdateRate);
    m_Game->SetConVarInt("r_shadowmaxrendered", m_ShadowOrigMaxRendered);
    m_Game->SetConVarFloat("cl_max_shadow_renderable_dist", m_ShadowOrigMaxRenderableDist);
    m_Game->SetConVarInt("r_FlashlightDetailProps", m_ShadowOrigFlashlightDetailProps);
    m_Game->SetConVarInt("z_mob_simple_shadows", m_ShadowOrigMobSimpleShadows);
    m_Game->SetConVarInt("r_shadowfromworldlights", m_ShadowOrigWorldLightShadows);
    m_Game->SetConVarInt("r_flashlightmodels", m_ShadowOrigFlashlightModels);
    m_Game->SetConVarInt("r_shadows_on_renderables_enable", m_ShadowOrigShadowsOnRenderables);
    m_Game->SetConVarInt("r_flashlightrendermodels", m_ShadowOrigFlashlightRenderModels);
    m_Game->SetConVarFloat("cl_player_shadow_dist", m_ShadowOrigPlayerShadowDist);
    m_Game->SetConVarInt("z_infected_shadows", m_ShadowOrigInfectedShadows);
    m_Game->SetConVarFloat("nb_shadow_blobby_dist", m_ShadowOrigNbShadowBlobbyDist);
    m_Game->SetConVarFloat("nb_shadow_cull_dist", m_ShadowOrigNbShadowCullDist);
    m_Game->SetConVarInt("r_flashlightinfectedshadows", m_ShadowOrigFlashlightInfectedShadows);
}

void VR::ResetShadowEntityOverrideTracking()
{
    m_ShadowEntityOverridesApplied = false;
    m_ShadowEntityLastRefreshTime = {};
    m_ShadowEntityOffsetsLogged = false;
    m_ShadowControlEntityDefaults.clear();
    m_EnvProjectedTextureEntityDefaults.clear();
}

void VR::RestoreShadowEntityDefaults()
{
    if (!m_Game || !m_Game->m_ClientEntityList)
    {
        ResetShadowEntityOverrideTracking();
        return;
    }

    const int shadowControlDisableOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_bDisableShadows");
    const int shadowControlMaxDistOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_flShadowMaxDist");
    const int shadowControlLocalLightOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_bEnableLocalLightShadows");
    const int projectedTextureEnableOffset = m_Game->FindRecvPropOffset("CEnvProjectedTexture", "m_bEnableShadows");
    const int projectedTextureQualityOffset = m_Game->FindRecvPropOffset("CEnvProjectedTexture", "m_nShadowQuality");

    int restoredShadowControls = 0;
    for (const auto& [entityIndex, defaults] : m_ShadowControlEntityDefaults)
    {
        C_BaseEntity* entity = m_Game->GetClientEntity(entityIndex);
        if (!entity)
            continue;

        VR_WriteShadowEntityField<bool>(entity, shadowControlDisableOffset, defaults.disableShadows);
        VR_WriteShadowEntityField<float>(entity, shadowControlMaxDistOffset, defaults.maxDist);
        VR_WriteShadowEntityField<bool>(entity, shadowControlLocalLightOffset, defaults.enableLocalLightShadows);
        ++restoredShadowControls;
    }

    int restoredProjectedTextures = 0;
    for (const auto& [entityIndex, defaults] : m_EnvProjectedTextureEntityDefaults)
    {
        C_BaseEntity* entity = m_Game->GetClientEntity(entityIndex);
        if (!entity)
            continue;

        VR_WriteShadowEntityField<bool>(entity, projectedTextureEnableOffset, defaults.enableShadows);
        VR_WriteShadowEntityField<int>(entity, projectedTextureQualityOffset, defaults.shadowQuality);
        ++restoredProjectedTextures;
    }

    if (restoredShadowControls > 0 || restoredProjectedTextures > 0)
    {
        Game::logMsg(
            "[VR][ShadowTweaks] Restored entity shadow defaults for %d ShadowControl and %d EnvProjectedTexture entities.",
            restoredShadowControls,
            restoredProjectedTextures);
    }

    ResetShadowEntityOverrideTracking();
}

void VR::ApplyShadowEntityOverrides(bool forceRefresh)
{
    if (!m_Game || !m_Game->m_ClientEntityList || !m_Game->m_EngineClient)
        return;

    if (!m_Game->m_EngineClient->IsInGame())
    {
        ResetShadowEntityOverrideTracking();
        return;
    }

    if (!m_ShadowEntityTweaksEnabled)
    {
        if (m_ShadowEntityOverridesApplied)
            RestoreShadowEntityDefaults();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!forceRefresh && m_ShadowEntityLastRefreshTime != std::chrono::steady_clock::time_point{} &&
        now - m_ShadowEntityLastRefreshTime < std::chrono::milliseconds(250))
    {
        return;
    }

    const int shadowControlDisableOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_bDisableShadows");
    const int shadowControlMaxDistOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_flShadowMaxDist");
    const int shadowControlLocalLightOffset = m_Game->FindRecvPropOffset("CShadowControl", "m_bEnableLocalLightShadows");
    const int projectedTextureEnableOffset = m_Game->FindRecvPropOffset("CEnvProjectedTexture", "m_bEnableShadows");
    const int projectedTextureQualityOffset = m_Game->FindRecvPropOffset("CEnvProjectedTexture", "m_nShadowQuality");

    const bool hasShadowControlOffsets =
        shadowControlDisableOffset >= 0 && shadowControlMaxDistOffset >= 0 && shadowControlLocalLightOffset >= 0;
    const bool hasProjectedTextureOffsets =
        projectedTextureEnableOffset >= 0 && projectedTextureQualityOffset >= 0;

    if (!hasShadowControlOffsets && !hasProjectedTextureOffsets)
    {
        if (!m_ShadowEntityOffsetsLogged)
        {
            Game::logMsg("[VR][ShadowTweaks] Failed to resolve ShadowControl / EnvProjectedTexture recv props from client.dll.");
            m_ShadowEntityOffsetsLogged = true;
        }
        m_ShadowEntityTweaksEnabled = false;
        return;
    }

    const int highestEntityIndex = m_Game->m_ClientEntityList->GetHighestEntityIndex();
    int touchedShadowControls = 0;
    int touchedProjectedTextures = 0;

    for (int entityIndex = 0; entityIndex <= highestEntityIndex; ++entityIndex)
    {
        C_BaseEntity* entity = m_Game->GetClientEntity(entityIndex);
        if (!entity)
            continue;

        const char* className = m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(entity));
        if (!className || !*className)
            continue;

        if (hasShadowControlOffsets &&
            (std::strcmp(className, "CShadowControl") == 0 || std::strcmp(className, "C_ShadowControl") == 0))
        {
            auto [it, inserted] = m_ShadowControlEntityDefaults.try_emplace(entityIndex);
            if (inserted)
            {
                it->second.disableShadows = VR_ReadShadowEntityField<bool>(entity, shadowControlDisableOffset, false);
                it->second.maxDist = VR_ReadShadowEntityField<float>(entity, shadowControlMaxDistOffset, 0.0f);
                it->second.enableLocalLightShadows =
                    VR_ReadShadowEntityField<bool>(entity, shadowControlLocalLightOffset, false);
            }

            VR_WriteShadowEntityField<bool>(entity, shadowControlDisableOffset, m_ShadowEntityDisableShadows);
            VR_WriteShadowEntityField<float>(entity, shadowControlMaxDistOffset, m_ShadowEntityMaxDist);
            VR_WriteShadowEntityField<bool>(entity, shadowControlLocalLightOffset, m_ShadowEntityLocalLightShadows);
            ++touchedShadowControls;
            continue;
        }

        if (hasProjectedTextureOffsets &&
            (std::strcmp(className, "CEnvProjectedTexture") == 0 || std::strcmp(className, "C_EnvProjectedTexture") == 0))
        {
            auto [it, inserted] = m_EnvProjectedTextureEntityDefaults.try_emplace(entityIndex);
            if (inserted)
            {
                it->second.enableShadows = VR_ReadShadowEntityField<bool>(entity, projectedTextureEnableOffset, true);
                it->second.shadowQuality = VR_ReadShadowEntityField<int>(entity, projectedTextureQualityOffset, 0);
            }

            VR_WriteShadowEntityField<bool>(entity, projectedTextureEnableOffset, m_ShadowProjectedTextureEnableShadows);
            VR_WriteShadowEntityField<int>(entity, projectedTextureQualityOffset, m_ShadowProjectedTextureQuality);
            ++touchedProjectedTextures;
        }
    }

    m_ShadowEntityLastRefreshTime = now;
    m_ShadowEntityOffsetsLogged = false;
    m_ShadowEntityOverridesApplied = (touchedShadowControls > 0 || touchedProjectedTextures > 0);

    if (forceRefresh || touchedShadowControls > 0 || touchedProjectedTextures > 0)
    {
        Game::logMsg(
            "[VR][ShadowTweaks] Applied entity shadow overrides: ShadowControl=%d EnvProjectedTexture=%d "
            "disable=%d maxdist=%.1f localLight=%d projectedShadows=%d projectedQuality=%d",
            touchedShadowControls,
            touchedProjectedTextures,
            m_ShadowEntityDisableShadows ? 1 : 0,
            m_ShadowEntityMaxDist,
            m_ShadowEntityLocalLightShadows ? 1 : 0,
            m_ShadowProjectedTextureEnableShadows ? 1 : 0,
            m_ShadowProjectedTextureQuality);
    }
}

void VR::ApplyShadowSettingsIfNeeded()
{
    const bool dirty = m_ShadowSettingsDirty.exchange(false, std::memory_order_acq_rel);
    const bool wantsEntityRefresh = false;
    const bool needsEntityRefresh =
        wantsEntityRefresh &&
        (dirty ||
            m_ShadowEntityLastRefreshTime == std::chrono::steady_clock::time_point{} ||
            (std::chrono::steady_clock::now() - m_ShadowEntityLastRefreshTime) >= std::chrono::milliseconds(250));

    if (!dirty && !needsEntityRefresh)
        return;

    if (!m_Game || !m_Game->m_Initialized)
    {
        m_ShadowSettingsDirty.store(true, std::memory_order_release);
        return;
    }

    if (!dirty)
        return;

    if (!m_ShadowTweaksEnabled)
    {
        if (m_ShadowTweaksApplied)
        {
            RestoreShadowCvarDefaults();
            Game::logMsg("[VR][ShadowTweaks] Restored original shadow cvars.");
        }

        m_ShadowTweaksApplied = false;
        return;
    }

    if (!m_ShadowOriginalsCaptured)
        CaptureShadowCvarDefaults();

    int appliedCount = 0;
    auto applyInt = [&](const char* name, int value)
        {
            if (m_Game->SetConVarInt(name, value))
                ++appliedCount;
        };

    applyInt("r_shadows", m_ShadowCvarShadows);
    applyInt("r_shadowrendertotexture", m_ShadowCvarRenderToTexture);
    applyInt("r_flashlightdepthtexture", m_ShadowCvarFlashlightDepthTexture);
    applyInt("r_flashlightdepthres", m_ShadowCvarFlashlightDepthRes);
    applyInt("r_shadow_half_update_rate", m_ShadowCvarHalfUpdateRate);
    applyInt("r_shadowmaxrendered", m_ShadowCvarMaxRendered);
    if (m_Game->SetConVarFloat("cl_max_shadow_renderable_dist", m_ShadowCvarMaxRenderableDist))
        ++appliedCount;
    applyInt("r_FlashlightDetailProps", m_ShadowCvarFlashlightDetailProps);
    applyInt("z_mob_simple_shadows", m_ShadowCvarMobSimpleShadows);
    applyInt("r_shadowfromworldlights", m_ShadowCvarWorldLightShadows);
    applyInt("r_flashlightmodels", m_ShadowCvarFlashlightModels);
    applyInt("r_shadows_on_renderables_enable", m_ShadowCvarShadowsOnRenderables);
    applyInt("r_flashlightrendermodels", m_ShadowCvarFlashlightRenderModels);
    if (m_Game->SetConVarFloat("cl_player_shadow_dist", m_ShadowCvarPlayerShadowDist))
        ++appliedCount;
    applyInt("z_infected_shadows", m_ShadowCvarInfectedShadows);
    if (m_Game->SetConVarFloat("nb_shadow_blobby_dist", m_ShadowCvarNbShadowBlobbyDist))
        ++appliedCount;
    if (m_Game->SetConVarFloat("nb_shadow_cull_dist", m_ShadowCvarNbShadowCullDist))
        ++appliedCount;
    applyInt("r_flashlightinfectedshadows", m_ShadowCvarFlashlightInfectedShadows);

    m_ShadowTweaksApplied = (appliedCount > 0);
    Game::logMsg(
        "[VR][ShadowTweaks] Applied %d shadow controls: r_shadows=%d r_shadowrendertotexture=%d "
        "r_flashlightdepthtexture=%d r_flashlightdepthres=%d r_shadow_half_update_rate=%d "
        "r_shadowmaxrendered=%d cl_max_shadow_renderable_dist=%.1f r_FlashlightDetailProps=%d "
        "z_mob_simple_shadows=%d r_shadowfromworldlights=%d r_flashlightmodels=%d "
        "r_shadows_on_renderables_enable=%d r_flashlightrendermodels=%d cl_player_shadow_dist=%.1f "
        "z_infected_shadows=%d nb_shadow_blobby_dist=%.1f nb_shadow_cull_dist=%.1f "
        "r_flashlightinfectedshadows=%d",
        appliedCount,
        m_ShadowCvarShadows,
        m_ShadowCvarRenderToTexture,
        m_ShadowCvarFlashlightDepthTexture,
        m_ShadowCvarFlashlightDepthRes,
        m_ShadowCvarHalfUpdateRate,
        m_ShadowCvarMaxRendered,
        m_ShadowCvarMaxRenderableDist,
        m_ShadowCvarFlashlightDetailProps,
        m_ShadowCvarMobSimpleShadows,
        m_ShadowCvarWorldLightShadows,
        m_ShadowCvarFlashlightModels,
        m_ShadowCvarShadowsOnRenderables,
        m_ShadowCvarFlashlightRenderModels,
        m_ShadowCvarPlayerShadowDist,
        m_ShadowCvarInfectedShadows,
        m_ShadowCvarNbShadowBlobbyDist,
        m_ShadowCvarNbShadowCullDist,
        m_ShadowCvarFlashlightInfectedShadows);
}

void VR::ApplyWriteOnlyPerformanceSettingsIfNeeded()
{
    const bool dirty = m_WriteOnlyPerformanceSettingsDirty.exchange(false, std::memory_order_acq_rel);
    if (!dirty)
        return;

    if (!m_Game || !m_Game->m_Initialized)
    {
        m_WriteOnlyPerformanceSettingsDirty.store(true, std::memory_order_release);
        return;
    }

    if (!m_WriteOnlyPerformanceTweaksEnabled)
    {
        if (m_WriteOnlyPerformanceTweaksApplied && m_WriteOnlyPerformanceOriginalsCaptured)
        {
            RestoreWriteOnlyPerformanceDefaults();
            Game::logMsg("[VR][WriteOnlyPerfTweaks] Disabled; restored one-shot snapshot defaults.");
        }

        m_WriteOnlyPerformanceTweaksApplied = false;
        return;
    }

    if (!m_WriteOnlyPerformanceOriginalsCaptured)
        CaptureWriteOnlyPerformanceDefaults();

    int appliedCount = 0;
    int verifiedCount = 0;

    struct IntWriteResult
    {
        bool setOk = false;
        int readback = 0;
        bool readbackMatches = false;
    };

    struct FloatWriteResult
    {
        bool setOk = false;
        float readback = 0.0f;
        bool readbackMatches = false;
    };

    auto applyInt = [&](const char* name, int value) -> IntWriteResult
        {
            IntWriteResult result{};
            result.setOk = m_Game->SetConVarInt(name, value);
            result.readback = m_Game->GetConVarInt(name, INT32_MIN);
            result.readbackMatches = (result.readback == value);
            if (result.setOk)
                ++appliedCount;
            if (result.setOk && result.readbackMatches)
                ++verifiedCount;
            return result;
        };

    auto applyFloat = [&](const char* name, float value) -> FloatWriteResult
        {
            FloatWriteResult result{};
            result.setOk = m_Game->SetConVarFloat(name, value);
            result.readback = m_Game->GetConVarFloat(name, -999999.0f);
            result.readbackMatches = (std::fabs(result.readback - value) <= 0.01f);
            if (result.setOk)
                ++appliedCount;
            if (result.setOk && result.readbackMatches)
                ++verifiedCount;
            return result;
        };

    const IntWriteResult threadedBoneSetup = applyInt("cl_threaded_bone_setup", m_WriteOnlyCvarThreadedBoneSetup);
    const IntWriteResult threadedParticles = applyInt("r_threaded_particles", m_WriteOnlyCvarThreadedParticles);
    const IntWriteResult queuedDecals = applyInt("r_queued_decals", m_WriteOnlyCvarQueuedDecals);
    const IntWriteResult queuedPostProcessing = applyInt("r_queued_post_processing", m_WriteOnlyCvarQueuedPostProcessing);
    const IntWriteResult rootLod = applyInt("r_rootlod", m_WriteOnlyCvarRootLod);
    const FloatWriteResult propsMaxDist = applyFloat("r_propsmaxdist", m_WriteOnlyCvarPropsMaxDist);
    const FloatWriteResult detailDist = applyFloat("cl_detaildist", m_WriteOnlyCvarDetailDist);
    const FloatWriteResult detailFade = applyFloat("cl_detailfade", m_WriteOnlyCvarDetailFade);

    m_WriteOnlyPerformanceTweaksApplied = (appliedCount > 0);
    Game::logMsg(
        "[VR][WriteOnlyPerfTweaks] Applied %d/8 write-only controls, verified %d/8 by readback: "
        "cl_threaded_bone_setup=%d(set=%d read=%d ok=%d) "
        "r_threaded_particles=%d(set=%d read=%d ok=%d) "
        "r_queued_decals=%d(set=%d read=%d ok=%d) "
        "r_queued_post_processing=%d(set=%d read=%d ok=%d) "
        "r_rootlod=%d(set=%d read=%d ok=%d) "
        "r_propsmaxdist=%.1f(set=%d read=%.1f ok=%d) "
        "cl_detaildist=%.1f(set=%d read=%.1f ok=%d) "
        "cl_detailfade=%.1f(set=%d read=%.1f ok=%d)",
        appliedCount,
        verifiedCount,
        m_WriteOnlyCvarThreadedBoneSetup,
        threadedBoneSetup.setOk ? 1 : 0,
        threadedBoneSetup.readback,
        threadedBoneSetup.readbackMatches ? 1 : 0,
        m_WriteOnlyCvarThreadedParticles,
        threadedParticles.setOk ? 1 : 0,
        threadedParticles.readback,
        threadedParticles.readbackMatches ? 1 : 0,
        m_WriteOnlyCvarQueuedDecals,
        queuedDecals.setOk ? 1 : 0,
        queuedDecals.readback,
        queuedDecals.readbackMatches ? 1 : 0,
        m_WriteOnlyCvarQueuedPostProcessing,
        queuedPostProcessing.setOk ? 1 : 0,
        queuedPostProcessing.readback,
        queuedPostProcessing.readbackMatches ? 1 : 0,
        m_WriteOnlyCvarRootLod,
        rootLod.setOk ? 1 : 0,
        rootLod.readback,
        rootLod.readbackMatches ? 1 : 0,
        m_WriteOnlyCvarPropsMaxDist,
        propsMaxDist.setOk ? 1 : 0,
        propsMaxDist.readback,
        propsMaxDist.readbackMatches ? 1 : 0,
        m_WriteOnlyCvarDetailDist,
        detailDist.setOk ? 1 : 0,
        detailDist.readback,
        detailDist.readbackMatches ? 1 : 0,
        m_WriteOnlyCvarDetailFade,
        detailFade.setOk ? 1 : 0,
        detailFade.readback,
        detailFade.readbackMatches ? 1 : 0);
}

void VR::CaptureWriteOnlyPerformanceDefaults()
{
    if (!m_Game)
        return;

    m_WriteOnlyOrigThreadedBoneSetup =
        m_Game->GetConVarInt("cl_threaded_bone_setup", m_WriteOnlyOrigThreadedBoneSetup);
    m_WriteOnlyOrigThreadedParticles =
        m_Game->GetConVarInt("r_threaded_particles", m_WriteOnlyOrigThreadedParticles);
    m_WriteOnlyOrigQueuedDecals =
        m_Game->GetConVarInt("r_queued_decals", m_WriteOnlyOrigQueuedDecals);
    m_WriteOnlyOrigQueuedPostProcessing =
        m_Game->GetConVarInt("r_queued_post_processing", m_WriteOnlyOrigQueuedPostProcessing);
    m_WriteOnlyOrigRootLod =
        m_Game->GetConVarInt("r_rootlod", m_WriteOnlyOrigRootLod);
    m_WriteOnlyOrigPropsMaxDist =
        m_Game->GetConVarFloat("r_propsmaxdist", m_WriteOnlyOrigPropsMaxDist);
    m_WriteOnlyOrigDetailDist =
        m_Game->GetConVarFloat("cl_detaildist", m_WriteOnlyOrigDetailDist);
    m_WriteOnlyOrigDetailFade =
        m_Game->GetConVarFloat("cl_detailfade", m_WriteOnlyOrigDetailFade);
    m_WriteOnlyPerformanceOriginalsCaptured = true;

    Game::logMsg(
        "[VR][WriteOnlyPerfTweaks] Captured one-shot snapshot defaults: cl_threaded_bone_setup=%d "
        "r_threaded_particles=%d r_queued_decals=%d r_queued_post_processing=%d r_rootlod=%d "
        "r_propsmaxdist=%.1f cl_detaildist=%.1f cl_detailfade=%.1f",
        m_WriteOnlyOrigThreadedBoneSetup,
        m_WriteOnlyOrigThreadedParticles,
        m_WriteOnlyOrigQueuedDecals,
        m_WriteOnlyOrigQueuedPostProcessing,
        m_WriteOnlyOrigRootLod,
        m_WriteOnlyOrigPropsMaxDist,
        m_WriteOnlyOrigDetailDist,
        m_WriteOnlyOrigDetailFade);
}

void VR::RestoreWriteOnlyPerformanceDefaults()
{
    if (!m_Game || !m_WriteOnlyPerformanceOriginalsCaptured)
        return;

    int restoredCount = 0;
    int verifiedCount = 0;

    auto restoreInt = [&](const char* name, int value)
        {
            const bool setOk = m_Game->SetConVarInt(name, value);
            const int readback = m_Game->GetConVarInt(name, INT32_MIN);
            const bool readbackMatches = (readback == value);
            if (setOk)
                ++restoredCount;
            if (setOk && readbackMatches)
                ++verifiedCount;
            return std::make_pair(readback, readbackMatches);
        };

    auto restoreFloat = [&](const char* name, float value)
        {
            const bool setOk = m_Game->SetConVarFloat(name, value);
            const float readback = m_Game->GetConVarFloat(name, -999999.0f);
            const bool readbackMatches = (std::fabs(readback - value) <= 0.01f);
            if (setOk)
                ++restoredCount;
            if (setOk && readbackMatches)
                ++verifiedCount;
            return std::make_pair(readback, readbackMatches);
        };

    const auto threadedBoneSetup = restoreInt("cl_threaded_bone_setup", m_WriteOnlyOrigThreadedBoneSetup);
    const auto threadedParticles = restoreInt("r_threaded_particles", m_WriteOnlyOrigThreadedParticles);
    const auto queuedDecals = restoreInt("r_queued_decals", m_WriteOnlyOrigQueuedDecals);
    const auto queuedPostProcessing = restoreInt("r_queued_post_processing", m_WriteOnlyOrigQueuedPostProcessing);
    const auto rootLod = restoreInt("r_rootlod", m_WriteOnlyOrigRootLod);
    const auto propsMaxDist = restoreFloat("r_propsmaxdist", m_WriteOnlyOrigPropsMaxDist);
    const auto detailDist = restoreFloat("cl_detaildist", m_WriteOnlyOrigDetailDist);
    const auto detailFade = restoreFloat("cl_detailfade", m_WriteOnlyOrigDetailFade);

    Game::logMsg(
        "[VR][WriteOnlyPerfTweaks] Restored %d/8 snapshot defaults, verified %d/8 by readback: "
        "cl_threaded_bone_setup=%d(read=%d ok=%d) "
        "r_threaded_particles=%d(read=%d ok=%d) "
        "r_queued_decals=%d(read=%d ok=%d) "
        "r_queued_post_processing=%d(read=%d ok=%d) "
        "r_rootlod=%d(read=%d ok=%d) "
        "r_propsmaxdist=%.1f(read=%.1f ok=%d) "
        "cl_detaildist=%.1f(read=%.1f ok=%d) "
        "cl_detailfade=%.1f(read=%.1f ok=%d)",
        restoredCount,
        verifiedCount,
        m_WriteOnlyOrigThreadedBoneSetup,
        threadedBoneSetup.first,
        threadedBoneSetup.second ? 1 : 0,
        m_WriteOnlyOrigThreadedParticles,
        threadedParticles.first,
        threadedParticles.second ? 1 : 0,
        m_WriteOnlyOrigQueuedDecals,
        queuedDecals.first,
        queuedDecals.second ? 1 : 0,
        m_WriteOnlyOrigQueuedPostProcessing,
        queuedPostProcessing.first,
        queuedPostProcessing.second ? 1 : 0,
        m_WriteOnlyOrigRootLod,
        rootLod.first,
        rootLod.second ? 1 : 0,
        m_WriteOnlyOrigPropsMaxDist,
        propsMaxDist.first,
        propsMaxDist.second ? 1 : 0,
        m_WriteOnlyOrigDetailDist,
        detailDist.first,
        detailDist.second ? 1 : 0,
        m_WriteOnlyOrigDetailFade,
        detailFade.first,
        detailFade.second ? 1 : 0);

    m_WriteOnlyPerformanceOriginalsCaptured = false;
}

namespace
{
    constexpr int kLocalVScriptFlagGameDll = 1 << 2;
    constexpr int kLocalVScriptFlagClientDll = 1 << 3;
    constexpr int kLocalVScriptFlagReplicated = 1 << 13;
    constexpr int kLocalVScriptFlagCheat = 1 << 14;
    enum class LocalVScriptValueKind
    {
        String,
        Bool,
        Int,
        Float
    };

    inline void TrimLocalVScriptValue(std::string& value)
    {
        auto ltrim = [](std::string& s)
        {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch)
                {
                    return !std::isspace(ch);
                }));
        };
        auto rtrim = [](std::string& s)
        {
            s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch)
                {
                    return !std::isspace(ch);
                }).base(), s.end());
        };

        ltrim(value);
        rtrim(value);
    }

    void StripLocalVScriptLineComment(std::string& line)
    {
        bool inString = false;
        bool escaped = false;
        for (size_t i = 0; i + 1 < line.size(); ++i)
        {
            const char ch = line[i];
            if (escaped)
            {
                escaped = false;
                continue;
            }

            if (inString && ch == '\\')
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
            {
                inString = !inString;
                continue;
            }

            if (!inString && ch == '/' && line[i + 1] == '/')
            {
                line.erase(i);
                return;
            }
        }
    }

    bool TryParseLocalVScriptBool(const std::string& rawValue, bool& outValue)
    {
        std::string value = rawValue;
        TrimLocalVScriptValue(value);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });

        if (value == "true" || value == "1" || value == "on" || value == "yes")
        {
            outValue = true;
            return true;
        }

        if (value == "false" || value == "0" || value == "off" || value == "no")
        {
            outValue = false;
            return true;
        }

        return false;
    }

    LocalVScriptValueKind InferLocalVScriptValueKind(
        const std::string& rawValue,
        bool& outBool,
        int& outInt,
        float& outFloat)
    {
        std::string value = rawValue;
        TrimLocalVScriptValue(value);

        if (TryParseLocalVScriptBool(value, outBool))
            return LocalVScriptValueKind::Bool;

        char* intEnd = nullptr;
        const long parsedInt = std::strtol(value.c_str(), &intEnd, 10);
        if (intEnd != nullptr && *value.c_str() != '\0' && *intEnd == '\0')
        {
            outInt = static_cast<int>(parsedInt);
            return LocalVScriptValueKind::Int;
        }

        char* floatEnd = nullptr;
        const float parsedFloat = std::strtof(value.c_str(), &floatEnd);
        if (floatEnd != nullptr && *value.c_str() != '\0' && *floatEnd == '\0' && std::isfinite(parsedFloat))
        {
            outFloat = parsedFloat;
            return LocalVScriptValueKind::Float;
        }

        return LocalVScriptValueKind::String;
    }

    const char* GetLocalVScriptValueKindName(LocalVScriptValueKind kind)
    {
        switch (kind)
        {
        case LocalVScriptValueKind::Bool:
            return "bool";
        case LocalVScriptValueKind::Int:
            return "int";
        case LocalVScriptValueKind::Float:
            return "float";
        default:
            return "string";
        }
    }

    bool LocalVScriptValuesEquivalent(const std::string& requestedRaw, const std::string& actualRaw)
    {
        std::string requested = requestedRaw;
        std::string actual = actualRaw;
        TrimLocalVScriptValue(requested);
        TrimLocalVScriptValue(actual);

        if (requested == actual)
            return true;

        bool requestedBool = false;
        bool actualBool = false;
        if (TryParseLocalVScriptBool(requested, requestedBool) &&
            TryParseLocalVScriptBool(actual, actualBool))
        {
            return requestedBool == actualBool;
        }

        char* requestedEnd = nullptr;
        char* actualEnd = nullptr;
        const double requestedNumber = std::strtod(requested.c_str(), &requestedEnd);
        const double actualNumber = std::strtod(actual.c_str(), &actualEnd);
        const bool requestedIsNumber =
            requestedEnd != nullptr && *requested.c_str() != '\0' && *requestedEnd == '\0';
        const bool actualIsNumber =
            actualEnd != nullptr && *actual.c_str() != '\0' && *actualEnd == '\0';

        if (requestedIsNumber && actualIsNumber)
            return std::fabs(requestedNumber - actualNumber) <= 0.0001;

        return false;
    }

    bool ShouldThrottleLocalVScriptLog(
        std::chrono::steady_clock::time_point& last,
        float maxHz)
    {
        if (maxHz <= 0.0f)
            return false;

        const auto now = std::chrono::steady_clock::now();
        const auto minInterval =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(1.0f / maxHz));
        if (last.time_since_epoch().count() != 0 && now - last < minInterval)
            return true;

        last = now;
        return false;
    }

    const char* GetLocalVScriptSkipReason(const std::string& name, int flags)
    {
        const auto startsWith = [&](const char* prefix)
        {
            return name.rfind(prefix, 0) == 0;
        };

        if (startsWith("sv_"))
            return "server prefix sv_";
        if (startsWith("z_"))
            return "server/gameplay prefix z_";
        if (startsWith("director_"))
            return "server/gameplay prefix director_";
        if (startsWith("melee_"))
            return "server/gameplay prefix melee_";
        if (startsWith("nb_"))
            return "server/gameplay prefix nb_";
        if (startsWith("sb_"))
            return "server/gameplay prefix sb_";
        if ((flags & kLocalVScriptFlagGameDll) != 0 && (flags & kLocalVScriptFlagClientDll) == 0)
            return "FCVAR_GAMEDLL without FCVAR_CLIENTDLL";
        if ((flags & kLocalVScriptFlagReplicated) != 0 && (flags & kLocalVScriptFlagClientDll) == 0)
            return "FCVAR_REPLICATED without FCVAR_CLIENTDLL";

        return "unknown";
    }

    bool ShouldSkipLocalVScriptConvar(const std::string& name, int flags)
    {
        const auto startsWith = [&](const char* prefix)
        {
            return name.rfind(prefix, 0) == 0;
        };

        if (startsWith("sv_") ||
            startsWith("z_") ||
            startsWith("director_") ||
            startsWith("melee_") ||
            startsWith("nb_") ||
            startsWith("sb_"))
        {
            return true;
        }

        if ((flags & kLocalVScriptFlagGameDll) != 0 && (flags & kLocalVScriptFlagClientDll) == 0)
            return true;

        if ((flags & kLocalVScriptFlagReplicated) != 0 && (flags & kLocalVScriptFlagClientDll) == 0)
            return true;

        return false;
    }

    bool ParseLocalVScriptConvarsFile(
        const std::string& path,
        std::vector<VR::LocalVScriptConvarEntry>& outEntries)
    {
        outEntries.clear();
        if (path.empty())
            return false;

        std::ifstream stream(path);
        if (!stream)
            return false;

        const std::regex pattern(
            R"VSCVAR(^\s*Convars\.SetValue\(\s*"([^"]+)"\s*,\s*(.+?)\s*\)\s*;?\s*$)VSCVAR");

        std::unordered_map<std::string, size_t> lastByName;
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            StripLocalVScriptLineComment(line);
            TrimLocalVScriptValue(line);
            if (line.empty())
                continue;

            std::smatch match;
            if (!std::regex_match(line, match, pattern))
                continue;

            std::string name = match[1].str();
            std::string value = match[2].str();
            TrimLocalVScriptValue(name);
            TrimLocalVScriptValue(value);
            if (name.empty())
                continue;

            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            {
                value = value.substr(1, value.size() - 2);
            }

            VR::LocalVScriptConvarEntry entry;
            entry.name = std::move(name);
            entry.value = std::move(value);
            auto existing = lastByName.find(entry.name);
            if (existing != lastByName.end())
            {
                outEntries[existing->second].value = entry.value;
            }
            else
            {
                lastByName.emplace(entry.name, outEntries.size());
                outEntries.push_back(std::move(entry));
            }
        }

        return true;
    }
}

void VR::RestoreLocalVScriptConvars()
{
    if (!m_Game)
        return;

    std::vector<LocalVScriptConvarEntry> entriesToRestore;
    {
        std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
        if (!m_LocalVScriptConvarsApplied)
        {
            m_LocalVScriptConvars.clear();
            m_LocalVScriptConvarBlockedWriteLastLog.clear();
            return;
        }

        entriesToRestore = m_LocalVScriptConvars;
        m_LocalVScriptConvars.clear();
        m_LocalVScriptConvarsApplied = false;
        m_LocalVScriptConvarBlockedWriteLastLog.clear();
    }

    if (entriesToRestore.empty())
    {
        return;
    }

    int restoredCount = 0;
    int restoreFailedCount = 0;
    Game::logMsg(
        "[VR][LocalVScriptConvars] Restore begin count=%zu",
        entriesToRestore.size());
    for (const LocalVScriptConvarEntry& entry : entriesToRestore)
    {
        if (entry.name.empty())
            continue;

        if ((entry.flags & kLocalVScriptFlagCheat) != 0)
        {
            const int writableFlags = entry.flags & ~kLocalVScriptFlagCheat;
            const bool cleared = m_Game->SetConVarFlags(entry.name.c_str(), writableFlags);
            Game::logMsg(
                "[VR][LocalVScriptConvars] RestoreFlag name=%s action=clear_cheat before=0x%08X after=0x%08X setOk=%d",
                entry.name.c_str(),
                entry.flags,
                writableFlags,
                cleared ? 1 : 0);
        }

        const std::string beforeValue = m_Game->GetConVarString(entry.name.c_str());
        const bool setOk = m_Game->SetConVarString(entry.name.c_str(), entry.originalValue.c_str());
        const std::string afterValue = m_Game->GetConVarString(entry.name.c_str());
        std::string directReadback = "-";
        bool boolValue = false;
        int intValue = 0;
        float floatValue = 0.0f;
        const LocalVScriptValueKind kind =
            InferLocalVScriptValueKind(entry.originalValue, boolValue, intValue, floatValue);
        bool verified = false;
        switch (kind)
        {
        case LocalVScriptValueKind::Bool:
        {
            const int afterDirect = m_Game->GetConVarIntDirect(entry.name.c_str(), INT32_MIN);
            directReadback = std::to_string(afterDirect);
            verified = (afterDirect == (boolValue ? 1 : 0));
            break;
        }
        case LocalVScriptValueKind::Int:
        {
            const int afterDirect = m_Game->GetConVarIntDirect(entry.name.c_str(), INT32_MIN);
            directReadback = std::to_string(afterDirect);
            verified = (afterDirect == intValue);
            break;
        }
        case LocalVScriptValueKind::Float:
        {
            const float afterDirect = m_Game->GetConVarFloatDirect(entry.name.c_str(), -999999.0f);
            char buffer[64] = {};
            sprintf_s(buffer, "%.9g", static_cast<double>(afterDirect));
            directReadback = buffer;
            verified = std::fabs(afterDirect - floatValue) <= 0.0001f;
            break;
        }
        default:
            directReadback = afterValue;
            verified = LocalVScriptValuesEquivalent(entry.originalValue, afterValue);
            break;
        }

        if (setOk)
            ++restoredCount;
        else
            ++restoreFailedCount;

        if ((entry.flags & kLocalVScriptFlagCheat) != 0)
        {
            const bool restoredFlags = m_Game->SetConVarFlags(entry.name.c_str(), entry.flags);
            Game::logMsg(
                "[VR][LocalVScriptConvars] RestoreFlag name=%s action=restore_flags target=0x%08X setOk=%d",
                entry.name.c_str(),
                entry.flags,
                restoredFlags ? 1 : 0);
        }

        Game::logMsg(
            "[VR][LocalVScriptConvars] Restore name=%s flags=0x%08X before='%s' target='%s' after='%s' afterDirect='%s' setOk=%d verified=%d",
            entry.name.c_str(),
            entry.flags,
            beforeValue.c_str(),
            entry.originalValue.c_str(),
            afterValue.c_str(),
            directReadback.c_str(),
            setOk ? 1 : 0,
            verified ? 1 : 0);
    }

    Game::logMsg(
        "[VR][LocalVScriptConvars] Restore done restored=%d failed=%d",
        restoredCount,
        restoreFailedCount);
}

bool VR::ShouldBlockExternalLocalVScriptConvarWrite(const char* name, const char* requestedValue)
{
    if (!m_LocalVScriptConvarsEnabled || !m_LocalVScriptConvarsBlockExternalWrites || !name || !*name)
        return false;

    std::string lockedValue;
    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
        if (!m_LocalVScriptConvarsApplied)
            return false;

        auto entryIt = std::find_if(
            m_LocalVScriptConvars.begin(),
            m_LocalVScriptConvars.end(),
            [&](const LocalVScriptConvarEntry& entry)
            {
                return entry.lockProtected && entry.name == name;
            });
        if (entryIt == m_LocalVScriptConvars.end())
            return false;

        lockedValue = entryIt->value;
        auto& last = m_LocalVScriptConvarBlockedWriteLastLog[entryIt->name];
        shouldLog = !ShouldThrottleLocalVScriptLog(last, m_LocalVScriptConvarsBlockedWriteLogHz);
    }

    if (shouldLog)
    {
        Game::logMsg(
            "[VR][LocalVScriptConvars] Blocked external write name=%s requested='%s' locked='%s'",
            name,
            requestedValue ? requestedValue : "",
            lockedValue.c_str());
    }

    return true;
}

void VR::ApplyLocalVScriptConvarsIfNeeded()
{
    const bool dirty = m_LocalVScriptConvarsDirty.exchange(false, std::memory_order_acq_rel);
    if (!dirty)
        return;

    if (!m_Game || !m_Game->m_Initialized)
    {
        m_LocalVScriptConvarsDirty.store(true, std::memory_order_release);
        return;
    }

    if (m_LocalVScriptConvarsApplied)
        RestoreLocalVScriptConvars();

    if (!m_LocalVScriptConvarsEnabled)
        return;

    std::vector<LocalVScriptConvarEntry> parsedEntries;
    if (!ParseLocalVScriptConvarsFile(m_LocalVScriptConvarsPath, parsedEntries))
    {
        Game::logMsg("[VR][LocalVScriptConvars] Failed to read %s", m_LocalVScriptConvarsPath.c_str());
        return;
    }

    Game::logMsg(
        "[VR][LocalVScriptConvars] Apply begin path=%s parsed=%zu",
        m_LocalVScriptConvarsPath.c_str(),
        parsedEntries.size());

    int appliedCount = 0;
    int skippedCount = 0;
    int missingCount = 0;
    int writeFailedCount = 0;
    int verifyMismatchCount = 0;
    std::vector<LocalVScriptConvarEntry> appliedEntries;

    for (LocalVScriptConvarEntry& entry : parsedEntries)
    {
        entry.flags = m_Game->GetConVarFlags(entry.name.c_str());
        if (entry.flags < 0)
        {
            ++missingCount;
            Game::logMsg(
                "[VR][LocalVScriptConvars] Missing name=%s requested='%s' reason=FindVar/GetFlags failed",
                entry.name.c_str(),
                entry.value.c_str());
            continue;
        }

        if (ShouldSkipLocalVScriptConvar(entry.name, entry.flags))
        {
            ++skippedCount;
            Game::logMsg(
                "[VR][LocalVScriptConvars] Skip name=%s flags=0x%08X requested='%s' reason=%s",
                entry.name.c_str(),
                entry.flags,
                entry.value.c_str(),
                GetLocalVScriptSkipReason(entry.name, entry.flags));
            continue;
        }

        entry.originalValue = m_Game->GetConVarString(entry.name.c_str());
        if ((entry.flags & kLocalVScriptFlagCheat) != 0)
        {
            const int writableFlags = entry.flags & ~kLocalVScriptFlagCheat;
            const bool cleared = m_Game->SetConVarFlags(entry.name.c_str(), writableFlags);
            Game::logMsg(
                "[VR][LocalVScriptConvars] ApplyFlag name=%s action=clear_cheat before=0x%08X after=0x%08X setOk=%d",
                entry.name.c_str(),
                entry.flags,
                writableFlags,
                cleared ? 1 : 0);
        }
        bool boolValue = false;
        int intValue = 0;
        float floatValue = 0.0f;
        const LocalVScriptValueKind kind =
            InferLocalVScriptValueKind(entry.value, boolValue, intValue, floatValue);

        bool setOk = false;
        switch (kind)
        {
        case LocalVScriptValueKind::Bool:
            setOk = m_Game->SetConVarBool(entry.name.c_str(), boolValue);
            break;
        case LocalVScriptValueKind::Int:
            setOk = m_Game->SetConVarInt(entry.name.c_str(), intValue);
            break;
        case LocalVScriptValueKind::Float:
            setOk = m_Game->SetConVarFloat(entry.name.c_str(), floatValue);
            break;
        default:
            setOk = m_Game->SetConVarString(entry.name.c_str(), entry.value.c_str());
            break;
        }
        const std::string readbackValue = m_Game->GetConVarString(entry.name.c_str());
        std::string directReadbackValue = "-";
        bool verified = false;
        switch (kind)
        {
        case LocalVScriptValueKind::Bool:
        {
            const int readbackDirect = m_Game->GetConVarIntDirect(entry.name.c_str(), INT32_MIN);
            directReadbackValue = std::to_string(readbackDirect);
            verified = (readbackDirect == (boolValue ? 1 : 0));
            break;
        }
        case LocalVScriptValueKind::Int:
        {
            const int readbackDirect = m_Game->GetConVarIntDirect(entry.name.c_str(), INT32_MIN);
            directReadbackValue = std::to_string(readbackDirect);
            verified = (readbackDirect == intValue);
            break;
        }
        case LocalVScriptValueKind::Float:
        {
            const float readbackDirect = m_Game->GetConVarFloatDirect(entry.name.c_str(), -999999.0f);
            char buffer[64] = {};
            sprintf_s(buffer, "%.9g", static_cast<double>(readbackDirect));
            directReadbackValue = buffer;
            verified = std::fabs(readbackDirect - floatValue) <= 0.0001f;
            break;
        }
        default:
            directReadbackValue = readbackValue;
            verified = LocalVScriptValuesEquivalent(entry.value, readbackValue);
            break;
        }

        if (!setOk)
        {
            ++writeFailedCount;
            if ((entry.flags & kLocalVScriptFlagCheat) != 0)
            {
                const bool restoredFlags = m_Game->SetConVarFlags(entry.name.c_str(), entry.flags);
                Game::logMsg(
                    "[VR][LocalVScriptConvars] ApplyFlag name=%s action=restore_flags_on_fail target=0x%08X setOk=%d",
                    entry.name.c_str(),
                    entry.flags,
                    restoredFlags ? 1 : 0);
            }
            Game::logMsg(
                "[VR][LocalVScriptConvars] WriteFail name=%s flags=0x%08X kind=%s before='%s' requested='%s' after='%s' afterDirect='%s' setOk=0 verified=%d",
                entry.name.c_str(),
                entry.flags,
                GetLocalVScriptValueKindName(kind),
                entry.originalValue.c_str(),
                entry.value.c_str(),
                readbackValue.c_str(),
                directReadbackValue.c_str(),
                verified ? 1 : 0);
            continue;
        }

        entry.lockProtected = true;
        appliedEntries.push_back(entry);
        if (verified)
            ++appliedCount;
        else
            ++verifyMismatchCount;

        Game::logMsg(
            "[VR][LocalVScriptConvars] Apply name=%s flags=0x%08X kind=%s before='%s' requested='%s' after='%s' afterDirect='%s' setOk=1 verified=%d",
            entry.name.c_str(),
            entry.flags,
            GetLocalVScriptValueKindName(kind),
            entry.originalValue.c_str(),
            entry.value.c_str(),
            readbackValue.c_str(),
            directReadbackValue.c_str(),
            verified ? 1 : 0);
    }

    {
        std::lock_guard<std::mutex> lock(m_LocalVScriptConvarLockMutex);
        m_LocalVScriptConvars = std::move(appliedEntries);
        m_LocalVScriptConvarsApplied = !m_LocalVScriptConvars.empty();
        m_LocalVScriptConvarBlockedWriteLastLog.clear();
    }
    Game::logMsg(
        "[VR][LocalVScriptConvars] Apply done path=%s applied=%d verifyMismatch=%d writeFailed=%d missing=%d skipped=%d trackedForRestore=%zu blockExternal=%d",
        m_LocalVScriptConvarsPath.c_str(),
        appliedCount,
        verifyMismatchCount,
        writeFailedCount,
        missingCount,
        skippedCount,
        m_LocalVScriptConvars.size(),
        m_LocalVScriptConvarsBlockExternalWrites ? 1 : 0);
}

void VR::WaitForConfigUpdate()
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char configDir[MAX_STR_LEN];
    sprintf_s(configDir, MAX_STR_LEN, "%s\\VR\\", currentDir);
    HANDLE fileChangeHandle = FindFirstChangeNotificationA(configDir, false, FILE_NOTIFY_CHANGE_LAST_WRITE);

    FILETIME configLastModified{};
    FILETIME hapticsLastModified{};
    FILETIME localVScriptLastModified{};
    bool localVScriptMissing = false;
    while (1)
    {
        WIN32_FILE_ATTRIBUTE_DATA fileAttributes{};
        if (!GetFileAttributesExA("VR\\config.txt", GetFileExInfoStandard, &fileAttributes))
        {
            m_Game->errorMsg("config.txt not found.");
            return;
        }

        if (CompareFileTime(&fileAttributes.ftLastWriteTime, &configLastModified) != 0)
        {
            configLastModified = fileAttributes.ftLastWriteTime;
            try
            {
                ParseConfigFile();
            }
            catch (const std::invalid_argument&)
            {
                m_Game->errorMsg("Failed to parse config.txt");
            }
        }

        WIN32_FILE_ATTRIBUTE_DATA hapticsAttributes{};
        if (GetFileAttributesExA("VR\\haptics_config.txt", GetFileExInfoStandard, &hapticsAttributes))
        {
            if (CompareFileTime(&hapticsAttributes.ftLastWriteTime, &hapticsLastModified) != 0)
            {
                const bool configAlsoChanged = CompareFileTime(&fileAttributes.ftLastWriteTime, &configLastModified) == 0;
                hapticsLastModified = hapticsAttributes.ftLastWriteTime;
                if (configAlsoChanged)
                {
                    try
                    {
                        ParseHapticsConfigFile();
                    }
                    catch (const std::invalid_argument&)
                    {
                        m_Game->errorMsg("Failed to parse haptics_config.txt");
                    }
                }
            }
        }

        if (!m_LocalVScriptConvarsPath.empty())
        {
            WIN32_FILE_ATTRIBUTE_DATA localVScriptAttributes{};
            if (GetFileAttributesExA(m_LocalVScriptConvarsPath.c_str(), GetFileExInfoStandard, &localVScriptAttributes))
            {
                if (localVScriptMissing ||
                    CompareFileTime(&localVScriptAttributes.ftLastWriteTime, &localVScriptLastModified) != 0)
                {
                    localVScriptLastModified = localVScriptAttributes.ftLastWriteTime;
                    localVScriptMissing = false;
                    m_LocalVScriptConvarsDirty.store(true, std::memory_order_release);
                }
            }
            else if (!localVScriptMissing)
            {
                ZeroMemory(&localVScriptLastModified, sizeof(localVScriptLastModified));
                localVScriptMissing = true;
                m_LocalVScriptConvarsDirty.store(true, std::memory_order_release);
            }
        }

        FindNextChangeNotification(fileChangeHandle);
        WaitForSingleObject(fileChangeHandle, INFINITE);
        Sleep(100); // Sometimes the thread tries to read config.txt before it's finished writing
    }
}
