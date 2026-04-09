namespace
{
	constexpr size_t kCBaseEntityShouldInterpolateVtableIndex = 165;
	constexpr size_t kIHandleEntityGetRefEHandleVtableIndex = 2;
	constexpr size_t kIClientRenderableGetModelVtableIndex = 8;
	constexpr size_t kIClientThinkableClientThinkVtableIndex = 1;
	constexpr size_t kIClientThinkableGetClientHandleVtableIndex = 2;
	constexpr size_t kUpdateClientSideAnimationVtableIndex = (0x328 / sizeof(void*));
	constexpr uintptr_t kClientSideAnimationListRva = 0x72213C;
	constexpr uintptr_t kClientSideAnimationCountRva = 0x722148;
	constexpr int kPrimaryEntityHandleMask = 0x1FFF;
	constexpr int kFallbackEntityHandleMask = 0x0FFF;
	constexpr uintptr_t kParticleSystemControlPointHandleOffset = 0x678;
	constexpr size_t kParticleSystemControlPointHandleCount = 63;
	constexpr uint32_t kClientThinkNeverThinkBits = 0x7f7fffff;
	constexpr float kClientThinkImmediateSentinel = -1293.0f;

	struct ClientSideAnimationEntry
	{
		C_BaseEntity* entity;
		uint32_t flags;
	};

	static_assert(sizeof(ClientSideAnimationEntry) == 8, "Unexpected client animation entry size");

	struct ContentCpuDebugCounters
	{
		std::atomic<uint32_t> clientThinkCalls{ 0 };
		std::atomic<uint32_t> clientThinkCulled{ 0 };
		std::atomic<uint32_t> setupBonesCalls{ 0 };
		std::atomic<uint32_t> setupBonesCulled{ 0 };
		std::atomic<uint32_t> clientAnimEntities{ 0 };
		std::atomic<uint32_t> clientAnimCulled{ 0 };
		std::atomic<uint32_t> studioFrameAdvanceCalls{ 0 };
		std::atomic<uint32_t> studioFrameAdvanceCulled{ 0 };
		std::atomic<uint32_t> particleCollectionCalls{ 0 };
		std::atomic<uint32_t> particleCollectionCulled{ 0 };
		std::atomic<uint32_t> particleThinkCalls{ 0 };
		std::atomic<uint32_t> particleThinkCulled{ 0 };
		std::atomic<uint32_t> flexSceneCalls{ 0 };
		std::atomic<uint32_t> flexSceneCulled{ 0 };
		std::atomic<uint32_t> muzzleEffectCalls{ 0 };
		std::atomic<uint32_t> muzzleEffectCulled{ 0 };
		std::atomic<uint32_t> muzzleFlashCalls{ 0 };
		std::atomic<uint32_t> muzzleFlashCulled{ 0 };
		std::chrono::steady_clock::time_point lastLog{};
	};

	inline ContentCpuDebugCounters& GetContentCpuDebugCounters()
	{
		static ContentCpuDebugCounters counters{};
		return counters;
	}

	struct ParticleCollectionThrottleState
	{
		float accumulatedDt = 0.0f;
		uint32_t lastTouchedFrame = 0;
	};

	struct ParticleCollectionThrottleCache
	{
		std::mutex mutex{};
		std::unordered_map<uintptr_t, ParticleCollectionThrottleState> states{};
		uint32_t lastPruneFrame = 0;
	};

	inline ParticleCollectionThrottleCache& GetParticleCollectionThrottleCache()
	{
		static ParticleCollectionThrottleCache cache{};
		return cache;
	}

	inline void MaybeLogContentCpuDebugStats()
	{
		if (!Hooks::m_Game || !Hooks::m_VR || !Hooks::m_VR->m_ContentCpuDebugLog)
			return;

		ContentCpuDebugCounters& counters = GetContentCpuDebugCounters();
		if (ShouldThrottleLog(counters.lastLog, Hooks::m_VR->m_ContentCpuDebugLogHz))
			return;

		const uint32_t clientThinkCalls = counters.clientThinkCalls.exchange(0, std::memory_order_relaxed);
		const uint32_t clientThinkCulled = counters.clientThinkCulled.exchange(0, std::memory_order_relaxed);
		const uint32_t setupBonesCalls = counters.setupBonesCalls.exchange(0, std::memory_order_relaxed);
		const uint32_t setupBonesCulled = counters.setupBonesCulled.exchange(0, std::memory_order_relaxed);
		const uint32_t clientAnimEntities = counters.clientAnimEntities.exchange(0, std::memory_order_relaxed);
		const uint32_t clientAnimCulled = counters.clientAnimCulled.exchange(0, std::memory_order_relaxed);
		const uint32_t studioFrameAdvanceCalls = counters.studioFrameAdvanceCalls.exchange(0, std::memory_order_relaxed);
		const uint32_t studioFrameAdvanceCulled = counters.studioFrameAdvanceCulled.exchange(0, std::memory_order_relaxed);
		const uint32_t particleCollectionCalls = counters.particleCollectionCalls.exchange(0, std::memory_order_relaxed);
		const uint32_t particleCollectionCulled = counters.particleCollectionCulled.exchange(0, std::memory_order_relaxed);
		const uint32_t particleThinkCalls = counters.particleThinkCalls.exchange(0, std::memory_order_relaxed);
		const uint32_t particleThinkCulled = counters.particleThinkCulled.exchange(0, std::memory_order_relaxed);
		const uint32_t flexSceneCalls = counters.flexSceneCalls.exchange(0, std::memory_order_relaxed);
		const uint32_t flexSceneCulled = counters.flexSceneCulled.exchange(0, std::memory_order_relaxed);
		const uint32_t muzzleEffectCalls = counters.muzzleEffectCalls.exchange(0, std::memory_order_relaxed);
		const uint32_t muzzleEffectCulled = counters.muzzleEffectCulled.exchange(0, std::memory_order_relaxed);
		const uint32_t muzzleFlashCalls = counters.muzzleFlashCalls.exchange(0, std::memory_order_relaxed);
		const uint32_t muzzleFlashCulled = counters.muzzleFlashCulled.exchange(0, std::memory_order_relaxed);

		Game::logMsg(
			"[ContentCPU][stats] clientThink=%u/%u setupBones=%u/%u clientAnim=%u/%u studioAdvance=%u/%u particleCollection=%u/%u particleThink=%u/%u flexScene=%u/%u muzzleFx=%u/%u muzzleFlash=%u/%u",
			clientThinkCulled, clientThinkCalls,
			setupBonesCulled, setupBonesCalls,
			clientAnimCulled, clientAnimEntities,
			studioFrameAdvanceCulled, studioFrameAdvanceCalls,
			particleCollectionCulled, particleCollectionCalls,
			particleThinkCulled, particleThinkCalls,
			flexSceneCulled, flexSceneCalls,
			muzzleEffectCulled, muzzleEffectCalls,
			muzzleFlashCulled, muzzleFlashCalls);
	}

	inline bool TryThrottleParticleCollectionSimulation(void* collection, float dt, float maxHz, float& outDt)
	{
		outDt = dt;
		if (!collection || maxHz <= 0.0f)
			return false;

		const float clampedDt = std::max(0.0f, dt);
		if (clampedDt <= 0.0f)
			return false;

		const float minInterval = 1.0f / std::max(1.0f, maxHz);
		const uint32_t frameId = Hooks::m_VR ? Hooks::m_VR->GetContentCpuFrameId() : 0;

		ParticleCollectionThrottleCache& cache = GetParticleCollectionThrottleCache();
		std::lock_guard<std::mutex> lock(cache.mutex);

		auto& state = cache.states[reinterpret_cast<uintptr_t>(collection)];
		state.accumulatedDt += clampedDt;
		state.lastTouchedFrame = frameId;

		if (frameId != 0 && frameId - cache.lastPruneFrame >= 240)
		{
			cache.lastPruneFrame = frameId;
			for (auto it = cache.states.begin(); it != cache.states.end();)
			{
				if (frameId - it->second.lastTouchedFrame > 600)
					it = cache.states.erase(it);
				else
					++it;
			}
		}

		if (state.accumulatedDt < minInterval)
			return true;

		outDt = state.accumulatedDt;
		state.accumulatedDt = 0.0f;
		return false;
	}

	inline bool IsTerrorPlayerClassName(const char* className)
	{
		return className &&
			(std::strcmp(className, "CTerrorPlayer") == 0 || std::strcmp(className, "C_TerrorPlayer") == 0);
	}

	inline bool TryGetObjectVtableFunction(const void* object, size_t vtableIndex, void*& outFnAddr)
	{
		outFnAddr = nullptr;
		if (!object)
			return false;

		void*** objectPtr = reinterpret_cast<void***>(const_cast<void*>(object));
		if (!objectPtr || !IsReadableMemoryRange(objectPtr, sizeof(void**)))
			return false;

		void** vtable = *objectPtr;
		if (!vtable || !IsReadableMemoryRange(vtable, (vtableIndex + 1) * sizeof(void*)))
			return false;

		outFnAddr = vtable[vtableIndex];
		return outFnAddr != nullptr && IsReadableMemoryRange(outFnAddr, 8);
	}

	inline bool TryGetRenderableModel(const C_BaseEntity* entity, model_t*& outModel)
	{
		outModel = nullptr;
		if (!entity)
			return false;

		void* renderable = const_cast<C_BaseEntity*>(entity)->GetClientRenderable();
		if (!renderable)
			return false;

		void*** renderableObject = reinterpret_cast<void***>(renderable);
		if (!renderableObject || !IsReadableMemoryRange(renderableObject, sizeof(void**)))
			return false;

		void** vtable = *renderableObject;
		if (!vtable || !IsReadableMemoryRange(vtable, (kIClientRenderableGetModelVtableIndex + 1) * sizeof(void*)))
			return false;

		void* fnAddr = vtable[kIClientRenderableGetModelVtableIndex];
		if (!fnAddr || !IsReadableMemoryRange(fnAddr, 8))
			return false;

		using tRenderableGetModel = model_t * (__thiscall*)(void* thisptr);
		outModel = reinterpret_cast<tRenderableGetModel>(fnAddr)(renderable);
		return outModel != nullptr;
	}

	inline bool TryGetEntityIndexFromPointerSlow(const C_BaseEntity* entity, int& outIndex)
	{
		outIndex = -1;
		if (!entity || !Hooks::m_Game || !Hooks::m_Game->m_ClientEntityList)
			return false;

		const int maxEntityIndex = Hooks::m_Game->m_ClientEntityList->GetHighestEntityIndex();
		if (maxEntityIndex <= 0)
			return false;

		void*** entityObject = reinterpret_cast<void***>(const_cast<C_BaseEntity*>(entity));
		if (entityObject && IsReadableMemoryRange(entityObject, sizeof(void**)))
		{
			void** vtable = *entityObject;
			if (vtable && IsReadableMemoryRange(vtable, (kIHandleEntityGetRefEHandleVtableIndex + 1) * sizeof(void*)))
			{
				void* fnAddr = vtable[kIHandleEntityGetRefEHandleVtableIndex];
				if (fnAddr && IsReadableMemoryRange(fnAddr, 8))
				{
					using tGetRefEHandle = int(__thiscall*)(const void* thisptr);
					const int handle = reinterpret_cast<tGetRefEHandle>(fnAddr)(entity);
					const int candidates[2] = { handle & kPrimaryEntityHandleMask, handle & kFallbackEntityHandleMask };
					for (const int candidate : candidates)
					{
						if (candidate <= 0 || candidate > maxEntityIndex)
							continue;

						if (Hooks::m_Game->GetClientEntity(candidate) == entity)
						{
							outIndex = candidate;
							return true;
						}
					}
				}
			}
		}

		for (int index = 1; index <= maxEntityIndex; ++index)
		{
			if (Hooks::m_Game->GetClientEntity(index) == entity)
			{
				outIndex = index;
				return true;
			}
		}

		return false;
	}

	inline bool TryResolveClientEntityFromHandle(uint32_t handle, C_BaseEntity*& outEntity, int& outEntityIndex)
	{
		outEntity = nullptr;
		outEntityIndex = -1;
		if (!Hooks::m_Game || !Hooks::m_Game->m_ClientEntityList)
			return false;

		if (handle == 0 || handle == 0xffffu || handle == 0xffffffffu)
			return false;

		outEntity = reinterpret_cast<C_BaseEntity*>(Hooks::m_Game->m_ClientEntityList->GetClientEntityFromHandle(static_cast<int>(handle)));
		if (!outEntity)
			return false;

		if (!Hooks::m_VR->GetContentCpuEntityIndexForPointer(outEntity, outEntityIndex))
			TryGetEntityIndexFromPointerSlow(outEntity, outEntityIndex);
		return true;
	}

	inline void* ResolveClientThinkableFromEntry(uint32_t* entry)
	{
		if (!entry || !Hooks::m_Game || !Hooks::m_Game->m_Offsets || !Hooks::m_Game->m_Offsets->ResolveClientThinkHandle.valid)
			return nullptr;

		const uint32_t thinkHandle = entry[0];
		if (thinkHandle == 0 || thinkHandle == 0xffffu || thinkHandle == 0xffffffffu)
			return nullptr;

		using tResolveClientThinkHandle = void* (__cdecl*)(uint32_t thinkHandle);
		auto resolve = reinterpret_cast<tResolveClientThinkHandle>(Hooks::m_Game->m_Offsets->ResolveClientThinkHandle.address);
		if (!resolve)
			return nullptr;

		__try
		{
			void* thinkable = resolve(thinkHandle);
			if (!thinkable)
				thinkable = resolve(thinkHandle);
			return thinkable;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	inline bool TryResolveEntityFromThinkable(void* thinkable, C_BaseEntity*& outEntity, int& outEntityIndex)
	{
		outEntity = nullptr;
		outEntityIndex = -1;
		if (!thinkable)
			return false;

		void* fnAddr = nullptr;
		if (!TryGetObjectVtableFunction(thinkable, kIClientThinkableGetClientHandleVtableIndex, fnAddr))
			return false;

		using tGetClientHandle = int(__thiscall*)(void* thisptr);
		const int handle = reinterpret_cast<tGetClientHandle>(fnAddr)(thinkable);
		if (handle == -1 || handle == 0xffff)
			return false;

		return TryResolveClientEntityFromHandle(static_cast<uint32_t>(handle), outEntity, outEntityIndex);
	}

	inline void TouchContentCpuEntityFromPointer(const C_BaseEntity* entity,
		int entityIndex,
		ContentCpuClass preferredClass = ContentCpuClass::Unknown,
		const char* knownClassName = nullptr)
	{
		if (!entity || entityIndex <= 0 || !Hooks::m_Game || !Hooks::m_VR)
			return;

		const Vector origin = const_cast<C_BaseEntity*>(entity)->GetAbsOrigin();

		ContentCpuEntityState existingState{};
		if (Hooks::m_VR->TryGetContentCpuEntityState(entityIndex, existingState) &&
			existingState.classification != ContentCpuClass::Unknown)
		{
			Hooks::m_VR->TouchContentCpuEntity(entityIndex, existingState.modelKey, entity, existingState.classification, origin);
			return;
		}

		const char* className = knownClassName
			? knownClassName
			: Hooks::m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(const_cast<C_BaseEntity*>(entity)));
		const bool isPlayerClass = IsTerrorPlayerClassName(className);

		uintptr_t modelKey = 0;
		std::string modelName;
		model_t* model = nullptr;
		if (Hooks::m_Game->m_ModelInfo && TryGetRenderableModel(entity, model) && model)
		{
			modelKey = reinterpret_cast<uintptr_t>(model);
			const char* rawModelName = Hooks::m_Game->m_ModelInfo->GetModelName(model);
			if (rawModelName)
				modelName = rawModelName;
		}

		VR::SpecialInfectedType infectedType = Hooks::m_VR->GetSpecialInfectedType(entity);
		if (infectedType == VR::SpecialInfectedType::None && !modelName.empty())
		{
			const auto modelType = Hooks::m_VR->GetSpecialInfectedTypeFromModel(modelName);
			if (modelType == VR::SpecialInfectedType::Tank || modelType == VR::SpecialInfectedType::Witch)
				infectedType = modelType;
		}

		ContentCpuClass classification = preferredClass;
		if (classification == ContentCpuClass::Unknown)
		{
			classification = Hooks::m_VR->ClassifyContentCpuRenderable(
				entityIndex,
				modelKey,
				entity,
				className,
				modelName,
				isPlayerClass,
				infectedType != VR::SpecialInfectedType::None);
		}

		Hooks::m_VR->TouchContentCpuEntity(entityIndex, modelKey, entity, classification, origin);
	}

	inline void TouchContentCpuEntityFromClientAnim(const C_BaseEntity* entity, int entityIndex)
	{
		TouchContentCpuEntityFromPointer(entity, entityIndex);
	}

	inline int GetContentCpuClassificationPriority(ContentCpuClass classification)
	{
		switch (classification)
		{
		case ContentCpuClass::Critical:
			return 5;
		case ContentCpuClass::SpecialInfected:
			return 4;
		case ContentCpuClass::CommonInfected:
			return 3;
		case ContentCpuClass::Ragdoll:
			return 2;
		case ContentCpuClass::DecorDynamic:
		case ContentCpuClass::DecorStatic:
			return 1;
		default:
			return 0;
		}
	}

	inline bool TryResolveParticleSystemRepresentativeEntity(void* particleSystem,
		C_BaseEntity*& outEntity,
		int& outEntityIndex,
		Vector& outOrigin)
	{
		outEntity = nullptr;
		outEntityIndex = -1;
		outOrigin = { 0.0f, 0.0f, 0.0f };
		if (!particleSystem || !Hooks::m_Game || !Hooks::m_VR)
			return false;

		int bestScore = -1;
		C_BaseEntity* directEntity = nullptr;
		int directEntityIndex = -1;
		if (TryResolveEntityFromThinkable(particleSystem, directEntity, directEntityIndex) &&
			directEntity &&
			directEntityIndex > 0)
		{
			TouchContentCpuEntityFromPointer(directEntity, directEntityIndex);

			ContentCpuEntityState state{};
			bestScore = Hooks::m_VR->IsLocalPlayerEntityIndex(directEntityIndex)
				? 100
				: (Hooks::m_VR->TryGetContentCpuEntityState(directEntityIndex, state)
					? 10 + GetContentCpuClassificationPriority(state.classification)
					: 1);
			outEntity = directEntity;
			outEntityIndex = directEntityIndex;
			outOrigin = directEntity->GetAbsOrigin();
			if (bestScore >= 100)
				return true;
		}

		auto* controlPointHandles = reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(particleSystem) + kParticleSystemControlPointHandleOffset);
		if (!IsReadableMemoryRange(controlPointHandles, sizeof(uint32_t) * kParticleSystemControlPointHandleCount))
			return outEntity != nullptr;

		for (size_t i = 0; i < kParticleSystemControlPointHandleCount; ++i)
		{
			C_BaseEntity* entity = nullptr;
			int entityIndex = -1;
			if (!TryResolveClientEntityFromHandle(controlPointHandles[i], entity, entityIndex) || !entity)
				continue;

			const Vector origin = entity->GetAbsOrigin();
			if (entityIndex > 0)
				TouchContentCpuEntityFromPointer(entity, entityIndex);

			int score = 1;
			if (entityIndex > 0)
			{
				if (Hooks::m_VR->IsLocalPlayerEntityIndex(entityIndex))
					score = 100;
				else
				{
					ContentCpuEntityState state{};
					if (Hooks::m_VR->TryGetContentCpuEntityState(entityIndex, state))
						score = 10 + GetContentCpuClassificationPriority(state.classification);
				}
			}

			if (score > bestScore)
			{
				bestScore = score;
				outEntity = entity;
				outEntityIndex = entityIndex;
				outOrigin = origin;
				if (score >= 100)
					break;
			}
		}

		return outEntity != nullptr;
	}

	inline bool ShouldCullParticleSystemClientThink(void* particleSystem)
	{
		if (!particleSystem || !Hooks::m_VR || !Hooks::m_Game || !Hooks::m_VR->m_ContentCpuParticleClientThinkCullEnabled)
			return false;

		C_BaseEntity* entity = nullptr;
		int entityIndex = -1;
		Vector origin{};
		if (!TryResolveParticleSystemRepresentativeEntity(particleSystem, entity, entityIndex, origin) ||
			!entity ||
			entityIndex <= 0)
		{
			return false;
		}

		TouchContentCpuEntityFromPointer(entity, entityIndex);
		if (Hooks::m_VR->IsLocalPlayerEntityIndex(entityIndex))
			return false;

		if (Hooks::m_VR->ShouldCullContentCpuClientThink(entityIndex))
			return true;

		if (Hooks::m_VR->WasContentCpuEntityRecentlySeen(entityIndex, static_cast<uint32_t>(std::max(0, Hooks::m_VR->m_ContentCpuRecentlySeenFrames))))
			return false;

		return Hooks::m_VR->ShouldCullContentCpuLocalFx(entityIndex, origin);
	}

	inline bool ShouldCullClientThinkableDispatch(void* thinkable)
	{
		if (!thinkable || !Hooks::m_VR || !Hooks::m_Game)
			return false;

		void* clientThinkFn = nullptr;
		if (!TryGetObjectVtableFunction(thinkable, kIClientThinkableClientThinkVtableIndex, clientThinkFn))
			return false;

		const void* particleThinkAddr =
			(Hooks::m_Game->m_Offsets && Hooks::m_Game->m_Offsets->ParticleSystemClientThink.valid)
			? reinterpret_cast<void*>(Hooks::m_Game->m_Offsets->ParticleSystemClientThink.address)
			: nullptr;
		if (particleThinkAddr && clientThinkFn == particleThinkAddr)
			return ShouldCullParticleSystemClientThink(thinkable);

		if (!Hooks::m_VR->m_ContentCpuClientThinkCullEnabled)
			return false;

		C_BaseEntity* entity = nullptr;
		int entityIndex = -1;
		if (!TryResolveEntityFromThinkable(thinkable, entity, entityIndex) || !entity || entityIndex <= 0)
			return false;

		TouchContentCpuEntityFromPointer(entity, entityIndex);
		return Hooks::m_VR->ShouldCullContentCpuClientThink(entityIndex);
	}

	inline void CallClientThinkUnregister(uint32_t thinkHandle)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_Offsets || !Hooks::m_Game->m_Offsets->UnregisterClientThink.valid)
			return;

		using tClientThinkUnregister = void(__cdecl*)(uint32_t thinkHandle);
		reinterpret_cast<tClientThinkUnregister>(Hooks::m_Game->m_Offsets->UnregisterClientThink.address)(thinkHandle);
	}

	inline void SkipClientThinkDispatchEntry(uint32_t* entry, uint32_t serial)
	{
		if (!entry)
			return;

		float scheduledThink = 0.0f;
		std::memcpy(&scheduledThink, &entry[1], sizeof(float));
		if (scheduledThink != kClientThinkImmediateSentinel)
		{
			if (entry[1] == kClientThinkNeverThinkBits)
			{
				CallClientThinkUnregister(entry[0]);
				entry[2] = serial;
				return;
			}

			entry[1] = kClientThinkNeverThinkBits;
		}

		entry[2] = serial;
	}

	inline int TrackContentCpuEntity(C_BaseEntity* entity,
		ContentCpuClass preferredClass,
		Vector* outOrigin = nullptr,
		const char** outClassName = nullptr)
	{
		if (!entity || !Hooks::m_Game || !Hooks::m_VR)
			return -1;

		if (outOrigin)
			*outOrigin = entity->GetAbsOrigin();

		const char* className = Hooks::m_Game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(entity));
		if (outClassName)
			*outClassName = className;

		int entityIndex = -1;
		if (!Hooks::m_VR->GetContentCpuEntityIndexForPointer(entity, entityIndex) &&
			!TryGetEntityIndexFromPointerSlow(entity, entityIndex))
		{
			return -1;
		}

		TouchContentCpuEntityFromPointer(entity, entityIndex, preferredClass, className);
		return entityIndex;
	}

	inline void CallUpdateClientSideAnimation(C_BaseEntity* entity)
	{
		if (!entity)
			return;

		void*** entityObject = reinterpret_cast<void***>(entity);
		if (!entityObject || !IsReadableMemoryRange(entityObject, sizeof(void**)))
			return;

		void** vtable = *entityObject;
		if (!vtable || !IsReadableMemoryRange(vtable, (kUpdateClientSideAnimationVtableIndex + 1) * sizeof(void*)))
			return;

		void* fnAddr = vtable[kUpdateClientSideAnimationVtableIndex];
		if (!fnAddr || !IsReadableMemoryRange(fnAddr, 8))
			return;

		using tUpdateClientSideAnimationMethod = void(__thiscall*)(void* thisptr);
		reinterpret_cast<tUpdateClientSideAnimationMethod>(fnAddr)(entity);
	}
}

bool VR::IsLocalPlayerEntityIndex(int entityIndex) const
{
	if (entityIndex <= 0 || !m_Game || !m_Game->m_EngineClient)
		return false;

	return entityIndex == m_Game->m_EngineClient->GetLocalPlayer();
}

bool VR::ShouldCullContentCpuLocalFx(int entityIndex, const Vector& origin) const
{
	if (IsLocalPlayerEntityIndex(entityIndex))
		return false;

	const float keepDistance = std::max(0.0f, m_ContentCpuLocalFxKeepDistance);
	if (keepDistance <= 0.0f)
		return true;

	return (origin - m_ContentCpuLastViewOrigin).LengthSqr() > (keepDistance * keepDistance);
}

void Hooks::TryInstallContentCpuHooksFromEntity(C_BaseEntity* entity)
{
	(void)entity;

	// The client-side ContentCPU hook set is temporarily disabled for stability.
	// Leave the discovery callsites intact, but do not install any dynamic hooks.
	return;

	if (!entity || !m_VR)
		return;

	void*** entityObject = reinterpret_cast<void***>(entity);
	if (!entityObject || !IsReadableMemoryRange(entityObject, sizeof(void**)))
		return;

	void** vtable = *entityObject;
	if (!vtable)
		return;

	if (m_VR->m_ContentCpuInterpolateCullEnabled && !hkShouldInterpolate.pTarget)
	{
		if (!IsReadableMemoryRange(vtable, (kCBaseEntityShouldInterpolateVtableIndex + 1) * sizeof(void*)))
			return;

		void* shouldInterpolateAddr = vtable[kCBaseEntityShouldInterpolateVtableIndex];
		if (!shouldInterpolateAddr || !IsReadableMemoryRange(shouldInterpolateAddr, 8))
			return;

		if (hkShouldInterpolate.createHook(shouldInterpolateAddr, &dShouldInterpolate) == 0 &&
			hkShouldInterpolate.enableHook() == 0)
		{
			Game::logMsg("[ContentCPU] Installed ShouldInterpolate hook at %p from entity %p", shouldInterpolateAddr, entity);
		}
	}

	if (m_VR->m_ContentCpuSetupBonesCullEnabled && !hkSetupBones.pTarget)
	{
		if (!m_Game || !m_Game->m_Offsets || !m_Game->m_Offsets->SetupBones.valid)
			return;

		void* setupBonesAddr = reinterpret_cast<void*>(m_Game->m_Offsets->SetupBones.address);
		if (!setupBonesAddr || !IsReadableMemoryRange(setupBonesAddr, 8))
			return;

		if (hkSetupBones.createHook(setupBonesAddr, &dSetupBones) == 0 &&
			hkSetupBones.enableHook() == 0)
		{
			Game::logMsg("[ContentCPU] Installed SetupBones hook at %p", setupBonesAddr);
		}
	}
}

bool Hooks::dSetupBones(void* ecx, void* edx, matrix3x4_t* pBoneToWorldOut, int nMaxBones, int boneMask, float currentTime)
{
	(void)edx;

	if (!hkSetupBones.fOriginal)
		return false;

	GetContentCpuDebugCounters().setupBonesCalls.fetch_add(1, std::memory_order_relaxed);

	if (!ecx || !m_VR || !m_Game || !m_VR->m_ContentCpuSetupBonesCullEnabled)
		return hkSetupBones.fOriginal(ecx, pBoneToWorldOut, nMaxBones, boneMask, currentTime);

	int entityIndex = -1;
	if (!m_VR->GetContentCpuEntityIndexForPointer(reinterpret_cast<C_BaseEntity*>(ecx), entityIndex))
		return hkSetupBones.fOriginal(ecx, pBoneToWorldOut, nMaxBones, boneMask, currentTime);

	if (!m_VR->ShouldCullContentCpuSetupBones(entityIndex))
		return hkSetupBones.fOriginal(ecx, pBoneToWorldOut, nMaxBones, boneMask, currentTime);

	GetContentCpuDebugCounters().setupBonesCulled.fetch_add(1, std::memory_order_relaxed);
	return false;
}

bool Hooks::dShouldInterpolate(C_BaseEntity* ecx, void* edx)
{
	(void)edx;

	if (!hkShouldInterpolate.fOriginal)
		return true;

	const bool original = hkShouldInterpolate.fOriginal(ecx);
	if (!original || !ecx || !m_VR || !m_Game || !m_VR->m_ContentCpuInterpolateCullEnabled)
		return original;

	int entityIndex = -1;
	if (!m_VR->GetContentCpuEntityIndexForPointer(ecx, entityIndex))
		return original;

	if (!m_VR->ShouldCullContentCpuInterpolation(entityIndex))
		return original;

	return false;
}

void Hooks::dDispatchClientThink(uint32_t* entry, uint32_t serial)
{
	if (!hkDispatchClientThink.fOriginal)
		return;

	GetContentCpuDebugCounters().clientThinkCalls.fetch_add(1, std::memory_order_relaxed);

	// Client-think culling is temporarily bypassed for stability. The hook stays installed so
	// we can keep debug counters/logging without touching fragile client think-handle resolution.
	hkDispatchClientThink.fOriginal(entry, serial);
	MaybeLogContentCpuDebugStats();
}

void __fastcall Hooks::dParticleCollectionSimulate(void* ecx, void* edx, float dt)
{
	(void)edx;

	if (!hkParticleCollectionSimulate.fOriginal)
		return;

	GetContentCpuDebugCounters().particleCollectionCalls.fetch_add(1, std::memory_order_relaxed);

	if (!ecx || !m_VR || !m_Game || !m_VR->m_ContentCpuParticleCollectionThrottleEnabled)
	{
		hkParticleCollectionSimulate.fOriginal(ecx, dt);
		MaybeLogContentCpuDebugStats();
		return;
	}

	float simulateDt = dt;
	if (TryThrottleParticleCollectionSimulation(ecx, dt, m_VR->m_ContentCpuParticleCollectionMaxHz, simulateDt))
	{
		GetContentCpuDebugCounters().particleCollectionCulled.fetch_add(1, std::memory_order_relaxed);
		MaybeLogContentCpuDebugStats();
		return;
	}

	hkParticleCollectionSimulate.fOriginal(ecx, simulateDt);
	MaybeLogContentCpuDebugStats();
}

void __fastcall Hooks::dParticleSystemClientThink(void* ecx, void* edx)
{
	(void)edx;

	if (!hkParticleSystemClientThink.fOriginal)
		return;

	GetContentCpuDebugCounters().particleThinkCalls.fetch_add(1, std::memory_order_relaxed);

	if (ShouldCullParticleSystemClientThink(ecx))
	{
		GetContentCpuDebugCounters().particleThinkCulled.fetch_add(1, std::memory_order_relaxed);
		MaybeLogContentCpuDebugStats();
		return;
	}

	hkParticleSystemClientThink.fOriginal(ecx);
	MaybeLogContentCpuDebugStats();
}

void __fastcall Hooks::dBaseFlexAddSceneEvent(void* ecx, void* edx, void* scene, void* event, void* actor, char flags, void* target)
{
	(void)edx;

	if (!hkBaseFlexAddSceneEvent.fOriginal)
		return;

	GetContentCpuDebugCounters().flexSceneCalls.fetch_add(1, std::memory_order_relaxed);

	if (!ecx || !m_VR || !m_Game || !m_VR->m_ContentCpuFlexSceneCullEnabled)
	{
		hkBaseFlexAddSceneEvent.fOriginal(ecx, scene, event, actor, flags, target);
		MaybeLogContentCpuDebugStats();
		return;
	}

	Vector origin = reinterpret_cast<C_BaseEntity*>(ecx)->GetAbsOrigin();
	const char* className = nullptr;
	const int entityIndex = TrackContentCpuEntity(reinterpret_cast<C_BaseEntity*>(ecx), ContentCpuClass::Critical, &origin, &className);
	if (!IsTerrorPlayerClassName(className) || !m_VR->ShouldCullContentCpuLocalFx(entityIndex, origin))
	{
		hkBaseFlexAddSceneEvent.fOriginal(ecx, scene, event, actor, flags, target);
		MaybeLogContentCpuDebugStats();
		return;
	}

	GetContentCpuDebugCounters().flexSceneCulled.fetch_add(1, std::memory_order_relaxed);
	MaybeLogContentCpuDebugStats();
}

void __fastcall Hooks::dDispatchMuzzleEffect(void* ecx, void* edx, void* arg1, void* arg2)
{
	(void)edx;

	if (!hkDispatchMuzzleEffect.fOriginal)
		return;

	GetContentCpuDebugCounters().muzzleEffectCalls.fetch_add(1, std::memory_order_relaxed);

	if (!ecx || !m_VR || !m_Game || !m_VR->m_ContentCpuMuzzleEffectCullEnabled)
	{
		hkDispatchMuzzleEffect.fOriginal(ecx, arg1, arg2);
		MaybeLogContentCpuDebugStats();
		return;
	}

	Vector origin = reinterpret_cast<C_BaseEntity*>(ecx)->GetAbsOrigin();
	const int entityIndex = TrackContentCpuEntity(reinterpret_cast<C_BaseEntity*>(ecx), ContentCpuClass::Critical, &origin);
	if (m_VR->ShouldCullContentCpuLocalFx(entityIndex, origin))
	{
		GetContentCpuDebugCounters().muzzleEffectCulled.fetch_add(1, std::memory_order_relaxed);
		MaybeLogContentCpuDebugStats();
		return;
	}

	hkDispatchMuzzleEffect.fOriginal(ecx, arg1, arg2);
	MaybeLogContentCpuDebugStats();
}

void __fastcall Hooks::dProcessMuzzleFlashEvent(void* ecx, void* edx)
{
	(void)edx;

	if (!hkProcessMuzzleFlashEvent.fOriginal)
		return;

	GetContentCpuDebugCounters().muzzleFlashCalls.fetch_add(1, std::memory_order_relaxed);

	if (!ecx || !m_VR || !m_Game || !m_VR->m_ContentCpuMuzzleEffectCullEnabled)
	{
		hkProcessMuzzleFlashEvent.fOriginal(ecx);
		MaybeLogContentCpuDebugStats();
		return;
	}

	Vector origin = reinterpret_cast<C_BaseEntity*>(ecx)->GetAbsOrigin();
	const int entityIndex = TrackContentCpuEntity(reinterpret_cast<C_BaseEntity*>(ecx), ContentCpuClass::Critical, &origin);
	if (m_VR->ShouldCullContentCpuLocalFx(entityIndex, origin))
	{
		GetContentCpuDebugCounters().muzzleFlashCulled.fetch_add(1, std::memory_order_relaxed);
		MaybeLogContentCpuDebugStats();
		return;
	}

	hkProcessMuzzleFlashEvent.fOriginal(ecx);
	MaybeLogContentCpuDebugStats();
}

float __fastcall Hooks::dStudioFrameAdvance(void* ecx, void* edx, float flInterval)
{
	(void)edx;

	if (!hkStudioFrameAdvance.fOriginal)
		return 0.0f;

	GetContentCpuDebugCounters().studioFrameAdvanceCalls.fetch_add(1, std::memory_order_relaxed);

	if (!ecx || !m_VR || !m_Game || !m_VR->m_ContentCpuStudioFrameAdvanceCullEnabled)
	{
		const float result = hkStudioFrameAdvance.fOriginal(ecx, flInterval);
		MaybeLogContentCpuDebugStats();
		return result;
	}

	C_BaseEntity* const entity = reinterpret_cast<C_BaseEntity*>(ecx);
	int entityIndex = -1;
	if (!m_VR->GetContentCpuEntityIndexForPointer(entity, entityIndex) &&
		!TryGetEntityIndexFromPointerSlow(entity, entityIndex))
	{
		const float result = hkStudioFrameAdvance.fOriginal(ecx, flInterval);
		MaybeLogContentCpuDebugStats();
		return result;
	}

	TouchContentCpuEntityFromPointer(entity, entityIndex);
	if (!m_VR->ShouldCullContentCpuStudioFrameAdvance(entityIndex))
	{
		const float result = hkStudioFrameAdvance.fOriginal(ecx, flInterval);
		MaybeLogContentCpuDebugStats();
		return result;
	}

	GetContentCpuDebugCounters().studioFrameAdvanceCulled.fetch_add(1, std::memory_order_relaxed);
	MaybeLogContentCpuDebugStats();
	return 0.0f;
}

void Hooks::dUpdateClientSideAnimations()
{
	if (!m_Game || !m_VR || !m_VR->m_ContentCpuClientAnimCullEnabled || !m_Game->m_BaseClient)
	{
		if (hkUpdateClientSideAnimations.fOriginal)
			hkUpdateClientSideAnimations.fOriginal();
		MaybeLogContentCpuDebugStats();
		return;
	}

	auto* const listPtr = reinterpret_cast<ClientSideAnimationEntry**>(m_Game->m_BaseClient + kClientSideAnimationListRva);
	auto* const countPtr = reinterpret_cast<int*>(m_Game->m_BaseClient + kClientSideAnimationCountRva);
	if (!IsReadableMemoryRange(listPtr, sizeof(ClientSideAnimationEntry*)) ||
		!IsReadableMemoryRange(countPtr, sizeof(int)))
	{
		if (hkUpdateClientSideAnimations.fOriginal)
			hkUpdateClientSideAnimations.fOriginal();
		MaybeLogContentCpuDebugStats();
		return;
	}

	ClientSideAnimationEntry* entries = *listPtr;
	const int count = *countPtr;
	if (!entries || count <= 0)
	{
		MaybeLogContentCpuDebugStats();
		return;
	}

	if (count > 8192 || !IsReadableMemoryRange(entries, sizeof(ClientSideAnimationEntry) * static_cast<size_t>(count)))
	{
		if (hkUpdateClientSideAnimations.fOriginal)
			hkUpdateClientSideAnimations.fOriginal();
		MaybeLogContentCpuDebugStats();
		return;
	}

	for (int i = 0; i < count; ++i)
	{
		const ClientSideAnimationEntry& entry = entries[i];
		C_BaseEntity* const entity = entry.entity;
		if (!entity || (entry.flags & 1u) == 0)
			continue;

		GetContentCpuDebugCounters().clientAnimEntities.fetch_add(1, std::memory_order_relaxed);

		int entityIndex = -1;
		if (!m_VR->GetContentCpuEntityIndexForPointer(entity, entityIndex))
		{
			if (!TryGetEntityIndexFromPointerSlow(entity, entityIndex))
			{
				CallUpdateClientSideAnimation(entity);
				continue;
			}
		}

		TouchContentCpuEntityFromClientAnim(entity, entityIndex);
		if (entityIndex > 0 && m_VR->ShouldCullContentCpuClientAnimation(entityIndex))
		{
			GetContentCpuDebugCounters().clientAnimCulled.fetch_add(1, std::memory_order_relaxed);
			continue;
		}

		CallUpdateClientSideAnimation(entity);
	}

	MaybeLogContentCpuDebugStats();
}
