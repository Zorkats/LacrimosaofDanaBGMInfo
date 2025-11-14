#define NOMINMAX
#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
#include <DDSTextureLoader.h>
#include <DirectXTex.h>
#pragma comment(lib, "dxguid.lib")

#include <MinHook.h>

#include <map>
#include <string>
#include <fstream>
#include <chrono>
#include <ctime>
#include <sstream>
#include <thread>
#include <algorithm>
#include <queue>
#include <mutex>
#include <atomic>

#include <yaml-cpp/yaml.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

// =============================================================
// LOGGING HELPER
// =============================================================
void Log(const std::string& message) {
    std::ofstream log_file("mod_log.txt", std::ios_base::app | std::ios_base::out);
    auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char time_str[26];
    ctime_s(time_str, sizeof(time_str), &time);
    time_str[24] = '\0';
    log_file << "[" << time_str << "] " << message << std::endl;
}

void WCharToString(const WCHAR* wstr, char* buffer, size_t bufferSize) {
    if (!wstr || !buffer) return;
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, buffer, (int)bufferSize, NULL, NULL);
}

// =============================================================
// GLOBAL VARIABLES
// =============================================================
struct BgmInfo {
    std::string songName;
    std::string disc;
    std::string track;
    std::string rawFileName; // Added to track filename for position logic
};

static std::map<std::string, BgmInfo> g_bgmMap;
static BgmInfo g_currentBgmInfo;
static std::map<std::string, std::chrono::steady_clock::time_point> g_songLastShown;
static std::string g_lastTriggeredFile = "";

// MODIFIED: Switched to float timer for seconds
static float g_toastTimer = 0.0f;
constexpr float TOAST_DURATION_SECONDS = 5.0f; // 8 seconds, independent of FPS
constexpr int COOLDOWN_HOURS = 5;

// MODIFIED: Changed reset value to be far offscreen for logic
static float g_toastCurrentX = -10000.0f;

// Graphics / ImGui Globals
static bool g_imguiInitialized = false;
static HWND g_hWindow = nullptr;
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static ID3D11RenderTargetView* g_pd3dRenderTargetView = nullptr;
static ImFont* g_pToastFont = nullptr;
static ID3D11ShaderResourceView* g_pToastTexture = nullptr;
static ID3D11Resource* g_pToastResource = nullptr;
static float g_TextureWidth = 0.0f;
static float g_TextureHeight = 0.0f;

// Threading Globals (Producer-Consumer)
static std::thread g_workerThread;
static std::mutex g_bufferMutex;
static char g_bgmFilenameBuffer[MAX_PATH];
static std::atomic<bool> g_bNewBgmAvailable = false;
static std::atomic<bool> g_bWorkerThreadActive = true;

// =============================================================
// HELPER FUNCTIONS
// =============================================================
std::string GetModDirectory()
{
    char path[MAX_PATH];
    HMODULE hModule = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&GetModDirectory, &hModule);

    GetModuleFileNameA(hModule, path, MAX_PATH);
    PathRemoveFileSpecA(path);
    return std::string(path);
}

void LoadBgmMap()
{
    std::string modDir = GetModDirectory();
    std::string yamlPath = modDir + "\\assets/BgmMap.yaml";

    try
    {
        std::ifstream file(yamlPath);
        if (!file.is_open())
        {
            Log("LoadBgmMap: BgmMap.yaml not found.");
            return;
        }

        YAML::Node config = YAML::Load(file);
        for (const auto& node : config)
        {
            std::string filepath = node.first.as<std::string>();
            std::string value = node.second.as<std::string>();

            // MODIFIED: Parse "Song Name|Disc|Track"
            BgmInfo info;
            info.rawFileName = filepath; // Store the key

            size_t pos1 = value.find('|');
            size_t pos2 = value.find('|', pos1 + 1);

            if (pos1 != std::string::npos && pos2 != std::string::npos)
            {
                info.songName = value.substr(0, pos1);
                info.disc = value.substr(pos1 + 1, pos2 - pos1 - 1);
                info.track = value.substr(pos2 + 1);
            }
            else
            {
                // Fallback if format is wrong
                info.songName = value;
                info.disc = "";
                info.track = "";
            }

            g_bgmMap[filepath] = info;
        }
        Log("LoadBgmMap: Map loaded successfully with " + std::to_string(g_bgmMap.size()) + " entries.");
    }
    catch (const YAML::Exception& e)
    {
        Log("LoadBgmMap: YAML parsing error: " + std::string(e.what()));
    }
}

// =============================================================
// GRAPHICS HOOKS (DirectX 11 & ImGui)
// =============================================================
typedef HRESULT(WINAPI* PFN_PRESENT)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
static PFN_PRESENT g_pfnOriginalPresent = nullptr;

LRESULT WINAPI WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static WNDPROC g_pfnOriginalWndProc = NULL;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
        return TRUE;

    return CallWindowProc(g_pfnOriginalWndProc, hWnd, uMsg, wParam, lParam);
}

void InitImGui(IDXGISwapChain* pSwapChain)
{
      
    if (g_imguiInitialized)
        return;

    Log("InitImGui called.");

    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_pd3dDevice)))
    {
        g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);

        g_pfnOriginalWndProc = (WNDPROC)SetWindowLongPtr(g_hWindow, GWLP_WNDPROC, (LONG_PTR)WndProc);

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = NULL;

        std::string fontPathStr = GetModDirectory() + "\\assets/mod_font.otf";
        g_pToastFont = io.Fonts->AddFontFromFileTTF(fontPathStr.c_str(), 28.0f);

        if (g_pToastFont == nullptr) {
            Log("Failed to load mod_font.otf from file!");
        } else {
            Log("mod_font.otf loaded successfully from file.");
        }

        std::string texturePathStr = GetModDirectory() + "\\assets/bgm_info.dds";
        std::wstring texturePathW(texturePathStr.begin(), texturePathStr.end());

        HRESULT hr = DirectX::CreateDDSTextureFromFile(
            g_pd3dDevice,
            texturePathW.c_str(),
            &g_pToastResource,
            &g_pToastTexture
        );

        if (SUCCEEDED(hr) && g_pToastResource)
        {
            Log("bgm_info.dds loaded successfully from file.");

            ID3D11Texture2D* pTexture = nullptr;
            if (SUCCEEDED(g_pToastResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTexture)))
            {
                D3D11_TEXTURE2D_DESC desc;
                pTexture->GetDesc(&desc);
                g_TextureWidth = (float)desc.Width;
                g_TextureHeight = (float)desc.Height;
                pTexture->Release();
                Log("Texture size found: " + std::to_string(g_TextureWidth) + "x" + std::to_string(g_TextureHeight));
            }
        }
        else
        {
            Log("Failed to load bgm_info.dds from file! HR: " + std::to_string(hr));
        }

        ImGui_ImplWin32_Init(g_hWindow);
        ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

        Log("ImGui initialized successfully.");
        g_imguiInitialized = true;
    }
    else
    {
        Log("InitImGui FAILED to get D3D11 Device!");
    }
}

HRESULT WINAPI My_Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    // ... (Init/Get Target Dimensions logic remains unchanged) ...
    static bool s_bFirstTime = true;
    if (s_bFirstTime) {
        Log("My_Present hook has been called!");
        s_bFirstTime = false;
    }

    InitImGui(pSwapChain);

    // Get render target dimensions
    ID3D11Texture2D* pBackBuffer = nullptr;
    float actual_width = 0.0f;
    float actual_height = 0.0f;

    if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer)))
    {
        D3D11_TEXTURE2D_DESC desc;
        pBackBuffer->GetDesc(&desc);
        actual_width = (float)desc.Width;
        actual_height = (float)desc.Height;
        pBackBuffer->Release();
    }

    ImGui_ImplWin32_NewFrame();

    // Override DisplaySize with actual render target size
    ImGuiIO& io = ImGui::GetIO();
    if (actual_width > 0.0f && actual_height > 0.0f) {
        io.DisplaySize.x = actual_width;
        io.DisplaySize.y = actual_height;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    // UI Configuration
    const float UI_SCALE = 0.65f;
    const float SCREEN_PADDING = 10.0f;
    const float TEXT_PADDING_X = 20.0f * UI_SCALE;
    const float TEXT_PADDING_Y = 15.0f * UI_SCALE;
    const float ROUNDING = 8.0f * UI_SCALE;

    // MODIFIED: Cleaned up text logic
    float text_width = 0.0f;
    float text_height = 0.0f;
    float line_height = 28.0f;

    // Prepare disc/track string
    std::string line2 = "";
    if (!g_currentBgmInfo.disc.empty() || !g_currentBgmInfo.track.empty()) {
        line2 = "Disc " + g_currentBgmInfo.disc + ", Track " + g_currentBgmInfo.track;
    }

    if (g_toastTimer > 0.0f || g_toastCurrentX != -10000.0f)
    {
        if (g_pToastFont) ImGui::PushFont(g_pToastFont);

        ImVec2 size_line1 = ImGui::CalcTextSize(g_currentBgmInfo.songName.c_str());
        ImVec2 size_line2 = ImGui::CalcTextSize(line2.c_str());

        text_width = std::max(size_line1.x, size_line2.x);
        line_height = size_line1.y;

        // Use 2 lines of height, plus a bit of padding
        text_height = line_height * 2.2f;

        if (g_pToastFont) ImGui::PopFont();
    }

    // UI layout calculations
    float total_height = text_height + (TEXT_PADDING_Y * 2.0f);
    float note_icon_width = total_height; // Make icon square
    float box_width = text_width + (TEXT_PADDING_X * 2.0f);
    float total_width = note_icon_width + box_width;

    float screen_width = io.DisplaySize.x;
    float top_y = SCREEN_PADDING;

    // MODIFIED: Logic for Top-Left vs Top-Right
    float target_onscreen_x = 0.0f;
    float target_offscreen_x = 0.0f;

    // Check if filename contains "y8_title"
    bool isTitleScreen = (!g_currentBgmInfo.rawFileName.empty() &&
                          g_currentBgmInfo.rawFileName.find("y8_title") != std::string::npos);

    if (isTitleScreen) {
        // LEFT SIDE
        target_onscreen_x = SCREEN_PADDING;
        target_offscreen_x = -total_width - SCREEN_PADDING; // Hide to the left
    } else {
        // RIGHT SIDE (Default)
        target_onscreen_x = screen_width - total_width - SCREEN_PADDING;
        target_offscreen_x = screen_width + SCREEN_PADDING; // Hide to the right
    }

    // Initialize position if reset
    if (g_toastCurrentX == -10000.0f) {
        g_toastCurrentX = target_offscreen_x;
    }

    // MODIFIED: Animation logic
    const float ANIMATION_SPEED = 1500.0f; // Pixels per second
    float delta_time = io.DeltaTime; // Seconds since last frame

    if (g_toastTimer > 0.0f)
    {
        // Sliding ON
        if (isTitleScreen) {
            // Move RIGHT to slide in
            if (g_toastCurrentX < target_onscreen_x) {
                g_toastCurrentX += ANIMATION_SPEED * delta_time;
                if (g_toastCurrentX > target_onscreen_x) g_toastCurrentX = target_onscreen_x;
            }
        } else {
            // Move LEFT to slide in
            if (g_toastCurrentX > target_onscreen_x) {
                g_toastCurrentX -= ANIMATION_SPEED * delta_time;
                if (g_toastCurrentX < target_onscreen_x) g_toastCurrentX = target_onscreen_x;
            }
        }
        g_toastTimer -= delta_time; // FPS-independent timer
    }
    else
    {
        // Sliding OFF
        if (isTitleScreen) {
            // Move LEFT to slide out
            if (g_toastCurrentX > target_offscreen_x) {
                g_toastCurrentX -= ANIMATION_SPEED * delta_time;
                if (g_toastCurrentX < target_offscreen_x) g_toastCurrentX = target_offscreen_x;
            }
        } else {
            // Move RIGHT to slide out
            if (g_toastCurrentX < target_offscreen_x) {
                g_toastCurrentX += ANIMATION_SPEED * delta_time;
                if (g_toastCurrentX > target_offscreen_x) g_toastCurrentX = target_offscreen_x;
            }
        }

        // Check if fully offscreen to reset
        bool offscreen = false;
        if (isTitleScreen && g_toastCurrentX <= target_offscreen_x) offscreen = true;
        if (!isTitleScreen && g_toastCurrentX >= target_offscreen_x) offscreen = true;

        if (offscreen) {
            g_toastCurrentX = -10000.0f; // Reset state
        }
    }

    // Draw UI
    if (g_toastCurrentX != -10000.0f)
    {
        ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

        ImVec2 pos_note_start(g_toastCurrentX, top_y);
        ImVec2 pos_note_end(g_toastCurrentX + note_icon_width, top_y + total_height);
        ImVec2 pos_box_start(pos_note_end.x, top_y);
        ImVec2 pos_box_end(pos_box_start.x + box_width, top_y + total_height);

        // Vertically center the text block
        float text_block_height = line_height * (line2.empty() ? 1.0f : 2.0f);
        float text_start_y = top_y + (total_height - text_block_height) * 0.5f;

        if (g_pToastTexture) {
            draw_list->AddImage(
                (void*)g_pToastTexture,
                pos_note_start,
                pos_note_end,
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f)
            );
        }

        draw_list->AddRectFilled(
            pos_box_start,
            pos_box_end,
            IM_COL32(0, 0, 0, 100),
            ROUNDING
        );

        if (g_pToastFont) ImGui::PushFont(g_pToastFont);

        // Line 1: Song Name
        ImVec2 pos_line1(pos_box_start.x + TEXT_PADDING_X, text_start_y);
        draw_list->AddText(pos_line1, IM_COL32_WHITE, g_currentBgmInfo.songName.c_str());

        // Line 2: Disc/Track
        if (!line2.empty()) {
            ImVec2 pos_line2(pos_box_start.x + TEXT_PADDING_X, text_start_y + line_height);
            draw_list->AddText(pos_line2, IM_COL32(180, 180, 180, 255), line2.c_str());
        }

        if (g_pToastFont) ImGui::PopFont();
    }

    ImGui::Render();

    // ... (Render Target logic remains unchanged) ...
    if (!g_pd3dRenderTargetView)
    {
        ID3D11Texture2D* pBackBuffer;
        if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer)))
        {
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_pd3dRenderTargetView);
            pBackBuffer->Release();
        }
    }

    if (g_pd3dRenderTargetView)
    {
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pd3dRenderTargetView, NULL);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pd3dRenderTargetView->Release();
        g_pd3dRenderTargetView = nullptr;
    }

    return g_pfnOriginalPresent(pSwapChain, SyncInterval, Flags);
}

// =============================================================
// FILE SYSTEM HOOK LOGIC (Kernel32::CreateFile A & W)
// =============================================================

// --- Hook for CreateFileW (Unicode) ---
  
typedef HANDLE(WINAPI* PFN_CREATEFILEW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static PFN_CREATEFILEW g_pfnOriginalCreateFileW = nullptr;

HANDLE WINAPI Detour_CreateFileW(LPCWSTR lpFileName, DWORD dwAccess, DWORD dwShare, LPSECURITY_ATTRIBUTES lpSec, DWORD dwDisp, DWORD dwFlags, HANDLE hTemplate) {
    if (lpFileName) {
        size_t len = wcslen(lpFileName);
        if (len > 4) {
            if (towlower(lpFileName[len - 1]) == 'g' &&
                towlower(lpFileName[len - 2]) == 'g' &&
                towlower(lpFileName[len - 3]) == 'o' &&
                lpFileName[len - 4] == L'.')
            {
                if (g_bufferMutex.try_lock()) {
                    if (!g_bNewBgmAvailable) {
                        WCharToString(lpFileName, g_bgmFilenameBuffer, MAX_PATH);
                        g_bNewBgmAvailable = true;
                    }
                    g_bufferMutex.unlock();
                }
            }
        }
    }
    return g_pfnOriginalCreateFileW(lpFileName, dwAccess, dwShare, lpSec, dwDisp, dwFlags, hTemplate);
}

// --- Hook for CreateFileA (ANSI) ---
  
typedef HANDLE(WINAPI* PFN_CREATEFILEA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static PFN_CREATEFILEA g_pfnOriginalCreateFileA = nullptr;

HANDLE WINAPI Detour_CreateFileA(LPCSTR lpFileName, DWORD dwAccess, DWORD dwShare, LPSECURITY_ATTRIBUTES lpSec, DWORD dwDisp, DWORD dwFlags, HANDLE hTemplate) {
    if (lpFileName) {
        std::string fname = lpFileName;
        if (fname.length() > 4) {
            std::string ext = fname.substr(fname.length() - 4);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (ext == ".ogg") {
                Log("Detour_CreateFileA caught: " + fname);

                if (g_bufferMutex.try_lock()) {
                    if (!g_bNewBgmAvailable) {
                        strcpy_s(g_bgmFilenameBuffer, MAX_PATH, lpFileName);
                        g_bNewBgmAvailable = true;
                    }
                    g_bufferMutex.unlock();
                }
            }
        }
    }
    return g_pfnOriginalCreateFileA(lpFileName, dwAccess, dwShare, lpSec, dwDisp, dwFlags, hTemplate);
}

void ProcessBgmTrigger(const std::string& s_filename)
{
    if (s_filename == g_lastTriggeredFile) return;
    g_lastTriggeredFile = s_filename;

    Log("Processing Audio File: " + s_filename);

    std::string normalizedInput = s_filename;
    std::replace(normalizedInput.begin(), normalizedInput.end(), '/', '\\');

    for (auto& entry : g_bgmMap)
    {
        std::string key = entry.first;
        std::replace(key.begin(), key.end(), '/', '\\');

        if (normalizedInput.length() >= key.length()) {
            if (normalizedInput.compare(normalizedInput.length() - key.length(), key.length(), key) == 0) {

                Log("MATCH FOUND for: " + key);

                // MODIFIED: Store the matched key (filename) for position logic
                g_currentBgmInfo = entry.second;
                g_currentBgmInfo.rawFileName = entry.first;

                std::string songKey = g_currentBgmInfo.songName;
                bool shouldShow = false;
                auto it = g_songLastShown.find(songKey);

                if (it == g_songLastShown.end()) {
                    shouldShow = true;
                } else {
                    auto now = std::chrono::steady_clock::now();
                    auto hours = std::chrono::duration_cast<std::chrono::hours>(now - it->second).count();
                    if (hours >= COOLDOWN_HOURS) shouldShow = true;
                }

                if (shouldShow) {
                    // MODIFIED: Use new timer variables
                    g_toastTimer = TOAST_DURATION_SECONDS;
                    g_songLastShown[songKey] = std::chrono::steady_clock::now();
                    g_toastCurrentX = -10000.0f; // Reset animation state
                }
                break;
            }
        }
    }
}

void BgmWorkerThread()
{
      
    Log("BGM Worker Thread started.");
    while (g_bWorkerThreadActive)
    {
        if (g_bNewBgmAvailable == true)
        {
            std::string filename_to_process;

            {
                std::lock_guard<std::mutex> lock(g_bufferMutex);
                filename_to_process = g_bgmFilenameBuffer;
                g_bNewBgmAvailable = false;
            }

            ProcessBgmTrigger(filename_to_process);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Log("BGM Worker Thread shutting down.");
}

// =============================================================
// HOOK INITIALIZATION
// =============================================================
uintptr_t FindPresentAddress(HWND hWnd)
{
      
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* pDevice = nullptr;
    IDXGISwapChain* pSwapChain = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &pSwapChain, &pDevice, NULL, NULL);

    uintptr_t pPresentAddr = 0;
    if (SUCCEEDED(hr))
    {
        void** pVTable = *(void***)pSwapChain;
        pPresentAddr = (uintptr_t)pVTable[8];

        pSwapChain->Release();
        pDevice->Release();
    }
    else
    {
        std::stringstream ss;
        ss << "D3D11CreateDeviceAndSwapChain failed! HRESULT: 0x" << std::hex << hr;
        Log(ss.str());
    }

    return pPresentAddr;
}

void InitializeHooks()
{
      
    Log("Hook thread started (Ys VIII Mode).");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Log("CoInitializeEx failed!");
    }

    // 1. FIND YS VIII WINDOW
    while (g_hWindow == NULL)
    {
        g_hWindow = FindWindowA(NULL, "Ys VIII: Lacrimosa of DANA");

        if (g_hWindow == NULL) {
             EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                WCHAR title[256];
                GetWindowTextW(hwnd, title, 256);
                if (wcsstr(title, L"Lacrimosa of DANA") != NULL || wcsstr(title, L"Ys VIII") != NULL) {
                    g_hWindow = hwnd;
                    return FALSE;
                }
                return TRUE;
            }, NULL);
        }

        Log("Searching for Ys VIII window...");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    Log("Ys VIII Window found!");

    if (MH_Initialize() != MH_OK) {
        Log("MH_Initialize failed!");
        return;
    }
    Log("MH_Initialize successful.");

    LoadBgmMap();

    g_bWorkerThreadActive = true;
    g_workerThread = std::thread(BgmWorkerThread);

    // 2. HOOK FILE SYSTEM (CreateFileW)
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        // 1. Hook CreateFileW (Unicode)
        void* pCreateFileW = (void*)GetProcAddress(hKernel32, "CreateFileW");
        if (pCreateFileW) {
            MH_CreateHook(pCreateFileW, &Detour_CreateFileW, (LPVOID*)&g_pfnOriginalCreateFileW);
            Log("Hooked CreateFileW");
        }

        // 2. Hook CreateFileA (ANSI) - THIS IS LIKELY THE ONE WE NEED
        void* pCreateFileA = (void*)GetProcAddress(hKernel32, "CreateFileA");
        if (pCreateFileA) {
            if (MH_CreateHook(pCreateFileA, &Detour_CreateFileA, (LPVOID*)&g_pfnOriginalCreateFileA) != MH_OK) {
                Log("Failed to hook CreateFileA!");
            } else {
                Log("Hooked CreateFileA successfully.");
            }
        }
    }
    // 3. HOOK GRAPHICS (IDXGISwapChain::Present)
    uintptr_t pPresentAddr = FindPresentAddress(g_hWindow);
    if (pPresentAddr)
    {
        std::string logMsg = "FindPresentAddress successful. Found at: 0x" + std::to_string(pPresentAddr);
        Log(logMsg);
        if (MH_CreateHook((LPVOID)pPresentAddr, &My_Present, (LPVOID*)&g_pfnOriginalPresent) != MH_OK)
        {
            Log("MH_CreateHook for Present failed!");
        }
        else
        {
            Log("MH_CreateHook for Present successful.");
        }
    }
    else
    {
        Log("FindPresentAddress FAILED! (No address returned)");
    }

    if (MH_EnableHook(nullptr) != MH_OK) {
        Log("MH_EnableHook(MH_ALL_HOOKS) failed!");
        return;
    }
    Log("All hooks enabled.");
}


// DLL Entry Point
BOOL WINAPI DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved)
{
    
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        std::thread(InitializeHooks).detach();
    }
    else if (fdwReason == DLL_PROCESS_DETACH)
    {
        Log("--- DLL_PROCESS_DETACH ---");

        g_bWorkerThreadActive = false;
        if (g_workerThread.joinable())
        {
            g_workerThread.join();
            Log("BGM Worker Thread joined.");
        }

        if (g_imguiInitialized)
        {
            SetWindowLongPtr(g_hWindow, GWLP_WNDPROC, (LONG_PTR)g_pfnOriginalWndProc);
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }

        if (g_pToastTexture) {
            g_pToastTexture->Release();
            g_pToastTexture = nullptr;
        }
        if (g_pToastResource) {
            g_pToastResource->Release();
            g_pToastResource = nullptr;
        }

        MH_Uninitialize();
    }

    return TRUE;
}