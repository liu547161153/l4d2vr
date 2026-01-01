// main.cpp
// L4D2VR Config Tool
// Reads/writes <exe>/vr/config.txt directly

#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <filesystem>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#include "Options.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"


std::unordered_map<std::string, std::string> g_Values;
bool g_UseChinese = false;
// ============================================================
// Win32 + DX11 boilerplate (ImGui official example, simplified)
// ============================================================

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRTV = nullptr;

void CreateRenderTarget()
{
    ID3D11Texture2D* backBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_pd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRTV);
    backBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
}

bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fls[1] = { D3D_FEATURE_LEVEL_11_0 };

    if (D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        fls, 1, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext) != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    if (msg == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ============================================================
// config.txt handling
// ============================================================

struct ConfigLine
{
    std::string raw;   // ԭʼ
    std::string key;   //  Key=Value key
};

static std::vector<ConfigLine> g_Lines;
// ݾ
static std::unordered_map<std::string, std::string> g_Aliases = {
    { "AimLinePersistence", "AimLineFrameDurationMultiplier" }
};

std::string GetExeDir()
{
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    size_t pos = s.find_last_of("\\/");
    return (pos == std::string::npos) ? "" : s.substr(0, pos);
}

std::string GetConfigPath()
{
    return GetExeDir() + "\\vr\\config.txt";
}

static bool IsChineseUILanguage()
{
    auto is_zh = [](LANGID lang) {
        return PRIMARYLANGID(lang) == LANG_CHINESE;
    };

    LANGID userLang = GetUserDefaultUILanguage();
    if (is_zh(userLang)) return true;
    LANGID sysLang = GetSystemDefaultUILanguage();
    if (is_zh(sysLang)) return true;
    return false;
}

void LoadConfig(const std::string& path)
{
    g_Lines.clear();
    g_Values.clear();

    std::ifstream in(path);
    if (!in.good())
        return;

    std::string line;
    while (std::getline(in, line))
    {
        ConfigLine cl;
        cl.raw = line;

        std::string t = line;
        if (!t.empty() && t[0] != '#' && t[0] != '/' && t.find('=') != std::string::npos)
        {
            auto eq = t.find('=');
            cl.key = t.substr(0, eq);
            std::string val = t.substr(eq + 1);

            // trim
            while (!cl.key.empty() && isspace(cl.key.back())) cl.key.pop_back();
            while (!val.empty() && isspace(val.front())) val.erase(val.begin());

            g_Values[cl.key] = val;

            // alias support
            if (g_Aliases.count(cl.key))
                g_Values[g_Aliases[cl.key]] = val;
        }

        g_Lines.push_back(cl);
    }
}

void SaveConfig(const std::string& path)
{
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    // 滻һγֵ key
    for (auto& [key, val] : g_Values)
    {
        bool written = false;
        for (int i = (int)g_Lines.size() - 1; i >= 0; --i)
        {
            if (g_Lines[i].key == key)
            {
                g_Lines[i].raw = key + "=" + val;
                written = true;
                break;
            }
        }
        if (!written)
        {
            g_Lines.push_back({ key + "=" + val, key });
        }
    }

    std::ofstream out(path, std::ios::trunc);
    for (auto& l : g_Lines)
        out << l.raw << "\n";
}

// ============================================================
// Main
// ============================================================

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    std::string configPath = GetConfigPath();
    LoadConfig(configPath);

    // Decide UI language before creating any windows/UI
    g_UseChinese = IsChineseUILanguage();

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
        _T("L4D2VRConfigTool"), nullptr };
    RegisterClassEx(&wc);

    const TCHAR* windowTitle = g_UseChinese ? _T("L4D2VR 配置工具") : _T("L4D2VR Config Tool");

    HWND hwnd = CreateWindow(wc.lpszClassName, windowTitle,
        WS_OVERLAPPEDWINDOW, 100, 100, 700, 500,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
        return 1;

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Load a Chinese-capable font from Windows (no bundled font files).
    // Note: do NOT hardcode "C:\Windows" (some systems aren't on C:), and do NOT use unescaped backslashes.
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = nullptr;

    auto WideToUtf8 = [](const std::wstring& w) -> std::string {
        if (w.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return {};
        std::string s(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
        return s;
    };

    wchar_t winDir[MAX_PATH] = { 0 };
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring fontsDir = std::wstring(winDir) + L"\\Fonts\\";

    auto TryFont = [&](const wchar_t* fileName) -> ImFont* {
        std::wstring wpath = fontsDir + fileName;
        if (GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES)
            return nullptr;
        std::string path = WideToUtf8(wpath);
        return io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    };

    // Prefer Microsoft YaHei when showing Chinese. Always ensure we have Chinese glyphs even in English UI.
    if (g_UseChinese)
    {
        font = TryFont(L"msyh.ttc");
        if (!font) font = TryFont(L"msyhbd.ttc");
        if (!font) font = TryFont(L"simhei.ttf");
        if (!font) font = TryFont(L"simsun.ttc");
        if (!font) font = io.Fonts->AddFontDefault();
    }
    else
    {
        font = io.Fonts->AddFontDefault(); // English UI

        // Merge a Chinese font so Chinese option text/tooltips render correctly
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        cfg.GlyphMinAdvanceX = 12.0f;
        auto merge = [&](const wchar_t* name) {
            std::wstring wpath = fontsDir + name;
            if (GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES)
                return false;
            std::string path = WideToUtf8(wpath);
            return io.Fonts->AddFontFromFileTTF(path.c_str(), 16.0f, &cfg, io.Fonts->GetGlyphRangesChineseFull()) != nullptr;
        };
        if (!merge(L"msyh.ttc"))
            if (!merge(L"msyhbd.ttc"))
                if (!merge(L"simhei.ttf"))
                    merge(L"simsun.ttc");

        // Ensure we still have a font
        if (!font) font = io.Fonts->AddFontDefault();
    }

    io.FontDefault = font;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    MSG msg{};
    bool running = true;

    while (running)
    {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        auto L = [](const char* en, const char* zh) { return (g_UseChinese && zh && *zh) ? zh : en; };

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);

        ImGuiWindowFlags winFlags =
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoTitleBar;

        const char* uiTitle = L("L4D2VR Config Tool", u8"L4D2VR 配置工具");
        ImGui::Begin(uiTitle, nullptr, winFlags);

        // Header
        ImGui::Text("%s %s", L("Config:", u8"配置:"), configPath.c_str());
        ImGui::SameLine();
        if (ImGui::Button(L("Save", u8"保存")))
            SaveConfig(configPath);

        // Search
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputTextWithHint("##OptionSearch", L("Search", u8"搜索"), g_OptionSearch, sizeof(g_OptionSearch));

        ImGui::Separator();

        // Scrollable options area
        ImGui::BeginChild("##OptionsScroll", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        DrawOptionsUI();
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        const float clear[4] = { 0.1f,0.1f,0.1f,1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}
