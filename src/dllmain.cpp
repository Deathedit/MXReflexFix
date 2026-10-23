// MXReflexFix: ultrawide + 16:9 HUD + unlocked framerate for MX vs ATV Reflex (Steam).
//
// Addresses in comments are for the Steam MXReflex.exe (md5 fa4dc6b636fac392ad07656d9b187baf);
// the code itself finds every hook site by signature.

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <inipp.h>
#include <safetyhook.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "helper.hpp"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
    const std::string sFixName = "MXReflexFix";
    const std::string sFixVersion = FIX_VERSION;
    const std::string sExeName = "MXReflex.exe";

    constexpr float fNativeAspect = 16.0f / 9.0f;
    constexpr float fHUDWidth = 1280.0f;
    constexpr float fHUDHeight = 720.0f;

    HMODULE thisModule = nullptr;
    HMODULE exeModule = nullptr;
    std::filesystem::path sFixDir;
    std::filesystem::path sExeDir;
    std::shared_ptr<spdlog::logger> logger;

    // Ini
    bool bFixResolution = true;
    bool bFixAspect = true;
    float fFOVMultiplier = 1.0f;
    bool bFixHUD = true;
    bool bMenuBlackBars = true;
    bool bPauseBlackBars = true;
    bool bSkipIntro = true;
    std::string sSkipVideos = "Nordic_Logo_wSound,Rainbow_Logo_wSound,DHLogo";
    bool bUnlockFramerate = true;
    int iFramerateCap = 0;

    // Runtime state
    std::atomic<int> iScreenWidth{0};
    std::atomic<int> iScreenHeight{0};
    std::atomic<float> fAspectRatio{fNativeAspect};
    std::atomic<std::uintptr_t> HUDCamera{0};
    float fTargetFramerate = 60.0f;

    // Cameras that render to textures (jumbotrons) keep their 16:9 projection.
    std::array<std::atomic<std::uintptr_t>, 64> RTTCameras{};
    std::atomic<int> iRTTCameraCount{0};

    // Perspective cameras seen (low bit set = overridden), logged once each for diagnosis.
    std::array<std::atomic<std::uintptr_t>, 32> SeenCameras{};
    std::atomic<int> iSeenCameraCount{0};

    SafetyHookMid RenderTargetHook;
    SafetyHookMid FOVHook;
    SafetyHookMid AspectHook;
    SafetyHookMid RTTCameraHook;
    SafetyHookMid HUDCameraHook;
    SafetyHookMid MouseHook;
    SafetyHookMid SetTargetFPSHook;
    SafetyHookMid FrameLimiterHook;
    SafetyHookMid EndFrameHook;

    SafetyHookInline BinkOpenHook;
    using BinkGotoFn = void(__stdcall*)(void* bink, std::uint32_t frame, std::int32_t flags);
    BinkGotoFn BinkGoto = nullptr;
    std::vector<std::string> SkipVideos; // lower-case base names
    // Bink handles of opened videos that are on the skip list.
    std::array<std::atomic<void*>, 32> SkipHandles{};
    SafetyHookMid VideoDecodeHook;

    std::uintptr_t AppPointer = 0;       // &dword_A95844
    std::uintptr_t UIManagerPointer = 0; // &dword_A93FCC

    std::filesystem::path ModuleDir(HMODULE module)
    {
        wchar_t buffer[MAX_PATH]{};
        GetModuleFileNameW(module, buffer, MAX_PATH);
        return std::filesystem::path(buffer).parent_path();
    }

    void Logging()
    {
        sFixDir = ModuleDir(thisModule);
        sExeDir = ModuleDir(exeModule);

        logger = spdlog::basic_logger_mt(sFixName, (sFixDir / (sFixName + ".log")).string(), true);
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::debug);
        spdlog::flush_on(spdlog::level::debug);

        spdlog::info("----------");
        spdlog::info("{} v{} loaded.", sFixName, sFixVersion);
        spdlog::info("ASI path: {}", sFixDir.string());
        spdlog::info("Game path: {}", sExeDir.string());
    }

    void Configuration()
    {
        const auto iniPath = sFixDir / (sFixName + ".ini");
        std::ifstream iniFile(iniPath);
        if (!iniFile)
        {
            spdlog::warn("Config: {} not found, using defaults.", iniPath.string());
        }
        else
        {
            inipp::Ini<char> ini;
            ini.parse(iniFile);
            ini.strip_trailing_comments();
            inipp::get_value(ini.sections["Fix Resolution"], "Enabled", bFixResolution);
            inipp::get_value(ini.sections["Fix Aspect Ratio"], "Enabled", bFixAspect);
            inipp::get_value(ini.sections["FOV"], "Multiplier", fFOVMultiplier);
            inipp::get_value(ini.sections["Fix HUD"], "Enabled", bFixHUD);
            inipp::get_value(ini.sections["Fix HUD"], "MenuBlackBars", bMenuBlackBars);
            inipp::get_value(ini.sections["Fix HUD"], "PauseBlackBars", bPauseBlackBars);
            inipp::get_value(ini.sections["Intro Skip"], "Enabled", bSkipIntro);
            inipp::get_value(ini.sections["Intro Skip"], "Videos", sSkipVideos);
            inipp::get_value(ini.sections["Framerate"], "Unlock", bUnlockFramerate);
            inipp::get_value(ini.sections["Framerate"], "Cap", iFramerateCap);
            for (const auto& error : ini.errors)
                spdlog::warn("Config: could not parse line \"{}\"", error);
        }

        fFOVMultiplier = std::clamp(fFOVMultiplier, 0.1f, 3.0f);

        spdlog::info("Config: [Fix Resolution] Enabled = {}", bFixResolution);
        spdlog::info("Config: [Fix Aspect Ratio] Enabled = {}", bFixAspect);
        spdlog::info("Config: [FOV] Multiplier = {:.3f}", fFOVMultiplier);
        spdlog::info("Config: [Fix HUD] Enabled = {}, MenuBlackBars = {}, PauseBlackBars = {}", bFixHUD, bMenuBlackBars,
                     bPauseBlackBars);
        spdlog::info("Config: [Intro Skip] Enabled = {}, Videos = {}", bSkipIntro, sSkipVideos);
        spdlog::info("Config: [Framerate] Unlock = {}, Cap = {}", bUnlockFramerate, iFramerateCap);

        // graphics.cfg holds the resolution the game is about to use, and the refresh rate.
        const auto graphics = Util::ReadGraphicsCfg(sExeDir / "graphics.cfg");
        const int width = Util::ToInt(graphics, "ScreenWidth").value_or(0);
        const int height = Util::ToInt(graphics, "ScreenHeight").value_or(0);
        const int refresh = Util::ToInt(graphics, "ScreenRefresh").value_or(0);
        spdlog::info("graphics.cfg: {}x{} @ {} Hz", width, height, refresh);

        if (width > 0 && height > 0)
        {
            iScreenWidth = width;
            iScreenHeight = height;
            fAspectRatio = static_cast<float>(width) / static_cast<float>(height);
        }

        if (iFramerateCap > 0)
        {
            fTargetFramerate = static_cast<float>(iFramerateCap);
        }
        else if (iFramerateCap < 0)
        {
            fTargetFramerate = 10000.0f; // Effectively uncapped; 1/fps feeds the game's limiter.
        }
        else
        {
            int hz = refresh;
            if (hz <= 0)
            {
                DEVMODEW mode{};
                mode.dmSize = sizeof(mode);
                if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode))
                    hz = static_cast<int>(mode.dmDisplayFrequency);
            }
            fTargetFramerate = hz > 1 ? static_cast<float>(hz) : 60.0f;
        }
        spdlog::info("Framerate: target {:.0f} fps", fTargetFramerate);
    }

    bool DetectGame()
    {
        wchar_t buffer[MAX_PATH]{};
        GetModuleFileNameW(exeModule, buffer, MAX_PATH);
        const auto exeName = std::filesystem::path(buffer).filename().string();
        if (_stricmp(exeName.c_str(), sExeName.c_str()) != 0)
        {
            spdlog::error("Detect: unsupported executable \"{}\", expected {}. Doing nothing.", exeName, sExeName);
            return false;
        }
        spdlog::info("Detect: {} found.", exeName);
        return true;
    }

    std::uint8_t* Scan(const char* name, const char* signature)
    {
        auto* result = Memory::PatternScan(exeModule, signature);
        if (result)
            spdlog::info("{}: signature found at {:#x}.", name, reinterpret_cast<std::uintptr_t>(result));
        else
            spdlog::error("{}: signature not found, skipping.", name);
        return result;
    }

    void InstallMidHook(SafetyHookMid& hook, const char* name, std::uint8_t* address, safetyhook::MidHookFn fn)
    {
        hook = safetyhook::create_mid(address, fn);
        if (!hook)
            spdlog::error("{}: failed to install hook.", name);
    }

    bool IsRTTCamera(std::uintptr_t camera)
    {
        const int count = std::min(iRTTCameraCount.load(), static_cast<int>(RTTCameras.size()));
        for (int i = 0; i < count; ++i)
        {
            if (RTTCameras[i].load() == camera)
                return true;
        }
        return false;
    }

    void AddRTTCamera(std::uintptr_t camera)
    {
        if (IsRTTCamera(camera))
            return;
        const int index = iRTTCameraCount.fetch_add(1);
        if (index < static_cast<int>(RTTCameras.size()))
        {
            RTTCameras[index] = camera;
            spdlog::info("Aspect Ratio: render-to-texture camera {:#x} keeps 16:9.", camera);
        }
    }

    void NoteAspect(std::uintptr_t camera, float aspect, bool overridden)
    {
        const std::uintptr_t key = camera | (overridden ? 1u : 0u);
        const int count = std::min(iSeenCameraCount.load(), static_cast<int>(SeenCameras.size()));
        for (int i = 0; i < count; ++i)
        {
            if (SeenCameras[i].load() == key)
                return;
        }
        const int index = iSeenCameraCount.fetch_add(1);
        if (index >= static_cast<int>(SeenCameras.size()))
            return;
        SeenCameras[index] = key;
        spdlog::info("Aspect Ratio: camera {:#x} uses aspect {:.4f}{}", camera, aspect,
                     overridden ? fmt::format(" -> {:.4f}", fAspectRatio.load()) : std::string{});
    }

    // Icon's static ortho camera defines the 1280x720 UI space. Widening its ortho volume to the
    // screen aspect keeps 1280x720 content at native 16:9 size, centered.
    void ApplyHUDCamera()
    {
        const auto camera = HUDCamera.load();
        if (!camera)
            return;

        const float aspect = fAspectRatio.load();
        float width = fHUDWidth;
        float height = fHUDHeight;
        if (aspect > fNativeAspect + 0.001f)
            width = fHUDHeight * aspect;
        else if (aspect < fNativeAspect - 0.001f)
            height = fHUDWidth / aspect;

        *reinterpret_cast<float*>(camera + 0x60) = width;
        *reinterpret_cast<float*>(camera + 0x64) = height;
        // Invalidate the cached projection/view-projection, as Camera::SetAspect (sub_791200) does.
        *reinterpret_cast<std::uint32_t*>(camera + 0xD4) = 0;
        *reinterpret_cast<std::uint32_t*>(camera + 0xD8) = 0;
        *reinterpret_cast<std::uint32_t*>(camera + 0xDC) = 0;

        spdlog::info("HUD: ortho camera {:#x} set to {:.1f}x{:.1f}.", camera, width, height);
    }

    void UpdateResolution(int width, int height)
    {
        if (width == iScreenWidth.load() && height == iScreenHeight.load())
            return;

        iScreenWidth = width;
        iScreenHeight = height;
        fAspectRatio = static_cast<float>(width) / static_cast<float>(height);
        spdlog::info("Resolution: now {}x{}, aspect {:.4f}.", width, height, fAspectRatio.load());

        if (bFixHUD)
            ApplyHUDCamera();
    }

    void Resolution()
    {
        if (!bFixResolution)
            return;

        // sub_7AD6B0 creates the main scene render target at config+0x08 x config+0x0C, which
        // sub_7AD3C0 shrinks to the largest 16:9 box. Make it the full backbuffer
        // (config+0x14 x config+0x18) instead; the game then derives a 0,0,W,H blit rect.
        auto* address = Scan("Render Target", "8B 70 0C 8B 01 8B 50 10 FF D2 8B 40 08 68 EC 02 00 00");
        if (!address)
            return;

        InstallMidHook(RenderTargetHook, "Render Target", address, [](SafetyHookContext& ctx) {
            const auto config = ctx.eax;
            auto& rtWidth = *reinterpret_cast<int*>(config + 0x08);
            auto& rtHeight = *reinterpret_cast<int*>(config + 0x0C);
            const int bbWidth = *reinterpret_cast<int*>(config + 0x14);
            const int bbHeight = *reinterpret_cast<int*>(config + 0x18);
            if (bbWidth < 320 || bbHeight < 180)
                return;

            if (rtWidth != bbWidth || rtHeight != bbHeight)
            {
                spdlog::info("Render Target: {}x{} -> {}x{}.", rtWidth, rtHeight, bbWidth, bbHeight);
                rtWidth = bbWidth;
                rtHeight = bbHeight;
            }
            UpdateResolution(bbWidth, bbHeight);
        });
    }

    void AspectFOV()
    {
        if (!bFixResolution)
            return;

        if (auto* address = Scan("RTT Camera", "F3 0F 10 05 ?? ?? ?? ?? 8D B3 90 00 00 00 F3 0F 11 46 54"))
        {
            // CameraRenderTargetEntity (sub_6CE960) hardcodes 16:9 for jumbotron cameras: remember them.
            InstallMidHook(RTTCameraHook, "RTT Camera", address + 14, [](SafetyHookContext& ctx) {
                AddRTTCamera(ctx.esi);
            });
        }

        // Camera::GetProjection (sub_791720), perspective branch; eax = camera.
        if (fFOVMultiplier != 1.0f)
        {
            if (auto* address = Scan("FOV", "F3 0F 10 48 50 F3 0F 59 0D ?? ?? ?? ?? 0F 57 D2 0F 2F CA"))
            {
                // xmm1 = vertical FOV (camera+0x50), about to be halved.
                InstallMidHook(FOVHook, "FOV", address + 5, [](SafetyHookContext& ctx) {
                    if (!IsRTTCamera(ctx.eax))
                        ctx.xmm1.f32[0] *= fFOVMultiplier;
                });
            }
        }

        if (bFixAspect)
        {
            if (auto* address = Scan("Aspect Ratio", "F3 0F 10 48 54 F3 0F 59 C6 F3 0F 59 44 24 18"))
            {
                // xmm1 = aspect (camera+0x54). The FOV is vertical, so the real aspect gives Hor+.
                InstallMidHook(AspectHook, "Aspect Ratio", address + 5, [](SafetyHookContext& ctx) {
                    const float aspect = ctx.xmm1.f32[0];
                    // Only an exact hardcoded 16:9: the gameplay camera already follows the
                    // full-width render target, and some cameras animate their aspect through 16:9.
                    const bool overridden = std::fabs(aspect - fNativeAspect) < 0.0005f && !IsRTTCamera(ctx.eax);
                    if (overridden)
                        ctx.xmm1.f32[0] = fAspectRatio.load();
                    NoteAspect(ctx.eax, aspect, overridden);
                });
            }
        }
    }

    void HUD()
    {
        if (!bFixResolution || !bFixHUD)
            return;

        // Icon ctor (sub_7FD8D0): "mov eax, offset camera; call ...; mov dword_A95798, 1" runs once
        // after the static UI camera got its 1280x720 ortho size.
        if (auto* address = Scan("HUD Camera", "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00 8B 4D FC"))
        {
            HUDCamera = 0;
            static std::uintptr_t camera = *reinterpret_cast<std::uintptr_t*>(address + 1);
            spdlog::info("HUD Camera: static camera at {:#x}.", camera);
            InstallMidHook(HUDCameraHook, "HUD Camera", address + 10, [](SafetyHookContext&) {
                HUDCamera = camera;
                ApplyHUDCamera();
            });
        }

        // End of sub_817400, which maps the cursor into 1280x720 UI space. Locals at this point:
        // esp+0x18 client origin (screen), esp+0x20 cursor (screen), esp+0x28 client rect;
        // esi/ebx point at the UI-space x/y it just wrote.
        if (auto* address = Scan("Mouse", "F3 0F 59 05 ?? ?? ?? ?? F3 0F 2C C8 89 0B 5F 5E 5D 5B 83 C4 2C C3"))
        {
            InstallMidHook(MouseHook, "Mouse", address + 14, [](SafetyHookContext& ctx) {
                const auto* origin = reinterpret_cast<const POINT*>(ctx.esp + 0x18);
                const auto* cursor = reinterpret_cast<const POINT*>(ctx.esp + 0x20);
                const auto* client = reinterpret_cast<const RECT*>(ctx.esp + 0x28);
                const float width = static_cast<float>(client->right - client->left);
                const float height = static_cast<float>(client->bottom - client->top);
                if (width <= 0.0f || height <= 0.0f || width / height <= fNativeAspect + 0.001f)
                    return;

                const float hudWidth = height * fNativeAspect;
                const float offsetX = (width - hudWidth) * 0.5f;
                const float x = static_cast<float>(cursor->x - origin->x);
                const float y = static_cast<float>(cursor->y - origin->y);
                *reinterpret_cast<int*>(ctx.esi) =
                    static_cast<int>(std::clamp((x - offsetX) / hudWidth * fHUDWidth, 0.0f, fHUDWidth));
                *reinterpret_cast<int*>(ctx.ebx) =
                    static_cast<int>(std::clamp(y / height * fHUDHeight, 0.0f, fHUDHeight));
            });
        }
    }

    // Front end = UI world active and race world inactive. SwitchToFrontEnd (sub_59B470) turns the
    // scene at app+0x20 (race world) off and app+0x1C (UI world) on; scene+0x34 is its active flag.
    bool InFrontEnd()
    {
        if (!AppPointer)
            return false;
        const auto app = *reinterpret_cast<std::uintptr_t*>(AppPointer);
        if (!app)
            return false;
        const auto uiWorld = *reinterpret_cast<std::uintptr_t*>(app + 0x1C);
        const auto raceWorld = *reinterpret_cast<std::uintptr_t*>(app + 0x20);
        const bool uiActive = uiWorld && *reinterpret_cast<int*>(uiWorld + 0x34) != 0;
        const bool raceActive = raceWorld && *reinterpret_cast<int*>(raceWorld + 0x34) != 0;
        return uiActive && !raceActive;
    }

    // Lua IsPaused (sub_72E180) returns UIManager+0x288.
    bool IsPaused()
    {
        if (!UIManagerPointer)
            return false;
        const auto uiManager = *reinterpret_cast<std::uintptr_t*>(UIManagerPointer);
        return uiManager && *reinterpret_cast<int*>(uiManager + 0x288) != 0;
    }

    // Clears the backbuffer outside the centered 16:9 area to black.
    void DrawMenuBars(IDirect3DDevice9* device)
    {
        IDirect3DSurface9* backBuffer = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
            return;

        D3DSURFACE_DESC desc{};
        backBuffer->GetDesc(&desc);
        const LONG width = static_cast<LONG>(desc.Width);
        const LONG height = static_cast<LONG>(desc.Height);
        const LONG barWidth = (width - std::lround(static_cast<float>(height) * fNativeAspect)) / 2;
        if (barWidth > 0)
        {
            IDirect3DSurface9* oldTarget = nullptr;
            device->GetRenderTarget(0, &oldTarget);
            D3DVIEWPORT9 oldViewport{};
            device->GetViewport(&oldViewport);
            DWORD scissorTest = FALSE;
            device->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissorTest);

            // SetRenderTarget resets the viewport to the whole backbuffer; Clear is clipped to it.
            device->SetRenderTarget(0, backBuffer);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            const D3DRECT bars[2] = {{0, 0, barWidth, height}, {width - barWidth, 0, width, height}};
            device->Clear(2, bars, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

            if (oldTarget)
            {
                device->SetRenderTarget(0, oldTarget);
                oldTarget->Release();
            }
            device->SetViewport(&oldViewport);
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, scissorTest);
        }
        backBuffer->Release();
    }

    void MenuBlackBars()
    {
        if (!bFixResolution || !bFixHUD || (!bMenuBlackBars && !bPauseBlackBars))
            return;

        // End of frame (sub_7AE3A0): device->EndScene(), then GetSwapChain(0)->Present(). The game
        // presents through the swap chain, so hook right before EndScene; esi = device.
        auto* endFrameAddress = Scan("End Frame", "E8 ?? ?? ?? ?? 8B 16 8B 82 A8 00 00 00 83 C4 10 56 FF D0 8B 0E 8B 41 38");
        if (!endFrameAddress)
            return;

        if (bMenuBlackBars)
        {
            // SwitchToFrontEnd: "mov edx, dword_A95844; mov eax, [edx+20h]; push eax; xor eax, eax; call ..."
            if (auto* appAddress = Scan("App Pointer", "8B 15 ?? ?? ?? ?? 8B 42 20 50 33 C0 E8"))
                AppPointer = *reinterpret_cast<std::uintptr_t*>(appAddress + 2);
        }
        if (bPauseBlackBars)
        {
            // IsPaused: "mov eax, dword_A93FCC; cvtsi2ss xmm0, dword ptr [eax+288h]"
            if (auto* uiManagerAddress = Scan("UI Manager", "A1 ?? ?? ?? ?? F3 0F 2A 80 88 02 00 00 F3 0F 11 01"))
                UIManagerPointer = *reinterpret_cast<std::uintptr_t*>(uiManagerAddress + 1);
        }
        if (!AppPointer && !UIManagerPointer)
            return;

        InstallMidHook(EndFrameHook, "End Frame", endFrameAddress + 5, [](SafetyHookContext& ctx) {
            enum class Bars { Off, FrontEnd, Paused };
            static Bars lastBars = Bars::Off;
            static bool loggedFirst = false;
            if (!loggedFirst)
            {
                loggedFirst = true;
                spdlog::info("Black Bars: end-of-frame hook active (device {:#x}).", ctx.esi);
            }

            Bars bars = Bars::Off;
            if (AppPointer && InFrontEnd())
                bars = Bars::FrontEnd;
            else if (IsPaused())
                bars = Bars::Paused;

            if (bars != lastBars)
            {
                lastBars = bars;
                spdlog::info("Black Bars: {}.", bars == Bars::FrontEnd ? "front end, bars on"
                                                : bars == Bars::Paused ? "paused, bars on"
                                                                       : "in game, bars off");
            }
            if (bars != Bars::Off)
                DrawMenuBars(reinterpret_cast<IDirect3DDevice9*>(ctx.esi));
        });
    }

    std::string ToLower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // "Videos\Nordic_Logo_wSound.bik" or "..._PAL.bik" -> "nordic_logo_wsound"
    std::string VideoBaseName(const char* path)
    {
        std::string name = path;
        if (const auto slash = name.find_last_of("\\/:"); slash != std::string::npos)
            name.erase(0, slash + 1);
        if (const auto dot = name.find_last_of('.'); dot != std::string::npos)
            name.erase(dot);
        name = ToLower(name);
        if (name.size() > 4 && name.ends_with("_pal"))
            name.erase(name.size() - 4);
        return name;
    }

    bool IsSkipHandle(void* bink)
    {
        for (const auto& handle : SkipHandles)
        {
            if (handle.load() == bink)
                return true;
        }
        return false;
    }

    void SetSkipHandle(void* bink, bool skip)
    {
        for (auto& handle : SkipHandles)
        {
            if (handle.load() == bink)
            {
                if (!skip)
                    handle = nullptr;
                return;
            }
        }
        if (!skip)
            return;
        for (auto& handle : SkipHandles)
        {
            void* expected = nullptr;
            if (handle.compare_exchange_strong(expected, bink))
                return;
        }
    }

    // BinkOpen (binkw32.dll, __stdcall). The game opens and caches every video at startup and
    // rewinds them when they play, so only remember which handles to skip here; the decode hook
    // below does the skipping. A reused handle address of a non-listed video is un-marked.
    void* __stdcall BinkOpenDetour(const char* path, std::uint32_t flags)
    {
        void* bink = BinkOpenHook.stdcall<void*>(path, flags);
        if (!path || !bink)
        {
            if (path)
                spdlog::info("Intro Skip: {} failed to open.", path);
            return bink;
        }

        const auto name = VideoBaseName(path);
        const bool skip = std::find(SkipVideos.begin(), SkipVideos.end(), name) != SkipVideos.end();
        SetSkipHandle(bink, skip);
        spdlog::info("Intro Skip: {} opened, {}.", path, skip ? "will be skipped" : "plays normally");
        return bink;
    }

    void IntroSkip()
    {
        if (!bSkipIntro)
            return;

        std::stringstream list(sSkipVideos);
        std::string item;
        while (std::getline(list, item, ','))
        {
            const auto first = item.find_first_not_of(" \t");
            const auto last = item.find_last_not_of(" \t");
            if (first != std::string::npos)
                SkipVideos.push_back(VideoBaseName(item.substr(first, last - first + 1).c_str()));
        }
        if (SkipVideos.empty())
            return;

        HMODULE binkModule = GetModuleHandleW(L"binkw32.dll");
        if (!binkModule)
        {
            spdlog::error("Intro Skip: binkw32.dll not loaded, skipping.");
            return;
        }
        auto* binkOpen = reinterpret_cast<void*>(GetProcAddress(binkModule, "_BinkOpen@8"));
        BinkGoto = reinterpret_cast<BinkGotoFn>(reinterpret_cast<void*>(GetProcAddress(binkModule, "_BinkGoto@12")));
        if (!binkOpen || !BinkGoto)
        {
            spdlog::error("Intro Skip: BinkOpen/BinkGoto not exported, skipping.");
            return;
        }

        // Per-frame video decode (sub_595690), right before BinkDoFrame; esi = video, [esi+0x88] = bink.
        auto* decodeAddress = Scan("Video Decode", "FF 15 ?? ?? ?? ?? 8B 8E 88 00 00 00 8B 1D ?? ?? ?? ?? 51 FF D3");
        if (!decodeAddress)
            return;

        BinkOpenHook = safetyhook::create_inline(binkOpen, reinterpret_cast<void*>(&BinkOpenDetour));
        if (!BinkOpenHook)
        {
            spdlog::error("Intro Skip: failed to hook BinkOpen.");
            return;
        }
        spdlog::info("Intro Skip: hooked BinkOpen, skipping {} video(s).", SkipVideos.size());

        // Jump a listed video to its last frame whenever it isn't there (first play, rewind or
        // replay), so the game's own end-of-video handling fires on this decode.
        InstallMidHook(VideoDecodeHook, "Video Decode", decodeAddress + 6, [](SafetyHookContext& ctx) {
            void* bink = *reinterpret_cast<void**>(ctx.esi + 0x88);
            if (!bink || !IsSkipHandle(bink))
                return;
            const auto frames = *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(bink) + 0x08);
            const auto frame = *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uintptr_t>(bink) + 0x0C);
            if (frames > 1 && frame < frames)
            {
                BinkGoto(bink, frames, 0);
                // Looping videos come back here every loop; log each handle once.
                static std::array<std::atomic<void*>, 32> logged{};
                for (auto& entry : logged)
                {
                    void* current = entry.load();
                    if (current == bink)
                        break;
                    if (!current && entry.compare_exchange_strong(current, bink))
                    {
                        spdlog::info("Intro Skip: skipped video {:#x} ({} frames).", reinterpret_cast<std::uintptr_t>(bink), frames);
                        break;
                    }
                }
            }
        });
    }

    void PreciseSleep(double seconds)
    {
        static HANDLE timer = [] {
            HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!handle)
                spdlog::warn("Framerate: high resolution timer unavailable, keeping the game's spin wait only.");
            return handle;
        }();
        if (!timer)
            return;

        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(seconds * 10'000'000.0);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer, INFINITE);
    }

    void Framerate()
    {
        if (!bUnlockFramerate)
            return;

        // Engine::SetTargetFPS (sub_84E070): this->minFrameTime = 1 / fps. Called with 60.0 at startup.
        if (auto* address = Scan("Set Target FPS", "F3 0F 10 05 ?? ?? ?? ?? F3 0F 5E 44 24 04 F3 0F 11 41 14 C2 04 00"))
        {
            InstallMidHook(SetTargetFPSHook, "Set Target FPS", address, [](SafetyHookContext& ctx) {
                auto& fps = *reinterpret_cast<float*>(ctx.esp + 0x4);
                if (fps >= 59.0f)
                {
                    spdlog::info("Framerate: target {:.0f} -> {:.0f} fps.", fps, fTargetFramerate);
                    fps = fTargetFramerate;
                }
            });
        }

        // Engine frame limiter (sub_84EA80) busy-waits on QueryPerformanceCounter until
        // engine+0x14 seconds have passed since engine+0x68. Sleep most of that away first;
        // the game's own spin still finishes the last ~1 ms, so timing and dt stay identical.
        if (auto* address = Scan("Frame Limiter", "0F 2F 44 24 08 76 ?? 8D 4C 24 30 51 FF 15 ?? ?? ?? ?? 8B 56 68 8B 46 6C"))
        {
            InstallMidHook(FrameLimiterHook, "Frame Limiter", address + 7, [](SafetyHookContext& ctx) {
                const auto engine = ctx.esi;
                const float target = *reinterpret_cast<float*>(engine + 0x14);
                if (!(target > 0.002f && target < 1.0f))
                    return;

                static const LONGLONG frequency = [] {
                    LARGE_INTEGER value{};
                    QueryPerformanceFrequency(&value);
                    return value.QuadPart;
                }();
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                const LONGLONG last = *reinterpret_cast<LONGLONG*>(engine + 0x68);
                const double remaining = target - static_cast<double>(now.QuadPart - last) / static_cast<double>(frequency);
                if (remaining > 0.002)
                    PreciseSleep(remaining - 0.001);
            });
        }
    }

    void Init()
    {
        Logging();
        if (!DetectGame())
            return;
        Configuration();
        Resolution();
        AspectFOV();
        HUD();
        MenuBlackBars();
        IntroSkip();
        Framerate();
        spdlog::info("Init: done.");
    }
}

extern "C" BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        thisModule = module;
        exeModule = GetModuleHandleW(nullptr);
        DisableThreadLibraryCalls(module);
        // Hooks go in before WinMain runs: the scene render target and the 60 fps cap are both
        // set up early in startup, so a deferred thread could miss them.
        try
        {
            Init();
        }
        catch (const std::exception& e)
        {
            if (logger)
                spdlog::critical("Init: exception: {}", e.what());
        }
    }
    return TRUE;
}
