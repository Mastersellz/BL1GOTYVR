#pragma once
#include <cstdint>
#include <vector>

namespace bl1gotyvr { namespace camera {

struct ScanResult {
    uintptr_t address;
    size_t length;
};

// Scan memory for byte pattern with wildcards
// Pattern uses '?' for wildcard bytes, e.g. "48 89 5C 24 ?? 57"
std::vector<ScanResult> ScanPattern(uintptr_t start, size_t size, const char* pattern);

}} // namespace bl1gotyvr::camera
