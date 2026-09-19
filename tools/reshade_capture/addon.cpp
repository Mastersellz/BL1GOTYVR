// ReShade add-on that publishes the post-effect back buffer for the
// Borderlands GOTY Enhanced VR mod.
//
// The VR mod's Present hook runs before ReShade's effect pass, so capturing
// the swap chain back buffer there misses every filter. This add-on listens to
// ReShade's finish-effects event, copies the filtered back buffer into a
// shared texture through the ReShade API only (no native pointer guessing) and
// publishes the exported handle through a named shared memory block that the
// mod opens with the game's device.

#include "reshade/ReShadeSharedFrame.hpp"

#include "reshade.hpp"

#include <Windows.h>

#include <cstdio>
#include <stdio.h>

namespace {

using bl1gotyvr::reshade::kSharedFrameMagic;
using bl1gotyvr::reshade::kSharedFrameName;
using bl1gotyvr::reshade::SharedFrame;

HANDLE g_mapping = nullptr;
SharedFrame* g_shared = nullptr;
reshade::api::resource g_copyResource = {};
HANDLE g_sharedHandle = nullptr;
uint32_t g_width = 0;
uint32_t g_height = 0;
uint32_t g_format = 0;
uint64_t g_frameSerial = 0;
bool g_disabled = false;

void LogLine(const char* message) {
    wchar_t path[MAX_PATH] = {};
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LogLine), &self) && self &&
        GetModuleFileNameW(self, path, MAX_PATH)) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) {
            wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - path),
                     L"BL1GOTYVR_ReShadeCapture.log");
            FILE* file = nullptr;
            if (_wfopen_s(&file, path, L"a, ccs=UTF-8") == 0 && file) {
                fputs(message, file);
                fputc('\n', file);
                fclose(file);
            }
        }
    }
}

void Publish(uint64_t frameSerial) {
    if (!g_shared || !g_sharedHandle) return;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_shared->sequence));
    g_shared->magic = static_cast<int32_t>(kSharedFrameMagic);
    g_shared->sharedHandle = reinterpret_cast<uint64_t>(g_sharedHandle);
    g_shared->width = g_width;
    g_shared->height = g_height;
    g_shared->format = g_format;
    g_shared->frameSerial = frameSerial;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_shared->sequence));
}

void ReleaseCopyResource(reshade::api::device* device) {
    if (g_copyResource.handle && device) {
        device->destroy_resource(g_copyResource);
    }
    g_copyResource = {};
    g_sharedHandle = nullptr;
    g_width = 0;
    g_height = 0;
}

void OnFinishEffects(reshade::api::effect_runtime* runtime,
                     reshade::api::command_list* cmd_list,
                     reshade::api::resource_view rtv,
                     reshade::api::resource_view rtv_srgb) {
    (void)rtv_srgb;
    if (g_disabled || !g_shared) return;
    __try {
        if (!runtime || !cmd_list || rtv.handle == 0) return;
        reshade::api::device* device = runtime->get_device();
        if (!device) return;
        const reshade::api::resource source = device->get_resource_from_view(rtv);
        if (source.handle == 0) return;
        const reshade::api::resource_desc desc = device->get_resource_desc(source);
        if (desc.type != reshade::api::resource_type::texture_2d ||
            desc.texture.width == 0 || desc.texture.height == 0)
            return;

        if (!g_copyResource.handle || desc.texture.width != g_width ||
            desc.texture.height != g_height) {
            ReleaseCopyResource(device);
            reshade::api::resource_desc copyDesc = desc;
            copyDesc.usage = reshade::api::resource_usage::copy_dest;
            copyDesc.flags = reshade::api::resource_flags::shared |
                reshade::api::resource_flags::shared_nt_handle;
            void* sharedHandle = nullptr;
            reshade::api::resource created = {};
            if (!device->create_resource(copyDesc, nullptr,
                                         reshade::api::resource_usage::copy_dest,
                                         &created, &sharedHandle) ||
                !created.handle || !sharedHandle) {
                g_disabled = true;
                LogLine("create_resource(shared copy) failed; bridge disabled");
                return;
            }
            g_copyResource = created;
            g_sharedHandle = static_cast<HANDLE>(sharedHandle);
            g_width = desc.texture.width;
            g_height = desc.texture.height;
            g_format = static_cast<uint32_t>(desc.texture.format);
            char buffer[160] = {};
            _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                        "Shared copy texture ready: %ux%u fmt=%u",
                        g_width, g_height, g_format);
            LogLine(buffer);
        }

        cmd_list->copy_resource(source, g_copyResource);
        Publish(++g_frameSerial);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Never let a bridge failure take the game down.
        g_disabled = true;
        LogLine("Exception in finish-effects bridge; disabled");
    }
}

bool RegisterAddonWithVersionFallback(HMODULE module) {
    HMODULE reshadeModule = static_cast<HMODULE>(
        reshade::internal::get_reshade_module_handle(nullptr));
    if (!reshadeModule) return false;
    const auto registerAddon = reinterpret_cast<bool(*)(void*, uint32_t)>(
        GetProcAddress(reshadeModule, "ReShadeRegisterAddon"));
    if (!registerAddon) return false;
    // Older ReShade builds reject add-ons compiled against a newer API, so
    // walk down until the host accepts the declaration.
    for (uint32_t version = RESHADE_API_VERSION; version >= 14; --version) {
        if (registerAddon(module, version)) return true;
    }
    return false;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (!RegisterAddonWithVersionFallback(module)) return FALSE;
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(
            &OnFinishEffects);
        g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                       PAGE_READWRITE, 0, sizeof(SharedFrame),
                                       kSharedFrameName);
        if (g_mapping) {
            g_shared = static_cast<SharedFrame*>(
                MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                              sizeof(SharedFrame)));
            if (g_shared) {
                ZeroMemory(g_shared, sizeof(SharedFrame));
                LogLine("Filtered-frame bridge active");
            }
        }
        return TRUE;
    }
    if (reason == DLL_PROCESS_DETACH) {
        reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(
            &OnFinishEffects);
        if (g_shared) {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_shared->sequence));
            g_shared->magic = 0;
            g_shared->sharedHandle = 0;
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_shared->sequence));
            UnmapViewOfFile(g_shared);
            g_shared = nullptr;
        }
        if (g_mapping) {
            CloseHandle(g_mapping);
            g_mapping = nullptr;
        }
        // The device is gone during process detach; just drop the handle.
        g_copyResource = {};
        g_sharedHandle = nullptr;
        reshade::unregister_addon(module);
    }
    return TRUE;
}
