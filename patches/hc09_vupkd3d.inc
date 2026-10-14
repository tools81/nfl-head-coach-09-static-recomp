        // ---- Added for NFL Head Coach 09: remaining vupkd3d128 formats ----
        // The packed data is in the source's last element(s): host u32[0] holds PPC element 3,
        // u32[1] element 2. Results are written in PPC order x..w = host u32[3]..u32[0].
        // Normalized integers unpack as 3.0 + v * 2^-22 (1.0 + v * 2^-22 for the 2-bit W).
        case 2: // 2_10_10_10 (x, y, z signed 10-bit; w unsigned 2-bit)
            println("\t{}.u32[3] = 0x40400000u + uint32_t(int32_t({}.u32[0] << 22) >> 22);", vTemp(), v(insn.operands[1]));
            println("\t{}.u32[2] = 0x40400000u + uint32_t(int32_t({}.u32[0] << 12) >> 22);", vTemp(), v(insn.operands[1]));
            println("\t{}.u32[1] = 0x40400000u + uint32_t(int32_t({}.u32[0] << 2) >> 22);", vTemp(), v(insn.operands[1]));
            println("\t{}.u32[0] = 0x3F800000u + ({}.u32[0] >> 30);", vTemp(), v(insn.operands[1]));
            println("\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        case 3: // 2 half floats
            println("\t{}.f32[3] = PPCHalfToFloat(uint16_t({}.u32[0] >> 16));", vTemp(), v(insn.operands[1]));
            println("\t{}.f32[2] = PPCHalfToFloat(uint16_t({}.u32[0]));", vTemp(), v(insn.operands[1]));
            println("\t{}.f32[1] = 0.0f;", vTemp());
            println("\t{}.f32[0] = 1.0f;", vTemp());
            println("\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        case 4: // 4 normalized shorts
            for (size_t i = 0; i < 4; i++)
                println("\t{}.u32[{}] = 0x40400000u + uint32_t(int32_t({}.s16[{}]));", vTemp(), 3 - i, v(insn.operands[1]), 3 - i);
            println("\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        case 5: // 4 half floats
            println("\t{}.f32[3] = PPCHalfToFloat(uint16_t({}.u32[1] >> 16));", vTemp(), v(insn.operands[1]));
            println("\t{}.f32[2] = PPCHalfToFloat(uint16_t({}.u32[1]));", vTemp(), v(insn.operands[1]));
            println("\t{}.f32[1] = PPCHalfToFloat(uint16_t({}.u32[0] >> 16));", vTemp(), v(insn.operands[1]));
            println("\t{}.f32[0] = PPCHalfToFloat(uint16_t({}.u32[0]));", vTemp(), v(insn.operands[1]));
            println("\t{} = {};", v(insn.operands[0]), vTemp());
            break;

