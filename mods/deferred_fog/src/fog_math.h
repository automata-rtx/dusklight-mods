#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// The (a, b, c) fog coefficients exactly as aurora's per-draw fog state ends up holding
// them. The game encodes fog into BP registers (J3DGDSetFog in J3DGD.cpp; the aurora
// GXSetFog shim is equivalent): A and C stored as floats truncated to sign|exp|11-bit
// mantissa, B as a 24-bit mantissa plus 5-bit shift. Aurora's command processor
// (command_processor.cpp, regs 0xEE-0xF1) decodes them back. Mirroring the full round trip,
// quantization included, makes the deferred fog pass bit-identical to forward fog.
namespace dusk_fog {

inline float truncate_fog_float(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    bits &= 0xFFFFF000u;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void compute_fog_coefficients(
    float startZ, float endZ, float nearZ, float farZ, float& outA, float& outB, float& outC) {
    float A, B, C;
    if (farZ == nearZ || endZ == startZ) {
        A = 0.0f;
        B = 0.5f;
        C = 0.0f;
    } else {
        A = (farZ * nearZ) / ((farZ - nearZ) * (endZ - startZ));
        B = farZ / (farZ - nearZ);
        C = startZ / (endZ - startZ);
    }

    // J3DGDSetFog's mantissa normalization (b_expn starts at 1).
    float B_mant = B;
    int b_expn = 1;
    while (B_mant > 1.0f) {
        B_mant *= 0.5f;
        b_expn++;
    }
    while (B_mant > 0.0f && B_mant < 0.5f) {
        B_mant *= 2.0f;
        b_expn--;
    }

    const int b_s = b_expn & 0x1F;  // the shift field is 5 bits wide
    const float A_f = A / static_cast<float>(1u << b_s);
    const auto b_m = static_cast<uint32_t>(8388638.0f * B_mant);

    // Aurora's decode: a = trunc(A_f) * 2^b_s, b = (b_m / 8388638) * 2^(b_s - 1), c = trunc(C).
    outA = std::ldexp(truncate_fog_float(A_f), b_s);
    outB = std::ldexp(static_cast<float>(b_m) / 8388638.0f, b_s - 1);
    outC = truncate_fog_float(C);
}

}  // namespace dusk_fog
