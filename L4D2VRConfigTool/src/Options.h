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
    Color,
    String,
    Vec3
};

struct L10nText
{
    const char* en;     // English text
    const char* zh;     // Simplified Chinese text
};

struct Option
{
    const char* key;        // config.txt key
    OptionType type;

    L10nText group;         // UI group
    L10nText title;         // UI title
    L10nText desc;          // description
    L10nText tip;           // tip / recommendation

    float min = 0.f;
    float max = 0.f;
    const char* defaultValue = "";
};

// Option table
extern Option g_Options[];
extern const int g_OptionCount;

// Global config values (defined in main.cpp)
extern std::unordered_map<std::string, std::string> g_Values;

// Whether UI should show Simplified Chinese, defined in main.cpp
extern bool g_UseChinese;

// Search text
extern char g_OptionSearch[128];

// UI
void DrawOptionsUI();
