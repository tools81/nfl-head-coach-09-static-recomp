// ---- Added for NFL Head Coach 09 ----
// IEEE half -> single conversion for vupkd3d128's half-float formats.
inline float PPCHalfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;
    uint32_t bits;
    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            bits = sign;
        }
        else
        {
            exponent = 113;  // renormalize the subnormal
            while (!(mantissa & 0x400))
            {
                mantissa <<= 1;
                exponent--;
            }
            bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
        }
    }
    else if (exponent == 31)
    {
        bits = sign | 0x7F800000u | (mantissa << 13);
    }
    else
    {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

// Single -> half for vpkd3d128, saturating like XenonRecomp's float16_4 packing:
// NaN and |x| > 65504 become 0x7FFF (with sign), small values flush through the
// subnormal range, the mantissa is truncated.
inline uint16_t PPCFloatToHalf(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000;
    const uint32_t magnitude = bits & 0x7FFFFFFF;
    float absolute;
    memcpy(&absolute, &magnitude, sizeof(absolute));
    if (absolute != absolute || absolute > 65504.0f)
        return uint16_t(sign | 0x7FFF);
    const uint32_t exponent = magnitude >> 23;
    const uint32_t mantissa = (magnitude >> 13) & 0x3FF;
    if (exponent > 0x70)
        return uint16_t(sign | (((exponent - 0x70) << 10) + mantissa));
    const uint32_t shift = 0x71 - exponent;
    return uint16_t(sign | (shift > 31 ? 0 : ((0x400 + mantissa) >> shift)));
}

// Clamps a float (given and returned as bits) between two float bounds (as bits);
// used by vpkd3d128's normalized-integer formats. NaN clamps to the lower bound.
inline uint32_t PPCClampBits(uint32_t valueBits, uint32_t lowBits, uint32_t highBits)
{
    float value, low, high;
    memcpy(&value, &valueBits, sizeof(value));
    memcpy(&low, &lowBits, sizeof(low));
    memcpy(&high, &highBits, sizeof(high));
    if (!(value >= low))
        return lowBits;
    if (value > high)
        return highBits;
    return valueBits;
}

