// dllmain.cpp : Defines the entry point for the DLL application.
#include <Windows.h>
#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>
#include "game.h"
#include "hooks.h"
#include "vr.h"
#include "sdk.h"

namespace
{
    struct WindowSearchContext
    {
        DWORD processId = 0;
        HWND hwnd = nullptr;
    };

    BOOL CALLBACK FindMainWindowProc(HWND hwnd, LPARAM lParam)
    {
        auto* ctx = reinterpret_cast<WindowSearchContext*>(lParam);
        if (!ctx || !IsWindow(hwnd))
            return TRUE;

        DWORD windowProcessId = 0;
        GetWindowThreadProcessId(hwnd, &windowProcessId);
        if (windowProcessId != ctx->processId)
            return TRUE;

        if (GetWindow(hwnd, GW_OWNER) != nullptr)
            return TRUE;

        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        if ((style & WS_VISIBLE) == 0)
            return TRUE;

        ctx->hwnd = hwnd;
        return FALSE;
    }

    HWND FindCurrentProcessMainWindow()
    {
        WindowSearchContext ctx;
        ctx.processId = GetCurrentProcessId();
        EnumWindows(FindMainWindowProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.hwnd;
    }

    bool ForceWindowForeground(HWND hwnd)
    {
        if (!IsWindow(hwnd))
            return false;

        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        else
            ShowWindow(hwnd, SW_SHOW);

        HWND fgWindow = GetForegroundWindow();
        DWORD fgThreadId = fgWindow ? GetWindowThreadProcessId(fgWindow, nullptr) : 0;
        DWORD curThreadId = GetCurrentThreadId();
        const bool needAttach = (fgThreadId != 0 && fgThreadId != curThreadId);

        if (needAttach)
            AttachThreadInput(fgThreadId, curThreadId, TRUE);

        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);

        if (needAttach)
            AttachThreadInput(fgThreadId, curThreadId, FALSE);

        return GetForegroundWindow() == hwnd;
    }

    DWORD WINAPI FocusGameWindowWorker(LPVOID)
    {
        constexpr int kMaxTries = 30;
        constexpr DWORD kRetryDelayMs = 500;
        constexpr int kSuccessStabilityTries = 3;

        int stableForegroundCount = 0;
        for (int i = 0; i < kMaxTries; ++i)
        {
            HWND window = FindCurrentProcessMainWindow();
            if (window && ForceWindowForeground(window))
            {
                ++stableForegroundCount;
                if (stableForegroundCount >= kSuccessStabilityTries)
                    return 0;
            }
            else
            {
                stableForegroundCount = 0;
            }

            Sleep(kRetryDelayMs);
        }

        return 0;
    }

    bool IsNoHmdLaunchArgPresent()
    {
        int nArgs = 0;
        LPWSTR* szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (!szArglist)
            return false;

        bool noHmdEnabled = false;
        for (int i = 0; i < nArgs; ++i)
        {
            if (_wcsicmp(szArglist[i], L"-nohmd") == 0)
            {
                noHmdEnabled = true;
                break;
            }
        }

        LocalFree(szArglist);
        return noHmdEnabled;
    }

    bool ReplaceConfigValueInLine(std::wstring& line, const wchar_t* key, const wchar_t* expectedValue)
    {
        if (line.find(key) == std::wstring::npos)
            return false;

        const size_t firstQuote = line.find(L'"');
        if (firstQuote == std::wstring::npos)
            return false;

        const size_t secondQuote = line.find(L'"', firstQuote + 1);
        if (secondQuote == std::wstring::npos)
            return false;

        const size_t valueStartQuote = line.find(L'"', secondQuote + 1);
        if (valueStartQuote == std::wstring::npos)
            return false;

        const size_t valueEndQuote = line.find(L'"', valueStartQuote + 1);
        if (valueEndQuote == std::wstring::npos)
            return false;

        const std::wstring currentValue = line.substr(valueStartQuote + 1, valueEndQuote - valueStartQuote - 1);
        if (currentValue == expectedValue)
            return false;

        line.replace(valueStartQuote + 1, valueEndQuote - valueStartQuote - 1, expectedValue);
        return true;
    }

    void EnsureVideoCfgSettings()
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return;

        std::filesystem::path videoCfgPath(exePath);
        videoCfgPath = videoCfgPath.parent_path() / L"left4dead2" / L"cfg" / L"video.txt";
        if (!std::filesystem::exists(videoCfgPath))
            return;

        std::wifstream input(videoCfgPath);
        if (!input.is_open())
            return;

        std::vector<std::wstring> lines;
        lines.reserve(64);

        std::wstring line;
        bool changed = false;
        while (std::getline(input, line))
        {
            changed |= ReplaceConfigValueInLine(line, L"\"setting.mat_antialias\"", L"1");
            changed |= ReplaceConfigValueInLine(line, L"\"setting.mat_vsync\"", L"0");
            lines.push_back(line);
        }
        input.close();

        if (!changed)
            return;

        std::wofstream output(videoCfgPath, std::ios::trunc);
        if (!output.is_open())
            return;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size())
                output << L'\n';
        }
        output.close();

    }
}

DWORD WINAPI InitL4D2VR(HMODULE hModule)
{
#ifdef _DEBUG
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
#endif

    // Make sure -insecure is used
    LPWSTR* szArglist;
    int nArgs;
    szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    bool insecureEnabled = false;
    for (int i = 0; i < nArgs; ++i)
    {
        if (wcscmp(szArglist[i], L"-insecure") == 0)
            insecureEnabled = true;
    }
    LocalFree(szArglist);

    if (IsNoHmdLaunchArgPresent())
        return 0;

    CreateThread(nullptr, 0, FocusGameWindowWorker, nullptr, 0, nullptr);

    EnsureVideoCfgSettings();

    g_Game = new Game();

    return 0;
}



BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitL4D2VR, hModule, 0, NULL);
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
