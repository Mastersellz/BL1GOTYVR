#include "../src/render/HudBlitter.hpp"
#include <wrl/client.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace bl1gotyvr {
void Log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}
}
static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void Hr(HRESULT hr, const char* message) { Check(SUCCEEDED(hr), message); }

static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, UINT width, UINT height,
                                      DXGI_FORMAT format, const void* data = nullptr) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA initial = {data, width * 4, 0};
    ComPtr<ID3D11Texture2D> result;
    Hr(device->CreateTexture2D(&desc, data ? &initial : nullptr, &result), "texture creation");
    return result;
}

static std::vector<unsigned char> Readback(ID3D11Device* device,
                                          ID3D11DeviceContext* context,
                                          ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Hr(device->CreateTexture2D(&desc, nullptr, &staging), "staging creation");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    // Blocking is intentional in the standalone correctness test only.
    Hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "test readback");
    std::vector<unsigned char> result(static_cast<size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y)
        memcpy(result.data() + static_cast<size_t>(y) * desc.Width * 4,
               static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch,
               desc.Width * 4);
    context->Unmap(staging.Get(), 0);
    return result;
}

struct Frames {
    ComPtr<ID3D11Texture2D> final, world, extracted, left, right;
};
static Frames MakeFrames(ID3D11Device* device, UINT size, UINT width, UINT height,
                          DXGI_FORMAT format) {
    std::vector<unsigned char> world(static_cast<size_t>(size) * size * 4);
    auto final = world;
    for (UINT y = 0; y < size; ++y) for (UINT x = 0; x < size; ++x) {
        const size_t index = (static_cast<size_t>(y) * size + x) * 4;
        for (UINT c = 0; c < 3; ++c) {
            const unsigned w = (x * 13 + y * 7 + c * 41) % 256;
            world[index + c] = static_cast<unsigned char>(w);
            // Thin colored UI, dark subtractive UI, translucent UI, and no UI.
            const unsigned region = ((x / 7) + (y / 5)) % 4;
            final[index + c] = static_cast<unsigned char>(region == 0 ? w :
                region == 1 ? (w * 3 + (c == 0 ? 255 : 0)) / 4 :
                region == 2 ? w / 3 : (c == 1 ? 255 : 0));
        }
        world[index + 3] = final[index + 3] = 255;
    }
    Frames f;
    f.world = Texture(device, size, size, format, world.data());
    f.final = Texture(device, size, size, format, final.data());
    f.extracted = Texture(device, size, size, format, final.data()); // deliberately dirty
    std::vector<unsigned> background(static_cast<size_t>(width) * height, 0xff3d5d7d);
    const auto targetFormat = format == DXGI_FORMAT_B8G8R8A8_TYPELESS
        ? DXGI_FORMAT_B8G8R8A8_UNORM : format == DXGI_FORMAT_R8G8B8A8_TYPELESS
        ? DXGI_FORMAT_R8G8B8A8_UNORM : format;
    f.left = Texture(device, width, height, targetFormat, background.data());
    f.right = Texture(device, width, height, targetFormat, background.data());
    return f;
}

static void Correctness(ID3D11Device* device, ID3D11DeviceContext* context) {
    auto& blitter = bl1gotyvr::render::HudBlitter::Instance();
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS,
        DXGI_FORMAT_B8G8R8A8_TYPELESS};
    unsigned cases = 0;
    for (auto format : formats) for (int variant = 0; variant < 4; ++variant) {
        auto f = MakeFrames(device, 129, 173, 157, format);
        D3D11_VIEWPORT viewport = {8.25f, 11.75f, 137.5f, 121.25f, 0.0f, 1.0f};
        if (variant == 1) viewport = {0, 0, 173, 157, 0, 1}; // magnification/edges
        if (variant == 2) viewport = {31, 43, 67, 61, 0, 1}; // minification
        const float opacity = variant == 3 ? 0.0f : variant == 1 ? 1.0f : 0.63f;
        const float dot[4] = {0.5f, 0.5f, 0.03f, 1.0f};
        Check(blitter.ExtractDifference(device, context, f.final.Get(), f.world.Get(),
                                        f.extracted.Get()), "extraction");
        Check(blitter.Composite(device, context, f.extracted.Get(), f.left.Get(),
                                viewport, opacity, dot, 173.0f / 157), "reference composition");
        Check(blitter.CompositeDifference(device, context, f.final.Get(), f.world.Get(),
            f.right.Get(), viewport, opacity, dot, 173.0f / 157), "fused composition");
        const auto reference = Readback(device, context, f.left.Get());
        const auto fused = Readback(device, context, f.right.Get());
        int maximum = 0;
        for (size_t i = 0; i < reference.size(); ++i)
            maximum = std::max(maximum, std::abs(int(reference[i]) - int(fused[i])));
        printf("HUD equivalence format=%d variant=%d maxByteError=%d\n", format, variant, maximum);
        Check(maximum <= 2, "fused HUD diverges from two-pass reference");

        // A fully transparent extraction must overwrite a previously dirty RT.
        Check(blitter.ExtractDifference(device, context, f.world.Get(), f.world.Get(),
                                        f.extracted.Get()), "empty extraction");
        const auto empty = Readback(device, context, f.extracted.Get());
        Check(std::all_of(empty.begin(), empty.end(), [](auto b) { return b == 0; }),
              "overwrite pass leaves stale HUD pixels");
        ++cases;
    }
    blitter.Shutdown(); // cached views released before device/resource teardown
    printf("PASS: %u HUD cases (filtering, opacity, reticle, formats, cache eviction)\n", cases);
}

static double Benchmark(ID3D11Device* device, ID3D11DeviceContext* context,
                         Frames& f, bool fused) {
    auto& blitter = bl1gotyvr::render::HudBlitter::Instance();
    auto draw = [&]() {
        if (!fused) Check(blitter.ExtractDifference(device, context,
            f.final.Get(), f.world.Get(), f.extracted.Get()), "benchmark extraction");
        for (int eye = 0; eye < 2; ++eye) {
            D3D11_VIEWPORT viewport = {eye == 0 ? 665.0f : 0.0f, 645, 1895, 1895, 0, 1};
            auto* target = eye == 0 ? f.left.Get() : f.right.Get();
            Check(fused ? blitter.CompositeDifference(device, context, f.final.Get(),
                f.world.Get(), target, viewport, 1, nullptr, 2560.0f / 2708) :
                blitter.Composite(device, context, f.extracted.Get(), target, viewport,
                    1, nullptr, 2560.0f / 2708), "benchmark composition");
        }
    };
    for (int i = 0; i < 3; ++i) draw();
    ComPtr<ID3D11Query> disjoint, begin, end;
    D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    Hr(device->CreateQuery(&desc, &disjoint), "query creation");
    desc.Query = D3D11_QUERY_TIMESTAMP;
    Hr(device->CreateQuery(&desc, &begin), "query creation");
    Hr(device->CreateQuery(&desc, &end), "query creation");
    context->Begin(disjoint.Get());
    context->End(begin.Get());
    constexpr int repeats = 30;
    for (int i = 0; i < repeats; ++i) draw();
    context->End(end.Get()); context->End(disjoint.Get()); context->Flush();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing = {};
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (context->GetData(disjoint.Get(), &timing, sizeof(timing), 0) == S_FALSE) {
        Check(GetTickCount64() < deadline, "GPU benchmark timeout");
        Sleep(1);
    }
    UINT64 a = 0, b = 0;
    Check(!timing.Disjoint && timing.Frequency, "GPU clock disjoint");
    auto timestamp = [&](ID3D11Query* query, UINT64& value) {
        HRESULT hr;
        while ((hr = context->GetData(query, &value, sizeof(value), 0)) == S_FALSE) {
            Check(GetTickCount64() < deadline, "timestamp timeout");
            Sleep(1);
        }
        Hr(hr, "timestamp unavailable");
    };
    timestamp(begin.Get(), a);
    timestamp(end.Get(), b);
    return double(b - a) * 1000.0 / double(timing.Frequency) / repeats;
}

int main(int argc, char** argv) {
    try {
        const bool benchmark = argc > 1 && strcmp(argv[1], "--benchmark") == 0;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level;
        Hr(D3D11CreateDevice(nullptr, benchmark ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context), "D3D11 device");
        Correctness(device.Get(), context.Get());
        if (benchmark) {
            auto frames = MakeFrames(device.Get(), 4096, 2560, 2708, DXGI_FORMAT_R8G8B8A8_UNORM);
            for (int run = 0; run < 3; ++run) {
                double normal = Benchmark(device.Get(), context.Get(), frames, false);
                double fused = Benchmark(device.Get(), context.Get(), frames, true);
                printf("GPU HUD 4096->2x1895 run=%d twoPassMs=%.3f fusedMs=%.3f speedup=%.2fx\n",
                       run, normal, fused, normal / fused);
            }
            bl1gotyvr::render::HudBlitter::Instance().Shutdown();
        }
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        bl1gotyvr::render::HudBlitter::Instance().Shutdown();
        return 1;
    }
}
