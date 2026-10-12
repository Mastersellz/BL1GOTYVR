#pragma once
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include "../core/VRMod.hpp"

namespace bl1gotyvr::render {

// Present/render-thread only. GPU samples are sparse and polled without a
// Flush, spin, or blocking Map. This measures the mod submission, not UE3 GPU
// render time; Present intervals include both the engine and runtime pacing.
class FrameProfiler {
public:
    ~FrameProfiler() = default; // owner resets before OpenXR/device shutdown
    class Scope {
    public:
        Scope(FrameProfiler& owner, ID3D11Device* device,
              ID3D11DeviceContext* context, bool native)
            : m_owner(owner), m_context(context) { m_owner.Begin(device, context, native); }
        ~Scope() { m_owner.End(m_context); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        FrameProfiler& m_owner;
        ID3D11DeviceContext* m_context;
    };

    void Reset() {
        for (auto& sample : m_samples) {
            if (sample.disjoint) sample.disjoint->Release();
            if (sample.begin) sample.begin->Release();
            if (sample.end) sample.end->Release();
            sample = {};
        }
        m_stats[0] = {}; m_stats[1] = {};
        m_previous = 0;
        m_active = -1;
        m_sequence = 0;
        m_queriesDisabled = false;
    }

private:
    struct Sample {
        ID3D11Query* disjoint = nullptr;
        ID3D11Query* begin = nullptr;
        ID3D11Query* end = nullptr;
        bool pending = false;
        int mode = 0;
    };
    struct Stats {
        std::array<double, 300> intervals{};
        size_t count = 0;
        double cpuSum = 0, cpuMax = 0;
        double gpuSum = 0, gpuMax = 0;
        unsigned gpuCount = 0;
    };
    static int64_t Now() {
        LARGE_INTEGER value;
        QueryPerformanceCounter(&value);
        return value.QuadPart;
    }
    static double Milliseconds(int64_t ticks) {
        static const double scale = [] {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            return 1000.0 / frequency.QuadPart;
        }();
        return ticks * scale;
    }
    void Begin(ID3D11Device* device, ID3D11DeviceContext* context, bool native) {
        m_start = Now();
        m_mode = native ? 1 : 0;
        const double interval = Milliseconds(m_start - m_previous);
        m_interval = m_previous && m_previousMode == m_mode && interval < 500
            ? interval : 0;
        m_previous = m_start;
        m_previousMode = m_mode;
        m_active = -1;
        if (!device || !context || (++m_sequence % 30) != 0 || m_queriesDisabled) return;
        for (auto& sample : m_samples) {
            if (!sample.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
            UINT64 begin = 0, end = 0;
            const UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            const HRESULT a = context->GetData(sample.disjoint, &disjoint, sizeof(disjoint), flags);
            const HRESULT b = context->GetData(sample.begin, &begin, sizeof(begin), flags);
            const HRESULT c = context->GetData(sample.end, &end, sizeof(end), flags);
            if (a == S_FALSE || b == S_FALSE || c == S_FALSE) continue;
            sample.pending = false;
            if (a == S_OK && b == S_OK && c == S_OK && !disjoint.Disjoint &&
                disjoint.Frequency && end >= begin) {
                const double ms = double(end - begin) * 1000.0 / disjoint.Frequency;
                auto& stats = m_stats[sample.mode];
                stats.gpuSum += ms;
                stats.gpuMax = (std::max)(stats.gpuMax, ms);
                ++stats.gpuCount;
            }
        }
        for (int i = 0; i < 4; ++i) {
            auto& sample = m_samples[i];
            if (sample.pending) continue;
            if (!sample.disjoint) {
                D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                HRESULT result = device->CreateQuery(&desc, &sample.disjoint);
                desc.Query = D3D11_QUERY_TIMESTAMP;
                if (SUCCEEDED(result)) result = device->CreateQuery(&desc, &sample.begin);
                if (SUCCEEDED(result)) result = device->CreateQuery(&desc, &sample.end);
                if (FAILED(result)) {
                    m_queriesDisabled = true;
                    Log("[Perf] GPU queries unavailable: 0x%08X; CPU timing retained", result);
                    return;
                }
            }
            sample.mode = m_mode;
            context->Begin(sample.disjoint);
            context->End(sample.begin);
            m_active = i;
            break;
        }
    }
    void End(ID3D11DeviceContext* context) {
        if (m_active >= 0) {
            auto& sample = m_samples[m_active];
            context->End(sample.end);
            context->End(sample.disjoint);
            sample.pending = true;
        }
        const double cpu = Milliseconds(Now() - m_start);
        if (m_interval <= 0) return;
        auto& stats = m_stats[m_mode];
        stats.intervals[stats.count++] = m_interval;
        stats.cpuSum += cpu;
        stats.cpuMax = (std::max)(stats.cpuMax, cpu);
        if (stats.count != stats.intervals.size()) return;
        std::sort(stats.intervals.begin(), stats.intervals.end());
        double sum = 0;
        for (double interval : stats.intervals) sum += interval;
        Log("[Perf] mode=%s presents=%zu presentHz=%.1f intervalMs[p50/p95/max]=%.2f/%.2f/%.2f "
            "submitCpuMs[avg/max]=%.2f/%.2f submitGpuMs[avg/max]=%.2f/%.2f gpuSamples=%u",
            m_mode ? "SFR" : "AFR", stats.count, stats.count * 1000.0 / sum,
            stats.intervals[149], stats.intervals[284], stats.intervals.back(),
            stats.cpuSum / stats.count, stats.cpuMax,
            stats.gpuCount ? stats.gpuSum / stats.gpuCount : 0, stats.gpuMax, stats.gpuCount);
        stats = {};
    }
    Sample m_samples[4] = {};
    Stats m_stats[2] = {};
    int m_active = -1, m_mode = 0, m_previousMode = -1;
    uint64_t m_sequence = 0;
    int64_t m_start = 0, m_previous = 0;
    double m_interval = 0;
    bool m_queriesDisabled = false;
};

} // namespace bl1gotyvr::render
