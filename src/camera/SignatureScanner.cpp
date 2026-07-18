#include "SignatureScanner.hpp"
#include <cstring>

namespace bl1gotyvr { namespace camera {

// Parse pattern string into bytes and mask
static void ParsePattern(const char* pattern, std::vector<uint8_t>& bytes, std::vector<bool>& mask) {
    bytes.clear();
    mask.clear();

    const char* p = pattern;
    while (*p) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?' || *p == '.') {
            bytes.push_back(0);
            mask.push_back(false); // wildcard
            if (p[1] == '?') p++; // handle ??
            p++;
        }
        else {
            char hex[3] = { p[0], p[1], 0 };
            bytes.push_back((uint8_t)strtoul(hex, nullptr, 16));
            mask.push_back(true); // must match
            p += 2;
        }
    }
}

std::vector<ScanResult> ScanPattern(uintptr_t start, size_t size, const char* pattern) {
    std::vector<uint8_t> patternBytes;
    std::vector<bool> patternMask;
    ParsePattern(pattern, patternBytes, patternMask);

    std::vector<ScanResult> results;
    if (patternBytes.empty()) return results;

    const uint8_t* base = reinterpret_cast<const uint8_t*>(start);
    size_t patLen = patternBytes.size();

    for (size_t i = 0; i + patLen <= size; i++) {
        bool found = true;
        for (size_t j = 0; j < patLen; j++) {
            if (patternMask[j] && base[i + j] != patternBytes[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            results.push_back({ start + i, patLen });
        }
    }
    return results;
}

}} // namespace bl1gotyvr::camera
