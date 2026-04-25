void __fastcall Hooks::dEndFrame(void* ecx, void* edx)
{
	return hkEndFrame.fOriginal(ecx);
}

static inline void CallCalcViewModelViewOriginal(void* ecx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
{
	if (!Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled || !Hooks::m_VR->m_ViewmodelDisableMoveBob || !owner)
	{
		Hooks::hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
		return;
	}

	C_BasePlayer* ownerPlayer = reinterpret_cast<C_BasePlayer*>(owner);
	const Vector savedVelocity = ownerPlayer->m_vecVelocity;
	ownerPlayer->m_vecVelocity = Vector{ 0.0f, 0.0f, 0.0f };
	Hooks::hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
	ownerPlayer->m_vecVelocity = savedVelocity;
}


void __fastcall Hooks::dCalcViewModelView(void* ecx, void* edx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
{
	Vector vecNewOrigin = eyePosition;
	QAngle vecNewAngles = eyeAngles;

	if (m_VR->m_IsVREnabled)
	{
		const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
		const bool multiCoreQueued = (queueMode == 2);
		const bool forceDisableMoveBob = m_VR->m_ViewmodelDisableMoveBob;

		// ------------------------------------------------------------
		// Single-thread path (mat_queue_mode 0/1)
		//
		// 你的“单线程 vm 正常版本”里就是这个行为：把 controller 侧的
		// viewmodel 推荐位姿当作 eye 输入喂给 CalcViewModelView。
		//
		// 这样 engine 生成的 viewmodel/weapon/attachment(枪口闪光/烟雾)
		// 都会围绕枪来算，而不会 head-locked 在 HMD 上。
		// ------------------------------------------------------------
		if (!multiCoreQueued)
		{
			vecNewOrigin = m_VR->GetRecommendedViewmodelAbsPos();
			vecNewAngles = m_VR->GetRecommendedViewmodelAbsAngle();
			if (!forceDisableMoveBob)
			{
				hkCalcViewModelView.fOriginal(ecx, owner, vecNewOrigin, vecNewAngles);
				return;
			}

			CallCalcViewModelViewOriginal(ecx, owner, vecNewOrigin, vecNewAngles);
			if (ecx)
			{
				IClientEntity* ent = reinterpret_cast<IClientEntity*>(ecx);
				ent->GetAbsOrigin() = vecNewOrigin;
				ent->GetAbsAngles() = vecNewAngles;
			}
			return;
		}

		// ------------------------------------------------------------
		// Multi-core queued path (mat_queue_mode 2)
		//
		// queued 渲染下 CalcViewModelView 可能跑在 material/queue 线程。
		// 这里把它当“纯渲染期”逻辑：
		//  1) 先用真实 eye 输入跑一遍 engine 内部状态（避免把 controller 当 eye
		//     导致 bob/lag 状态被污染）；
		//  2) 再用 controller 目标位姿做一次 best-effort 覆盖（视觉稳定）。
		// ------------------------------------------------------------
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
		} tlsGuard(true);

		// Call engine logic (keeps internal viewmodel state up to date).
		CallCalcViewModelViewOriginal(ecx, owner, eyePosition, eyeAngles);

		if (m_VR->m_QueuedViewmodelStabilize || forceDisableMoveBob)
		{
			const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos();
			const QAngle targetAngles = m_VR->GetRecommendedViewmodelAbsAngle();

			// Capture what the engine produced (before we override) for debug.
			Vector engineOrigin = {};
			QAngle engineAngles = {};
			if (ecx)
			{
				IClientEntity* ent = reinterpret_cast<IClientEntity*>(ecx);
				engineOrigin = ent->GetAbsOrigin();
				engineAngles = ent->GetAbsAngles();
			}

			// Hard-override viewmodel transform (best-effort).
			// NOTE: queued 渲染下尽量别写 entity 状态，但为了让枪口 attachment/FX
			// 更贴近 controller，这里保留你当前工程的覆盖逻辑。
			bool originSet = false;
			if (m_Game && m_Game->m_Offsets && m_Game->m_Offsets->CBaseEntity_SetAbsOrigin_Client.address && ecx)
			{
				using SetAbsOriginFn = void(__thiscall*)(void*, const Vector&);
				auto setAbsOrigin = (SetAbsOriginFn)m_Game->m_Offsets->CBaseEntity_SetAbsOrigin_Client.address;
				setAbsOrigin(ecx, targetOrigin);
				originSet = true;
			}

			if (ecx)
			{
				IClientEntity* ent = reinterpret_cast<IClientEntity*>(ecx);
				ent->GetAbsAngles() = targetAngles;
				if (!originSet)
					ent->GetAbsOrigin() = targetOrigin;
			}

			if (m_VR->m_QueuedViewmodelStabilizeDebugLog)
			{
				static thread_local std::chrono::steady_clock::time_point s_last{};
				if (!ShouldThrottleLog(s_last, m_VR->m_QueuedViewmodelStabilizeDebugLogHz))
				{
					const uint32_t seq = m_VR->m_RenderFrameSeq.load(std::memory_order_relaxed);
					const uint32_t tid = (uint32_t)GetCurrentThreadId();

					const float dx = targetOrigin.x - engineOrigin.x;
					const float dy = targetOrigin.y - engineOrigin.y;
					const float dz = targetOrigin.z - engineOrigin.z;
					const float dpos = sqrtf(dx * dx + dy * dy + dz * dz);

					Game::logMsg(
						"[VR][VM][queue] tid=%u qmode=%d seq=%u dpos=%.2f eyeO=(%.2f %.2f %.2f) eyeA=(%.2f %.2f %.2f) tgtO=(%.2f %.2f %.2f) tgtA=(%.2f %.2f %.2f) engO=(%.2f %.2f %.2f) engA=(%.2f %.2f %.2f)",
						tid, queueMode, seq, dpos,
						eyePosition.x, eyePosition.y, eyePosition.z,
						eyeAngles.x, eyeAngles.y, eyeAngles.z,
						targetOrigin.x, targetOrigin.y, targetOrigin.z,
						targetAngles.x, targetAngles.y, targetAngles.z,
						engineOrigin.x, engineOrigin.y, engineOrigin.z,
						engineAngles.x, engineAngles.y, engineAngles.z);
				}
			}
		}

		return; // we already called original
	}

	return hkCalcViewModelView.fOriginal(ecx, owner, vecNewOrigin, vecNewAngles);
}

namespace
{
	static constexpr int kMaxGameLaserBeamEffects = 1;
	static constexpr int kMaxGameLaserDotEffects = 1;

	using tCreateParticleEffect = void* (__thiscall*)(void* thisptr, const char* particleName, int attachType, int attachment, Vector originOffset, int unknown);
	using tStopParticleEffect = void(__thiscall*)(void* thisptr, void* effect);
	using tParticleSetControlPointPosition = void(__thiscall*)(void* thisptr, int controlPoint, const Vector& position);
	using tParticleSetControlPointForwardVector = void(__thiscall*)(void* thisptr, int controlPoint, const Vector& forward);

	struct LocalWorldLaserParticleState
	{
		void* beamEffects[kMaxGameLaserBeamEffects] = {};
		void* dotEffects[kMaxGameLaserDotEffects] = {};
		int beamCount = 0;
		int dotCount = 0;
		void* particleProperty = nullptr;
		C_BaseCombatWeapon* weapon = nullptr;
		void* owner = nullptr;
	};

	static LocalWorldLaserParticleState s_localWorldLaserParticle;

	static inline bool TryReadPointer(const void* ptrAddress, void*& out)
	{
		out = nullptr;
		if (!IsReadableMemoryRange(ptrAddress, sizeof(void*)))
			return false;

		__try
		{
			out = *reinterpret_cast<void* const*>(ptrAddress);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out = nullptr;
			return false;
		}
	}

	static inline bool TryWritePointer(void* ptrAddress, void* value)
	{
		if (!ptrAddress)
			return false;

		__try
		{
			*reinterpret_cast<void**>(ptrAddress) = value;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static inline bool TryReadInt(const void* ptrAddress, int& out)
	{
		out = 0;
		if (!IsReadableMemoryRange(ptrAddress, sizeof(int)))
			return false;

		__try
		{
			out = *reinterpret_cast<const int*>(ptrAddress);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			out = 0;
			return false;
		}
	}

	static inline void* CreateParticleEffect(void* particleProperty, const char* particleName, int attachType, int attachment, const Vector& originOffset, int unknown = 0)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_Offsets || !Hooks::m_Game->m_Offsets->CreateParticleEffect.valid)
			return nullptr;
		if (!particleProperty || !particleName || !particleName[0])
			return nullptr;

		auto createParticleEffect = reinterpret_cast<tCreateParticleEffect>(
			Hooks::m_Game->m_Offsets->CreateParticleEffect.address);
		if (!createParticleEffect)
			return nullptr;

		__try
		{
			return createParticleEffect(particleProperty, particleName, attachType, attachment, originOffset, unknown);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	static inline void StopParticleEffect(void* particleProperty, void* effect)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_Offsets || !Hooks::m_Game->m_Offsets->StopParticleEffect.valid)
			return;
		if (!particleProperty || !effect || !IsReadableMemoryRange(particleProperty, sizeof(void*)) || !IsReadableMemoryRange(effect, sizeof(void*)))
			return;

		auto stopParticleEffect = reinterpret_cast<tStopParticleEffect>(
			Hooks::m_Game->m_Offsets->StopParticleEffect.address);
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

	static inline void ClearLocalWorldLaserParticle()
	{
		if (s_localWorldLaserParticle.particleProperty)
		{
			for (void* effect : s_localWorldLaserParticle.beamEffects)
			{
				if (effect)
					StopParticleEffect(s_localWorldLaserParticle.particleProperty, effect);
			}
			for (void* effect : s_localWorldLaserParticle.dotEffects)
			{
				if (effect)
					StopParticleEffect(s_localWorldLaserParticle.particleProperty, effect);
			}
		}

		s_localWorldLaserParticle = {};
	}

	static inline bool SetParticleControlPoint(void* effect, int controlPoint, const Vector& position)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_Offsets || !Hooks::m_Game->m_Offsets->ParticleSetControlPointPosition.valid)
			return false;
		if (!effect || !IsReadableMemoryRange(effect, sizeof(void*)))
			return false;

		auto setControlPoint = reinterpret_cast<tParticleSetControlPointPosition>(
			Hooks::m_Game->m_Offsets->ParticleSetControlPointPosition.address);
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

	static inline bool SetParticleControlPointForwardVector(void* effect, int controlPoint, const Vector& forward)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_Offsets || !Hooks::m_Game->m_Offsets->ParticleSetControlPointForwardVector.valid)
			return false;
		if (!effect || !IsReadableMemoryRange(effect, sizeof(void*)))
			return false;

		Vector normalizedForward = forward;
		if (normalizedForward.IsZero())
			return false;
		VectorNormalize(normalizedForward);

		auto setForward = reinterpret_cast<tParticleSetControlPointForwardVector>(
			Hooks::m_Game->m_Offsets->ParticleSetControlPointForwardVector.address);
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

	static inline bool HasLocalWorldLaserParticle()
	{
		for (void* effect : s_localWorldLaserParticle.beamEffects)
		{
			if (effect)
				return true;
		}
		for (void* effect : s_localWorldLaserParticle.dotEffects)
		{
			if (effect)
				return true;
		}
		return false;
	}

	static inline bool LocalWorldLaserParticleHasInvalidEffect()
	{
		for (void* effect : s_localWorldLaserParticle.beamEffects)
		{
			if (effect && !IsReadableMemoryRange(effect, sizeof(void*)))
				return true;
		}
		for (void* effect : s_localWorldLaserParticle.dotEffects)
		{
			if (effect && !IsReadableMemoryRange(effect, sizeof(void*)))
				return true;
		}
		return false;
	}

	static inline int GetGameLaserBeamCount(const VR* vr)
	{
		if (!vr || vr->m_GameLaserSightColorA <= 0)
			return 0;

		const float thickness = std::clamp(vr->m_GameLaserSightThickness, 0.0f, 8.0f);
		const float alpha = std::clamp(static_cast<float>(vr->m_GameLaserSightColorA) / 255.0f, 0.0f, 1.0f);

		int count = 1;
		if (thickness >= 0.20f || alpha >= 0.35f)
			count = 3;
		if (thickness >= 0.50f || alpha >= 0.65f)
			count = 5;
		if (thickness >= 1.00f || alpha >= 0.85f)
			count = 9;

		return std::clamp(count, 1, kMaxGameLaserBeamEffects);
	}

	static inline int GetGameLaserDotCount(const VR* vr)
	{
		// Keep only the main laser beam.
		// The extra dot uses another weapon_laser_sight effect and makes the result look multi-stroked.
		(void)vr;
		return 0;
	}

	static inline void GetLaserBillboardBasis(VR* vr, const Vector& direction, Vector& right, Vector& up)
	{
		right = vr ? vr->m_HmdRight : Vector{ 0.0f, 0.0f, 0.0f };
		up = vr ? vr->m_HmdUp : Vector{ 0.0f, 0.0f, 0.0f };

		if (right.IsZero() && vr)
			right = vr->m_RightControllerRight;
		if (up.IsZero() && vr)
			up = vr->m_RightControllerUp;
		if (up.IsZero())
			up = Vector{ 0.0f, 0.0f, 1.0f };

		if (right.IsZero())
			right = CrossProduct(up, direction);
		if (right.IsZero())
			right = CrossProduct(Vector{ 0.0f, 0.0f, 1.0f }, direction);

		if (!right.IsZero())
			VectorNormalize(right);
		if (!up.IsZero())
			VectorNormalize(up);
	}

	static inline void SetLaserParticleLine(void* effect, const Vector& start, const Vector& end)
	{
		if (!effect)
			return;

		Vector direction = end - start;
		if (direction.IsZero())
			return;
		VectorNormalize(direction);

		SetParticleControlPoint(effect, 1, start);
		SetParticleControlPoint(effect, 2, end);
		SetParticleControlPoint(effect, 3, start + direction * 32.0f);
		SetParticleControlPointForwardVector(effect, 1, direction);
	}

	static inline void ClearOriginalLocalLaserParticleSlot(void* terrorPlayer, int effectOffset)
	{
		if (!terrorPlayer)
			return;

		auto* base = reinterpret_cast<uint8_t*>(terrorPlayer);
		void* effect = nullptr;
		void* effectAddress = base + effectOffset;
		if (!TryReadPointer(effectAddress, effect) || !effect)
			return;

		void* particleProperty = base + 0x2A8;
		StopParticleEffect(particleProperty, effect);
		TryWritePointer(effectAddress, nullptr);
	}

	static inline void ClearOriginalLocalLaserParticles(void* terrorPlayer)
	{
		ClearOriginalLocalLaserParticleSlot(terrorPlayer, 0x28D0);
		ClearOriginalLocalLaserParticleSlot(terrorPlayer, 0x28E8);
	}

	static inline bool GetLaserAimSegment(Vector& start, Vector& end)
	{
		VR* vr = Hooks::m_VR;
		if (!vr || !vr->m_IsVREnabled)
			return false;

		if (vr->m_HasAimLine)
		{
			start = vr->m_AimLineStart;
			end = vr->m_AimLineEnd;

			Vector direction = end - start;
			if (!direction.IsZero())
				return true;
		}

		QAngle controllerAng = vr->GetRightControllerAbsAngle();
		Vector direction{}, right{}, up{};
		QAngle::AngleVectors(controllerAng, &direction, &right, &up);
		if (direction.IsZero())
			direction = vr->m_LastAimDirection.IsZero() ? vr->m_HmdForward : vr->m_LastAimDirection;
		if (direction.IsZero())
			return false;

		VectorNormalize(direction);
		start = vr->GetRightControllerAbsPos() + direction * 2.0f;
        if (vr->m_ForceNonVRServerMovement && vr->m_HasNonVRAimSolution)
            end = vr->m_NonVRAimHitPoint;
        else
            end = start + direction * 8192.0f;

		return !((end - start).IsZero());
	}

	static inline Vector BuildLaserSightEndOffset(const Vector& direction)
	{
		VR* vr = Hooks::m_VR;
		if (!vr)
			return Vector{ 0.0f, 0.0f, 0.0f };

		Vector right = vr->m_HmdRight;
		Vector up = vr->m_HmdUp;

		if (right.IsZero())
			right = vr->m_RightControllerRight;
		if (up.IsZero())
			up = vr->m_RightControllerUp;
		if (up.IsZero())
			up = Vector(0.0f, 0.0f, 1.0f);

		if (right.IsZero())
			right = CrossProduct(up, direction);
		if (right.IsZero())
			right = CrossProduct(Vector(0.0f, 0.0f, 1.0f), direction);

		if (!right.IsZero())
			VectorNormalize(right);
		if (!up.IsZero())
			VectorNormalize(up);

		const Vector& offset = vr->m_GameLaserSightEndOffset;
		return right * offset.x + up * offset.y + direction * offset.z;
	}

	static inline void UpdateLocalWorldLaserParticle(void* terrorPlayer)
	{
		if (!terrorPlayer || !Hooks::m_Game || !Hooks::m_Game->m_EngineClient || !Hooks::m_VR)
			return;

		VR* vr = Hooks::m_VR;
		const int localPlayerIndex = Hooks::m_Game->m_EngineClient->GetLocalPlayer();
		C_BaseEntity* localEntity = Hooks::m_Game->GetClientEntity(localPlayerIndex);
		if (localEntity && reinterpret_cast<void*>(localEntity) != terrorPlayer)
			return;

		auto* localPlayer = reinterpret_cast<C_BasePlayer*>(localEntity ? localEntity : terrorPlayer);
		if (!localPlayer)
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		if (!vr->m_IsVREnabled || !vr->m_GameLaserSightBeamEnabled)
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		C_BaseCombatWeapon* activeWeapon = nullptr;
		__try
		{
			activeWeapon = localPlayer->GetActiveWeapon();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			activeWeapon = nullptr;
		}
		if (!activeWeapon)
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		auto* weapon = reinterpret_cast<C_WeaponCSBase*>(activeWeapon);
		if (!vr->IsWeaponLaserSightActive(weapon))
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		Vector start{}, end{};
		if (!GetLaserAimSegment(start, end))
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		Vector direction = end - start;
		if (direction.IsZero())
		{
			ClearLocalWorldLaserParticle();
			return;
		}
		VectorNormalize(direction);

		const Vector laserEnd = end + BuildLaserSightEndOffset(direction);
		Vector laserDirection = laserEnd - start;
		if (laserDirection.IsZero())
			laserDirection = direction;
		VectorNormalize(laserDirection);

		const int desiredBeamCount = GetGameLaserBeamCount(vr);
		const int desiredDotCount = GetGameLaserDotCount(vr);
		if (desiredBeamCount <= 0)
		{
			ClearLocalWorldLaserParticle();
			return;
		}

		void* particleProperty = reinterpret_cast<uint8_t*>(activeWeapon) + 0x2A8;
		if (s_localWorldLaserParticle.weapon != activeWeapon ||
			s_localWorldLaserParticle.owner != terrorPlayer ||
			s_localWorldLaserParticle.particleProperty != particleProperty ||
			s_localWorldLaserParticle.beamCount != desiredBeamCount ||
			s_localWorldLaserParticle.dotCount != desiredDotCount)
		{
			ClearLocalWorldLaserParticle();
		}

		if (LocalWorldLaserParticleHasInvalidEffect())
			ClearLocalWorldLaserParticle();

		if (!HasLocalWorldLaserParticle())
		{
			const auto* weaponBase = reinterpret_cast<const uint8_t*>(activeWeapon);
			int attachment = 0;
			TryReadInt(weaponBase + 0xCE0, attachment);
			const int attachType = (attachment > 0) ? 5 : 0;
			if (attachment <= 0)
				attachment = 0;

			const Vector originOffset{ 0.0f, 0.0f, 0.0f };
			for (int i = 0; i < desiredBeamCount; ++i)
				s_localWorldLaserParticle.beamEffects[i] = CreateParticleEffect(particleProperty, "weapon_laser_sight", attachType, attachment, originOffset);
			for (int i = 0; i < desiredDotCount; ++i)
				s_localWorldLaserParticle.dotEffects[i] = CreateParticleEffect(particleProperty, "weapon_laser_sight", 0, 0, originOffset);

			s_localWorldLaserParticle.beamCount = desiredBeamCount;
			s_localWorldLaserParticle.dotCount = desiredDotCount;
			s_localWorldLaserParticle.particleProperty = particleProperty;
			s_localWorldLaserParticle.weapon = activeWeapon;
			s_localWorldLaserParticle.owner = terrorPlayer;

			if (!HasLocalWorldLaserParticle())
			{
				static bool s_loggedCreateFailed = false;
				if (!s_loggedCreateFailed)
				{
					s_loggedCreateFailed = true;
				}
				return;
			}

			static bool s_loggedCreated = false;
			if (!s_loggedCreated)
			{
				s_loggedCreated = true;
			}
		}

		Vector right{}, up{};
		GetLaserBillboardBasis(vr, laserDirection, right, up);
		const float radius = std::clamp(vr->m_GameLaserSightThickness, 0.0f, 8.0f);
		static const float kBeamPattern[kMaxGameLaserBeamEffects][2] = {
			{ 0.0f, 0.0f }
		};

		for (int i = 0; i < s_localWorldLaserParticle.beamCount && i < kMaxGameLaserBeamEffects; ++i)
		{
			void* effect = s_localWorldLaserParticle.beamEffects[i];
			if (!effect)
				continue;

			const Vector offset = right * (kBeamPattern[i][0] * radius) + up * (kBeamPattern[i][1] * radius);
			SetLaserParticleLine(effect, start + offset, laserEnd + offset);
		}

		const float dotRadius = std::max(2.0f, radius * 3.0f);
		Vector diagonalA = right + up;
		Vector diagonalB = right - up;
		if (!diagonalA.IsZero())
			VectorNormalize(diagonalA);
		if (!diagonalB.IsZero())
			VectorNormalize(diagonalB);

		const Vector dotAxes[kMaxGameLaserDotEffects] = {
			right
		};
		for (int i = 0; i < s_localWorldLaserParticle.dotCount && i < kMaxGameLaserDotEffects; ++i)
		{
			void* effect = s_localWorldLaserParticle.dotEffects[i];
			if (!effect)
				continue;

			Vector axis = dotAxes[i];
			if (axis.IsZero())
				continue;
			VectorNormalize(axis);
			SetLaserParticleLine(effect, laserEnd - axis * dotRadius, laserEnd + axis * dotRadius);
		}
	}
}

void __fastcall Hooks::dUpdateLaserSight(void* ecx, void* edx)
{
	const bool hasVr = (m_VR != nullptr);
	const bool vrEnabled = hasVr && m_VR->m_IsVREnabled;
	const bool replacementEnabled = vrEnabled
		&& m_VR->m_GameLaserSightBeamEnabled
		&& m_VR->m_GameLaserSightReplaceParticle;

	bool skipOriginalForLocalPlayer = false;
	if (replacementEnabled && m_Game && m_Game->m_EngineClient)
	{
		const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
		C_BaseEntity* localEntity = m_Game->GetClientEntity(localPlayerIndex);
		skipOriginalForLocalPlayer = (localEntity != nullptr) && (reinterpret_cast<void*>(localEntity) == ecx);
	}

	if (!replacementEnabled)
	{
		hkUpdateLaserSight.fOriginal(ecx);
		ClearLocalWorldLaserParticle();
		return;
	}

	if (!skipOriginalForLocalPlayer)
		hkUpdateLaserSight.fOriginal(ecx);
	else
		ClearOriginalLocalLaserParticles(ecx);

	// Keep the helper behavior identical to the original code path:
	// it silently ignores non-local callers instead of clearing the local replacement particle.
	UpdateLocalWorldLaserParticle(ecx);
}

int Hooks::dServerFireTerrorBullets(int playerId, const Vector& vecOrigin, const QAngle& vecAngles, int a4, int a5, int a6, float a7)
{
	Vector vecNewOrigin = vecOrigin;
	QAngle vecNewAngles = vecAngles;

	// Server host
	if (m_VR->m_IsVREnabled && playerId == m_Game->m_EngineClient->GetLocalPlayer())
	{
		const bool scopeActive = m_VR->IsScopeActive();
		vecNewOrigin = m_VR->m_MouseModeEnabled ? GetMouseModeGunOriginAbs(m_VR) : m_VR->GetRightControllerAbsPos();

		// ForceNonVRServerMovement: aim the *visual* bullet line to the solved hit point (H)
		// so what you see matches what the remote non-VR server will hit.
		if (m_VR->m_ForceNonVRServerMovement && m_VR->m_HasNonVRAimSolution)
		{
			Vector to = m_VR->m_NonVRAimHitPoint - vecNewOrigin;
			if (!to.IsZero())
			{
				VectorNormalize(to);
				QAngle ang;
				QAngle::VectorAngles(to, ang);
				NormalizeAndClampViewAngles(ang);
				vecNewAngles = ang;
			}
			else
			{
				vecNewAngles = m_VR->m_NonVRAimAngles;
			}
		}
		else if (scopeActive)
		{
			vecNewOrigin = m_VR->GetScopeCameraAbsPos();
			vecNewAngles = m_VR->GetScopeCameraAbsAngle();
		}
		// Third-person convergence
		else if (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
		{
			Vector to = m_VR->m_AimConvergePoint - vecNewOrigin;
			if (!to.IsZero())
			{
				VectorNormalize(to);
				QAngle ang;
				QAngle::VectorAngles(to, ang);
				NormalizeAndClampViewAngles(ang);
				vecNewAngles = ang;
			}
			else
			{
				vecNewAngles = m_VR->m_MouseModeEnabled ? GetMouseModeFallbackAimAngles(m_VR) : m_VR->GetRightControllerAbsAngle();
			}
		}
		else if (m_VR->m_MouseModeEnabled)
		{
			const Vector target = (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
				? m_VR->m_AimConvergePoint
				: GetMouseModeDefaultTargetAbs(m_VR);

			QAngle ang;
			if (GetMouseModeAimAnglesToTarget(m_VR, vecNewOrigin, target, ang))
				vecNewAngles = ang;
			else
				vecNewAngles = GetMouseModeFallbackAimAngles(m_VR);
		}
		else
		{
			vecNewAngles = m_VR->GetRightControllerAbsAngle();
		}
	}
	// Clients
	else if (m_Game->IsValidPlayerIndex(playerId) && m_Game->m_PlayersVRInfo[playerId].isUsingVR)
	{
		vecNewOrigin = m_Game->m_PlayersVRInfo[playerId].controllerPos;
		vecNewAngles = m_Game->m_PlayersVRInfo[playerId].controllerAngle;
	}

	return hkServerFireTerrorBullets.fOriginal(playerId, vecNewOrigin, vecNewAngles, a4, a5, a6, a7);
}

int Hooks::dClientFireTerrorBullets(
	int playerId,
	const Vector& vecOrigin,
	const QAngle& vecAngles,
	int a4, int a5, int a6,
	float a7)
{
	Vector vecNewOrigin = vecOrigin;
	QAngle vecNewAngles = vecAngles;

	// 只改本地玩家的“本地预测/表现”
	if (m_VR->m_IsVREnabled && playerId == m_Game->m_EngineClient->GetLocalPlayer())
	{
		// If looking through scope: bullets originate from scope camera and go through its center
		const bool scopeActive = m_VR->IsScopeActive();
		const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
		// In mat_queue_mode!=0, client-side bullet FX can lag behind the rendered gun/aim line if they
		// read main-thread poses. Treat this hook as visual-only and allow it to consume the render-frame snapshot.
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
		} tlsGuard(queueMode == 2);


		if (!m_VR->m_ForceNonVRServerMovement)
		{
			// VR-aware server：默认起点/方向都跟控制器；鼠标模式改用鼠标枪口锚点 + 目标方向
			if (scopeActive)
			{
				vecNewOrigin = m_VR->GetScopeCameraAbsPos();
				vecNewAngles = m_VR->GetScopeCameraAbsAngle();
			}
			else
			{
				if (m_VR->m_MouseModeEnabled)
				{
					vecNewOrigin = GetMouseModeGunOriginAbs(m_VR);

					const Vector target = (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
						? m_VR->m_AimConvergePoint
						: GetMouseModeDefaultTargetAbs(m_VR);

					QAngle ang;
					if (GetMouseModeAimAnglesToTarget(m_VR, vecNewOrigin, target, ang))
						vecNewAngles = ang;
					else
						vecNewAngles = GetMouseModeFallbackAimAngles(m_VR);
				}
				else
				{
					vecNewOrigin = m_VR->GetRightControllerAbsPos();

					if (m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
					{
						Vector to = m_VR->m_AimConvergePoint - vecNewOrigin;
						if (!to.IsZero())
						{
							VectorNormalize(to);
							QAngle ang;
							QAngle::VectorAngles(to, ang);
							NormalizeAndClampViewAngles(ang);
							vecNewAngles = ang;
						}
						else
						{
							vecNewAngles = m_VR->GetRightControllerAbsAngle();
						}
					}
					else
					{
						vecNewAngles = m_VR->GetRightControllerAbsAngle();
					}
				}
			}
		}
		else
		{
			// Non-VR server：服务器判定仍以 eye(vecOrigin)+cmd->viewangles 为准（我们不会改服务器判定）。
			// 但客户端的弹道线/枪口火焰/烟雾等特效可以选择从控制器枪口发出（纯视觉）。
			if (m_VR->m_NonVRServerMovementEffectsFromController)
			{
				if (scopeActive)
				{
					vecNewOrigin = m_VR->GetScopeCameraAbsPos();
				}
				else
				{
					vecNewOrigin = m_VR->m_MouseModeEnabled ? GetMouseModeGunOriginAbs(m_VR) : m_VR->GetRightControllerAbsPos();
				}

				// 开关决定：要不要把 angles 替换成“控制器纯角度/汇聚角度”
				// - true  : 覆盖 angles（通常会让本地弹道更“直/更跟手”，但只是本地表现）
				// - false : 保持 vecAngles（通常包含引擎/服务器那套散布偏转 → 看起来更“标准散布”）
				if (m_VR->m_NonVRServerMovementAngleOverride)
				{
					// Prefer the solved eye-based aim. This keeps client prediction + hit feedback
					// consistent with what the non-VR server will do.
					if (m_VR->m_HasNonVRAimSolution)
					{
						vecNewAngles = m_VR->m_NonVRAimAngles;
					}
					else
					{
						if (!scopeActive && m_VR->IsThirdPersonCameraActive() && m_VR->m_HasAimConvergePoint)
						{
							Vector to = m_VR->m_AimConvergePoint - vecNewOrigin; // 注意：这里是 vecOrigin
							if (!to.IsZero())
							{
								VectorNormalize(to);
								QAngle ang;
								QAngle::VectorAngles(to, ang);
								NormalizeAndClampViewAngles(ang);
								vecNewAngles = ang;
							}
							else
							{
								vecNewAngles = m_VR->GetRightControllerAbsAngle();
							}
						}
						else
						{
							vecNewAngles = m_VR->GetRightControllerAbsAngle();
						}
					}
				}
				// else：不动 vecNewAngles = vecAngles（保留“标准散布”的那套）
			}
		}

		// Final bullet FX alignment: apply hit-point offset AFTER all angle overrides.
		// This is intentionally visual-only (client FX). It does not affect server hit registration.
		if (m_VR->m_IsVREnabled
			&& m_VR->m_ForceNonVRServerMovement
			&& m_VR->m_HasNonVRAimSolution)
		{
			const int qmode = (m_Game ? m_Game->GetMatQueueMode() : 0);
			Vector visualOff = m_VR->m_BulletVisualHitOffset;
			if (qmode != 0)
			{
				visualOff.x += m_VR->m_QueuedBulletVisualHitOffset.x;
				visualOff.y += m_VR->m_QueuedBulletVisualHitOffset.y;
				visualOff.z += m_VR->m_QueuedBulletVisualHitOffset.z;
			}

			if (!visualOff.IsZero())
			{
				Vector originForDir = vecNewOrigin;
				Vector targetH = m_VR->m_NonVRAimHitPoint;

				Vector fwd = targetH - originForDir;
				if (!fwd.IsZero())
				{
					VectorNormalize(fwd);
					Vector worldUp(0.0f, 0.0f, 1.0f);
					Vector right;
					CrossProduct(worldUp, fwd, right);
					if (right.LengthSqr() < 1e-6f)
					{
						worldUp = Vector(0.0f, 1.0f, 0.0f);
						CrossProduct(worldUp, fwd, right);
					}
					VectorNormalize(right);
					Vector up;
					CrossProduct(fwd, right, up);
					VectorNormalize(up);

					const float su = m_VR->m_VRScale;
					const Vector offSu = visualOff * su;
					targetH += (fwd * offSu.x) + (right * offSu.y) + (up * offSu.z);

					Vector to = targetH - originForDir;
					if (!to.IsZero())
					{
						VectorNormalize(to);
						QAngle ang;
						QAngle::VectorAngles(to, ang);
						NormalizeAndClampViewAngles(ang);
						vecNewAngles = ang;

						if (m_VR->m_NonVRServerMovementEffectsDebugLog)
						{
							static thread_local std::chrono::steady_clock::time_point s_lastOff{};
							if (!ShouldThrottleLog(s_lastOff, m_VR->m_NonVRServerMovementEffectsDebugLogHz))
							{
								const uint32_t tid = (uint32_t)GetCurrentThreadId();
								const uint32_t seq = m_VR->m_RenderFrameSeq.load(std::memory_order_relaxed);
								Game::logMsg(
									"[VR][FX][bullets][offset] tid=%u qmode=%d seq=%u offTotal=(%.3f %.3f %.3f)m base=(%.3f %.3f %.3f)m qExtra=(%.3f %.3f %.3f)m origin=(%.2f %.2f %.2f) H=(%.2f %.2f %.2f)",
									tid, qmode, seq,
									visualOff.x, visualOff.y, visualOff.z,
									m_VR->m_BulletVisualHitOffset.x, m_VR->m_BulletVisualHitOffset.y, m_VR->m_BulletVisualHitOffset.z,
									m_VR->m_QueuedBulletVisualHitOffset.x, m_VR->m_QueuedBulletVisualHitOffset.y, m_VR->m_QueuedBulletVisualHitOffset.z,
									originForDir.x, originForDir.y, originForDir.z,
									targetH.x, targetH.y, targetH.z);
							}
						}
					}
				}
			}
		}



		if (m_VR->m_IsVREnabled && m_Game && m_Game->m_EngineClient
			&& playerId == m_Game->m_EngineClient->GetLocalPlayer())
		{
			int weaponId = (int)C_WeaponCSBase::WeaponID::NONE;
			C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerId);
			if (localPlayer)
			{
				C_WeaponCSBase* activeWeapon = (C_WeaponCSBase*)localPlayer->GetActiveWeapon();
				if (activeWeapon)
					weaponId = (int)activeWeapon->GetWeaponID();
			}
			m_VR->TriggerWeaponFireHaptics(weaponId, false);
		}

		// RightAmmoHUD: hit-based target HP bar has been removed.
	// The ammo HUD now shows HP%% for the *aimed* special infected (and Witch) and hides instantly on leave.
		if (m_VR->m_IsVREnabled && m_Game && m_Game->m_EngineClient
			&& playerId == m_Game->m_EngineClient->GetLocalPlayer())
		{
			// Start a fresh shot window once per FireTerrorBullets call.
			// RegisterPotentialKillSoundHit keeps a VR-corrected impact candidate for
			// later hurt/death events and also serves as the fallback path for
			// common-infected hit feedback when local hurt events are unavailable.
			m_VR->BeginPredictedHitFeedbackShot();
			// Use the final VR-corrected shot ray for predicted hit feedback so the
			// hit sound / hit indicator matches the actual VR muzzle origin and aim.
			m_VR->RegisterPotentialKillSoundHit(vecNewOrigin, vecNewAngles);
		}

		const auto original = hkClientFireTerrorBullets.fOriginal;
		if (!original)
			return 0;

		return original(playerId, vecNewOrigin, vecNewAngles, a4, a5, a6, a7);
	}
}


// === 用下面这整个函数替换你当前的 Hooks::dProcessUsercmds ===
float __fastcall Hooks::dProcessUsercmds(void* ecx, void* edx, edict_t* player,
	void* buf, int numcmds, int totalcmds,
	int dropped_packets, bool ignore, bool paused)
{
	// ★ 进入该钩子，说明本进程正在跑“服务器”逻辑（listen/dedicated）
	Hooks::s_ServerUnderstandsVR = true;

	// Function pointer for CBaseEntity::entindex
	typedef int(__thiscall* tEntindex)(void* thisptr);
	static tEntindex oEntindex = (tEntindex)(m_Game->m_Offsets->CBaseEntity_entindex.address);

	IServerUnknown* pUnknown = player->m_pUnk;
	Server_BaseEntity* pPlayer = (Server_BaseEntity*)pUnknown->GetBaseEntity();
	m_Game->m_CurrentUsercmdPlayer = pPlayer;

	int index = oEntindex(pPlayer);
	m_Game->m_CurrentUsercmdID = index;

	float result = hkProcessUsercmds.fOriginal(ecx, player, buf, numcmds, totalcmds, dropped_packets, ignore, paused);

	// ===== 你原有的“近战挥砍检测/追踪”逻辑，保持不变 =====
	const bool hasValidPlayer = m_Game->IsValidPlayerIndex(index);

	if (hasValidPlayer && m_Game->m_PlayersVRInfo[index].isUsingVR && m_Game->m_PlayersVRInfo[index].isMeleeing)
	{
		typedef Server_WeaponCSBase* (__thiscall* tGetActiveWep)(void* thisptr);
		static tGetActiveWep oGetActiveWep = (tGetActiveWep)(m_Game->m_Offsets->GetActiveWeapon.address);
		Server_WeaponCSBase* curWep = oGetActiveWep(pPlayer);

		if (curWep)
		{
			int wepID = curWep->GetWeaponID();
			if (wepID == 19) // melee weapon
			{
				if (m_Game->m_PlayersVRInfo[index].isNewSwing)
				{
					m_Game->m_PlayersVRInfo[index].isNewSwing = false;
					curWep->entitiesHitThisSwing = 0;
				}

				typedef void* (__thiscall* tGetMeleeWepInfo)(void* thisptr);
				static tGetMeleeWepInfo oGetMeleeWepInfo = (tGetMeleeWepInfo)(m_Game->m_Offsets->GetMeleeWeaponInfo.address);
				void* meleeWepInfo = oGetMeleeWepInfo(curWep);

				Vector initialForward, initialRight, initialUp;
				QAngle::AngleVectors(m_Game->m_PlayersVRInfo[index].prevControllerAngle, &initialForward, &initialRight, &initialUp);
				Vector initialMeleeDirection = VectorRotate(initialForward, initialRight, 50.0f);
				VectorNormalize(initialMeleeDirection);

				Vector finalForward, finalRight, finalUp;
				QAngle::AngleVectors(m_Game->m_PlayersVRInfo[index].controllerAngle, &finalForward, &finalRight, &finalUp);
				Vector finalMeleeDirection = VectorRotate(finalForward, finalRight, 50.0f);
				VectorNormalize(finalMeleeDirection);

				Vector pivot;
				CrossProduct(initialMeleeDirection, finalMeleeDirection, pivot);
				VectorNormalize(pivot);

				float swingAngle = acosf(DotProduct(initialMeleeDirection, finalMeleeDirection)) * 180.0f / 3.14159265f;

				m_Game->m_Hooks->hkGetPrimaryAttackActivity.fOriginal(curWep, meleeWepInfo); // Needed to call TestMeleeSwingCollision

				m_Game->m_PerformingMelee = true;

				Vector traceDirection = initialMeleeDirection;
				int numTraces = 10;
				float traceAngle = swingAngle / numTraces;
				for (int i = 0; i < numTraces; ++i)
				{
					traceDirection = VectorRotate(traceDirection, pivot, traceAngle);
					m_Game->m_Hooks->hkTestMeleeSwingCollisionServer.fOriginal(curWep, traceDirection);
				}

				m_Game->m_PerformingMelee = false;
			}
		}
	}
	else if (hasValidPlayer)
	{
		m_Game->m_PlayersVRInfo[index].isNewSwing = true;
	}

	if (hasValidPlayer)
	{
		m_Game->m_PlayersVRInfo[index].prevControllerAngle = m_Game->m_PlayersVRInfo[index].controllerAngle;
	}

	return result;
}

int Hooks::dReadUsercmd(void* buf, CUserCmd* move, CUserCmd* from)
{
	hkReadUsercmd.fOriginal(buf, move, from);

	int i = m_Game->m_CurrentUsercmdID;
	const bool hasValidPlayer = m_Game->IsValidPlayerIndex(i);
	if (m_VR->m_EncodeVRUsercmd && move->tick_count < 0) // Signal for VR CUserCmd
	{
		move->tick_count *= -1;

		if (move->command_number < 0)
		{
			move->command_number *= -1;
			if (hasValidPlayer)
			{
				m_Game->m_PlayersVRInfo[i].isMeleeing = true;
			}
		}
		else
		{
			if (hasValidPlayer)
			{
				m_Game->m_PlayersVRInfo[i].isMeleeing = false;
			}
		}

		if (hasValidPlayer)
		{
			m_Game->m_PlayersVRInfo[i].isUsingVR = true;
			m_Game->m_PlayersVRInfo[i].controllerAngle.x = (float)move->mousedx / 10;
			m_Game->m_PlayersVRInfo[i].controllerAngle.y = (float)move->mousedy / 10;
			m_Game->m_PlayersVRInfo[i].controllerPos.x = move->viewangles.z;
			m_Game->m_PlayersVRInfo[i].controllerPos.y = move->upmove;
		}

		// Decode controllerAngle.z
		int rollEncoding = move->command_number / 10000000;
		move->command_number -= rollEncoding * 10000000;
		if (hasValidPlayer)
		{
			m_Game->m_PlayersVRInfo[i].controllerAngle.z = (rollEncoding * 2) - 180;
		}

		// Decode viewangles.x
		int decodedZInt = (move->viewangles.x / 10000);
		float decodedAngle = fabsf((float)(move->viewangles.x - (decodedZInt * 10000)) / 10);
		decodedAngle -= 360.0f;
		float decodedZ = (float)decodedZInt / 10.0f;

		if (hasValidPlayer)
		{
			m_Game->m_PlayersVRInfo[i].controllerPos.z = decodedZ;
		}

		move->viewangles.x = decodedAngle;
		move->viewangles.z = 0;
		move->upmove = 0;
	}
	else
	{
		if (hasValidPlayer)
		{
			m_Game->m_PlayersVRInfo[i].isUsingVR = false;
		}
	}

	// ---- roomscale 1:1 server apply ----
	if (m_Game && m_VR && m_VR->m_Roomscale1To1Movement && !m_VR->m_ForceNonVRServerMovement)
	{
		static constexpr uint32_t kRSButtonsMask = 0xFC000000u; // bits 26..31
		const uint32_t rawButtons = (uint32_t)move->buttons;
		const uint32_t rawPacked = (rawButtons & kRSButtonsMask);
		// NOTE: Do NOT reuse m_Roomscale1To1DebugLastServer for both "pre" and "apply".
		// Otherwise "pre" consumes the throttle budget and "apply" never prints.
		static std::chrono::steady_clock::time_point s_roomscaleServerPreLast{};
		if (m_VR->m_Roomscale1To1DebugLog && rawPacked != 0 && !ShouldThrottleLog(s_roomscaleServerPreLast, m_VR->m_Roomscale1To1DebugLogHz))
		{
			Game::logMsg("[VR][1to1][server] pre cmd=%d tick=%d player=%d wsel=%d buttons=0x%08X",
				move->command_number, move->tick_count, i, move->weaponselect, (unsigned)rawButtons);
		}
		Vector deltaM;
		if (VR::DecodeRoomscale1To1Delta((int)rawPacked, deltaM) && move->weaponselect == 0)
		{
			// Hide our custom packed bits from the stock server movement/weapon code.
			move->buttons &= ~(int)kRSButtonsMask;
			Server_BaseEntity* ent = m_Game->m_CurrentUsercmdPlayer;
			if (ent && m_Game->m_EngineTrace && m_Game->m_Offsets)
			{
				using SetAbsOriginFn = void(__thiscall*)(void*, const Vector&);
				auto setAbsOrigin = (SetAbsOriginFn)(m_Game->m_Offsets->CBaseEntity_SetAbsOrigin_Server.address);

				if (setAbsOrigin)
				{
					Vector origin = *(Vector*)((uintptr_t)ent + 0x2CC);
					Vector deltaU(deltaM.x * m_VR->m_VRScale, deltaM.y * m_VR->m_VRScale, 0.0f);
					Vector target = origin + deltaU;

					Vector mins(-16, -16, 0);
					Vector maxs(16, 16, 72);
					// NOTE: Our server entity type is a thin vtable stub; pointer-casting it to IHandleEntity
					// is not reliable for self-filtering. For 1:1 roomscale we only need world collision here.
					struct TraceFilterWorldOnly final : public CTraceFilter
					{
						TraceFilterWorldOnly() : CTraceFilter(nullptr, 0) {}
						bool ShouldHitEntity(IHandleEntity*, int) override { return false; }
						TraceType GetTraceType() const override { return TraceType::TRACE_WORLD_ONLY; }
					} filter;

					// Lift slightly to avoid rare floor-penetration marking the start as solid.
					Vector o2 = origin;
					Vector t2 = target;
					o2.z += 0.25f;
					t2.z += 0.25f;
					Ray_t ray;
					ray.Init(o2, t2, mins, maxs);

					trace_t tr;

					const unsigned int mask = CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_GRATE;
					m_Game->m_EngineTrace->TraceRay(ray, mask, &filter, &tr);

					bool didMove = false;
					if (!tr.startsolid)
					{
						Vector end = tr.endpos;
						end.z = origin.z; // keep vertical position unchanged (we only apply planar room-scale)
						setAbsOrigin(ent, end);
						didMove = true;
					}

					// Hard debug sample: once every 32 cmds, no throttle. This tells us whether the server actually moved the entity.
					if (m_VR->m_Roomscale1To1DebugLog && ((move->command_number & 31) == 0))
					{
						Game::logMsg("[VR][1to1][server] APPLY-SAMPLE cmd=%d tick=%d player=%d ent=%p setAbsOrigin=%p packed=0x%08X dM=(%.3f %.3f) origin=(%.2f %.2f %.2f) target=(%.2f %.2f %.2f) end=(%.2f %.2f %.2f) frac=%.3f startsolid=%d didMove=%d",
							move->command_number, move->tick_count, i, (void*)ent, (void*)setAbsOrigin, (unsigned)rawPacked,
							deltaM.x, deltaM.y,
							origin.x, origin.y, origin.z,
							target.x, target.y, target.z,
							tr.endpos.x, tr.endpos.y, tr.endpos.z,
							tr.fraction, (int)tr.startsolid, (int)didMove);
					}

					// Normal throttled "apply" log (separate from pre).
					if (m_VR->m_Roomscale1To1DebugLog && !ShouldThrottleLog(m_VR->m_Roomscale1To1DebugLastServer, m_VR->m_Roomscale1To1DebugLogHz))
					{
						Game::logMsg("[VR][1to1][server] apply player=%d dM=(%.3f %.3f) origin=(%.2f %.2f %.2f) target=(%.2f %.2f %.2f) end=(%.2f %.2f %.2f) frac=%.3f startsolid=%d",
							i, deltaM.x, deltaM.y, origin.x, origin.y, origin.z, target.x, target.y, target.z, tr.endpos.x, tr.endpos.y, tr.endpos.z, tr.fraction, (int)tr.startsolid);
					}
				}
			}
		}
	}
	return 1;
}

void __fastcall Hooks::dWriteUsercmdDeltaToBuffer(void* ecx, void* edx, int a1, void* buf, int from, int to, bool isnewcommand)
{
	return hkWriteUsercmdDeltaToBuffer.fOriginal(ecx, a1, buf, from, to, isnewcommand);
}

int Hooks::dWriteUsercmd(void* buf, CUserCmd* to, CUserCmd* from)
{
	// VR 未启用：原样走引擎
	if (!m_VR->m_IsVREnabled)
		return hkWriteUsercmd.fOriginal(buf, to, from);

	// 只有（配置开启编码）且（本进程确实在跑服务器钩子＝能解码）且（未强制走非 VR 标准）时才编码
	const bool canEncode = (m_VR->m_EncodeVRUsercmd && !m_VR->m_ForceNonVRServerMovement);

	if (!canEncode)
	{
		// 非 VR 服务器：不要动 tick_count/command_number/viewangles.z/upmove 等
		// 保持标准 CUserCmd，让 dCreateMove 写入的 forwardmove/sidemove 正常生效
		return hkWriteUsercmd.fOriginal(buf, to, from);
	}

	// ======== 以下为原有“编码”逻辑，保持不变，仅包在 canEncode 分支内 ========
	CInput* m_Input = **(CInput***)(m_Game->m_Offsets->g_pppInput.address);
	CVerifiedUserCmd* pVerifiedCommands = *(CVerifiedUserCmd**)((uintptr_t)m_Input + 0xF0);
	CVerifiedUserCmd* pVerified = &pVerifiedCommands[(to->command_number) % 150];

	if (to && m_VR->ShouldSuppressPrimaryFire(to, nullptr))
	{
		to->buttons &= ~(1 << 0); // IN_ATTACK
	}

	// Signal to the server that this CUserCmd has VR info
	to->tick_count *= -1;

	int originalCommandNum = to->command_number;

	QAngle controllerAngles = m_VR->GetRightControllerAbsAngle();
	to->mousedx = (int)(controllerAngles.x * 10.0f); // Strip off 2nd decimal to save bits.
	to->mousedy = (int)(controllerAngles.y * 10.0f);
	int rollEncoding = (((int)controllerAngles.z + 180) / 2 * 10000000);
	to->command_number += rollEncoding;

	if (VectorLength(m_VR->m_RightControllerPose.TrackedDeviceVel) > 1.1f)
	{
		to->command_number *= -1; // Signal to server that melee swing in motion
	}

	Vector controllerPos = m_VR->GetRightControllerAbsPos();
	float xAngleOrig = to->viewangles.x; // 备份

	to->viewangles.z = controllerPos.x;
	to->upmove = controllerPos.y;

	// Space in CUserCmd is tight, so encode viewangle.x and controllerPos.z together.
	// Encoding will overflow if controllerPos.z goes beyond +-21474.8
	int encodedAngle = (int)((xAngleOrig + 360.0f) * 10.0f);
	int encoding = (int)(controllerPos.z * 10.0f) * 10000;
	encoding += (encoding < 0) ? -encodedAngle : encodedAngle;
	to->viewangles.x = (float)encoding;

	// 写入
	hkWriteUsercmd.fOriginal(buf, to, from);

	// 还原本地 CUserCmd
	to->viewangles.x = xAngleOrig;
	to->tick_count *= -1;
	to->viewangles.z = 0.0f;
	to->upmove = 0.0f;
	to->command_number = originalCommandNum;

	// 重算校验，否则多人下枪声会异常
	pVerified->m_cmd = *to;
	pVerified->m_crc = to->GetChecksum();
	return 1;
}

