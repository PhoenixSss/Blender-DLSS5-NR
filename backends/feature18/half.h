// SPDX-License-Identifier: MIT
// backends/feature18/half.h — IEEE 754 binary16 <-> binary32 conversion.
//
// Round-to-nearest-even float->half; exact half->float. Deterministic and
// dependency-free (no _Float16 intrinsics, so /MT and older MSVC are fine).

#pragma once

#include <cstdint>
#include <cstring>

namespace blender_dlss5::backends::feature18 {

inline uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mant = x & 0x7FFFFFu;

    if (exp <= 0) {
        // Subnormal (or zero) in half space.
        if (exp < -10) return static_cast<uint16_t>(sign);
        const uint32_t m = (mant | 0x800000u) >> (1 - exp);
        return static_cast<uint16_t>(sign | (m >> 13));
    }
    if (exp >= 31) {
        // Overflow to infinity (preserving NaN payloads would need extra
        // handling; the probe never feeds NaN/Inf by construction).
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    // Normal value with round-to-nearest-even on the dropped 13 bits.
    uint32_t m = mant >> 13;
    const uint32_t rest = mant & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (m & 1u))) {
        ++m;
        if (m == 0x400u) {
            // Mantissa overflow carries into the exponent.
            return static_cast<uint16_t>(sign | ((exp + 1) << 10));
        }
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | m);
}

inline float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    uint32_t x;
    if (exp == 0) {
        if (mant == 0) {
            x = sign << 31;
        } else {
            // Subnormal: normalize.
            uint32_t e = exp;
            uint32_t m = mant;
            e = 1;
            while (!(m & 0x400u)) {
                m <<= 1;
                --e;
            }
            m &= 0x3FFu;
            x = (sign << 31) | ((e + 112) << 23) | (m << 13);
        }
    } else if (exp == 0x1F) {
        x = (sign << 31) | 0x7F800000u | (mant << 13);
    } else {
        x = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &x, sizeof(f));
    return f;
}

}  // namespace blender_dlss5::backends::feature18
