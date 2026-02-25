#pragma once
#include <Windows.h>
#include <psapi.h>
#include <vector>
#include <sstream>
#include <limits>

class SigScanner
{
public:
	// Returns 0 if current offset matches, -1 if no matches found.
	// A value > 0 is the new offset.
	static int VerifyOffset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0)
	{
		HMODULE hModule = GetModuleHandle(moduleName.c_str());
		if (!hModule)
			return -1;

		MODULEINFO moduleInfo;
		GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo));

		uint8_t* bytes = (uint8_t*)moduleInfo.lpBaseOfDll;

		std::vector<int> pattern;
		std::stringstream ss(signature);
		std::string sigByte;
		while (ss >> sigByte)
		{
			if (sigByte == "?" || sigByte == "??")
				pattern.push_back(-1);
			else
				pattern.push_back(strtoul(sigByte.c_str(), NULL, 16));
		}

		const int patternLen = (int)pattern.size();

		// Check if current offset is good when a known offset exists.
		if (currentOffset > 0)
		{
			bool offsetMatchesSig = true;
			for (int i = 0; i < patternLen; ++i)
			{
				if ((bytes[currentOffset - sigOffset + i] != pattern[i]) && (pattern[i] != -1))
				{
					offsetMatchesSig = false;
					break;
				}
			}

			if (offsetMatchesSig)
				return 0;
		}

		// Scan the dll for new offset.
		//
		// IMPORTANT: Many of our signatures are "short" (prologue-only) and can match multiple sites.
		// If we have a previous known offset, pick the closest match to reduce false positives.
		int bestOffset = -1;
		int bestDist = std::numeric_limits<int>::max();

		for (int i = 0; i <= (int)moduleInfo.SizeOfImage - patternLen; ++i)
		{
			bool found = true;
			for (int j = 0; j < patternLen; ++j)
			{
				if ((bytes[i + j] != pattern[j]) && (pattern[j] != -1))
				{
					found = false;
					break;
				}
			}
			if (!found)
				continue;

			const int candidate = i + sigOffset;

			// No prior offset: keep old behavior (first match).
			if (currentOffset <= 0)
				return candidate;

			int dist = candidate - currentOffset;
			if (dist < 0)
				dist = -dist;

			if (dist < bestDist)
			{
				bestDist = dist;
				bestOffset = candidate;
			}
		}

		return bestOffset;
	}
};
