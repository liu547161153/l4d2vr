#pragma once

#include <cstdint>

// Minimal Source engine ICvar/ConVar interfaces for querying client ConVars.

using CreateInterfaceFn = void* (*)(const char* name, int* returnCode);

enum InitReturnVal_t
{
	INIT_FAILED = 0,
	INIT_OK,
};

struct AppSystemInfo_t
{
	const char* pSystemName;
	const char* pInterfaceName;
};

enum AppSystemTier_t
{
	APP_SYSTEM_TIER0 = 0,
	APP_SYSTEM_TIER1,
	APP_SYSTEM_TIER2,
	APP_SYSTEM_TIER3,
};

class IAppSystem
{
public:
	virtual bool Connect(CreateInterfaceFn factory) = 0;
	virtual void Disconnect() = 0;
	virtual void* QueryInterface(const char* pInterfaceName) = 0;
	virtual InitReturnVal_t Init() = 0;
	virtual void Shutdown() = 0;
	virtual const AppSystemInfo_t* GetDependencies() = 0;
	virtual AppSystemTier_t GetTier() = 0;
	virtual void Reconnect(CreateInterfaceFn factory, const char* pInterfaceName) = 0;
	virtual bool IsSingleton() = 0;
};

struct Color
{
	uint8_t r, g, b, a;
};

class ConCommandBase;

class IConVar
{
public:
	virtual void SetValue(const char* pValue) = 0;
	virtual void SetValue(float flValue) = 0;
	virtual void SetValue(int nValue) = 0;
	virtual void SetValue(Color value) = 0;
};

class ConVar : public IConVar
{
public:
	virtual bool IsFlagSet(int flag) const = 0;
	virtual const char* GetHelpText(void) const = 0;
	virtual bool IsRegistered(void) const = 0;
	virtual const char* GetName(void) const = 0;
	virtual const char* GetBaseName(void) const = 0;
	virtual int GetSplitScreenPlayerSlot() const = 0;
	virtual void AddFlags(int flags) = 0;
	virtual int GetFlags() const = 0;
	virtual bool IsCommand(void) const = 0;
	virtual void SetValue(const char* value) = 0;
	virtual void SetValue(float value) = 0;
	virtual void SetValue(int value) = 0;
	virtual void SetValue(Color value) = 0;
	virtual void InternalSetValue(const char* value) = 0;
	virtual void InternalSetFloatValue(float fNewValue) = 0;
	virtual void InternalSetIntValue(int nValue) = 0;
	virtual void ChangeStringValue(const char* tempVal, float flOldValue) = 0;
	virtual void Create(const char* pszName, const char* pszHelpString = 0, int flags = 0, const char* pszDefaultValue = 0, bool bHasMin = false, float fMinVal = 0.0, bool bHasMax = false, float fMaxVal = 0.0, void* callback = 0) = 0;
	virtual void Init() = 0;
	virtual const char* GetDefault(void) const = 0;
	virtual void SetDefault(const char* pszDefault) = 0;
	virtual float GetFloat(void) const = 0;
	virtual int GetInt(void) const = 0;
	virtual bool GetBool(void) const = 0;
	virtual const char* GetString(void) const = 0;
};

class IConsoleDisplayFunc;

class ICvar : public IAppSystem
{
public:
	using CVarDLLIdentifier_t = int;

	virtual CVarDLLIdentifier_t AllocateDLLIdentifier() = 0;
	virtual void RegisterConCommand(ConCommandBase* pCommandBase) = 0;
	virtual void UnregisterConCommand(ConCommandBase* pCommandBase) = 0;
	virtual void UnregisterConCommands(CVarDLLIdentifier_t id) = 0;
	virtual const char* GetCommandLineValue(const char* pVariableName) = 0;
	virtual ConCommandBase* FindCommandBase(const char* name) = 0;
	virtual const ConCommandBase* FindCommandBase(const char* name) const = 0;
	virtual ConVar* FindVar(const char* var_name) = 0;
	virtual const ConVar* FindVar(const char* var_name) const = 0;
	virtual void CallGlobalChangeCallbacks(ConVar* var, const char* pOldString, float flOldValue) = 0;
	virtual void InstallConsoleDisplayFunc(IConsoleDisplayFunc* pDisplayFunc) = 0;
	virtual void RemoveConsoleDisplayFunc(IConsoleDisplayFunc* pDisplayFunc) = 0;
	virtual void ConsoleColorPrintf(const Color& clr, const char* pFormat, ...) const = 0;
	virtual void ConsolePrintf(const char* pFormat, ...) const = 0;
	virtual void ConsoleDPrintf(const char* pFormat, ...) const = 0;
	virtual void RevertFlaggedConVars(int nFlag) = 0;
	virtual void InstallGlobalChangeCallback(void (*callback)(ConVar*, const char*, float)) = 0;
	virtual void RemoveGlobalChangeCallback(void (*callback)(ConVar*, const char*, float)) = 0;
	virtual void CallGlobalChangeCallbacks(ConVar* var, const char* pOldString, float flOldValue, bool invokeCallback) = 0;
	virtual void InstallConsoleCommandBase(ConCommandBase* pCommandBase) = 0;
	virtual void AssignConCommandBase(CVarDLLIdentifier_t id, ConCommandBase* pCommandBase) = 0;
};
