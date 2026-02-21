void VR::GetAimLineColor(int& r, int& g, int& b, int& a) const
{
    if (m_SpecialInfectedBlindSpotWarningActive)
    {
        r = m_AimLineWarningColorR;
        g = m_AimLineWarningColorG;
        b = m_AimLineWarningColorB;
    }
    else if (m_SpecialInfectedPreWarningActive)
    {
        r = 0;
        g = 255;
        b = 0;
    }
    else
    {
        r = m_AimLineColorR;
        g = m_AimLineColorG;
        b = m_AimLineColorB;
    }

    a = m_AimLineColorA;
}

Vector VR::GetViewAngle()
{
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
    Vector viewOriginLeft;

    viewOriginLeft = m_HmdPosAbs + (m_HmdForward * (-(m_EyeZ * m_VRScale)));
    viewOriginLeft = viewOriginLeft + (m_HmdRight * (-((m_Ipd * m_IpdScale * m_VRScale) / 2)));

    return viewOriginLeft;
}

Vector VR::GetViewOriginRight()
{
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
        try { return std::stof(it->second); }
        catch (...) { return defVal; }
        };
    auto getInt = [&](const char* k, int defVal)->int {
        auto it = userConfig.find(k);
        if (it == userConfig.end() || it->second.empty()) return defVal;
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
    m_ThirdPersonCameraSmoothing = std::clamp(getFloat("ThirdPersonCameraSmoothing", m_ThirdPersonCameraSmoothing), 0.0f, 0.99f);
    m_ThirdPersonMapLoadCooldownMs = std::max(0, getInt("ThirdPersonMapLoadCooldownMs", m_ThirdPersonMapLoadCooldownMs));
    m_ThirdPersonRenderOnCustomWalk = getBool("ThirdPersonRenderOnCustomWalk", m_ThirdPersonRenderOnCustomWalk);
    m_HideArms = getBool("HideArms", m_HideArms);
    m_HudDistance = getFloat("HudDistance", m_HudDistance);
    m_HudSize = getFloat("HudSize", m_HudSize);
    m_HudAlwaysVisible = getBool("HudAlwaysVisible", m_HudAlwaysVisible);
    m_HudToggleState = m_HudAlwaysVisible;
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
    m_AutoRepeatSemiAutoFire = getBool("AutoRepeatSemiAutoFire", m_AutoRepeatSemiAutoFire);
    m_AutoRepeatSemiAutoFireHz = std::max(0.0f, getFloat("AutoRepeatSemiAutoFireHz", m_AutoRepeatSemiAutoFireHz));
    m_MeleeAimLineEnabled = getBool("MeleeAimLineEnabled", m_MeleeAimLineEnabled);
    auto aimColor = getColor("AimLineColor", m_AimLineColorR, m_AimLineColorG, m_AimLineColorB, m_AimLineColorA);
    m_AimLineColorR = aimColor[0];
    m_AimLineColorG = aimColor[1];
    m_AimLineColorB = aimColor[2];
    m_AimLineColorA = aimColor[3];
    m_AimLinePersistence = std::max(0.0f, getFloat("AimLinePersistence", m_AimLinePersistence));
    m_AimLineFrameDurationMultiplier = std::max(0.0f, getFloat("AimLineFrameDurationMultiplier", m_AimLineFrameDurationMultiplier));
    m_AimLineMaxHz = std::max(0.0f, getFloat("AimLineMaxHz", m_AimLineMaxHz));
    m_ThrowArcLandingOffset = std::max(-10000.0f, std::min(10000.0f, getFloat("ThrowArcLandingOffset", m_ThrowArcLandingOffset)));
    m_ThrowArcMaxHz = std::max(0.0f, getFloat("ThrowArcMaxHz", m_ThrowArcMaxHz));

    // Debug / memory
    const bool prevVASLog = m_DebugVASLog;
    m_DebugVASLog = getBool("DebugVASLog", m_DebugVASLog);
    m_LazyScopeRearMirrorRTT = getBool("LazyScopeRearMirrorRTT", m_LazyScopeRearMirrorRTT);
    if (!prevVASLog && m_DebugVASLog)
        LogVAS("DebugVASLog enabled");

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
    //   ScopeAimSensitivityScale=80            (80% for all magnifications)
    //   ScopeAimSensitivityScale=100,85,70,55  (per ScopeMagnification index)
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

                m_ScopeAimSensitivityScales.push_back(std::clamp(s, 0.05f, 1.0f));
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

    m_ForceNonVRServerMovement = getBool("ForceNonVRServerMovement", m_ForceNonVRServerMovement);
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
    m_NonVRMeleeAttackDelay = std::max(0.0f, getFloat("NonVRMeleeAttackDelay", m_NonVRMeleeAttackDelay));
    m_NonVRMeleeAimLockTime = std::max(0.0f, getFloat("NonVRMeleeAimLockTime", m_NonVRMeleeAimLockTime));
    m_NonVRMeleeHysteresis = std::clamp(getFloat("NonVRMeleeHysteresis", m_NonVRMeleeHysteresis), 0.1f, 0.95f);
    m_NonVRMeleeAngVelThreshold = std::max(0.0f, getFloat("NonVRMeleeAngVelThreshold", m_NonVRMeleeAngVelThreshold));
    m_NonVRMeleeSwingDirBlend = std::clamp(getFloat("NonVRMeleeSwingDirBlend", m_NonVRMeleeSwingDirBlend), 0.0f, 1.0f);
    m_RequireSecondaryAttackForItemSwitch = getBool("RequireSecondaryAttackForItemSwitch", m_RequireSecondaryAttackForItemSwitch);
    m_SpecialInfectedWarningActionEnabled = getBool("SpecialInfectedAutoEvade", m_SpecialInfectedWarningActionEnabled);
    m_SpecialInfectedArrowEnabled = getBool("SpecialInfectedArrowEnabled", m_SpecialInfectedArrowEnabled);
    m_SpecialInfectedDebug = getBool("SpecialInfectedDebug", m_SpecialInfectedDebug);
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
    const float runCommandShotWindow = getFloat("SpecialInfectedRunCommandShotWindow", m_SpecialInfectedRunCommandShotWindow);
    m_SpecialInfectedRunCommandShotWindow = std::max(0.0f, runCommandShotWindow);

    const float runCommandShotLerp = getFloat("SpecialInfectedRunCommandShotLerp", m_SpecialInfectedRunCommandShotLerp);
    m_SpecialInfectedRunCommandShotLerp = m_SpecialInfectedDebug
        ? std::max(0.0f, runCommandShotLerp)
        : std::clamp(runCommandShotLerp, 0.0f, 1.0f);

    m_SpecialInfectedRunCommandSecondaryPredictEnabled = getBool(
        "SpecialInfectedRunCommandSecondaryPredictEnabled",
        m_SpecialInfectedRunCommandSecondaryPredictEnabled);
    m_SpecialInfectedRunCommandSecondaryForceAttack = getBool(
        "SpecialInfectedRunCommandSecondaryForceAttack",
        m_SpecialInfectedRunCommandSecondaryForceAttack);
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
}

void VR::WaitForConfigUpdate()
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char configDir[MAX_STR_LEN];
    sprintf_s(configDir, MAX_STR_LEN, "%s\\VR\\", currentDir);
    HANDLE fileChangeHandle = FindFirstChangeNotificationA(configDir, false, FILE_NOTIFY_CHANGE_LAST_WRITE);

    FILETIME configLastModified{};
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

        FindNextChangeNotification(fileChangeHandle);
        WaitForSingleObject(fileChangeHandle, INFINITE);
        Sleep(100); // Sometimes the thread tries to read config.txt before it's finished writing
    }
}
