        // ---- Added for NFL Head Coach 09: remaining vpkd3d128 formats ----
        // Source floats are PPC x..w = host f32[3]..f32[0]. Normalized-integer formats expect
        // values already biased as 3.0 + v * 2^-22 (1.0 + v * 2^-22 for 2-bit W): they are
        // clamped to the format's range and the low mantissa bits are kept. A one-word result
        // goes to host u32[position]; a two-word result puts its high (x, y) word at
        // u32[position + 1] and its low (z, w) word at u32[position].
        case 1: // 2 normalized shorts: x << 16 | y
            println("\t{}.u32 = (PPCClampBits({}.u32[3], 0x403F8001u, 0x40407FFFu) & 0xFFFF) << 16;", temp(), v(insn.operands[1]));
            println("\t{}.u32 |= PPCClampBits({}.u32[2], 0x403F8001u, 0x40407FFFu) & 0xFFFF;", temp(), v(insn.operands[1]));
            println("\t{}.u32[{}] = {}.u32;", v(insn.operands[0]), insn.operands[4], temp());
            break;

        case 2: // 2_10_10_10: x | y << 10 | z << 20 | w << 30
            println("\t{}.u32 = PPCClampBits({}.u32[3], 0x403FFE01u, 0x404001FFu) & 0x3FF;", temp(), v(insn.operands[1]));
            println("\t{}.u32 |= (PPCClampBits({}.u32[2], 0x403FFE01u, 0x404001FFu) & 0x3FF) << 10;", temp(), v(insn.operands[1]));
            println("\t{}.u32 |= (PPCClampBits({}.u32[1], 0x403FFE01u, 0x404001FFu) & 0x3FF) << 20;", temp(), v(insn.operands[1]));
            println("\t{}.u32 |= (PPCClampBits({}.u32[0], 0x3F800000u, 0x3F800003u) & 0x3) << 30;", temp(), v(insn.operands[1]));
            println("\t{}.u32[{}] = {}.u32;", v(insn.operands[0]), insn.operands[4], temp());
            break;

        case 3: // 2 half floats: half(x) << 16 | half(y)
            println("\t{}.u32[{}] = (uint32_t(PPCFloatToHalf({}.f32[3])) << 16) | PPCFloatToHalf({}.f32[2]);",
                v(insn.operands[0]), insn.operands[4], v(insn.operands[1]), v(insn.operands[1]));
            break;

        case 4: // 4 normalized shorts: (x << 16 | y), (z << 16 | w)
            println("\t{}.u32[{}] = (PPCClampBits({}.u32[3], 0x403F8001u, 0x40407FFFu) << 16) | (PPCClampBits({}.u32[2], 0x403F8001u, 0x40407FFFu) & 0xFFFF);",
                v(insn.operands[0]), insn.operands[4] + 1, v(insn.operands[1]), v(insn.operands[1]));
            println("\t{}.u32[{}] = (PPCClampBits({}.u32[1], 0x403F8001u, 0x40407FFFu) << 16) | (PPCClampBits({}.u32[0], 0x403F8001u, 0x40407FFFu) & 0xFFFF);",
                v(insn.operands[0]), insn.operands[4], v(insn.operands[1]), v(insn.operands[1]));
            break;

