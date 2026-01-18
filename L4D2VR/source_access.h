#pragma once

// Lightweight, version-tolerant helpers for Source (L4D2) client entity access.
//
// Design goals:
//  - Keep dependencies minimal and avoid pulling in huge SDK headers.
//  - Prefer safe, read-only accessors that are useful for VR rendering and debugging.
//  - Centralize entity sub-interface offsets and common vtable indices.
//
// This file intentionally does NOT provide packet manipulation, command forgery, or other
// exploit-oriented primitives.

#include <cstdint>
#include "vector.h"

class C_BaseEntity;

namespace SourceAccess
{
	constexpr std::ptrdiff_t kRenderableOffset  = 0x4;
	constexpr std::ptrdiff_t kNetworkableOffset = 0x8;

	struct ClientClass
	{
		void* m_pCreateFn;          // 0x00
		void* m_pCreateEventFn;     // 0x04
		const char* m_pNetworkName; // 0x08
		void* m_pRecvTable;         // 0x0C
		int m_ClassID;              // 0x10
		ClientClass* m_pNext;       // 0x14
	};

	namespace VTable
	{
		constexpr int GetClientClass = 1;
		constexpr int IsDormant      = 7;
		constexpr int EntIndex       = 8;

		constexpr int SetupBones     = 13;

		constexpr int GetAbsOrigin   = 11;
		constexpr int GetAbsAngles   = 12;
	}

	template <typename Fn>
	inline Fn GetVFunc(void* base, int index)
	{
		if (!base)
			return nullptr;
		auto** vtable = *reinterpret_cast<void***>(base);
		return vtable ? reinterpret_cast<Fn>(vtable[index]) : nullptr;
	}

	inline void* GetRenderable(const void* entity)
	{
		if (!entity)
			return nullptr;
		return *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(entity) + kRenderableOffset);
	}

	inline void* GetNetworkable(const void* entity)
	{
		if (!entity)
			return nullptr;
		return *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(entity) + kNetworkableOffset);
	}

	inline const ClientClass* GetClientClassPtr(const void* entity)
	{
		void* net = GetNetworkable(entity);
		using Fn = ClientClass*(__thiscall*)(void*);
		Fn fn = GetVFunc<Fn>(net, VTable::GetClientClass);
		return fn ? fn(net) : nullptr;
	}

	inline const char* GetNetworkName(const void* entity)
	{
		const ClientClass* cc = GetClientClassPtr(entity);
		return cc ? cc->m_pNetworkName : nullptr;
	}

	inline int GetClassID(const void* entity)
	{
		const ClientClass* cc = GetClientClassPtr(entity);
		return cc ? cc->m_ClassID : -1;
	}

	inline bool IsDormantEntity(const void* entity)
	{
		void* net = GetNetworkable(entity);
		using Fn = bool(__thiscall*)(void*);
		Fn fn = GetVFunc<Fn>(net, VTable::IsDormant);
		return fn ? fn(net) : false;
	}

	inline int EntIndex(const void* entity)
	{
		void* net = GetNetworkable(entity);
		using Fn = int(__thiscall*)(void*);
		Fn fn = GetVFunc<Fn>(net, VTable::EntIndex);
		return fn ? fn(net) : -1;
	}

	inline bool SetupBones(const void* entity, matrix3x4_t* out, int maxBones, int boneMask, float currentTime)
	{
		void* rend = GetRenderable(entity);
		using Fn = bool(__thiscall*)(void*, matrix3x4_t*, int, int, float);
		Fn fn = GetVFunc<Fn>(rend, VTable::SetupBones);
		return fn ? fn(rend, out, maxBones, boneMask, currentTime) : false;
	}

	inline Vector GetAbsOrigin(const void* entity)
	{
		using Fn = const Vector&(__thiscall*)(void*);
		Fn fn = GetVFunc<Fn>(const_cast<void*>(entity), VTable::GetAbsOrigin);
		return fn ? fn(const_cast<void*>(entity)) : Vector{ 0.f, 0.f, 0.f };
	}

	inline QAngle GetAbsAngles(const void* entity)
	{
		using Fn = const QAngle&(__thiscall*)(void*);
		Fn fn = GetVFunc<Fn>(const_cast<void*>(entity), VTable::GetAbsAngles);
		return fn ? fn(const_cast<void*>(entity)) : QAngle{ 0.f, 0.f, 0.f };
	}
}

