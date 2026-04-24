namespace
{
    // Render-thread safety: entity pointers/netvar memory can be transient when we run from Present().
    // Use SEH-guarded reads to avoid rare AVs when entities are freed/repurposed mid-frame.
    static inline bool VR_TryReadU8(const unsigned char* base, int off, unsigned char& out)
    {
        __try { out = *reinterpret_cast<const unsigned char*>(base + off); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
    }
    static inline bool VR_TryReadI32(const unsigned char* base, int off, int& out)
    {
        __try { out = *reinterpret_cast<const int*>(base + off); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
    }
    static inline bool VR_TryReadU32(const unsigned char* base, int off, uint32_t& out)
    {
        __try { out = *reinterpret_cast<const uint32_t*>(base + off); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = 0u; return false; }
    }
    static inline bool VR_TryReadF32(const unsigned char* base, int off, float& out)
    {
        __try { out = *reinterpret_cast<const float*>(base + off); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { out = 0.0f; return false; }
    }

    static inline int VR_FindClientEntityIndex(IClientEntityList* entityList, const void* ptr)
    {
        if (!entityList || !ptr)
            return -1;

        int highestIndex = entityList->GetHighestEntityIndex();
        if (highestIndex < 0)
            return -1;

        // Keep the scan bounded; L4D2 client entity counts stay well below this.
        highestIndex = (std::min)(highestIndex, 4096);
        for (int i = 0; i <= highestIndex; ++i)
        {
            if (entityList->GetClientEntity(i) == ptr)
                return i;
        }

        return -1;
    }

    static inline bool VR_IsKnownClientEntityPointer(IClientEntityList* entityList, const void* ptr)
    {
        return VR_FindClientEntityIndex(entityList, ptr) >= 0;
    }

    static inline IHandleEntity* VR_GetSafeTraceSkipEntity(IClientEntityList* entityList, IHandleEntity* entity)
    {
        return VR_IsKnownClientEntityPointer(entityList, entity) ? entity : nullptr;
    }

    static inline bool VR_SafeTraceRay(IEngineTrace* engineTrace, const Ray_t& ray, unsigned int mask, CTraceFilter* filter, CGameTrace& out)
    {
        out.fraction = 1.0f;
        out.allsolid = false;
        out.startsolid = false;
        out.m_pEnt = nullptr;

        if (!engineTrace)
            return false;

        __try
        {
            engineTrace->TraceRay(ray, mask, filter, &out);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Fail closed (no hit) rather than crash the game when entity memory is transient.
            out.fraction = 1.0f;
            out.allsolid = false;
            out.startsolid = false;
            out.m_pEnt = nullptr;
            return false;
        }
    }

    static std::string VR_NormalizeResourcePath(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        while (!path.empty() && (path.front() == '/' || path.front() == '.'))
            path.erase(path.begin());
        std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return path;
    }

    static std::string VR_TrimCopy(std::string value)
    {
        auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) { return !isSpace(static_cast<unsigned char>(ch)); }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) { return !isSpace(static_cast<unsigned char>(ch)); }).base(), value.end());
        return value;
    }

    static std::string VR_JoinWindowsPath(const std::string& base, const std::string& child)
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

    static std::string VR_GetModuleDirectoryA()
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

    static std::string VR_GetDirectoryNameA(const std::string& path)
    {
        const size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos)
            return {};
        return path.substr(0, slash);
    }

    static std::string VR_GetFileStemA(const std::string& path)
    {
        size_t begin = path.find_last_of("\\/");
        begin = (begin == std::string::npos) ? 0 : begin + 1;
        const size_t dot = path.find_last_of('.');
        const size_t end = (dot == std::string::npos || dot < begin) ? path.size() : dot;
        return path.substr(begin, end - begin);
    }

    static bool VR_EndsWithInsensitive(const std::string& value, const char* suffix)
    {
        if (!suffix)
            return false;
        const size_t suffixLen = std::strlen(suffix);
        if (value.size() < suffixLen)
            return false;
        return _stricmp(value.c_str() + value.size() - suffixLen, suffix) == 0;
    }

    static bool VR_ReadAllTextFile(const std::string& path, std::string& outText)
    {
        outText.clear();
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return false;

        std::ostringstream ss;
        ss << file.rdbuf();
        outText = ss.str();
        return !outText.empty();
    }

    static bool VR_ReadVpkCString(std::ifstream& file, std::string& out)
    {
        out.clear();
        char c = 0;
        while (file.get(c))
        {
            if (c == '\0')
                return true;
            out.push_back(c);
            if (out.size() > 4096)
                return false;
        }
        return false;
    }

    template <typename T>
    static bool VR_ReadBinaryValue(std::ifstream& file, T& out)
    {
        file.read(reinterpret_cast<char*>(&out), sizeof(T));
        return file.good();
    }

    static bool VR_TryReadVpkTextFileAtPath(const std::string& dirPath, const std::string& relativePath, std::string& outText, std::string* outSource = nullptr)
    {
        outText.clear();
        if (outSource)
            outSource->clear();

        std::ifstream dirFile(dirPath, std::ios::binary);
        if (!dirFile.is_open())
            return false;

        uint32_t signature = 0;
        uint32_t version = 0;
        uint32_t treeSize = 0;
        if (!VR_ReadBinaryValue(dirFile, signature) || !VR_ReadBinaryValue(dirFile, version) || !VR_ReadBinaryValue(dirFile, treeSize))
            return false;
        if (signature != 0x55AA1234u || version != 1u)
            return false;

        const std::streamoff treeStart = dirFile.tellg();
        const std::string targetPath = VR_NormalizeResourcePath(relativePath);
        const std::string archiveDir = VR_GetDirectoryNameA(dirPath);
        std::string archivePrefix = VR_GetFileStemA(dirPath);
        if (VR_EndsWithInsensitive(archivePrefix, "_dir"))
            archivePrefix.resize(archivePrefix.size() - 4);

        std::string extension;
        while (VR_ReadVpkCString(dirFile, extension))
        {
            if (extension.empty())
                break;

            std::string path;
            while (VR_ReadVpkCString(dirFile, path))
            {
                if (path.empty())
                    break;

                std::string name;
                while (VR_ReadVpkCString(dirFile, name))
                {
                    if (name.empty())
                        break;

                    uint32_t crc = 0;
                    uint16_t preloadBytes = 0;
                    uint16_t archiveIndex = 0;
                    uint32_t entryOffset = 0;
                    uint32_t entryLength = 0;
                    uint16_t terminator = 0;
                    if (!VR_ReadBinaryValue(dirFile, crc)
                        || !VR_ReadBinaryValue(dirFile, preloadBytes)
                        || !VR_ReadBinaryValue(dirFile, archiveIndex)
                        || !VR_ReadBinaryValue(dirFile, entryOffset)
                        || !VR_ReadBinaryValue(dirFile, entryLength)
                        || !VR_ReadBinaryValue(dirFile, terminator))
                    {
                        return false;
                    }

                    std::string preload;
                    if (preloadBytes > 0)
                    {
                        preload.resize(preloadBytes);
                        dirFile.read(&preload[0], preloadBytes);
                        if (!dirFile.good())
                            return false;
                    }

                    const std::string fullPath = VR_NormalizeResourcePath(path + "/" + name + "." + extension);
                    if (fullPath != targetPath)
                        continue;

                    if (entryLength > (4u * 1024u * 1024u))
                        return false;

                    std::string archivePath;
                    std::streamoff dataOffset = 0;
                    if (archiveIndex == 0x7FFFu)
                    {
                        archivePath = dirPath;
                        dataOffset = treeStart + static_cast<std::streamoff>(treeSize) + static_cast<std::streamoff>(entryOffset);
                    }
                    else
                    {
                        if (archiveDir.empty() || archivePrefix.empty())
                            return false;
                        char archiveName[MAX_PATH] = {};
                        sprintf_s(archiveName, "%s_%03u.vpk", archivePrefix.c_str(), static_cast<unsigned int>(archiveIndex));
                        archivePath = VR_JoinWindowsPath(archiveDir, archiveName);
                        dataOffset = static_cast<std::streamoff>(entryOffset);
                    }

                    std::ifstream dataFile(archivePath, std::ios::binary);
                    if (!dataFile.is_open())
                        return false;

                    dataFile.seekg(dataOffset, std::ios::beg);
                    if (!dataFile.good())
                        return false;

                    std::string data;
                    data.resize(entryLength);
                    if (entryLength > 0)
                    {
                        dataFile.read(&data[0], entryLength);
                        if (!dataFile.good())
                            return false;
                    }

                    outText = preload + data;
                    if (outSource && !outText.empty())
                        *outSource = std::string("vpk:") + archivePath + "!" + fullPath;
                    return !outText.empty();
                }
            }
        }

        return false;
    }

    static bool VR_TryReadVpkTextFile(const std::string& gameDir, const std::string& relativePath, std::string& outText, std::string* outSource = nullptr)
    {
        return VR_TryReadVpkTextFileAtPath(VR_JoinWindowsPath(gameDir, "pak01_dir.vpk"), relativePath, outText, outSource);
    }

    static bool VR_TryReadVpkTextFileFromDirectory(const std::string& directory, const std::string& relativePath, std::string& outText, std::string* outSource = nullptr)
    {
        outText.clear();
        if (outSource)
            outSource->clear();

        const std::string pattern = VR_JoinWindowsPath(directory, "*.vpk");
        WIN32_FIND_DATAA data{};
        HANDLE find = ::FindFirstFileA(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
            return false;

        std::vector<std::string> vpkPaths;
        do
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                continue;
            if (data.nFileSizeHigh == 0 && data.nFileSizeLow < 12)
                continue;

            const std::string name(data.cFileName);
            if (VR_EndsWithInsensitive(name, "_000.vpk")
                || VR_EndsWithInsensitive(name, "_001.vpk")
                || VR_EndsWithInsensitive(name, "_002.vpk")
                || VR_EndsWithInsensitive(name, "_003.vpk")
                || VR_EndsWithInsensitive(name, "_004.vpk")
                || VR_EndsWithInsensitive(name, "_005.vpk")
                || VR_EndsWithInsensitive(name, "_006.vpk")
                || VR_EndsWithInsensitive(name, "_007.vpk")
                || VR_EndsWithInsensitive(name, "_008.vpk")
                || VR_EndsWithInsensitive(name, "_009.vpk"))
            {
                continue;
            }

            vpkPaths.push_back(VR_JoinWindowsPath(directory, name));
        } while (::FindNextFileA(find, &data));
        ::FindClose(find);

        std::sort(vpkPaths.begin(), vpkPaths.end());
        for (const std::string& vpkPath : vpkPaths)
        {
            if (VR_TryReadVpkTextFileAtPath(vpkPath, relativePath, outText, outSource))
                return true;
        }

        return false;
    }

    static bool VR_TryReadGameResourceText(const std::string& relativePath, std::string& outText, std::string* outSource = nullptr)
    {
        outText.clear();
        if (outSource)
            outSource->clear();

        const std::string moduleDir = VR_GetModuleDirectoryA();
        if (moduleDir.empty())
            return false;

        const std::string l4d2Dir = VR_JoinWindowsPath(moduleDir, "left4dead2");
        const std::array<std::string, 2> looseOverrideRoots =
        {
            VR_JoinWindowsPath(l4d2Dir, "downloads"),
            l4d2Dir
        };
        for (const std::string& root : looseOverrideRoots)
        {
            const std::string loosePath = VR_JoinWindowsPath(root, relativePath);
            if (VR_ReadAllTextFile(loosePath, outText))
            {
                if (outSource)
                    *outSource = std::string("loose:") + loosePath;
                return true;
            }
        }

        const std::array<std::string, 3> vpkOverrideRoots =
        {
            VR_JoinWindowsPath(l4d2Dir, "downloads"),
            VR_JoinWindowsPath(l4d2Dir, "addons"),
            VR_JoinWindowsPath(VR_JoinWindowsPath(l4d2Dir, "addons"), "workshop")
        };
        for (const std::string& root : vpkOverrideRoots)
        {
            if (VR_TryReadVpkTextFileFromDirectory(root, relativePath, outText, outSource))
                return true;
        }

        const std::array<const char*, 5> searchRoots =
        {
            "update",
            "left4dead2_dlc3",
            "left4dead2_dlc2",
            "left4dead2_dlc1",
            "left4dead2"
        };

        for (const char* root : searchRoots)
        {
            const std::string gameDir = VR_JoinWindowsPath(moduleDir, root);
            const std::string loosePath = VR_JoinWindowsPath(gameDir, relativePath);
            if (VR_ReadAllTextFile(loosePath, outText))
            {
                if (outSource)
                    *outSource = std::string("loose:") + loosePath;
                return true;
            }
            if (VR_TryReadVpkTextFile(gameDir, relativePath, outText, outSource))
                return true;
        }

        return false;
    }

    static bool VR_ReadQuotedToken(const std::string& line, size_t& pos, std::string& out)
    {
        out.clear();

        const size_t begin = line.find('"', pos);
        if (begin == std::string::npos)
            return false;
        const size_t end = line.find('"', begin + 1);
        if (end == std::string::npos)
            return false;

        out = line.substr(begin + 1, end - begin - 1);
        pos = end + 1;
        return true;
    }

    static bool VR_ParseWeaponScriptFloat(const std::string& text, const char* key, float& outValue)
    {
        if (!key || !*key)
            return false;

        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            const size_t comment = line.find("//");
            if (comment != std::string::npos)
                line = line.substr(0, comment);

            size_t pos = 0;
            std::string parsedKey;
            std::string parsedValue;
            if (!VR_ReadQuotedToken(line, pos, parsedKey) || !VR_ReadQuotedToken(line, pos, parsedValue))
                continue;

            if (_stricmp(parsedKey.c_str(), key) != 0)
                continue;

            try
            {
                outValue = std::stof(VR_TrimCopy(parsedValue));
                return std::isfinite(outValue);
            }
            catch (...)
            {
                return false;
            }
        }

        return false;
    }

    static bool VR_ParseWeaponScriptInt(const std::string& text, const char* key, int& outValue)
    {
        float value = 0.0f;
        if (!VR_ParseWeaponScriptFloat(text, key, value))
            return false;

        outValue = static_cast<int>(std::lround(value));
        return true;
    }

    static bool VR_ParseEffectiveWeaponData(const std::string& text, VR::EffectiveAttackRangeWeaponData& outData)
    {
        VR::EffectiveAttackRangeWeaponData data{};
        bool hasSpread = false;
        hasSpread |= VR_ParseWeaponScriptFloat(text, "MinStandingSpread", data.minStandingSpread);
        hasSpread |= VR_ParseWeaponScriptFloat(text, "MinDuckingSpread", data.minDuckingSpread);
        hasSpread |= VR_ParseWeaponScriptFloat(text, "MinInAirSpread", data.minInAirSpread);
        VR_ParseWeaponScriptFloat(text, "MaxMovementSpread", data.maxMovementSpread);
        VR_ParseWeaponScriptFloat(text, "PelletScatterPitch", data.pelletScatterPitch);
        VR_ParseWeaponScriptFloat(text, "PelletScatterYaw", data.pelletScatterYaw);
        VR_ParseWeaponScriptFloat(text, "Range", data.range);
        VR_ParseWeaponScriptFloat(text, "MaxPlayerSpeed", data.maxPlayerSpeed);
        VR_ParseWeaponScriptInt(text, "Bullets", data.bullets);

        if (!hasSpread)
            return false;

        if (data.minDuckingSpread <= 0.0f)
            data.minDuckingSpread = data.minStandingSpread;
        if (data.minInAirSpread <= 0.0f)
            data.minInAirSpread = data.minStandingSpread;
        data.minStandingSpread = std::max(0.0f, data.minStandingSpread);
        data.minDuckingSpread = std::max(0.0f, data.minDuckingSpread);
        data.minInAirSpread = std::max(0.0f, data.minInAirSpread);
        data.maxMovementSpread = std::max(0.0f, data.maxMovementSpread);
        data.pelletScatterPitch = std::max(0.0f, data.pelletScatterPitch);
        data.pelletScatterYaw = std::max(0.0f, data.pelletScatterYaw);
        data.range = std::clamp(data.range, 1.0f, 65536.0f);
        data.maxPlayerSpeed = std::clamp(data.maxPlayerSpeed, 1.0f, 1000.0f);
        data.bullets = std::max(1, data.bullets);
        data.valid = true;
        outData = data;
        return true;
    }

    struct VR_EntityNpcPlayerFlags
    {
        bool isNpc = false;
        bool isPlayer = false;
    };

    static VR_EntityNpcPlayerFlags VR_GetEntityNpcPlayerFlags(const C_BaseEntity* entity)
    {
        VR_EntityNpcPlayerFlags flags{};
        if (!entity)
            return flags;

#ifdef _MSC_VER
        __try
        {
            C_BaseEntity* mutableEntity = const_cast<C_BaseEntity*>(entity);
            flags.isNpc = mutableEntity->IsNPC() != nullptr;
            flags.isPlayer = mutableEntity->IsPlayer() != nullptr;
            return flags;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return {};
        }
#else
        C_BaseEntity* mutableEntity = const_cast<C_BaseEntity*>(entity);
        flags.isNpc = mutableEntity->IsNPC() != nullptr;
        flags.isPlayer = mutableEntity->IsPlayer() != nullptr;
        return flags;
#endif
    }

    static bool VR_IsEntityNpcOrPlayer(const C_BaseEntity* entity)
    {
        const VR_EntityNpcPlayerFlags flags = VR_GetEntityNpcPlayerFlags(entity);
        return flags.isNpc || flags.isPlayer;
    }

    static bool VR_TryGetEntityAbsOrigin(const C_BaseEntity* entity, Vector& out)
    {
        if (!entity)
            return false;

#ifdef _MSC_VER
        __try
        {
            out = const_cast<C_BaseEntity*>(entity)->GetAbsOrigin();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        out = const_cast<C_BaseEntity*>(entity)->GetAbsOrigin();
        return true;
#endif
    }

    static bool VR_IsEffectiveAttackRangeRelaxedMovementState(const C_BasePlayer* localPlayer)
    {
        if (!localPlayer)
            return false;

        const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
        int flags = 0;
        const bool hasFlags = VR_TryReadI32(base, VR::kFlagsOffset, flags);
        const bool onGround = !hasFlags || ((flags & VR::kOnGroundFlag) != 0);
        if (!onGround)
            return true;

        const float speed2D = localPlayer->m_vecVelocity.Length2D();
        return std::isfinite(speed2D) && speed2D > 5.0f;
    }

    using VR_CreateParticleEffectFn = void* (__thiscall*)(void* thisptr, const char* particleName, int attachType, int attachment, Vector originOffset, int unknown);
    using VR_StopParticleEffectFn = void(__thiscall*)(void* thisptr, void* effect);
    using VR_SetParticleControlPointPositionFn = void(__thiscall*)(void* thisptr, int controlPoint, const Vector& position);
    using VR_SetParticleControlPointForwardVectorFn = void(__thiscall*)(void* thisptr, int controlPoint, const Vector& forward);

    struct VR_GameLaserParticleState
    {
        void* effect = nullptr;
        void* particleProperty = nullptr;
        C_BaseCombatWeapon* weapon = nullptr;
        void* owner = nullptr;
    };

    static VR_GameLaserParticleState g_GameLaserParticle;

    static inline void* VR_CreateParticleEffect(Game* game, void* particleProperty, const char* particleName, int attachType, int attachment)
    {
        if (!game || !game->m_Offsets || !game->m_Offsets->CreateParticleEffect.valid)
            return nullptr;
        if (!particleProperty || !particleName || !particleName[0])
            return nullptr;

        auto createParticleEffect = reinterpret_cast<VR_CreateParticleEffectFn>(
            game->m_Offsets->CreateParticleEffect.address);
        if (!createParticleEffect)
            return nullptr;

        const Vector originOffset{ 0.0f, 0.0f, 0.0f };
        __try
        {
            return createParticleEffect(particleProperty, particleName, attachType, attachment, originOffset, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static inline void VR_StopParticleEffect(Game* game, void* particleProperty, void* effect)
    {
        if (!game || !game->m_Offsets || !game->m_Offsets->StopParticleEffect.valid || !particleProperty || !effect)
            return;

        auto stopParticleEffect = reinterpret_cast<VR_StopParticleEffectFn>(
            game->m_Offsets->StopParticleEffect.address);
        if (!stopParticleEffect)
            return;

        __try
        {
            stopParticleEffect(particleProperty, effect);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    static inline void VR_ClearGameLaserParticle(Game* game)
    {
        if (g_GameLaserParticle.effect && g_GameLaserParticle.particleProperty)
            VR_StopParticleEffect(game, g_GameLaserParticle.particleProperty, g_GameLaserParticle.effect);

        g_GameLaserParticle = {};
    }

    static inline bool VR_SetParticleControlPoint(Game* game, void* effect, int controlPoint, const Vector& position)
    {
        if (!game || !game->m_Offsets || !game->m_Offsets->ParticleSetControlPointPosition.valid || !effect)
            return false;

        auto setControlPoint = reinterpret_cast<VR_SetParticleControlPointPositionFn>(
            game->m_Offsets->ParticleSetControlPointPosition.address);
        if (!setControlPoint)
            return false;

        __try
        {
            setControlPoint(effect, controlPoint, position);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static inline bool VR_SetParticleForward(Game* game, void* effect, int controlPoint, const Vector& forward)
    {
        if (!game || !game->m_Offsets || !game->m_Offsets->ParticleSetControlPointForwardVector.valid || !effect)
            return false;

        Vector normalizedForward = forward;
        if (normalizedForward.IsZero())
            return false;
        VectorNormalize(normalizedForward);

        auto setForward = reinterpret_cast<VR_SetParticleControlPointForwardVectorFn>(
            game->m_Offsets->ParticleSetControlPointForwardVector.address);
        if (!setForward)
            return false;

        __try
        {
            setForward(effect, controlPoint, normalizedForward);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static inline uint32_t VR_PackD3DColor(int r, int g, int b, int a)
    {
        const uint32_t cr = static_cast<uint32_t>(std::clamp(r, 0, 255));
        const uint32_t cg = static_cast<uint32_t>(std::clamp(g, 0, 255));
        const uint32_t cb = static_cast<uint32_t>(std::clamp(b, 0, 255));
        const uint32_t ca = static_cast<uint32_t>(std::clamp(a, 0, 255));
        return (ca << 24) | (cr << 16) | (cg << 8) | cb;
    }

    static inline bool VR_ProjectAimLinePointToView(
        const CViewSetup& view,
        const Vector& forward,
        const Vector& right,
        const Vector& up,
        const Vector& point,
        float tanHalfFovX,
        float tanHalfFovY,
        float& outX,
        float& outY,
        float& outDepth)
    {
        const Vector rel = point - view.origin;
        const float depth = DotProduct(rel, forward);
        if (depth <= 0.001f || !std::isfinite(depth))
            return false;

        const float px = DotProduct(rel, right) / (depth * tanHalfFovX);
        const float py = DotProduct(rel, up) / (depth * tanHalfFovY);
        if (!std::isfinite(px) || !std::isfinite(py))
            return false;

        outX = (0.5f + px * 0.5f) * static_cast<float>(view.width);
        outY = (0.5f - py * 0.5f) * static_cast<float>(view.height);
        outDepth = depth;
        return std::isfinite(outX) && std::isfinite(outY);
    }

    static inline bool VR_ProjectAimLineSegmentToView(
        const CViewSetup& view,
        Vector start,
        Vector end,
        float& x0,
        float& y0,
        float& x1,
        float& y1)
    {
        if (view.width <= 0 || view.height <= 0 || view.fov <= 1.0f)
            return false;

        QAngle viewAngles(view.angles.x, view.angles.y, view.angles.z);
        Vector forward{}, right{}, up{};
        QAngle::AngleVectors(viewAngles, &forward, &right, &up);
        if (forward.IsZero() || right.IsZero() || up.IsZero())
            return false;

        VectorNormalize(forward);
        VectorNormalize(right);
        VectorNormalize(up);

        const float aspect = (view.m_flAspectRatio > 0.01f)
            ? view.m_flAspectRatio
            : (static_cast<float>(view.width) / static_cast<float>(std::max(view.height, 1)));
        const float tanHalfFovX = std::tan(DEG2RAD(view.fov * 0.5f));
        const float tanHalfFovY = tanHalfFovX / std::max(aspect, 0.01f);
        if (tanHalfFovX <= 0.0001f || tanHalfFovY <= 0.0001f)
            return false;

        const float nearDepth = std::max(view.zNear, 1.0f);
        const float startDepth = DotProduct(start - view.origin, forward);
        const float endDepth = DotProduct(end - view.origin, forward);
        if (startDepth <= nearDepth && endDepth <= nearDepth)
            return false;

        if (startDepth <= nearDepth || endDepth <= nearDepth)
        {
            const float denom = endDepth - startDepth;
            if (std::fabs(denom) <= 0.0001f)
                return false;

            const float t = std::clamp((nearDepth - startDepth) / denom, 0.0f, 1.0f);
            const Vector clipped = start + (end - start) * t;
            if (startDepth <= nearDepth)
                start = clipped;
            else
                end = clipped;
        }

        float d0 = 0.0f;
        float d1 = 0.0f;
        return VR_ProjectAimLinePointToView(view, forward, right, up, start, tanHalfFovX, tanHalfFovY, x0, y0, d0)
            && VR_ProjectAimLinePointToView(view, forward, right, up, end, tanHalfFovX, tanHalfFovY, x1, y1, d1);
    }

}

bool VR::IsUsingMountedGun(const C_BasePlayer* localPlayer) const
{
    if (!localPlayer)
        return false;

    // L4D2 uses two adjacent netvars for mounted weapons:
    // - m_usingMountedGun: typically .50cal
    // - m_usingMountedWeapon: typically minigun/gatling
    const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
    unsigned char usingGun = 0;
    unsigned char usingWeapon = 0;
    VR_TryReadU8(base, kUsingMountedGunOffset, usingGun);
    VR_TryReadU8(base, kUsingMountedWeaponOffset, usingWeapon);
    return (usingGun | usingWeapon) != 0;
}

C_BaseEntity* VR::GetMountedGunUseEntity(C_BasePlayer* localPlayer) const
{
    if (!IsUsingMountedGun(localPlayer) || !m_Game || !m_Game->m_ClientEntityList)
        return nullptr;

    const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
    uint32_t hUse = 0u;
    if (!VR_TryReadU32(base, kUseEntityHandleOffset, hUse))
        return nullptr;

    // EHANDLE / CBaseHandle is invalid when 0 or 0xFFFFFFFF (common patterns across Source builds).
    if (hUse == 0u || hUse == 0xFFFFFFFFu)
        return nullptr;

    return reinterpret_cast<C_BaseEntity*>(m_Game->m_ClientEntityList->GetClientEntityFromHandle(hUse));
}

void VR::UpdateNonVRAimSolution(C_BasePlayer* localPlayer)
{
    // Keep the previous solution alive if we throttle this frame.
    if (!m_ForceNonVRServerMovement)
    {
        m_HasNonVRAimSolution = false;
        return;
    }

    if (!localPlayer || !m_Game || !m_Game->m_EngineTrace)
    {
        m_HasNonVRAimSolution = false;
        return;
    }

    // Throttle expensive trace work. Keep this local/static so we don't depend on
    // optional header fields across branches/patches.
    static std::chrono::steady_clock::time_point s_lastSolve{};
    const float kSolveMaxHz = 90.0f; // 0 disables throttling
    if (ShouldThrottle(s_lastSolve, kSolveMaxHz))
        return;

    C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);
    IClientEntityList* entityList = m_Game ? m_Game->m_ClientEntityList : nullptr;
    IHandleEntity* safeMountedUseEnt = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(mountedUseEnt));

    if (m_MouseModeEnabled)
    {
        const Vector eye = localPlayer->EyePosition();
        QAngle eyeAng;
        Vector eyeDir;
        GetMouseModeEyeRay(eyeDir, &eyeAng);

        if (eyeDir.IsZero())
        {
            m_HasNonVRAimSolution = false;
            return;
        }

        const float maxDistance = 8192.0f;

        Vector endEye = eye + eyeDir * maxDistance;
        CGameTrace traceH;
        Ray_t rayH;
        // Skip self + mounted gun use entity + active weapon so the ray doesn't collide with your own gun/turret.
        C_BaseCombatWeapon* activeWeapon = localPlayer->GetActiveWeapon();
        IHandleEntity* safeActiveWeapon = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(activeWeapon));
        CTraceFilterSkipThreeEntities tracefilterThree((IHandleEntity*)localPlayer, safeMountedUseEnt, safeActiveWeapon, 0);
        CTraceFilter* pTraceFilter = static_cast<CTraceFilter*>(&tracefilterThree);
        rayH.Init(eye, endEye);
        VR_SafeTraceRay(m_Game->m_EngineTrace, rayH, STANDARD_TRACE_MASK, pTraceFilter, traceH);

        const Vector H = (traceH.fraction < 1.0f && traceH.fraction > 0.0f) ? traceH.endpos : endEye;
        m_NonVRAimHitPoint = H;

        const float convergeDist = (m_MouseModeAimConvergeDistance > 0.0f)
            ? std::min(m_MouseModeAimConvergeDistance, maxDistance)
            : maxDistance;
        m_NonVRAimDesiredPoint = eye + eyeDir * convergeDist;

        m_NonVRAimAngles = eyeAng;
        m_HasNonVRAimSolution = true;
        return;
    }

    const bool frontViewEyeAim = m_ThirdPersonFrontViewEnabled && m_IsThirdPersonCamera && m_ThirdPersonFrontScopeFromEye;
    const bool frontViewControllerEyeOrigin = m_ThirdPersonFrontViewEnabled && m_IsThirdPersonCamera && !m_ThirdPersonFrontScopeFromEye;
    if (frontViewEyeAim)
    {
        const Vector eye = localPlayer->EyePosition();
        Vector eyeDir = m_HmdForward;
        if (eyeDir.IsZero())
        {
            QAngle eyeAngFallback = m_HmdAngAbs;
            NormalizeAndClampViewAngles(eyeAngFallback);
            QAngle::AngleVectors(eyeAngFallback, &eyeDir, nullptr, nullptr);
        }

        if (eyeDir.IsZero())
        {
            m_HasNonVRAimSolution = false;
            return;
        }
        VectorNormalize(eyeDir);

        const float maxDistance = 8192.0f;
        Vector endEye = eye + eyeDir * maxDistance;
        CGameTrace traceH;
        Ray_t rayH;
        // Skip self + mounted gun use entity + active weapon so the ray doesn't collide with your own gun/turret.
        C_BaseCombatWeapon* activeWeapon = localPlayer->GetActiveWeapon();
        IHandleEntity* safeActiveWeapon = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(activeWeapon));
        CTraceFilterSkipThreeEntities tracefilterThree((IHandleEntity*)localPlayer, safeMountedUseEnt, safeActiveWeapon, 0);
        CTraceFilter* pTraceFilter = static_cast<CTraceFilter*>(&tracefilterThree);
        rayH.Init(eye, endEye);
        VR_SafeTraceRay(m_Game->m_EngineTrace, rayH, STANDARD_TRACE_MASK, pTraceFilter, traceH);

        const Vector H = (traceH.fraction < 1.0f && traceH.fraction > 0.0f) ? traceH.endpos : endEye;
        m_NonVRAimHitPoint = H;
        m_NonVRAimDesiredPoint = endEye;

        QAngle eyeAng;
        QAngle::VectorAngles(eyeDir, eyeAng);
        NormalizeAndClampViewAngles(eyeAng);
        m_NonVRAimAngles = eyeAng;
        m_HasNonVRAimSolution = true;
        return;
    }
    if (frontViewControllerEyeOrigin)
    {
        Vector direction = m_RightControllerForward;
        if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
            direction = m_RightControllerForwardUnforced;
        if (direction.IsZero())
            direction = m_LastAimDirection.IsZero() ? m_LastUnforcedAimDirection : m_LastAimDirection;
        if (direction.IsZero())
        {
            m_HasNonVRAimSolution = false;
            return;
        }
        VectorNormalize(direction);

        const Vector eye = localPlayer->EyePosition();
        const float maxDistance = 8192.0f;
        Vector endEye = eye + direction * maxDistance;

        CGameTrace traceH;
        Ray_t rayH;
        C_BaseCombatWeapon* activeWeapon = localPlayer->GetActiveWeapon();
        IHandleEntity* safeActiveWeapon = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(activeWeapon));
        CTraceFilterSkipThreeEntities tracefilterThree((IHandleEntity*)localPlayer, safeMountedUseEnt, safeActiveWeapon, 0);
        CTraceFilter* pTraceFilter = static_cast<CTraceFilter*>(&tracefilterThree);
        rayH.Init(eye, endEye);
        VR_SafeTraceRay(m_Game->m_EngineTrace, rayH, STANDARD_TRACE_MASK, pTraceFilter, traceH);

        const Vector H = (traceH.fraction < 1.0f && traceH.fraction > 0.0f) ? traceH.endpos : endEye;
        m_NonVRAimHitPoint = H;
        m_NonVRAimDesiredPoint = endEye;

        QAngle ang;
        QAngle::VectorAngles(direction, ang);
        NormalizeAndClampViewAngles(ang);
        m_NonVRAimAngles = ang;
        m_HasNonVRAimSolution = true;
        return;
    }

    // Use the same controller direction/origin rules as UpdateAimingLaser.
    Vector direction = m_RightControllerForward;
    if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
        direction = m_RightControllerForwardUnforced;

    if (direction.IsZero())
        direction = m_LastAimDirection.IsZero() ? m_LastUnforcedAimDirection : m_LastAimDirection;

    if (direction.IsZero())
    {
        m_HasNonVRAimSolution = false;
        return;
    }

    VectorNormalize(direction);

    // In queued render + view smoothing, the rendered controller pose may differ from the
    // update-thread controller pose. Always use the same source as the aiming laser so
    // the non-VR server aim solution stays consistent with what the player sees.
    const Vector controllerPosAbs = GetRightControllerAbsPos();

    Vector originBase = controllerPosAbs;
    // Keep non-3P codepath identical to legacy behavior; only use the new render-center delta in 3P.
    Vector camDelta = m_IsThirdPersonCamera
        ? (m_ThirdPersonRenderCenter - m_SetupOrigin)
        : (m_ThirdPersonViewOrigin - m_SetupOrigin);
    if (m_IsThirdPersonCamera && camDelta.LengthSqr() > (5.0f * 5.0f))
        originBase += camDelta;

    Vector origin = originBase + direction * 2.0f;

    const float maxDistance = 8192.0f;
    Vector target = origin + direction * maxDistance;

    // 1) Controller ray -> P
    CGameTrace traceP;
    Ray_t rayP;
    // Skip self + mounted gun use entity + active weapon so the ray doesn't collide with your own gun/turret.
    C_BaseCombatWeapon* activeWeapon = localPlayer->GetActiveWeapon();
    IHandleEntity* safeActiveWeapon = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(activeWeapon));
    CTraceFilterSkipThreeEntities tracefilterThree((IHandleEntity*)localPlayer, safeMountedUseEnt, safeActiveWeapon, 0);
    CTraceFilter* pTraceFilter = static_cast<CTraceFilter*>(&tracefilterThree);
    rayP.Init(origin, target);
    VR_SafeTraceRay(m_Game->m_EngineTrace, rayP, STANDARD_TRACE_MASK, pTraceFilter, traceP);

    const Vector P = (traceP.fraction < 1.0f && traceP.fraction > 0.0f) ? traceP.endpos : target;
    m_NonVRAimDesiredPoint = P;

    // 2) Eye ray towards P -> H (what the server will actually hit)
    const Vector eye = localPlayer->EyePosition();
    Vector toP = P - eye;
    if (toP.IsZero())
    {
        m_HasNonVRAimSolution = false;
        return;
    }
    VectorNormalize(toP);

    Vector endEye = eye + toP * maxDistance;
    CGameTrace traceH;
    Ray_t rayH;
    rayH.Init(eye, endEye);
    VR_SafeTraceRay(m_Game->m_EngineTrace, rayH, STANDARD_TRACE_MASK, pTraceFilter, traceH);

    const Vector H = (traceH.fraction < 1.0f && traceH.fraction > 0.0f) ? traceH.endpos : endEye;
    m_NonVRAimHitPoint = H;

    // 3) Eye -> H angles
    Vector toH = H - eye;
    if (toH.IsZero())
    {
        m_HasNonVRAimSolution = false;
        return;
    }
    VectorNormalize(toH);

    QAngle ang;
    QAngle::VectorAngles(toH, ang);
    // IMPORTANT: VectorAngles(forward) returns pitch/yaw in [0,360). Normalize BEFORE clamping.
    NormalizeAndClampViewAngles(ang);

    m_NonVRAimAngles = ang;
    m_HasNonVRAimSolution = true;
}

bool VR::UpdateFriendlyFireAimHit(C_BasePlayer* localPlayer)
{
    m_AimLineHitsFriendly = false;
    if (!m_BlockFireOnFriendlyAimEnabled)
        return false;
    if (!localPlayer || !m_Game || !m_Game->m_EngineTrace)
        return false;

    C_WeaponCSBase* activeWeapon = static_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());

    if (!activeWeapon || IsThrowableWeapon(activeWeapon))
        return false;

    C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);
    IClientEntityList* entityList = m_Game ? m_Game->m_ClientEntityList : nullptr;
    IHandleEntity* safeMountedUseEnt = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(mountedUseEnt));
    IHandleEntity* safeActiveWeapon = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(activeWeapon));

    const bool useMouse = m_MouseModeEnabled;
    const bool frontViewEyeAim = m_ThirdPersonFrontViewEnabled && m_IsThirdPersonCamera && m_ThirdPersonFrontScopeFromEye;
    const bool frontViewControllerEyeOrigin = m_ThirdPersonFrontViewEnabled && m_IsThirdPersonCamera && !m_ThirdPersonFrontScopeFromEye;

    // Eye-center ray direction (mouse aim in mouse mode).
    Vector eyeDir{ 0.0f, 0.0f, 0.0f };
    if (useMouse)
    {
        GetMouseModeEyeRay(eyeDir);
    }
    else if (frontViewEyeAim)
    {
        eyeDir = m_HmdForward;
        if (eyeDir.IsZero())
        {
            QAngle eyeAngFallback = m_HmdAngAbs;
            NormalizeAndClampViewAngles(eyeAngFallback);
            QAngle::AngleVectors(eyeAngFallback, &eyeDir, nullptr, nullptr);
        }
        if (!eyeDir.IsZero())
            VectorNormalize(eyeDir);
    }

    // ---- Build the "aim line" (gun/hand) ray (existing behavior) ----
    Vector gunDir{ 0.0f, 0.0f, 0.0f };
    Vector gunOrigin{ 0.0f, 0.0f, 0.0f };
    if (frontViewEyeAim || frontViewControllerEyeOrigin)
    {
        gunOrigin = localPlayer->EyePosition();
        if (frontViewEyeAim)
        {
            gunDir = eyeDir;
        }
        else
        {
            gunDir = m_RightControllerForward;
            if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
                gunDir = m_RightControllerForwardUnforced;
        }
    }
    else
    {
        gunDir = m_RightControllerForward;
        if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
            gunDir = m_RightControllerForwardUnforced;

        gunOrigin = m_RightControllerPosAbs;
        if (useMouse)
        {
            const Vector& anchor = IsMouseModeScopeActive() ? m_MouseModeScopedViewmodelAnchorOffset : m_MouseModeViewmodelAnchorOffset;
            gunOrigin = m_HmdPosAbs
                + (m_HmdForward * (anchor.x * m_VRScale))
                + (m_HmdRight * (anchor.y * m_VRScale))
                + (m_HmdUp * (anchor.z * m_VRScale));

            const float convergeDist = (m_MouseModeAimConvergeDistance > 0.0f) ? m_MouseModeAimConvergeDistance : 8192.0f;
            Vector target = m_HmdPosAbs + eyeDir * convergeDist;
            gunDir = target - gunOrigin;
        }
    }

    if (gunDir.IsZero())
        return false;
    VectorNormalize(gunDir);

    Vector gunOriginBase = gunOrigin;
    // Keep non-3P codepath identical to legacy behavior; only use the new render-center delta in 3P.
    Vector camDelta = m_IsThirdPersonCamera
        ? (m_ThirdPersonRenderCenter - m_SetupOrigin)
        : (m_ThirdPersonViewOrigin - m_SetupOrigin);
    if (!frontViewEyeAim && !frontViewControllerEyeOrigin && m_IsThirdPersonCamera && camDelta.LengthSqr() > (5.0f * 5.0f))
        gunOriginBase += camDelta;

    Vector gunStart = gunOriginBase + gunDir * 2.0f;
    Vector gunEnd = gunStart + gunDir * 8192.0f;
    // Friendly-fire guard: optional hull radius around the aim rays (meters -> Source units via VRScale).
    // This makes the check more conservative, helping with bullet spread, latency, and near-misses.
    //
    // IMPORTANT: weapon spread increases while moving. If the base radius is 0,
    // a strict line-trace can miss near-teammate shots that deviate due to movement inaccuracy.
    // To address this, we automatically add a small, speed-scaled radius while the player is moving.
    float friendlyGuardRadiusMeters = m_BlockFireOnFriendlyAimRadiusMeters;
    if (localPlayer && m_VRScale > 1.0f)
    {
        // Speed in meters/sec (Source units/sec divided by units-per-meter).
        const float speed2D_mps = localPlayer->m_vecVelocity.Length2D() / m_VRScale;
        // Ramp extra radius from ~0 when almost-still to max when running.
        const float startMps = 0.2f;
        const float fullMps = 3.0f;
        float t = (speed2D_mps - startMps) / (fullMps - startMps);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        const float autoExtraMeters = 0.06f * t; // up to +6cm
        friendlyGuardRadiusMeters = std::clamp(friendlyGuardRadiusMeters + autoExtraMeters, 0.0f, 0.5f);
    }

    const float hullRadiusUnits = (friendlyGuardRadiusMeters > 0.0f)
        ? (friendlyGuardRadiusMeters * m_VRScale)
        : 0.0f;
    const bool useHull = (hullRadiusUnits > 0.01f);
    const Vector hullMins = { -hullRadiusUnits, -hullRadiusUnits, -hullRadiusUnits };
    const Vector hullMaxs = { hullRadiusUnits,  hullRadiusUnits,  hullRadiusUnits };

    // IMPORTANT: Some Source builds are unstable/crashy if you request CONTENTS_HITBOX in a *hull* trace.
    // For hull traces we therefore switch to MASK_SHOT_HULL (no CONTENTS_HITBOX).
    const unsigned int traceMask = useHull ? (unsigned int)MASK_SHOT_HULL : (unsigned int)STANDARD_TRACE_MASK;

    // Filters (shared across traces): Skip self + mounted gun use entity + active weapon.
    // This prevents the aim ray from being blocked by your own weapon and flickering on/off.
    CTraceFilterSkipThreeEntities tracefilterThree((IHandleEntity*)localPlayer, safeMountedUseEnt, safeActiveWeapon, 0);
    CTraceFilter* pTraceFilter = static_cast<CTraceFilter*>(&tracefilterThree);

    auto SafeTraceRay = [&](Ray_t& ray, unsigned int mask, CTraceFilter* filter, CGameTrace& out) -> bool
        {
            if (!m_Game || !m_Game->m_EngineTrace)
            {
                out.fraction = 1.0f;
                out.m_pEnt = nullptr;
                return false;
            }
            __try
            {
                m_Game->m_EngineTrace->TraceRay(ray, mask, filter, &out);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // Fail closed (no hit) rather than crash the game.
                out.fraction = 1.0f;
                out.m_pEnt = nullptr;
                return false;
            }
        };

    auto hasValidHandle = [](uint32_t h) -> bool
        {
            // EHANDLE / CBaseHandle is invalid when 0 or 0xFFFFFFFF (common patterns across Source builds).
            return h != 0u && h != 0xFFFFFFFFu;
        };

    struct ControlHandles
    {
        uint32_t tongueOwner{};
        uint32_t pummelAttacker{};
        uint32_t carryAttacker{};
        uint32_t pounceAttacker{};
        uint32_t jockeyAttacker{};
    };

    auto hasAnyControlHandle = [&](const ControlHandles& h) -> bool
        {
            return hasValidHandle(h.tongueOwner)
                || hasValidHandle(h.pummelAttacker)
                || hasValidHandle(h.carryAttacker)
                || hasValidHandle(h.pounceAttacker)
                || hasValidHandle(h.jockeyAttacker);
        };

    auto getControlHandles = [&](C_BaseEntity* ent) -> ControlHandles
        {
            ControlHandles h{};
            if (!ent)
                return h;

            const unsigned char* base = reinterpret_cast<const unsigned char*>(ent);
            VR_TryReadU32(base, kTongueOwnerOffset, h.tongueOwner);
            VR_TryReadU32(base, kPummelAttackerOffset, h.pummelAttacker);
            VR_TryReadU32(base, kCarryAttackerOffset, h.carryAttacker);
            VR_TryReadU32(base, kPounceAttackerOffset, h.pounceAttacker);
            VR_TryReadU32(base, kJockeyAttackerOffset, h.jockeyAttacker);
            return h;
        };

    auto isSameTeamAlivePlayer = [&](C_BaseEntity* a, C_BaseEntity* b) -> bool
        {
            if (!a || !b)
                return false;
            if (b->IsPlayer() == nullptr)
                return false;

            const unsigned char* aBase = reinterpret_cast<const unsigned char*>(a);
            const unsigned char* bBase = reinterpret_cast<const unsigned char*>(b);
            int aTeam = 0;
            int bTeam = 0;
            if (!VR_TryReadI32(aBase, kTeamNumOffset, aTeam) || !VR_TryReadI32(bBase, kTeamNumOffset, bTeam))
                return false;
            return (aTeam != 0 && aTeam == bTeam && IsEntityAlive(b));
        };

    auto resolveHandleEntity = [&](uint32_t h) -> C_BaseEntity*
        {
            if (!hasValidHandle(h) || !m_Game || !m_Game->m_ClientEntityList)
                return nullptr;
            return reinterpret_cast<C_BaseEntity*>(m_Game->m_ClientEntityList->GetClientEntityFromHandle(h));
        };

    // Evaluate: does this trace mean "friendly is in the way"?
    // IMPORTANT: In remote servers, the authoritative bullet trace is often closer to an EYE ray
    // (plus lag compensation), while our aim line is a GUN/HAND ray. To reduce "first few shots"
    // leaking through, we treat either ray hitting a teammate as a block.
    auto evalFriendlyHitForTrace = [&](const CGameTrace& tr1, const Vector& start, const Vector& end) -> bool
        {
            C_BaseEntity* hitEnt = reinterpret_cast<C_BaseEntity*>(tr1.m_pEnt);
            if (!hitEnt || hitEnt == localPlayer || !isSameTeamAlivePlayer(localPlayer, hitEnt))
                return false;

            // Default behavior: block when the ray hits a teammate.
            bool friendlyHit = true;

            // Tightened special-case (“被控队友放行”):
            // Only when teammate is ACTUALLY controlled (tongue/pummel/carry/pounce/jockey),
            // and the second trace hits the controller entity itself,
            // and that hit point is very close to the teammate hit point.
            const ControlHandles ctrl = getControlHandles(hitEnt);
            if (hasAnyControlHandle(ctrl))
            {
                CGameTrace tr2;
                Ray_t ray2;

                // If we're on a mounted gun, also skip the mounted gun entity itself so we can "see through"
                // both the teammate and the turret when checking what's behind them.
                CTraceFilterSkipTwoEntities tracefilter2((IHandleEntity*)localPlayer, (IHandleEntity*)hitEnt, 0);
                CTraceFilterSkipThreeEntities tracefilter3((IHandleEntity*)localPlayer, (IHandleEntity*)hitEnt, safeMountedUseEnt, 0);
                CTraceFilter* pTraceFilter2 = (safeMountedUseEnt && safeMountedUseEnt != hitEnt)
                    ? static_cast<CTraceFilter*>(&tracefilter3)
                    : static_cast<CTraceFilter*>(&tracefilter2);

                ray2.Init(start, end);
                VR_SafeTraceRay(m_Game->m_EngineTrace, ray2, STANDARD_TRACE_MASK, pTraceFilter2, tr2);

                const Vector hitPos1 = (tr1.fraction < 1.0f && tr1.fraction > 0.0f) ? tr1.endpos : hitEnt->GetAbsOrigin();
                const Vector hitPos2 = (tr2.fraction < 1.0f && tr2.fraction > 0.0f) ? tr2.endpos : hitPos1;
                const float distSqr = (hitPos2 - hitPos1).LengthSqr();

                if (distSqr <= (kAllowThroughControlledTeammateMaxDist * kAllowThroughControlledTeammateMaxDist))
                {
                    C_BaseEntity* hitEnt2 = reinterpret_cast<C_BaseEntity*>(tr2.m_pEnt);
                    if (hitEnt2 && hitEnt2 != localPlayer)
                    {
                        C_BaseEntity* a1 = resolveHandleEntity(ctrl.tongueOwner);
                        C_BaseEntity* a2 = resolveHandleEntity(ctrl.pummelAttacker);
                        C_BaseEntity* a3 = resolveHandleEntity(ctrl.carryAttacker);
                        C_BaseEntity* a4 = resolveHandleEntity(ctrl.pounceAttacker);
                        C_BaseEntity* a5 = resolveHandleEntity(ctrl.jockeyAttacker);

                        if (hitEnt2 == a1 || hitEnt2 == a2 || hitEnt2 == a3 || hitEnt2 == a4 || hitEnt2 == a5)
                            friendlyHit = false;
                    }
                }
            }

            return friendlyHit;
        };

    // 1) Gun/hand ray (aim line)
    CGameTrace traceGun;
    Ray_t rayGun;
    if (useHull)
        rayGun.Init(gunStart, gunEnd, hullMins, hullMaxs);
    else
        rayGun.Init(gunStart, gunEnd);
    SafeTraceRay(rayGun, traceMask, pTraceFilter, traceGun);
    const bool friendlyGun = evalFriendlyHitForTrace(traceGun, gunStart, gunEnd);

    // 2) Eye ray (closer to authoritative server bullets, esp. with lag compensation)
    Vector eye = localPlayer->EyePosition();

    // Keep NonVR aim solution fresh when enabled; this is throttled internally.
    if (m_ForceNonVRServerMovement)
        UpdateNonVRAimSolution(localPlayer);

    Vector eyeTarget = gunEnd;

    if (frontViewEyeAim || frontViewControllerEyeOrigin)
    {
        const Vector frontDir = frontViewEyeAim ? eyeDir : gunDir;
        if (!frontDir.IsZero())
            eyeTarget = eye + frontDir * 8192.0f;
    }
    else if (useMouse)
    {
        if (!eyeDir.IsZero())
            eyeTarget = eye + eyeDir * 8192.0f;
    }
    else
    {
        // When ForceNonVRServerMovement is enabled, we already solved for a point P that the
        // controller ray wants, and an eye-ray towards P that better matches server logic.
        if (m_ForceNonVRServerMovement && m_HasNonVRAimSolution)
            eyeTarget = m_NonVRAimDesiredPoint;
    }

    Vector eyeDir2 = eyeTarget - eye;
    if (!eyeDir2.IsZero())
    {
        VectorNormalize(eyeDir2);
        Vector eyeStart = eye + eyeDir2 * 2.0f;
        Vector eyeEnd = eyeStart + eyeDir2 * 8192.0f;

        CGameTrace traceEye;
        Ray_t rayEye;
        if (useHull)
            rayEye.Init(eyeStart, eyeEnd, hullMins, hullMaxs);
        else
            rayEye.Init(eyeStart, eyeEnd);
        SafeTraceRay(rayEye, traceMask, pTraceFilter, traceEye);

        const bool friendlyEye = evalFriendlyHitForTrace(traceEye, eyeStart, eyeEnd);

        m_AimLineHitsFriendly = (friendlyGun || friendlyEye);
        return m_AimLineHitsFriendly;
    }

    m_AimLineHitsFriendly = friendlyGun;
    return m_AimLineHitsFriendly;
}

bool VR::ShouldSuppressPrimaryFire(const CUserCmd* cmd, C_BasePlayer* localPlayer)
{
    if (!m_BlockFireOnFriendlyAimEnabled)
    {
        m_FriendlyFireGuardLatched = false;
        return false;
    }

    const bool attackDown = (cmd != nullptr) && ((cmd->buttons & (1 << 0)) != 0); // IN_ATTACK

    // Keep the hit result current on input ticks (CreateMove).
    if (localPlayer)
        UpdateFriendlyFireAimHit(localPlayer);

    const bool friendlyNow = m_AimLineHitsFriendly;

    // Latch until attack is released: prevents “pause then resume firing” while still holding.
    if (m_FriendlyFireGuardLatched)
    {
        if (!attackDown)
            m_FriendlyFireGuardLatched = false;
        return true;
    }

    if (attackDown && friendlyNow)
    {
        m_FriendlyFireGuardLatched = true;
        return true;
    }

    return false;
}

void VR::UpdateAimingLaser(C_BasePlayer* localPlayer)
{
    UpdateSpecialInfectedWarningState();
    UpdateSpecialInfectedPreWarningState();


    // In mat_queue_mode!=0, aim-line visuals should prefer the render-frame snapshot
    // to stay aligned with queued render smoothing (controller/viewmodel/eyes).
    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    struct RenderSnapshotTLSGuard
    {
        bool enabled = false;
        bool prev = false;
        RenderSnapshotTLSGuard(bool en)
        {
            enabled = en;
            if (enabled)
            {
                prev = VR::t_UseRenderFrameSnapshot;
                VR::t_UseRenderFrameSnapshot = true;
            }
        }
        ~RenderSnapshotTLSGuard()
        {
            if (enabled)
                VR::t_UseRenderFrameSnapshot = prev;
        }
    } tlsGuard(queueMode != 0);

    const bool queued = (queueMode != 0);
    const bool d3dAimLineEffective = m_D3DAimLineOverlayEnabled && !queued;

    const bool canDraw = (m_Game->m_DebugOverlay != nullptr) || d3dAimLineEffective;


    C_WeaponCSBase* activeWeapon = nullptr;
    if (localPlayer)
        activeWeapon = static_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());
    const bool allowAimLineDraw = ShouldDrawAimLine(activeWeapon);
    const bool scopeOnlyAimLine = m_ScopeAimLineOnlyInScope
        && m_ThirdPersonFrontViewEnabled
        && m_IsThirdPersonCamera
        && m_ScopeWeaponIsFirearm;
    if (!ShouldShowAimLine(activeWeapon))
    {
        m_LastAimDirection = Vector{ 0.0f, 0.0f, 0.0f };
        m_HasAimLine = false;
        m_HasAimConvergePoint = false;
        m_HasThrowArc = false;
        m_LastAimWasThrowable = false;
        {
            std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
            m_HasD3DAimLineWorldSegment = false;
        }

        // Even when the aim line is hidden/disabled, still run the friendly-fire guard trace
        // if the user toggled it on.
        UpdateFriendlyFireAimHit(localPlayer);
        // Aim-line teammate HUD hint is only meaningful while the aim line is active.
        UpdateAimTeammateHudTarget(localPlayer, Vector{}, Vector{}, false);

        return;
    }

    // If neither DebugOverlay nor the D3D overlay is available, don't draw, but keep the guard working.
    if (!canDraw)
    {
        UpdateFriendlyFireAimHit(localPlayer);
        UpdateAimTeammateHudTarget(localPlayer, Vector{}, Vector{}, false);
        {
            std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
            m_HasD3DAimLineWorldSegment = false;
        }

        return;
    }

    bool isThrowable = IsThrowableWeapon(activeWeapon);
    const bool useMouse = m_MouseModeEnabled;
    const bool frontViewEyeAim = m_ThirdPersonFrontViewEnabled
        && m_IsThirdPersonCamera
        && m_ThirdPersonFrontScopeFromEye
        && (localPlayer != nullptr);
    const bool frontViewControllerEyeOrigin = m_ThirdPersonFrontViewEnabled
        && m_IsThirdPersonCamera
        && !m_ThirdPersonFrontScopeFromEye
        && (localPlayer != nullptr);

    // Eye-center ray direction (mouse aim in mouse mode).
    Vector eyeDir = { 0.0f, 0.0f, 0.0f };
    if (useMouse)
    {
        GetMouseModeEyeRay(eyeDir);
    }
    else if (frontViewEyeAim)
    {
        eyeDir = m_HmdForward;
        if (eyeDir.IsZero())
        {
            QAngle eyeAngFallback = m_HmdAngAbs;
            NormalizeAndClampViewAngles(eyeAngFallback);
            QAngle::AngleVectors(eyeAngFallback, &eyeDir, nullptr, nullptr);
        }
        if (!eyeDir.IsZero())
            VectorNormalize(eyeDir);
    }

    // Aim direction:
    //  - Normal mode: controller forward (prefer render snapshot in queued mode).
    //  - Mouse mode (scheme B): start at the viewmodel anchor, but steer the ray to converge
    //    to the eye-center ray at MouseModeAimConvergeDistance.
    const Vector controllerPosAbs = GetRightControllerAbsPos();
    const QAngle controllerAngAbs = GetRightControllerAbsAngle();
    Vector controllerForward{}, controllerRight{}, controllerUp{};
    QAngle::AngleVectors(controllerAngAbs, &controllerForward, &controllerRight, &controllerUp);

    Vector direction = controllerForward;
    if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
        direction = m_RightControllerForwardUnforced;

    if (frontViewEyeAim)
    {
        direction = eyeDir;
    }
    else if (useMouse)
    {
        const Vector& anchor = IsMouseModeScopeActive() ? m_MouseModeScopedViewmodelAnchorOffset : m_MouseModeViewmodelAnchorOffset;
        Vector gunOrigin = m_HmdPosAbs
            + (m_HmdForward * (anchor.x * m_VRScale))
            + (m_HmdRight * (anchor.y * m_VRScale))
            + (m_HmdUp * (anchor.z * m_VRScale));

        const float convergeDist = (m_MouseModeAimConvergeDistance > 0.0f) ? m_MouseModeAimConvergeDistance : 8192.0f;
        Vector target = m_HmdPosAbs + eyeDir * convergeDist;

        direction = target - gunOrigin;
    }
    if (direction.IsZero())
    {
        if (m_LastAimDirection.IsZero())
        {
            const float duration = std::max(m_AimLinePersistence, m_LastFrameDuration * m_AimLineFrameDurationMultiplier);

            if (m_LastAimWasThrowable && m_HasThrowArc)
            {
                DrawThrowArcFromCache(duration);
                m_AimLineHitsFriendly = false;
                m_AimLineEffectiveAttackRangeActive = false;
                m_AimLineEffectiveAttackRangeHoldUntil = {};
                m_AimLineEffectiveAttackRangeCacheValid = false;
                return;
            }

            if (!m_HasAimLine)
            {
                m_AimLineHitsFriendly = false;
                m_AimLineEffectiveAttackRangeActive = false;
                m_AimLineEffectiveAttackRangeHoldUntil = {};
                m_AimLineEffectiveAttackRangeCacheValid = false;
                return;
            }


            if (!queued && allowAimLineDraw && !scopeOnlyAimLine)
                DrawLineWithThickness(m_AimLineStart, m_AimLineEnd, duration);
            return;
        }

        direction = m_LastAimDirection;
        isThrowable = m_LastAimWasThrowable;
    }
    else
    {
        m_LastAimDirection = direction;
        m_LastAimWasThrowable = isThrowable;
    }
    VectorNormalize(direction);

    // Aim line origin is controller-based. In third-person the rendered camera is offset behind the player,
    // so we translate the controller position into the rendered-camera frame.
    // We key off the actual camera delta (not just the boolean) to avoid cases where 3P detection flickers.
    Vector originBase = controllerPosAbs;
    if (frontViewEyeAim || frontViewControllerEyeOrigin)
    {
        originBase = localPlayer->EyePosition();
    }
    else if (useMouse)
    {
        const Vector& anchor = IsMouseModeScopeActive() ? m_MouseModeScopedViewmodelAnchorOffset : m_MouseModeViewmodelAnchorOffset;
        originBase = m_HmdPosAbs
            + (m_HmdForward * (anchor.x * m_VRScale))
            + (m_HmdRight * (anchor.y * m_VRScale))
            + (m_HmdUp * (anchor.z * m_VRScale));
    }
    // Keep non-3P codepath identical to legacy behavior; only use the new render-center delta in 3P.
    Vector camDelta = m_IsThirdPersonCamera
        ? (m_ThirdPersonRenderCenter - m_SetupOrigin)
        : (m_ThirdPersonViewOrigin - m_SetupOrigin);
    if (!frontViewEyeAim && !frontViewControllerEyeOrigin && m_IsThirdPersonCamera && camDelta.LengthSqr() > (5.0f * 5.0f))
        originBase += camDelta;

    Vector origin = originBase + direction * 2.0f;

    if (isThrowable)
    {
        m_AimLineHitsFriendly = false;
        m_HasAimConvergePoint = false;
        UpdateAimTeammateHudTarget(localPlayer, Vector{}, Vector{}, false);

        Vector pitchSource = direction;
        if ((useMouse || frontViewEyeAim) && !eyeDir.IsZero())
            pitchSource = eyeDir;
        else if (!m_ForceNonVRServerMovement && !m_HmdForward.IsZero())
            pitchSource = m_HmdForward;

        DrawThrowArc(origin, direction, pitchSource);
        return;
    }

    const float maxDistance = 8192.0f;
    Vector target = origin + direction * maxDistance;


    if (m_ForceNonVRServerMovement && m_HasNonVRAimSolution)
    {
        // On non-VR servers, H is the authoritative eye-ray hit point.
        // Use it in both 1P and 3P so the rendered aim line follows the same target
        // that cmd->viewangles is steering toward.
        m_AimConvergePoint = m_NonVRAimHitPoint;
        m_HasAimConvergePoint = true;
        target = m_AimConvergePoint;
    }
    else
    {
        // Third-person convergence point P: where the *rendered* aim ray hits.
        // IMPORTANT: We do NOT "correct" P based on what the bullet line can reach.
        if (m_IsThirdPersonCamera && localPlayer && m_Game->m_EngineTrace)
        {
            CGameTrace trace;
            Ray_t ray;
            C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);
            C_BaseCombatWeapon* activeWeapon = localPlayer->GetActiveWeapon();
            IClientEntityList* entityList = m_Game ? m_Game->m_ClientEntityList : nullptr;
            IHandleEntity* safeMountedUseEnt = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(mountedUseEnt));
            IHandleEntity* safeActiveWeapon = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(activeWeapon));
            CTraceFilterSkipThreeEntities tracefilterThree((IHandleEntity*)localPlayer, safeMountedUseEnt, safeActiveWeapon, 0);
            CTraceFilter* pTraceFilter = static_cast<CTraceFilter*>(&tracefilterThree);

            ray.Init(origin, target);
            VR_SafeTraceRay(m_Game->m_EngineTrace, ray, STANDARD_TRACE_MASK, pTraceFilter, trace);

            m_AimConvergePoint = (trace.fraction < 1.0f && trace.fraction > 0.0f) ? trace.endpos : target;
            m_HasAimConvergePoint = true;
            target = m_AimConvergePoint; // draw to P
        }
        else
        {
            m_HasAimConvergePoint = false;
        }
    }
    // Friendly-fire aim guard: see if the aim ray currently hits a teammate player.
    // This runs regardless of whether the line is actually drawn.
    UpdateFriendlyFireAimHit(localPlayer);

    // Teammate info hint: track which teammate the aim line is resting on.
    UpdateAimTeammateHudTarget(localPlayer, origin, target, true);


    m_AimLineStart = origin;
    m_AimLineEnd = target;
    m_HasAimLine = true;
    m_HasThrowArc = false;

    if (d3dAimLineEffective && localPlayer && m_Game && m_Game->m_EngineTrace
        && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame())
    {
        Vector visibleEnd = target;
        CGameTrace visibleTrace;
        Ray_t visibleRay;
        C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);
        C_BaseCombatWeapon* activeWeaponForTrace = localPlayer->GetActiveWeapon();
        IClientEntityList* entityList = m_Game ? m_Game->m_ClientEntityList : nullptr;
        IHandleEntity* safeMountedUseEnt = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(mountedUseEnt));
        IHandleEntity* safeActiveWeapon = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(activeWeaponForTrace));
        CTraceFilterSkipThreeEntities tracefilterThree((IHandleEntity*)localPlayer, safeMountedUseEnt, safeActiveWeapon, 0);
        CTraceFilter* pTraceFilter = static_cast<CTraceFilter*>(&tracefilterThree);
        visibleRay.Init(origin, target);
        if (VR_SafeTraceRay(m_Game->m_EngineTrace, visibleRay, STANDARD_TRACE_MASK, pTraceFilter, visibleTrace)
            && visibleTrace.fraction < 1.0f
            && visibleTrace.fraction > 0.0f)
        {
            visibleEnd = visibleTrace.endpos;
        }

        std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
        m_D3DAimLineWorldStart = origin;
        m_D3DAimLineWorldEnd = visibleEnd;
        m_HasD3DAimLineWorldSegment = !((visibleEnd - origin).IsZero());
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
        m_D3DAimLineWorldStart = origin;
        m_D3DAimLineWorldEnd = target;
        m_HasD3DAimLineWorldSegment = !((target - origin).IsZero());
    }

    if (!queued && canDraw && allowAimLineDraw && !scopeOnlyAimLine)
        DrawAimLine(origin, target);
}

void VR::UpdateAimTeammateHudTarget(C_BasePlayer* localPlayer, const Vector& start, const Vector& end, bool aimLineActive)
{
    using namespace std::chrono;
    const auto now = steady_clock::now();

    // Behavior update: show immediately on teammate hit, hide 1s after leaving teammate.
    constexpr float kLingerSeconds = 1.0f;
    // Small stickiness to prevent hitbox-edge flicker from causing false leave/reacquire.
    constexpr float kStickinessSeconds = 0.15f;

    if (!localPlayer || !m_Game || !m_Game->m_EngineTrace || !m_Game->m_ClientEntityList)
    {
        m_AimTeammateCandidateIndex = -1;
        m_AimTeammateLastRawIndex = -1;
        m_AimLineEffectiveAttackRangeActive = false;
        m_AimLineEffectiveAttackRangeHoldUntil = {};
        m_AimLineEffectiveAttackRangeCacheValid = false;
        if (m_AimTeammateDisplayIndex > 0 && now > m_AimTeammateDisplayUntil)
            m_AimTeammateDisplayIndex = -1;
        ClearAmmoHudAimTarget();
        return;
    }

    if (!aimLineActive)
    {
        m_AimTeammateCandidateIndex = -1;
        m_AimTeammateLastRawIndex = -1;
        m_AimLineEffectiveAttackRangeActive = false;
        m_AimLineEffectiveAttackRangeHoldUntil = {};
        m_AimLineEffectiveAttackRangeCacheValid = false;
        if (m_AimTeammateDisplayIndex > 0 && now > m_AimTeammateDisplayUntil)
            m_AimTeammateDisplayIndex = -1;
        ClearAmmoHudAimTarget();
        return;
    }

    C_BaseEntity* hitEnt = nullptr;
    int hitIdx = -1;
    {
        CGameTrace tr;
        Ray_t ray;

        // Skip self + mounted gun use entity + active weapon so the ray doesn't collide with your own gun/turret.
        C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);
        C_BaseCombatWeapon* activeWeapon = localPlayer->GetActiveWeapon();
        IClientEntityList* entityList = m_Game ? m_Game->m_ClientEntityList : nullptr;
        IHandleEntity* safeMountedUseEnt = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(mountedUseEnt));
        IHandleEntity* safeActiveWeapon = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(activeWeapon));
        CTraceFilterSkipThreeEntities filterThree((IHandleEntity*)localPlayer, safeMountedUseEnt, safeActiveWeapon, 0);
        CTraceFilter* pFilter = static_cast<CTraceFilter*>(&filterThree);

        ray.Init(start, end);
        VR_SafeTraceRay(m_Game->m_EngineTrace, ray, STANDARD_TRACE_MASK, pFilter, tr);

        hitEnt = reinterpret_cast<C_BaseEntity*>(tr.m_pEnt);
        UpdateAimLineEffectiveAttackRange(
            localPlayer,
            static_cast<C_WeaponCSBase*>(activeWeapon),
            hitEnt,
            start,
            end,
            tr.endpos,
            tr.fraction < 1.0f && tr.fraction > 0.0f && !tr.startsolid && !tr.allsolid);

        if (hitEnt && hitEnt != localPlayer)
        {
            const unsigned char* eb = reinterpret_cast<const unsigned char*>(hitEnt);
            unsigned char lifeState = 0;
            int team = 0;
            if (!VR_TryReadU8(eb, kLifeStateOffset, lifeState) || !VR_TryReadI32(eb, kTeamNumOffset, team))
                lifeState = 1;

            if (lifeState == 0 && team == 2)
            {
                // Resolve to a player index by scanning the entity list (cheap: cap to 64).
                const int hi = std::min(64, m_Game->m_ClientEntityList->GetHighestEntityIndex());
                for (int i = 1; i <= hi; ++i)
                {
                    if (m_Game->GetClientEntity(i) == hitEnt)
                    {
                        hitIdx = i;
                        break;
                    }
                }
            }
        }

        // RightAmmoHUD: show HP%% for the *aimed* special infected (and Witch).
        // Add a tiny sticky window on "target lost" so hitbox-edge jitter / transient netvar reads
        // don't cause the HUD to flash.
        constexpr float kAimTargetStickySeconds = 0.08f; // 80ms
        const long long nowTicks = (long long)now.time_since_epoch().count();

        // NOTE: use a function-static atomic so we don't require adding new members to VR (avoids header mismatch).
        static std::atomic<long long> s_AimTargetStickyUntilTicks{ 0 };

        auto clearAimTargetWithSticky = [&]()
            {
                const long long untilTicks = s_AimTargetStickyUntilTicks.load(std::memory_order_relaxed);
                if (untilTicks != 0 && nowTicks <= untilTicks)
                    return;

                s_AimTargetStickyUntilTicks.store(0, std::memory_order_relaxed);
                ClearAmmoHudAimTarget();
            };
        auto refreshAimTargetSticky = [&]()
            {
                const auto until = now + duration_cast<steady_clock::duration>(duration<float>(kAimTargetStickySeconds));
                s_AimTargetStickyUntilTicks.store((long long)until.time_since_epoch().count(), std::memory_order_relaxed);
            };
        auto updateAimTarget = [&](C_BaseEntity* ent)
            {
                if (!ent || !m_Game)
                {
                    clearAimTargetWithSticky();
                    return;
                }

                const unsigned char* eb = reinterpret_cast<const unsigned char*>(ent);

                int hp = 0;
                if (!VR_TryReadI32(eb, kHealthOffset, hp) || hp <= 0)
                {
                    clearAimTargetWithSticky();
                    return;
                }

                // L4D2 special infected are usually network-class "CTerrorPlayer" (team 3) with m_zombieClass set.
                // Witch also reports a special zombieClass value (see GetSpecialInfectedType).
                const SpecialInfectedType siType = GetSpecialInfectedType(ent);
                if (siType == SpecialInfectedType::None)
                {
                    // Ignore common infected (and everything else).
                    clearAimTargetWithSticky();
                    return;
                }

                // Extra safety: never show for survivors even if zombieClass read glitches.
                int team = 0;
                if (VR_TryReadI32(eb, kTeamNumOffset, team) && team == 2)
                {
                    clearAimTargetWithSticky();
                    return;
                }

                int maxHp = 0;
                VR_TryReadI32(eb, kMaxHealthOffset, maxHp);
                NotifyAmmoHudAimTarget((std::uintptr_t)ent, hp, maxHp);
                refreshAimTargetSticky();
            };

        updateAimTarget(hitEnt);

    }

    if (hitIdx > 0)
    {
        m_AimTeammateLastRawIndex = hitIdx;
        m_AimTeammateLastRawTime = now;
    }
    else if (m_AimTeammateLastRawIndex > 0)
    {
        const float dt = duration_cast<duration<float>>(now - m_AimTeammateLastRawTime).count();
        if (dt <= kStickinessSeconds)
            hitIdx = m_AimTeammateLastRawIndex;
        else
            m_AimTeammateLastRawIndex = -1;
    }

    // Leaving all teammates: keep last shown target for 1 second, then clear.
    if (hitIdx <= 0)
    {
        m_AimTeammateCandidateIndex = -1;
        if (m_AimTeammateDisplayIndex > 0 && now > m_AimTeammateDisplayUntil)
            m_AimTeammateDisplayIndex = -1;
        return;
    }

    // Aiming at teammate: show immediately and refresh linger window.
    m_AimTeammateCandidateIndex = hitIdx;
    m_AimTeammateCandidateSince = now;
    m_AimTeammateDisplayIndex = hitIdx;
    m_AimTeammateDisplayUntil = now + duration_cast<steady_clock::duration>(duration<float>(kLingerSeconds));
}

int VR::GetIncapMaxHealth() const
{
    return 300;
}

bool VR::GetAimTeammateHudInfo(int& outPlayerIndex, int& outPercent, char* outName, size_t outNameSize)
{
    if (!outName || outNameSize == 0)
        return false;

    outName[0] = 0;
    outPlayerIndex = -1;
    outPercent = 0;

    const auto now = std::chrono::steady_clock::now();
    if (m_AimTeammateDisplayIndex <= 0)
        return false;
    if (now > m_AimTeammateDisplayUntil)
    {
        m_AimTeammateDisplayIndex = -1;
        return false;
    }

    if (!m_Game)
        return false;

    C_BasePlayer* p = (C_BasePlayer*)m_Game->GetClientEntity(m_AimTeammateDisplayIndex);
    if (!p)
        return false;

    const unsigned char* pb = reinterpret_cast<const unsigned char*>(p);

    auto TryReadU8 = [](const unsigned char* base, int off, unsigned char& out) -> bool
        {
            __try { out = *reinterpret_cast<const unsigned char*>(base + off); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
        };
    auto TryReadInt = [](const unsigned char* base, int off, int& out) -> bool
        {
            __try { out = *reinterpret_cast<const int*>(base + off); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { out = 0; return false; }
        };
    auto TryReadFloat = [](const unsigned char* base, int off, float& out) -> bool
        {
            __try { out = *reinterpret_cast<const float*>(base + off); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { out = 0.0f; return false; }
        };

    unsigned char lifeState = 0;
    int team = 0;
    if (!TryReadU8(pb, kLifeStateOffset, lifeState) || !TryReadInt(pb, kTeamNumOffset, team))
        return false;
    if (lifeState != 0 || team != 2)
        return false;

    int hp = 0;
    float tempHPf = 0.0f;
    if (!TryReadInt(pb, kHealthOffset, hp) || !TryReadFloat(pb, kHealthBufferOffset, tempHPf))
        return false;

    const int tempHP = (int)std::max(0.0f, std::round(tempHPf));

    unsigned char downFlag = 0;
    const bool incap = TryReadU8(pb, kIsIncapacitatedOffset, downFlag) ? (downFlag != 0) : false;
    const bool ledge = TryReadU8(pb, kIsHangingFromLedgeOffset, downFlag) ? (downFlag != 0) : false;
    const bool down = incap || ledge;

    int pct = 0;
    if (down)
    {
        const int maxDown = GetIncapMaxHealth();
        if (maxDown > 0)
            pct = (int)((int64_t)hp * 100 / maxDown);
        pct = std::max(0, std::min(100, pct));
    }
    else
    {
        pct = std::max(0, std::min(100, hp + tempHP));
    }

    // Name: prefer the actual player name (UTF-8). We render via GDI in the HUD when needed.
    // NOTE: GetPlayerInfo() struct layouts vary across Source branches; use a robust extractor.
    GetPlayerNameUtf8Safe(m_Game ? m_Game->m_EngineClient : nullptr, m_AimTeammateDisplayIndex, outName, outNameSize);

    // Fallback: survivor character label (offline / bots / name unavailable).
    if (!outName[0])
    {
        int survivorChar = -1;
        if (TryReadInt(pb, kSurvivorCharacterOffset, survivorChar))
        {
            const char* sname = nullptr;
            switch (survivorChar)
            {
            case 0: sname = "NICK"; break;
            case 1: sname = "ROCHELLE"; break;
            case 2: sname = "COACH"; break;
            case 3: sname = "ELLIS"; break;
            case 4: sname = "BILL"; break;
            case 5: sname = "ZOEY"; break;
            case 6: sname = "FRANCIS"; break;
            case 7: sname = "LOUIS"; break;
            default: break;
            }
            if (sname && sname[0])
                ByteSafeCopy(outName, outNameSize, sname);
        }
    }

    if (!outName[0])
        std::snprintf(outName, outNameSize, "P%d", m_AimTeammateDisplayIndex);

    outPlayerIndex = m_AimTeammateDisplayIndex;
    outPercent = pct;
    return true;
}

bool VR::IsWeaponLaserSightActive(C_WeaponCSBase* weapon) const
{
    if (!weapon)
        return false;

    // L4D2 uses an upgrade bit-vector on most firearms (client netvar: m_upgradeBitVec).
    // We only care about whether the laser sight upgrade is active.
    // NOTE: The offset is stable across the common firearm weapon classes; if it ever changes, update it here.
    constexpr int kUpgradeBitVecOffset = 0xCF0;
    constexpr int kLaserSightBit = (1 << 2);

    int bitVec = 0;
    if (!VR_TryReadI32(reinterpret_cast<const unsigned char*>(weapon), kUpgradeBitVecOffset, bitVec))
        return false;
    return (bitVec & kLaserSightBit) != 0;
}

bool VR::IsEffectiveAttackRangeTarget(const C_BaseEntity* entity) const
{
    if (!entity || !m_Game)
        return false;

    const unsigned char* base = reinterpret_cast<const unsigned char*>(entity);
    int team = 0;
    const bool hasTeam = VR_TryReadI32(base, kTeamNumOffset, team);
    if (hasTeam && team == 2)
        return false;

    const int entityIndex = VR_FindClientEntityIndex(m_Game->m_ClientEntityList, entity);

    const char* className = m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(const_cast<C_BaseEntity*>(entity)));
    if (!className || !*className)
        return hasTeam && team == 3 && entityIndex > 0;

    std::string lowered(className);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const bool obstacle =
        lowered.find("world") != std::string::npos ||
        lowered.find("func") != std::string::npos ||
        lowered.find("brush") != std::string::npos ||
        lowered.find("prop") != std::string::npos ||
        lowered.find("door") != std::string::npos ||
        lowered.find("breakable") != std::string::npos ||
        lowered.find("weapon") != std::string::npos ||
        lowered.find("projectile") != std::string::npos ||
        lowered.find("physics") != std::string::npos ||
        lowered.find("moveable") != std::string::npos ||
        lowered.find("ragdoll") != std::string::npos ||
        lowered.find("trigger") != std::string::npos ||
        lowered.find("ladder") != std::string::npos ||
        lowered.find("env") != std::string::npos ||
        lowered.find("point") != std::string::npos ||
        lowered.find("info") != std::string::npos ||
        lowered.find("beam") != std::string::npos ||
        lowered.find("sprite") != std::string::npos ||
        lowered.find("particle") != std::string::npos ||
        lowered.find("light") != std::string::npos ||
        lowered.find("shadow") != std::string::npos ||
        lowered.find("water") != std::string::npos ||
        lowered.find("fog") != std::string::npos ||
        lowered.find("sky") != std::string::npos ||
        lowered.find("tonemap") != std::string::npos ||
        lowered.find("colorcorrection") != std::string::npos ||
        lowered.find("postprocess") != std::string::npos ||
        lowered.find("inferno") != std::string::npos ||
        lowered.find("baseentity") != std::string::npos;
    if (obstacle)
        return false;

    if (hasTeam && team == 0)
        return false;

    return true;
}

bool VR::IsEffectiveAttackRangeWitchTarget(const C_BaseEntity* entity) const
{
    if (!entity || !m_Game)
        return false;

    if (GetSpecialInfectedType(entity) == SpecialInfectedType::Witch)
        return true;

    const char* className = m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(const_cast<C_BaseEntity*>(entity)));
    if (!className || !*className)
        return false;

    std::string lowered(className);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered.find("witch") != std::string::npos;
}

void VR::LogEffectiveAttackRangeTarget(C_BaseEntity* entity, C_WeaponCSBase* weapon, float distance, float maxRange, float spreadDegrees, bool cached, const char* dataSource)
{
    if (!m_EffectiveAttackRangeDebugLog || !entity || !m_Game)
        return;

    const auto now = std::chrono::steady_clock::now();
    const std::uintptr_t entityTag = reinterpret_cast<std::uintptr_t>(entity);
    const bool sameEntity = m_EffectiveAttackRangeDebugLastEntity == entityTag;
    if (sameEntity && m_EffectiveAttackRangeDebugLastLog.time_since_epoch().count() != 0)
    {
        if (m_EffectiveAttackRangeDebugLogHz <= 0.0f)
            return;

        const float minInterval = 1.0f / m_EffectiveAttackRangeDebugLogHz;
        const float elapsed = std::chrono::duration<float>(now - m_EffectiveAttackRangeDebugLastLog).count();
        if (elapsed >= 0.0f && elapsed < minInterval)
            return;
    }

    m_EffectiveAttackRangeDebugLastEntity = entityTag;
    m_EffectiveAttackRangeDebugLastLog = now;

    const unsigned char* base = reinterpret_cast<const unsigned char*>(entity);
    int team = -1;
    unsigned char lifeState = 255;
    const bool hasTeam = VR_TryReadI32(base, kTeamNumOffset, team);
    const bool hasLifeState = VR_TryReadU8(base, kLifeStateOffset, lifeState);
    const char* className = m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(entity));
    const int entityIndex = VR_FindClientEntityIndex(m_Game->m_ClientEntityList, entity);
    const int weaponId = weapon ? static_cast<int>(weapon->GetWeaponID()) : -1;
    const VR_EntityNpcPlayerFlags npcPlayerFlags = VR_GetEntityNpcPlayerFlags(entity);
    const bool npcOrPlayer = npcPlayerFlags.isNpc || npcPlayerFlags.isPlayer;

    Game::logMsg(
        "[VR][EffectiveRange][hit] idx=%d ent=%p class=%s team=%d%s life=%d%s isNpc=%d isPlayer=%d npcOrPlayer=%d weaponId=%d dist=%.1f range=%.1f spreadDeg=%.4f hitPointTol=%.1f cached=%d dataSource=\"%s\"",
        entityIndex,
        static_cast<void*>(entity),
        (className && *className) ? className : "<null>",
        hasTeam ? team : -1,
        hasTeam ? "" : "?",
        hasLifeState ? static_cast<int>(lifeState) : -1,
        hasLifeState ? "" : "?",
        npcPlayerFlags.isNpc ? 1 : 0,
        npcPlayerFlags.isPlayer ? 1 : 0,
        npcOrPlayer ? 1 : 0,
        weaponId,
        distance,
        maxRange,
        spreadDegrees,
        std::clamp(m_EffectiveAttackRangeHitPointTolerance, 0.0f, 64.0f),
        cached ? 1 : 0,
        (dataSource && *dataSource) ? dataSource : "<unknown>");
}

bool VR::EnsureEffectiveAttackRangeWeaponDataLoaded()
{
    auto hasLoadedData = [&]() -> bool
        {
            for (const auto& data : m_EffectiveAttackRangeWeaponData)
            {
                if (data.valid)
                    return true;
            }
            return false;
        };

    const auto now = std::chrono::steady_clock::now();
    if (m_EffectiveAttackRangeWeaponDataLoaded && hasLoadedData())
        return hasLoadedData();
    if (m_EffectiveAttackRangeWeaponDataLoaded &&
        std::chrono::duration<float>(now - m_EffectiveAttackRangeWeaponDataLastLoad).count() < 2.0f)
    {
        return false;
    }

    m_EffectiveAttackRangeWeaponDataLoaded = true;
    m_EffectiveAttackRangeWeaponDataLastLoad = now;
    m_EffectiveAttackRangeWeaponData = {};

    auto loadScript = [&](C_WeaponCSBase::WeaponID weaponId, const char* scriptPath)
        {
            const size_t index = static_cast<size_t>(weaponId);
            if (!scriptPath || index >= m_EffectiveAttackRangeWeaponData.size())
                return;

            std::string text;
            std::string source;
            EffectiveAttackRangeWeaponData data{};
            if (VR_TryReadGameResourceText(scriptPath, text, &source) && VR_ParseEffectiveWeaponData(text, data))
            {
                data.source = source.empty() ? scriptPath : source;
                m_EffectiveAttackRangeWeaponData[index] = data;
            }
        };

    loadScript(C_WeaponCSBase::PISTOL, "scripts/weapon_pistol.txt");
    loadScript(C_WeaponCSBase::MAGNUM, "scripts/weapon_pistol_magnum.txt");
    loadScript(C_WeaponCSBase::UZI, "scripts/weapon_smg.txt");
    loadScript(C_WeaponCSBase::MAC10, "scripts/weapon_smg_silenced.txt");
    loadScript(C_WeaponCSBase::MP5, "scripts/weapon_smg_mp5.txt");
    loadScript(C_WeaponCSBase::M16A1, "scripts/weapon_rifle.txt");
    loadScript(C_WeaponCSBase::AK47, "scripts/weapon_rifle_ak47.txt");
    loadScript(C_WeaponCSBase::SCAR, "scripts/weapon_rifle_desert.txt");
    loadScript(C_WeaponCSBase::SG552, "scripts/weapon_rifle_sg552.txt");
    loadScript(C_WeaponCSBase::HUNTING_RIFLE, "scripts/weapon_hunting_rifle.txt");
    loadScript(C_WeaponCSBase::SNIPER_MILITARY, "scripts/weapon_sniper_military.txt");
    loadScript(C_WeaponCSBase::AWP, "scripts/weapon_sniper_awp.txt");
    loadScript(C_WeaponCSBase::SCOUT, "scripts/weapon_sniper_scout.txt");
    loadScript(C_WeaponCSBase::M60, "scripts/weapon_rifle_m60.txt");
    loadScript(C_WeaponCSBase::PUMPSHOTGUN, "scripts/weapon_pumpshotgun.txt");
    loadScript(C_WeaponCSBase::SHOTGUN_CHROME, "scripts/weapon_shotgun_chrome.txt");
    loadScript(C_WeaponCSBase::AUTOSHOTGUN, "scripts/weapon_autoshotgun.txt");
    loadScript(C_WeaponCSBase::SPAS, "scripts/weapon_shotgun_spas.txt");

    const bool anyLoaded = hasLoadedData();
    if (!anyLoaded && m_Game)
        Game::logMsg("[VR][EffectiveRange] no weapon script spread data loaded from game VPKs.");

    return anyLoaded;
}

const VR::EffectiveAttackRangeWeaponData* VR::GetEffectiveAttackRangeWeaponData(C_WeaponCSBase* weapon)
{
    if (!weapon || !EnsureEffectiveAttackRangeWeaponDataLoaded())
        return nullptr;

    const size_t index = static_cast<size_t>(weapon->GetWeaponID());
    if (index >= m_EffectiveAttackRangeWeaponData.size())
        return nullptr;

    const EffectiveAttackRangeWeaponData& data = m_EffectiveAttackRangeWeaponData[index];
    return data.valid ? &data : nullptr;
}

float VR::GetEffectiveAttackRangeSpreadDegrees(C_BasePlayer* localPlayer, C_WeaponCSBase* weapon, const EffectiveAttackRangeWeaponData& data) const
{
    if (!localPlayer || !weapon || !data.valid)
        return -1.0f;

    const unsigned char* base = reinterpret_cast<const unsigned char*>(localPlayer);
    int flags = 0;
    const bool hasFlags = VR_TryReadI32(base, kFlagsOffset, flags);
    const bool onGround = !hasFlags || ((flags & kOnGroundFlag) != 0);
    const bool ducking = hasFlags && ((flags & 0x2) != 0); // FL_DUCKING

    float spread = data.minStandingSpread;
    if (!onGround)
        spread = data.minInAirSpread;
    else if (ducking)
        spread = data.minDuckingSpread;

    if (onGround && data.maxMovementSpread > 0.0f && data.maxPlayerSpeed > 1.0f)
    {
        const float speed2D = localPlayer->m_vecVelocity.Length2D();
        const float moveT = std::clamp(speed2D / data.maxPlayerSpeed, 0.0f, 1.0f);
        spread = std::max(spread, data.maxMovementSpread * moveT);
    }

    if (data.bullets > 1)
        spread = std::max(spread, std::max(data.pelletScatterPitch, data.pelletScatterYaw));

    if (IsWeaponLaserSightActive(weapon))
    {
        const float laserScale = m_Game ? m_Game->GetConVarFloat("upgrade_laser_sight_spread_factor", 0.4f) : 0.4f;
        spread *= std::clamp(laserScale, 0.0f, 1.0f);
    }

    return std::clamp(spread, 0.0f, 45.0f);
}

float VR::GetEffectiveAttackRangeHitPointTolerance(const Vector& start, const Vector& centerHitPos, float spreadDegrees, float maxRange) const
{
    const float baseTolerance = std::clamp(m_EffectiveAttackRangeHitPointTolerance, 0.0f, 64.0f);
    const float maxTolerance = std::clamp(m_EffectiveAttackRangeHitPointMaxTolerance, baseTolerance, 64.0f);
    const float spreadScale = std::clamp(m_EffectiveAttackRangeHitPointSpreadScale, 0.0f, 2.0f);

    if (spreadScale <= 0.0f || spreadDegrees <= 0.0f)
        return baseTolerance;

    const float centerDistance = std::clamp((centerHitPos - start).Length(), 0.0f, std::clamp(maxRange, 1.0f, 65536.0f));
    const float coneRadiusAtHit = std::tan(DEG2RAD(std::clamp(spreadDegrees, 0.0f, 45.0f))) * centerDistance;
    const float dynamicTolerance = baseTolerance + (coneRadiusAtHit * spreadScale);
    return std::clamp(dynamicTolerance, baseTolerance, maxTolerance);
}

bool VR::DoesEffectiveAttackRangeSpreadConeHitTarget(C_BasePlayer* localPlayer, C_WeaponCSBase* weapon, C_BaseEntity* hitEntity, const Vector& start, const Vector& end, const Vector& centerHitPos, float spreadDegrees, float maxRange) const
{
    if (!localPlayer || !weapon || !hitEntity || !m_Game || !m_Game->m_EngineTrace || !m_Game->m_ClientEntityList)
        return false;

    Vector centerDir = end - start;
    if (centerDir.IsZero())
        return false;
    VectorNormalize(centerDir);

    if (spreadDegrees <= 0.0f)
        return true;

    const bool relaxedMovement = VR_IsEffectiveAttackRangeRelaxedMovementState(localPlayer);
    maxRange = std::clamp(maxRange, 1.0f, 65536.0f);
    const float hitPointTolerance = GetEffectiveAttackRangeHitPointTolerance(start, centerHitPos, spreadDegrees, maxRange);
    const float hitPointToleranceSqr = hitPointTolerance * hitPointTolerance;
    const float coneRadians = DEG2RAD(spreadDegrees);
    const float coneSin = std::sin(coneRadians);
    const float coneCos = std::cos(coneRadians);

    Vector up(0.0f, 0.0f, 1.0f);
    Vector right = CrossProduct(centerDir, up);
    if (right.LengthSqr() < 0.0001f)
    {
        up = Vector(1.0f, 0.0f, 0.0f);
        right = CrossProduct(centerDir, up);
    }
    if (right.IsZero())
        return false;
    VectorNormalize(right);
    up = CrossProduct(right, centerDir);
    if (up.IsZero())
        return false;
    VectorNormalize(up);

    C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);
    IClientEntityList* entityList = m_Game ? m_Game->m_ClientEntityList : nullptr;
    IHandleEntity* safeMountedUseEnt = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(mountedUseEnt));
    IHandleEntity* safeActiveWeapon = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(weapon));
    CTraceFilterSkipThreeEntities filterThree((IHandleEntity*)localPlayer, safeMountedUseEnt, safeActiveWeapon, 0);
    CTraceFilter* pFilter = static_cast<CTraceFilter*>(&filterThree);

    constexpr int kSamples = 8;
    constexpr int kRelaxedMovementRequiredHits = 5;
    int hitCount = 0;
    for (int i = 0; i < kSamples; ++i)
    {
        const float theta = (2.0f * 3.14159265358979323846f * static_cast<float>(i)) / static_cast<float>(kSamples);
        const Vector radial = right * std::cos(theta) + up * std::sin(theta);
        Vector sampleDir = centerDir * coneCos + radial * coneSin;
        if (sampleDir.IsZero())
            return false;
        VectorNormalize(sampleDir);

        CGameTrace sampleTrace;
        Ray_t sampleRay;
        sampleRay.Init(start, start + sampleDir * maxRange);
        if (!VR_SafeTraceRay(m_Game->m_EngineTrace, sampleRay, STANDARD_TRACE_MASK, pFilter, sampleTrace))
            return false;

        C_BaseEntity* sampleHit = reinterpret_cast<C_BaseEntity*>(sampleTrace.m_pEnt);
        const bool sampleHitsTarget =
            sampleHit == hitEntity &&
            sampleTrace.fraction > 0.0f &&
            sampleTrace.fraction < 1.0f &&
            !sampleTrace.startsolid &&
            !sampleTrace.allsolid;
        if (!sampleHitsTarget)
        {
            if (!relaxedMovement)
                return false;
            continue;
        }

        if (!relaxedMovement)
        {
            const Vector hitDelta = sampleTrace.endpos - centerHitPos;
            if (hitDelta.LengthSqr() > hitPointToleranceSqr)
                return false;
        }

        ++hitCount;
        if (relaxedMovement && hitCount >= kRelaxedMovementRequiredHits)
            return true;
    }

    return relaxedMovement ? (hitCount >= kRelaxedMovementRequiredHits) : true;
}

bool VR::TryFindEffectiveAttackRangeMeleeFanTarget(C_BasePlayer* localPlayer, C_WeaponCSBase* weapon, const Vector& start, const Vector& end, float maxDistance, C_BaseEntity*& outTarget, Vector& outTargetPos, float& outDistance) const
{
    outTarget = nullptr;
    outTargetPos = { 0.0f, 0.0f, 0.0f };
    outDistance = 0.0f;

    if (!localPlayer || !weapon || !m_Game || !m_Game->m_ClientEntityList || !m_Game->m_EngineTrace)
        return false;

    const float fanAngle = std::clamp(m_EffectiveAttackRangeMeleeFanAngle, 0.0f, 180.0f);
    if (fanAngle <= 0.0f || maxDistance <= 0.0f)
        return false;

    Vector forward = end - start;
    forward.z = 0.0f;
    if (forward.IsZero())
        return false;
    VectorNormalize(forward);

    const float halfAngleRad = fanAngle * (3.14159265358979323846f / 180.0f) * 0.5f;
    const float minDot = std::cos(halfAngleRad);

    const Vector traceStart = localPlayer->EyePosition();
    C_BaseEntity* mountedUseEnt = GetMountedGunUseEntity(localPlayer);
    IClientEntityList* entityList = m_Game->m_ClientEntityList;
    IHandleEntity* safeMountedUseEnt = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(mountedUseEnt));
    IHandleEntity* safeActiveWeapon = VR_GetSafeTraceSkipEntity(entityList, reinterpret_cast<IHandleEntity*>(weapon));
    CTraceFilterSkipThreeEntities filterThree(reinterpret_cast<IHandleEntity*>(localPlayer), safeMountedUseEnt, safeActiveWeapon, 0);
    CTraceFilter* pFilter = static_cast<CTraceFilter*>(&filterThree);

    float bestDistance = maxDistance + 1.0f;
    float bestDot = -1.0f;
    const int highestEntityIndex = entityList->GetHighestEntityIndex();
    for (int entityIndex = 1; entityIndex <= highestEntityIndex; ++entityIndex)
    {
        C_BaseEntity* candidate = m_Game->GetClientEntity(entityIndex);
        if (!candidate || candidate == localPlayer)
            continue;

        if (!IsEntityAlive(candidate))
            continue;

        const unsigned char* base = reinterpret_cast<const unsigned char*>(candidate);
        int team = 0;
        const bool hasTeam = VR_TryReadI32(base, kTeamNumOffset, team);
        if ((hasTeam && team == 2) || (hasTeam && team == 0))
            continue;

        Vector targetPos{};
        if (!VR_TryGetEntityAbsOrigin(candidate, targetPos))
            continue;
        targetPos.z += 36.0f;

        Vector toTargetPlanar = targetPos - traceStart;
        toTargetPlanar.z = 0.0f;
        const float planarDistance = toTargetPlanar.Length();
        if (!std::isfinite(planarDistance) || planarDistance <= 0.1f || planarDistance > maxDistance)
            continue;

        Vector targetDir = toTargetPlanar;
        VectorNormalize(targetDir);
        const float targetDot = DotProduct(forward, targetDir);
        if (!std::isfinite(targetDot) || targetDot < minDot)
            continue;

        if (!IsEffectiveAttackRangeTarget(candidate))
            continue;

        CGameTrace trace;
        Ray_t ray;
        ray.Init(traceStart, targetPos);
        if (!VR_SafeTraceRay(m_Game->m_EngineTrace, ray, STANDARD_TRACE_MASK, pFilter, trace))
            continue;

        const C_BaseEntity* traceEntity = reinterpret_cast<C_BaseEntity*>(trace.m_pEnt);
        const bool unobstructed =
            (!trace.startsolid && !trace.allsolid) &&
            (traceEntity == candidate || trace.fraction >= 0.999f);
        if (!unobstructed)
            continue;

        const bool betterCandidate =
            (planarDistance + 0.01f < bestDistance) ||
            (std::fabs(planarDistance - bestDistance) <= 0.01f && targetDot > bestDot);
        if (!betterCandidate)
            continue;

        outTarget = candidate;
        outTargetPos = targetPos;
        outDistance = planarDistance;
        bestDistance = planarDistance;
        bestDot = targetDot;
    }

    return outTarget != nullptr;
}

void VR::UpdateAimLineEffectiveAttackRange(C_BasePlayer* localPlayer, C_WeaponCSBase* weapon, C_BaseEntity* hitEntity, const Vector& start, const Vector& end, const Vector& hitPos, bool hasAimHit)
{
    const auto now = std::chrono::steady_clock::now();
    const auto holdDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(std::clamp(m_EffectiveAttackRangeHoldSeconds, 0.0f, 1.0f)));

    auto clearImmediate = [&]()
        {
            m_AimLineEffectiveAttackRangeActive = false;
            m_AimLineEffectiveAttackRangeHoldUntil = {};
            m_AimLineEffectiveAttackRangeCacheValid = false;
            m_AimLineEffectiveAttackRangeTarget = 0;
            m_AimLineEffectiveAttackRangeTargetIsWitch = false;
        };

    auto updateHeldState = [&](bool rawActive)
        {
            if (rawActive)
            {
                m_AimLineEffectiveAttackRangeActive = true;
                m_AimLineEffectiveAttackRangeHoldUntil = now + holdDuration;
                return;
            }

            const bool stillHeld = m_AimLineEffectiveAttackRangeHoldUntil.time_since_epoch().count() != 0
                && now <= m_AimLineEffectiveAttackRangeHoldUntil;
            if (stillHeld)
            {
                m_AimLineEffectiveAttackRangeActive = true;
                return;
            }

            clearImmediate();
        };

    if (!m_EffectiveAttackRangeIndicatorEnabled || !localPlayer || !weapon)
    {
        clearImmediate();
        return;
    }

    const C_WeaponCSBase::WeaponID weaponId = weapon->GetWeaponID();
    if (weaponId == C_WeaponCSBase::MELEE || weaponId == C_WeaponCSBase::CHAINSAW)
    {
        const float meleeDistance = std::max(1.0f, m_EffectiveAttackRangeMeleeDistance);
        bool meleeActive = false;

        if (hasAimHit && hitEntity && hitEntity != localPlayer && IsEffectiveAttackRangeTarget(hitEntity))
        {
            const float directDistance = (hitPos - start).Length();
            if (std::isfinite(directDistance) && directDistance > 0.1f && directDistance <= meleeDistance)
            {
                m_AimLineEffectiveAttackRangeTarget = reinterpret_cast<std::uintptr_t>(hitEntity);
                m_AimLineEffectiveAttackRangeTargetIsWitch = IsEffectiveAttackRangeWitchTarget(hitEntity);
                LogEffectiveAttackRangeTarget(hitEntity, weapon, directDistance, meleeDistance, 0.0f, false, "config:EffectiveAttackRangeMeleeDistance(aim)");
                meleeActive = true;
            }
        }

        if (!meleeActive)
        {
            C_BaseEntity* fanTarget = nullptr;
            Vector fanTargetPos{};
            float fanDistance = 0.0f;
            if (TryFindEffectiveAttackRangeMeleeFanTarget(localPlayer, weapon, start, end, meleeDistance, fanTarget, fanTargetPos, fanDistance))
            {
                m_AimLineEffectiveAttackRangeTarget = reinterpret_cast<std::uintptr_t>(fanTarget);
                m_AimLineEffectiveAttackRangeTargetIsWitch = IsEffectiveAttackRangeWitchTarget(fanTarget);
                LogEffectiveAttackRangeTarget(fanTarget, weapon, fanDistance, meleeDistance, 0.0f, false, "config:EffectiveAttackRangeMeleeDistance+FanAngle");
                meleeActive = true;
            }
        }

        if (!meleeActive)
            m_AimLineEffectiveAttackRangeTargetIsWitch = false;

        updateHeldState(meleeActive);
        return;
    }

    if (!hasAimHit || !hitEntity || hitEntity == localPlayer)
    {
        updateHeldState(false);
        return;
    }

    m_AimLineEffectiveAttackRangeTarget = reinterpret_cast<std::uintptr_t>(hitEntity);
    m_AimLineEffectiveAttackRangeTargetIsWitch = IsEffectiveAttackRangeWitchTarget(hitEntity);

    if (!IsEffectiveAttackRangeTarget(hitEntity))
    {
        updateHeldState(false);
        return;
    }

    const float distance = (hitPos - start).Length();
    if (!std::isfinite(distance) || distance <= 0.1f)
    {
        updateHeldState(false);
        return;
    }

    const EffectiveAttackRangeWeaponData* data = GetEffectiveAttackRangeWeaponData(weapon);
    if (!data)
    {
        updateHeldState(false);
        return;
    }

    if (distance > data->range)
    {
        updateHeldState(false);
        return;
    }

    const float spreadDeg = GetEffectiveAttackRangeSpreadDegrees(localPlayer, weapon, *data);
    if (spreadDeg < 0.0f)
    {
        updateHeldState(false);
        return;
    }

    Vector centerDir = end - start;
    if (centerDir.IsZero())
    {
        updateHeldState(false);
        return;
    }
    VectorNormalize(centerDir);

    const std::uintptr_t entityTag = reinterpret_cast<std::uintptr_t>(hitEntity);
    const std::uintptr_t weaponTag = reinterpret_cast<std::uintptr_t>(weapon);
    const bool relaxedMovement = VR_IsEffectiveAttackRangeRelaxedMovementState(localPlayer);
    const float cacheAge = m_AimLineEffectiveAttackRangeCacheValid
        ? std::chrono::duration<float>(now - m_AimLineEffectiveAttackRangeCacheTime).count()
        : 999.0f;
    const float dirDot = m_AimLineEffectiveAttackRangeCacheDirection.IsZero()
        ? -1.0f
        : DotProduct(centerDir, m_AimLineEffectiveAttackRangeCacheDirection);
    const float cacheHitPointTolerance = GetEffectiveAttackRangeHitPointTolerance(start, hitPos, spreadDeg, data->range);
    const float cacheHitPointToleranceSqr = cacheHitPointTolerance * cacheHitPointTolerance;
    const bool canReuseCachedResult =
        m_AimLineEffectiveAttackRangeCacheValid &&
        m_AimLineEffectiveAttackRangeCacheEntity == entityTag &&
        m_AimLineEffectiveAttackRangeCacheWeapon == weaponTag &&
        cacheAge >= 0.0f &&
        cacheAge <= std::clamp(m_EffectiveAttackRangeCacheSeconds, 0.0f, 1.0f) &&
        std::fabs(distance - m_AimLineEffectiveAttackRangeCacheDistance) <= std::clamp(m_EffectiveAttackRangeCacheDistanceTolerance, 0.0f, 256.0f) &&
        std::fabs(spreadDeg - m_AimLineEffectiveAttackRangeCacheSpread) <= std::clamp(m_EffectiveAttackRangeCacheSpreadTolerance, 0.0f, 10.0f) &&
        (hitPos - m_AimLineEffectiveAttackRangeCacheHitPos).LengthSqr() <= cacheHitPointToleranceSqr &&
        m_AimLineEffectiveAttackRangeCacheRelaxedMovement == relaxedMovement &&
        dirDot >= std::clamp(m_EffectiveAttackRangeCacheDirectionDot, 0.0f, 1.0f);

    bool rawActive = false;
    bool usedCache = false;
    if (canReuseCachedResult)
    {
        rawActive = m_AimLineEffectiveAttackRangeCacheResult;
        usedCache = true;
    }
    else
    {
        rawActive = DoesEffectiveAttackRangeSpreadConeHitTarget(
            localPlayer,
            weapon,
            hitEntity,
            start,
            end,
            hitPos,
            spreadDeg,
            data->range);

        m_AimLineEffectiveAttackRangeCacheValid = true;
        m_AimLineEffectiveAttackRangeCacheResult = rawActive;
        m_AimLineEffectiveAttackRangeCacheEntity = entityTag;
        m_AimLineEffectiveAttackRangeCacheWeapon = weaponTag;
        m_AimLineEffectiveAttackRangeCacheTime = now;
        m_AimLineEffectiveAttackRangeCacheDistance = distance;
        m_AimLineEffectiveAttackRangeCacheSpread = spreadDeg;
        m_AimLineEffectiveAttackRangeCacheHitPos = hitPos;
        m_AimLineEffectiveAttackRangeCacheDirection = centerDir;
        m_AimLineEffectiveAttackRangeCacheRelaxedMovement = relaxedMovement;
    }

    if (rawActive)
        LogEffectiveAttackRangeTarget(hitEntity, weapon, distance, data->range, spreadDeg, usedCache, data->source.c_str());

    updateHeldState(rawActive);
}

bool VR::ShouldDrawAimLine(C_WeaponCSBase* weapon) const
{
    if (!m_AimLineOnlyWhenLaserSight)
        return true;

    if (!weapon)
        return false;

    // Never suppress throwable arcs; those are handled separately and do not use DrawAimLine().
    if (IsThrowableWeapon(weapon))
        return false;

    // Only show for firearms that have an active in-game laser sight.
    switch (weapon->GetWeaponID())
    {
    case C_WeaponCSBase::PISTOL:
    case C_WeaponCSBase::MAGNUM:
    case C_WeaponCSBase::UZI:
    case C_WeaponCSBase::PUMPSHOTGUN:
    case C_WeaponCSBase::AUTOSHOTGUN:
    case C_WeaponCSBase::M16A1:
    case C_WeaponCSBase::HUNTING_RIFLE:
    case C_WeaponCSBase::MAC10:
    case C_WeaponCSBase::SHOTGUN_CHROME:
    case C_WeaponCSBase::SCAR:
    case C_WeaponCSBase::SNIPER_MILITARY:
    case C_WeaponCSBase::SPAS:
    case C_WeaponCSBase::AK47:
    case C_WeaponCSBase::MP5:
    case C_WeaponCSBase::SG552:
    case C_WeaponCSBase::AWP:
    case C_WeaponCSBase::SCOUT:
    case C_WeaponCSBase::GRENADE_LAUNCHER:
    case C_WeaponCSBase::M60:
        return IsWeaponLaserSightActive(weapon);
    default:
        return false;
    }
}

bool VR::ShouldShowAimLine(C_WeaponCSBase* weapon) const
{
    // While pinned/controlled by SI, the player model/camera can be driven by animations,
// causing the aim line to wildly drift and feel broken. Disable it in those states.
    if (m_PlayerControlledBySI)
        return false;

    if (!weapon)
        return false;

    switch (weapon->GetWeaponID())
    {
    case C_WeaponCSBase::TANK_CLAW:
    case C_WeaponCSBase::HUNTER_CLAW:
    case C_WeaponCSBase::CHARGER_CLAW:
    case C_WeaponCSBase::BOOMER_CLAW:
    case C_WeaponCSBase::SMOKER_CLAW:
    case C_WeaponCSBase::SPITTER_CLAW:
    case C_WeaponCSBase::JOCKEY_CLAW:
    case C_WeaponCSBase::VOMIT:
    case C_WeaponCSBase::SPLAT:
    case C_WeaponCSBase::POUNCE:
    case C_WeaponCSBase::LOUNGE:
    case C_WeaponCSBase::PULL:
    case C_WeaponCSBase::CHOKE:
    case C_WeaponCSBase::ROCK:
    case C_WeaponCSBase::MELEE:
    case C_WeaponCSBase::CHAINSAW:
        return m_MeleeAimLineEnabled;
        // Firearms and throwables use the main aim-line toggle.
    case C_WeaponCSBase::PISTOL:
    case C_WeaponCSBase::MAGNUM:
    case C_WeaponCSBase::UZI:
    case C_WeaponCSBase::PUMPSHOTGUN:
    case C_WeaponCSBase::AUTOSHOTGUN:
    case C_WeaponCSBase::M16A1:
    case C_WeaponCSBase::HUNTING_RIFLE:
    case C_WeaponCSBase::MAC10:
    case C_WeaponCSBase::SHOTGUN_CHROME:
    case C_WeaponCSBase::SCAR:
    case C_WeaponCSBase::SNIPER_MILITARY:
    case C_WeaponCSBase::SPAS:
    case C_WeaponCSBase::AK47:
    case C_WeaponCSBase::MP5:
    case C_WeaponCSBase::SG552:
    case C_WeaponCSBase::AWP:
    case C_WeaponCSBase::SCOUT:
    case C_WeaponCSBase::GRENADE_LAUNCHER:
    case C_WeaponCSBase::M60:
    case C_WeaponCSBase::MOLOTOV:
    case C_WeaponCSBase::PIPE_BOMB:
    case C_WeaponCSBase::VOMITJAR:
        return m_AimLineEnabled;
    default:
        return false;
    }
}

void VR::CycleScopeMagnification()
{
    if (!m_ScopeEnabled || m_ScopeMagnificationOptions.empty())
        return;

    m_ScopeMagnificationIndex = (m_ScopeMagnificationIndex + 1) % m_ScopeMagnificationOptions.size();
    m_ScopeFov = std::clamp(m_ScopeMagnificationOptions[m_ScopeMagnificationIndex], 1.0f, 179.0f);

    // Changing magnification changes the sensitivity scale. Rebase on next frame to avoid a sudden jump.
    if (m_ScopeActive)
        m_ScopeAimSensitivityInit = false;
    Game::logMsg("[VR] Scope magnification set to %.2f", m_ScopeFov);
}

void VR::UpdateScopeAimLineState()
{
    if (m_AimLineConfigEnabled)
    {
        m_ScopeForcingAimLine = false;
        return;
    }

    if (!m_ScopeEnabled)
    {
        if (m_ScopeForcingAimLine)
        {
            m_AimLineEnabled = false;
            m_HasAimLine = false;
            m_ScopeForcingAimLine = false;
        }
        return;
    }

    const bool scopeActive = IsScopeActive();
    if (scopeActive && !m_AimLineEnabled)
    {
        m_AimLineEnabled = true;
        m_ScopeForcingAimLine = true;
    }
    else if (!scopeActive && m_ScopeForcingAimLine)
    {
        m_AimLineEnabled = false;
        m_HasAimLine = false;
        m_ScopeForcingAimLine = false;
    }
}

void VR::ToggleMouseModeScope()
{
    if (!m_MouseModeEnabled)
        return;

    m_MouseModeScopeToggleActive = !m_MouseModeScopeToggleActive;

    // Clear lingering stabilization history.
    m_ScopeStabilizationInit = false;
    m_ScopeStabilizationLastTime = {};
}

bool VR::IsThrowableWeapon(C_WeaponCSBase* weapon) const
{
    if (!weapon)
        return false;

    switch (weapon->GetWeaponID())
    {
    case C_WeaponCSBase::MOLOTOV:
    case C_WeaponCSBase::PIPE_BOMB:
    case C_WeaponCSBase::VOMITJAR:
        return true;
    default:
        return false;
    }
}

float VR::CalculateThrowArcDistance(const Vector& pitchSource, bool* clampedToMax) const
{
    Vector direction = pitchSource;
    if (direction.IsZero())
        return m_ThrowArcMinDistance;

    VectorNormalize(direction);

    const float pitchInfluence = direction.z * m_ThrowArcPitchScale;
    const float scaledDistance = m_ThrowArcBaseDistance * (1.0f + pitchInfluence);
    const float maxDistance = std::max(m_ThrowArcMinDistance, m_ThrowArcMaxDistance);
    const float clampedDistance = std::clamp(scaledDistance, m_ThrowArcMinDistance, maxDistance);

    if (clampedToMax)
        *clampedToMax = clampedDistance >= maxDistance;

    return clampedDistance;
}

void VR::DrawAimLine(const Vector& start, const Vector& end)
{
    if (!m_AimLineEnabled || !m_Game || !m_Game->m_DebugOverlay)
        return;
    const int queueMode = m_Game ? m_Game->GetMatQueueMode() : 0;
    const bool d3dAimLineEffective = m_D3DAimLineOverlayEnabled && (queueMode == 0);
    if (d3dAimLineEffective && !m_ScopeRenderingPass)
        return;

    const bool scopeOnlyAimLine = m_ScopeAimLineOnlyInScope
        && m_ThirdPersonFrontViewEnabled
        && m_IsThirdPersonCamera
        && m_ScopeWeaponIsFirearm;
    if (scopeOnlyAimLine && !m_ScopeRenderingPass)
        return;

    // Draw every frame with ~single-frame lifetime. DebugOverlay primitives persist for "duration" seconds;
    // if duration spans multiple frames while we also draw every frame, you get visible "ghost" trails.
    // Keeping duration close to the current frame interval avoids both ghosting and flicker.
    float dt = std::clamp(m_LastFrameDuration, 1.0f / 240.0f, 1.0f / 20.0f);

    float duration = dt * 0.99f;
    duration = std::clamp(duration, 0.001f, 0.050f);
    if (scopeOnlyAimLine && m_ScopeRenderingPass)
        duration = 0.0f;

    DrawLineWithThickness(start, end, duration);
}



bool VR::BuildRenderAimLineSegment(C_BasePlayer* localPlayer, Vector& start, Vector& end)
{
    start = Vector{ 0.0f, 0.0f, 0.0f };
    end = Vector{ 0.0f, 0.0f, 0.0f };

    if (!m_Game || m_HasThrowArc)
        return false;

    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (queueMode == 0)
    {
        if (!m_HasAimLine)
            return false;
        start = m_AimLineStart;
        end = m_AimLineEnd;
        return !((end - start).IsZero());
    }

    // Ensure render-thread getters pull from the render-frame snapshot.
    struct RenderSnapshotTLSGuard
    {
        bool prev = false;
        RenderSnapshotTLSGuard()
        {
            prev = VR::t_UseRenderFrameSnapshot;
            VR::t_UseRenderFrameSnapshot = true;
        }
        ~RenderSnapshotTLSGuard()
        {
            VR::t_UseRenderFrameSnapshot = prev;
        }
    } tlsGuard;

    // If update-side aiming has no valid ray this frame, don't guess here.
    if (!m_HasAimLine && m_LastAimDirection.IsZero())
        return false;

    const bool useMouse = m_MouseModeEnabled;
    const bool frontViewEyeAim = m_ThirdPersonFrontViewEnabled
        && m_IsThirdPersonCamera
        && m_ThirdPersonFrontScopeFromEye;
    const bool frontViewControllerEyeOrigin = m_ThirdPersonFrontViewEnabled
        && m_IsThirdPersonCamera
        && !m_ThirdPersonFrontScopeFromEye;

    if (useMouse)
    {
        // Mouse mode uses an HMD-anchored origin. Keep the update-side solution (it already handles convergence).
        if (!m_HasAimLine)
            return false;

        start = m_AimLineStart;
        end = m_AimLineEnd;
        return !((end - start).IsZero());
    }

    Vector originBase = GetRightControllerAbsPos();
    Vector dir{};

    if (frontViewEyeAim || frontViewControllerEyeOrigin)
    {
        // Front-view mode: keep the rendered aim line origin at the eye in queued rendering.
        originBase = m_SetupOrigin;
        if (frontViewEyeAim)
        {
            const Vector viewAng = GetViewAngle();
            QAngle eyeAng(viewAng.x, viewAng.y, viewAng.z);
            NormalizeAndClampViewAngles(eyeAng);

            Vector eyeForward{};
            QAngle::AngleVectors(eyeAng, &eyeForward, nullptr, nullptr);
            dir = eyeForward;
            if (dir.IsZero())
                dir = m_HmdForward;
        }
        else
        {
            const QAngle angAbs = GetRightControllerAbsAngle();

            Vector f{}, r{}, u{};
            QAngle::AngleVectors(angAbs, &f, &r, &u);
            dir = f;
            if (m_IsThirdPersonCamera && !m_RightControllerForwardUnforced.IsZero())
                dir = m_RightControllerForwardUnforced;
        }
    }
    else
    {
        // Use the *render-frame* controller pose so the line stays glued to the hand/gun.
        if (m_IsThirdPersonCamera)
        {
            // In third-person, the VR render camera is moved away from the player eye.
            // The local VR hand/viewmodel visuals are shifted by the same delta; apply it so the aim line stays on the hand.
            const Vector camDelta = (m_ThirdPersonRenderCenter - m_SetupOrigin);
            if (camDelta.LengthSqr() > (5.0f * 5.0f))
                originBase += camDelta;
        }

        const QAngle angAbs = GetRightControllerAbsAngle();

        Vector f{}, r{}, u{};
        QAngle::AngleVectors(angAbs, &f, &r, &u);

        dir = f;
    }

    if (dir.IsZero())
        dir = m_LastAimDirection.IsZero() ? m_LastUnforcedAimDirection : m_LastAimDirection;

    if (dir.IsZero())
        return false;

    VectorNormalize(dir);

    start = originBase + dir * 2.0f;

    const float maxDistance = 8192.0f;
    if (m_ForceNonVRServerMovement && m_HasNonVRAimSolution)
    {
        // Keep queued-render aim-line target authoritative in 3P as well.
        end = m_NonVRAimHitPoint;
    }
    else
    {
        // In third-person, avoid using the update-thread converge point here (it may be in a different phase);
        // draw a pure controller ray so the line always stays attached to the hand.
        end = start + dir * maxDistance;
    }

    return !((end - start).IsZero());
}

void VR::ClearD3DAimLineOverlayEye(int eyeIndex)
{
    if (eyeIndex < 0 || eyeIndex >= static_cast<int>(m_D3DAimLineOverlayEyes.size()))
        return;

    std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
    m_D3DAimLineOverlayEyes[eyeIndex] = {};
}

void VR::ClearD3DAimLineOverlay()
{
    std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
    for (auto& eye : m_D3DAimLineOverlayEyes)
        eye = {};
    m_D3DAimLineWorldStart = {};
    m_D3DAimLineWorldEnd = {};
    m_HasD3DAimLineWorldSegment = false;
}

bool VR::GetD3DAimLineOverlayEye(int eyeIndex, D3DAimLineOverlayEyeState& out) const
{
    if (eyeIndex < 0 || eyeIndex >= static_cast<int>(m_D3DAimLineOverlayEyes.size()))
        return false;

    std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
    out = m_D3DAimLineOverlayEyes[eyeIndex];
    return out.valid;
}

void VR::UpdateD3DAimLineOverlayForView(C_BasePlayer* localPlayer, const CViewSetup& view, int eyeIndex)
{
    if (eyeIndex < 0 || eyeIndex >= static_cast<int>(m_D3DAimLineOverlayEyes.size()))
        return;

    auto clearEye = [&]()
        {
            std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
            m_D3DAimLineOverlayEyes[eyeIndex] = {};
        };

    const int queueMode = m_Game ? m_Game->GetMatQueueMode() : 0;
    if (!m_D3DAimLineOverlayEnabled || queueMode != 0 || !m_AimLineEnabled || !m_IsVREnabled || !localPlayer)
    {
        clearEye();
        return;
    }

    const bool scopeOnlyAimLine = m_ScopeAimLineOnlyInScope
        && m_ThirdPersonFrontViewEnabled
        && m_IsThirdPersonCamera
        && m_ScopeWeaponIsFirearm;
    if (scopeOnlyAimLine && !m_ScopeRenderingPass)
    {
        clearEye();
        return;
    }

    Vector start{};
    Vector end{};
    if (!BuildRenderAimLineSegment(localPlayer, start, end))
    {
        clearEye();
        return;
    }

    Vector cachedWorldStart{};
    Vector cachedWorldEnd{};
    bool hasCachedWorldSegment = false;
    {
        std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
        cachedWorldStart = m_D3DAimLineWorldStart;
        cachedWorldEnd = m_D3DAimLineWorldEnd;
        hasCachedWorldSegment = m_HasD3DAimLineWorldSegment;
    }

    if (hasCachedWorldSegment)
    {
        Vector dir = end - start;
        const float visibleDistance = (cachedWorldEnd - cachedWorldStart).Length();
        if (!dir.IsZero() && visibleDistance > 0.1f)
        {
            VectorNormalize(dir);
            end = start + dir * visibleDistance;
        }
    }

    D3DAimLineOverlayEyeState state{};
    state.valid = VR_ProjectAimLineSegmentToView(view, start, end, state.x0, state.y0, state.x1, state.y1);
    if (!state.valid)
    {
        clearEye();
        return;
    }

    state.widthPixels = m_D3DAimLineOverlayWidthPixels;
    state.outlinePixels = m_D3DAimLineOverlayOutlinePixels;
    state.endpointPixels = m_D3DAimLineOverlayEndpointPixels;

    int colorR = m_D3DAimLineOverlayColorR;
    int colorG = m_D3DAimLineOverlayColorG;
    int colorB = m_D3DAimLineOverlayColorB;
    int colorA = m_D3DAimLineOverlayColorA;
    if (m_D3DAimLineOverlaySyncAimLineColor)
    {
        GetAimLineColor(colorR, colorG, colorB, colorA);
        if (colorA > 0)
            colorA = m_D3DAimLineOverlayColorA;
    }

    state.color = VR_PackD3DColor(colorR, colorG, colorB, colorA);
    state.outlineColor = VR_PackD3DColor(
        m_D3DAimLineOverlayOutlineColorR, m_D3DAimLineOverlayOutlineColorG, m_D3DAimLineOverlayOutlineColorB, m_D3DAimLineOverlayOutlineColorA);

    if (((state.color >> 24) & 0xFFu) == 0u || state.widthPixels <= 0.0f)
    {
        clearEye();
        return;
    }

    std::lock_guard<std::mutex> lock(m_D3DAimLineOverlayMutex);
    m_D3DAimLineOverlayEyes[eyeIndex] = state;
}

void VR::RenderDrawAimLineQueued(C_BasePlayer* localPlayer)
{
    // Render-thread aim line drawing for mat_queue_mode!=0.
    //
    // Why: In queued rendering, the viewmodel/hands are driven by a render-thread snapshot (predicted pose +
    // queued-view smoothing). If we add debug-overlay geometry from the update thread, it will often be 1+ frames
    // behind (especially during stick locomotion/turning), making the aim line feel like it "doesn't follow" the hand.
    //
    // This render-thread path recomputes the visual ray from the current render-frame snapshot and draws a
    // single-frame-duration overlay so it stays crisp without accumulating ghosts.

    if (!m_AimLineEnabled || !m_Game || !m_Game->m_DebugOverlay)
        return;

    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (queueMode == 0)
        return;
    const bool scopeOnlyAimLine = m_ScopeAimLineOnlyInScope
        && m_ThirdPersonFrontViewEnabled
        && m_IsThirdPersonCamera
        && m_ScopeWeaponIsFirearm;
    if (scopeOnlyAimLine && !m_ScopeRenderingPass)
        return;

    Vector start{};
    Vector end{};
    if (!BuildRenderAimLineSegment(localPlayer, start, end))
        return;

    // Duration: keep it alive for ~one render interval so we don't accumulate multiple historical lines.
    using namespace std::chrono;
    static thread_local steady_clock::time_point s_last{};
    const auto now = steady_clock::now();

    float dt = 1.0f / 90.0f;
    if (s_last.time_since_epoch().count() != 0)
        dt = duration<float>(now - s_last).count();
    s_last = now;

    dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 20.0f);

    float durationSec = dt * 0.99f;
    durationSec = std::clamp(durationSec, 0.001f, 0.050f);
    if (scopeOnlyAimLine && m_ScopeRenderingPass)
        durationSec = 0.0f;

    DrawLineWithThickness(start, end, durationSec);
}

void VR::RenderDrawGameLaserSight(C_BasePlayer* localPlayer)
{
    const bool keepReplacementParticle = m_IsVREnabled
        && m_GameLaserSightBeamEnabled
        && m_GameLaserSightReplaceParticle
        && (localPlayer != nullptr);

    if (!keepReplacementParticle)
        VR_ClearGameLaserParticle(m_Game);
}


void VR::DrawThrowArc(const Vector& origin, const Vector& forward, const Vector& pitchSource)
{
    if (!m_Game->m_DebugOverlay || !m_AimLineEnabled)
        return;

    Vector direction = forward;
    if (direction.IsZero())
        return;
    VectorNormalize(direction);

    Vector planarForward(direction.x, direction.y, 0.0f);
    if (planarForward.IsZero())
        planarForward = direction;
    VectorNormalize(planarForward);

    Vector distanceSource = pitchSource.IsZero() ? direction : pitchSource;
    VectorNormalize(distanceSource);

    bool clampedToMaxDistance = false;
    const float distance = CalculateThrowArcDistance(distanceSource, &clampedToMaxDistance);
    if (clampedToMaxDistance)
    {
        m_HasThrowArc = false;
        m_HasAimLine = false;
        return;
    }
    const float arcHeight = std::max(distance * m_ThrowArcHeightRatio, m_ThrowArcMinDistance * 0.5f);

    Vector landingPoint = origin + planarForward * distance;
    landingPoint.z += m_ThrowArcLandingOffset;
    Vector apex = origin + planarForward * distance * 0.5f;
    apex.z += arcHeight;

    auto lerp = [](const Vector& a, const Vector& b, float t)
        {
            return a + (b - a) * t;
        };

    const float duration = std::max(m_AimLinePersistence, m_LastFrameDuration * m_AimLineFrameDurationMultiplier);
    for (int i = 0; i <= THROW_ARC_SEGMENTS; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(THROW_ARC_SEGMENTS);
        Vector ab = lerp(origin, apex, t);
        Vector bc = lerp(apex, landingPoint, t);
        m_LastThrowArcPoints[i] = lerp(ab, bc, t);
    }

    m_HasThrowArc = true;
    m_HasAimLine = false;

    DrawThrowArcFromCache(duration);
}

void VR::DrawThrowArcFromCache(float duration)
{
    if (!m_Game->m_DebugOverlay || !m_HasThrowArc)
        return;

    // Throttle throw arc drawing; it's a lot of overlay primitives.
    if (ShouldThrottle(m_LastThrowArcDrawTime, m_ThrowArcMaxHz))
        return;

    for (int i = 0; i < THROW_ARC_SEGMENTS; ++i)
    {
        DrawLineWithThickness(m_LastThrowArcPoints[i], m_LastThrowArcPoints[i + 1], duration);
    }
}

void VR::DrawLineWithThickness(const Vector& start, const Vector& end, float duration)
{
    int colorR = 0;
    int colorG = 0;
    int colorB = 0;
    int colorA = 0;
    GetAimLineColor(colorR, colorG, colorB, colorA);
    if (colorA <= 0)
        return;

    m_Game->m_DebugOverlay->AddLineOverlay(start, end, colorR, colorG, colorB, false, duration);

    const float thickness = std::max(m_AimLineThickness, 0.0f);
    if (thickness <= 0.0f)
        return;

    Vector forward = end - start;
    if (forward.IsZero())
        return;

    VectorNormalize(forward);

    Vector referenceUp = m_RightControllerUp;
    if (referenceUp.IsZero())
        referenceUp = Vector(0.0f, 0.0f, 1.0f);

    Vector basis1 = CrossProduct(forward, referenceUp);
    if (basis1.IsZero())
        basis1 = CrossProduct(forward, Vector(0.0f, 1.0f, 0.0f));
    if (basis1.IsZero())
        basis1 = CrossProduct(forward, Vector(1.0f, 0.0f, 0.0f));
    if (basis1.IsZero())
        return;

    VectorNormalize(basis1);
    Vector basis2 = CrossProduct(forward, basis1);
    if (basis2.IsZero())
        return;
    VectorNormalize(basis2);

    const int segments = 16;
    const float radius = thickness * 0.5f;
    const float twoPi = 6.28318530718f;

    for (int i = 0; i < segments; ++i)
    {
        const float angle0 = twoPi * static_cast<float>(i) / static_cast<float>(segments);
        const float angle1 = twoPi * static_cast<float>(i + 1) / static_cast<float>(segments);

        const float cos0 = std::cos(angle0);
        const float sin0 = std::sin(angle0);
        const float cos1 = std::cos(angle1);
        const float sin1 = std::sin(angle1);

        Vector offset0 = (basis1 * cos0 + basis2 * sin0) * radius;
        Vector offset1 = (basis1 * cos1 + basis2 * sin1) * radius;

        Vector start0 = start + offset0;
        Vector start1 = start + offset1;
        Vector end0 = end + offset0;
        Vector end1 = end + offset1;

        m_Game->m_DebugOverlay->AddTriangleOverlay(start0, start1, end1, colorR, colorG, colorB, colorA, false, duration);
        m_Game->m_DebugOverlay->AddTriangleOverlay(start0, end1, end0, colorR, colorG, colorB, colorA, false, duration);
    }
}

bool VR::IsEntityAlive(const C_BaseEntity* entity) const
{
    if (!entity)
        return false;

    const auto base = reinterpret_cast<const unsigned char*>(entity);
    unsigned char lifeState = 1;
    if (!VR_TryReadU8(base, kLifeStateOffset, lifeState))
        return false;

    return lifeState == 0;
}

void VR::RefreshDeathFirstPersonLock(const C_BasePlayer* localPlayer)
{
    if (!localPlayer)
    {
        m_DeathFirstPersonLockEnd = {};
        m_DeathWasAlivePrev = true;
        return;
    }

    const bool aliveNow = IsEntityAlive(localPlayer);
    const auto now = std::chrono::steady_clock::now();

    if (aliveNow)
    {
        // Fully clear the lock once we're alive again.
        m_DeathFirstPersonLockEnd = {};
    }
    else if (m_DeathWasAlivePrev)
    {
        // Rising edge: alive -> dead. Start a first-person lock window.
        m_DeathFirstPersonLockEnd = now + std::chrono::seconds(10);
    }

    m_DeathWasAlivePrev = aliveNow;
}

bool VR::IsDeathFirstPersonLockActive() const
{
    if (m_DeathFirstPersonLockEnd == std::chrono::steady_clock::time_point{})
        return false;

    return std::chrono::steady_clock::now() < m_DeathFirstPersonLockEnd;
}
bool VR::IsThirdPersonMapLoadCooldownActive() const
{
    if (m_ThirdPersonMapLoadCooldownMs <= 0)
        return false;
    if (m_ThirdPersonMapLoadCooldownEnd == std::chrono::steady_clock::time_point{})
        return false;
    return std::chrono::steady_clock::now() < m_ThirdPersonMapLoadCooldownEnd;
}

bool VR::IsGameplayHandLeftPhysical(bool leftHand) const
{
    return m_LeftHanded ? !leftHand : leftHand;
}

vr::TrackedDeviceIndex_t VR::GetPhysicalControllerIndexForHand(bool leftHand) const
{
    if (!m_System)
        return vr::k_unTrackedDeviceIndexInvalid;

    return m_System->GetTrackedDeviceIndexForControllerRole(
        leftHand ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
}

void VR::TriggerLegacyHapticPulse(vr::TrackedDeviceIndex_t deviceIndex, float durationSeconds, float amplitude) const
{
    if (!m_System || deviceIndex == vr::k_unTrackedDeviceIndexInvalid || !m_System->IsTrackedDeviceConnected(deviceIndex))
        return;

    const float safeAmplitude = std::clamp(amplitude, 0.0f, 1.0f);
    const float safeDuration = std::clamp(durationSeconds, 0.0f, 0.5f);
    if (safeAmplitude <= 0.0f || safeDuration <= 0.0f)
        return;

    // Legacy pulses only expose width, so fold the higher-level profile into a 0.5-4.0ms pulse.
    const float durationWeight = std::clamp(safeDuration / 0.05f, 0.0f, 1.0f);
    const float pulseStrength = std::clamp(safeAmplitude * 0.8f + durationWeight * 0.2f, 0.0f, 1.0f);
    const unsigned short pulseMicroseconds = static_cast<unsigned short>(std::clamp(
        static_cast<long>(std::lround(500.0f + pulseStrength * 3500.0f)),
        500l,
        4000l));

    m_System->TriggerHapticPulse(deviceIndex, 0, pulseMicroseconds);
}

void VR::TriggerPhysicalHandHapticPulse(bool leftHand, float durationSeconds, float frequency, float amplitude, int priority)
{
    if (!m_IsVREnabled)
        return;

    const float safeDuration = std::clamp(durationSeconds, 0.0f, 0.5f);
    const float safeFrequency = std::clamp(frequency, 0.0f, 320.0f);
    const float safeAmplitude = std::clamp(amplitude, 0.0f, 1.0f);
    if (safeDuration <= 0.0f || safeAmplitude <= 0.0f)
        return;

    HapticMixState& mix = leftHand ? m_LeftHapticMix : m_RightHapticMix;

    if (!mix.pending)
    {
        mix.pending = true;
        mix.amplitude = safeAmplitude;
        mix.frequency = safeFrequency;
        mix.durationSeconds = safeDuration;
        mix.weight = safeAmplitude;
        mix.priority = priority;
        return;
    }

    if (priority > mix.priority)
    {
        mix.priority = priority;
        mix.amplitude = std::clamp((mix.amplitude * 0.45f) + (safeAmplitude * 0.75f), 0.0f, 1.0f);
        mix.frequency = safeFrequency;
        mix.durationSeconds = std::max(mix.durationSeconds, safeDuration);
        mix.weight = safeAmplitude;
        return;
    }

    if (priority == mix.priority)
    {
        mix.amplitude = 1.0f - (1.0f - mix.amplitude) * (1.0f - safeAmplitude);
        const float combinedWeight = mix.weight + safeAmplitude;
        if (combinedWeight > 0.0001f)
            mix.frequency = (mix.frequency * mix.weight + safeFrequency * safeAmplitude) / combinedWeight;
        mix.weight = combinedWeight;
        mix.durationSeconds = std::max(mix.durationSeconds, safeDuration);
        return;
    }

    // Lower priority contributes softly to avoid erasing high-priority feedback.
    const float attenuatedAmp = safeAmplitude * 0.35f;
    mix.amplitude = 1.0f - (1.0f - mix.amplitude) * (1.0f - attenuatedAmp);
    mix.durationSeconds = std::max(mix.durationSeconds, safeDuration * 0.6f);
}

void VR::FlushHapticMixer()
{
    if (!m_IsVREnabled || !m_System)
        return;

    const auto now = std::chrono::steady_clock::now();
    const float minInterval = std::max(0.0f, m_HapticMixMinIntervalSeconds);

    auto flushOne = [&](bool leftHand, HapticMixState& mix)
        {
            if (!mix.pending)
                return;

            if (mix.lastSubmit.time_since_epoch().count() != 0 && minInterval > 0.0f)
            {
                const float elapsed = std::chrono::duration<float>(now - mix.lastSubmit).count();
                if (elapsed < minInterval)
                    return;
            }

            TriggerLegacyHapticPulse(
                GetPhysicalControllerIndexForHand(leftHand),
                mix.durationSeconds,
                mix.amplitude);

            mix.pending = false;
            mix.weight = 0.0f;
            mix.priority = -1;
            mix.lastSubmit = now;
        };

    flushOne(true, m_LeftHapticMix);
    flushOne(false, m_RightHapticMix);
}

WeaponHapticsProfile VR::GetWeaponHapticsProfile(int weaponId) const
{
    const std::string weaponName = WeaponIdToString(weaponId);
    if (!weaponName.empty())
    {
        auto it = m_WeaponHapticsOverrides.find(weaponName);
        if (it != m_WeaponHapticsOverrides.end())
            return it->second;
    }

    using W = C_WeaponCSBase::WeaponID;
    switch ((W)weaponId)
    {
    case W::PISTOL:            return { 0.018f, 165.0f, 0.33f };
    case W::MAGNUM:            return { 0.032f, 85.0f, 0.66f };
    case W::UZI:               return { 0.012f, 185.0f, 0.23f };
    case W::MAC10:             return { 0.011f, 195.0f, 0.24f };
    case W::MP5:               return { 0.012f, 190.0f, 0.26f };
    case W::M16A1:             return { 0.015f, 145.0f, 0.34f };
    case W::AK47:              return { 0.020f, 120.0f, 0.44f };
    case W::SCAR:              return { 0.017f, 135.0f, 0.39f };
    case W::SG552:             return { 0.018f, 130.0f, 0.40f };
    case W::PUMPSHOTGUN:       return { 0.040f, 72.0f, 0.78f };
    case W::SHOTGUN_CHROME:    return { 0.042f, 70.0f, 0.80f };
    case W::AUTOSHOTGUN:       return { 0.030f, 78.0f, 0.65f };
    case W::SPAS:              return { 0.029f, 82.0f, 0.62f };
    case W::HUNTING_RIFLE:     return { 0.038f, 88.0f, 0.72f };
    case W::SNIPER_MILITARY:   return { 0.033f, 92.0f, 0.61f };
    case W::SCOUT:             return { 0.036f, 96.0f, 0.69f };
    case W::AWP:               return { 0.052f, 62.0f, 0.94f };
    case W::M60:               return { 0.019f, 115.0f, 0.50f };
    case W::GRENADE_LAUNCHER:  return { 0.060f, 55.0f, 1.00f };
    case W::MELEE:             return { 0.028f, 105.0f, 0.54f };
    case W::CHAINSAW:          return { 0.014f, 175.0f, 0.34f };
    default:                   return m_DefaultWeaponHapticsProfile;
    }
}

void VR::TriggerWeaponFireHaptics(int weaponId, bool leftHand)
{
    if (!m_WeaponHapticsEnabled)
        return;

    const WeaponHapticsProfile profile = GetWeaponHapticsProfile(weaponId);
    const bool physicalLeftHand = IsGameplayHandLeftPhysical(leftHand);
    TriggerPhysicalHandHapticPulse(physicalLeftHand, profile.durationSeconds, profile.frequency, profile.amplitude);
}

void VR::TriggerMeleeSwingHaptics(bool leftHand)
{
    if (!m_WeaponHapticsEnabled)
        return;

    const bool physicalLeftHand = IsGameplayHandLeftPhysical(leftHand);
    TriggerPhysicalHandHapticPulse(
        physicalLeftHand,
        m_MeleeSwingHapticsProfile.durationSeconds,
        m_MeleeSwingHapticsProfile.frequency,
        m_MeleeSwingHapticsProfile.amplitude);
}

void VR::TriggerShoveHaptics(bool leftHand)
{
    if (!m_WeaponHapticsEnabled)
        return;

    const bool physicalLeftHand = IsGameplayHandLeftPhysical(leftHand);
    TriggerPhysicalHandHapticPulse(
        physicalLeftHand,
        m_ShoveHapticsProfile.durationSeconds,
        m_ShoveHapticsProfile.frequency,
        m_ShoveHapticsProfile.amplitude);
}
// Special infected recognition

#if __has_include("special_infected_features.cpp")
#define L4D2VR_HAS_SPECIAL_INFECTED_FEATURES 1
#include "special_infected_features.cpp"
#else
#define L4D2VR_HAS_SPECIAL_INFECTED_FEATURES 0

VR::SpecialInfectedType VR::GetSpecialInfectedType(const C_BaseEntity* /*entity*/) const { return SpecialInfectedType::None; }
VR::SpecialInfectedType VR::GetSpecialInfectedTypeFromModel(const std::string& /*modelName*/) const { return SpecialInfectedType::None; }
void VR::DrawSpecialInfectedArrow(const Vector& /*origin*/, SpecialInfectedType /*type*/) {}
void VR::RefreshSpecialInfectedPreWarning(const Vector& /*infectedOrigin*/, SpecialInfectedType /*type*/, int /*entityIndex*/, bool /*isPlayerClass*/) {}
void VR::RefreshSpecialInfectedBlindSpotWarning(const Vector& /*infectedOrigin*/) {}
bool VR::HasLineOfSightToSpecialInfected(const Vector& /*infectedOrigin*/, int /*entityIndex*/) const { return false; }
bool VR::IsSpecialInfectedInBlindSpot(const Vector& /*infectedOrigin*/) const { return false; }

void VR::UpdateSpecialInfectedWarningState()
{
    m_SpecialInfectedBlindSpotWarningActive = false;
    m_SpecialInfectedPreWarningActive = false;
    m_SpecialInfectedPreWarningInRange = false;
    m_SpecialInfectedWarningTargetActive = false;
    m_SpecialInfectedPreWarningTargetEntityIndex = -1;
    m_SpecialInfectedPreWarningTargetIsPlayer = false;
    m_SpecialInfectedAutoAimDirection = {};
    m_SpecialInfectedAutoAimCooldownEnd = {};
}
void VR::UpdateSpecialInfectedPreWarningState()
{
    m_SpecialInfectedPreWarningActive = false;
    m_SpecialInfectedPreWarningInRange = false;
    m_SpecialInfectedPreWarningTargetEntityIndex = -1;
    m_SpecialInfectedPreWarningTargetIsPlayer = false;
    m_SpecialInfectedAutoAimDirection = {};
    m_SpecialInfectedAutoAimCooldownEnd = {};
}

void VR::StartSpecialInfectedWarningAction() {}
void VR::UpdateSpecialInfectedWarningAction() {}
void VR::ResetSpecialInfectedWarningAction()
{
    m_SpecialInfectedWarningAttack2CmdOwned = false;
    m_SpecialInfectedWarningJumpCmdOwned = false;
    m_SpecialInfectedWarningActionStep = SpecialInfectedWarningActionStep::None;
    m_SpecialInfectedWarningNextActionTime = {};
    m_SuppressPlayerInput = false;
}

#endif
