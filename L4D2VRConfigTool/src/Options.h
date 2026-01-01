#pragma once
#include <string>
#include <unordered_map>
#include "imgui.h"

// =============================
// Option definitions
// =============================

enum class OptionType
{
    Bool,
    Float,
    Int,
    Color
};

struct Option
{
    const char* key;        // config.txt key
    OptionType type;

    const char* group;      // UI group
    const char* title;      // UI title
    const char* desc;       // description
    const char* tip;        // tip / recommendation

    float min = 0.f;
    float max = 0.f;
};

// Option table
extern Option g_Options[];
extern const int g_OptionCount;

// Global config values (defined in main.cpp)
extern std::unordered_map<std::string, std::string> g_Values;

// Search text
extern char g_OptionSearch[128];

// UI
void DrawOptionsUI();
