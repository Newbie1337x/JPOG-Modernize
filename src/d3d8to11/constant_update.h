#pragma once
#include <cstring>
#include <cstddef>

// Bitwise comparison preserves NaN payloads and signed zero. A duplicate write
// must not clear an already-pending upload.
inline bool CopyConstantsIfChanged(void* destination, const void* source, size_t bytes) {
    if (!bytes || std::memcmp(destination, source, bytes) == 0) return false;
    std::memcpy(destination, source, bytes);
    return true;
}
