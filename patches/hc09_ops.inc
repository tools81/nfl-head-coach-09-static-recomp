    // ---- Added for NFL Head Coach 09 ----------------------------------------------------
    // Vector registers are stored as one little-endian 128-bit value: PPC (big-endian)
    // element k of an N-element vector lives at host index N-1-k. Element-wise ops are
    // layout-agnostic; packs put vB in the host low half; whole-register shifts take the
    // shift amount from host byte 0 (PPC byte 15).

    case PPC_INST_BDNZT:
    {
        constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
        println("\t--{}.u64;", ctr());
        println("\tif ({}.u32 != 0 && {}.{}) goto loc_{:X};", ctr(), cr(insn.operands[0] / 4), fields[insn.operands[0] % 4], insn.operands[1]);
        break;
    }

    case PPC_INST_CROR:
    case PPC_INST_CRORC:
    {
        constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
        println("\t{}.{} = {}.{} | {}{}.{};",
            cr(insn.operands[0] / 4), fields[insn.operands[0] % 4],
            cr(insn.operands[1] / 4), fields[insn.operands[1] % 4],
            insn.opcode->id == PPC_INST_CRORC ? "!" : "",
            cr(insn.operands[2] / 4), fields[insn.operands[2] % 4]);
        break;
    }

    case PPC_INST_EQV:
        println("\t{}.u64 = ~({}.u64 ^ {}.u64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int32_t>({}.s32, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_MULHD:
        println("\t{}.s64 = int64_t((__int128({}.s64) * __int128({}.s64)) >> 64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int64_t>({}.s64, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

    case PPC_INST_MULHDU:
        println("\t{}.u64 = uint64_t((__uint128_t({}.u64) * __uint128_t({}.u64)) >> 64);", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.compare<int64_t>({}.s64, 0, {});", cr(0), r(insn.operands[0]), xer());
        break;

#define HC09_LD "simde_mm_load_si128((simde__m128i*){}.u8)"
#define HC09_BINOP(op, intrinsic) \
    case op: \
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, " intrinsic "(" HC09_LD ", " HC09_LD "));", \
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2])); \
        break;

    HC09_BINOP(PPC_INST_VADDSBS, "simde_mm_adds_epi8")
    HC09_BINOP(PPC_INST_VAVGUH, "simde_mm_avg_epu16")
    HC09_BINOP(PPC_INST_VMAXSH, "simde_mm_max_epi16")
    HC09_BINOP(PPC_INST_VMINSH, "simde_mm_min_epi16")
    HC09_BINOP(PPC_INST_VSUBSHS, "simde_mm_subs_epi16")
    HC09_BINOP(PPC_INST_VSUBUBM, "simde_mm_sub_epi8")
#undef HC09_BINOP

    case PPC_INST_VANDC:
        // vD = vA & ~vB
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_andnot_si128(" HC09_LD ", " HC09_LD "));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;

    case PPC_INST_VNOR:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_xor_si128(simde_mm_or_si128(" HC09_LD ", " HC09_LD "), simde_mm_set1_epi32(-1)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;

    case PPC_INST_VADDSWS:
        for (size_t i = 0; i < 4; i++)
        {
            println("\t{}.s64 = int64_t({}.s32[{}]) + int64_t({}.s32[{}]);", temp(), v(insn.operands[1]), i, v(insn.operands[2]), i);
            println("\t{}.s32[{}] = {}.s64 > INT_MAX ? INT_MAX : {}.s64 < INT_MIN ? INT_MIN : {}.s64;", vTemp(), i, temp(), temp(), temp());
        }
        println("\t{} = {};", v(insn.operands[0]), vTemp());
        break;

#define HC09_CMP(op, intrinsic) \
    case op: \
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, " intrinsic "(" HC09_LD ", " HC09_LD "));", \
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2])); \
        if (strchr(insn.opcode->name, '.')) \
            println("\t{}.setFromMask(" HC09_LD ", 0xFFFF);", cr(6), v(insn.operands[0])); \
        break;

    HC09_CMP(PPC_INST_VCMPEQUH, "simde_mm_cmpeq_epi16")
    HC09_CMP(PPC_INST_VCMPGTSH, "simde_mm_cmpgt_epi16")
    HC09_CMP(PPC_INST_VCMPGTSW, "simde_mm_cmpgt_epi32")
    HC09_CMP(PPC_INST_VCMPGTUH, "simde_mm_cmpgt_epu16")
#undef HC09_CMP

    case PPC_INST_VCMPGTUW:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_cmpgt_epi32(simde_mm_xor_si128(" HC09_LD ", simde_mm_set1_epi32(INT_MIN)), simde_mm_xor_si128(" HC09_LD ", simde_mm_set1_epi32(INT_MIN))));",
            v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\t{}.setFromMask(" HC09_LD ", 0xFFFF);", cr(6), v(insn.operands[0]));
        break;

    case PPC_INST_VCTUXS:
        // float -> uint32 with scale 2^uimm, saturating; NaN and negatives -> 0
        for (size_t i = 0; i < 4; i++)
        {
            println("\t{}.f64 = double({}.f32[{}]) * {}.0;", temp(), v(insn.operands[1]), i, 1ull << insn.operands[2]);
            println("\t{}.u32[{}] = ({}.f64 != {}.f64 || {}.f64 <= 0.0) ? 0u : ({}.f64 >= 4294967295.0 ? 0xFFFFFFFFu : uint32_t({}.f64));",
                vTemp(), i, temp(), temp(), temp(), temp(), temp());
        }
        println("\t{} = {};", v(insn.operands[0]), vTemp());
        break;

    // Packs: PPC result = pack(vA) || pack(vB); host low half holds vB.
    case PPC_INST_VPKSHSS:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_packs_epi16(" HC09_LD ", " HC09_LD "));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;
    case PPC_INST_VPKSWSS:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_packs_epi32(" HC09_LD ", " HC09_LD "));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;
    case PPC_INST_VPKSWUS:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_packus_epi32(" HC09_LD ", " HC09_LD "));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;
    case PPC_INST_VPKUHUS:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_packus_epi16(simde_mm_min_epu16(" HC09_LD ", simde_mm_set1_epi16(0xFF)), simde_mm_min_epu16(" HC09_LD ", simde_mm_set1_epi16(0xFF))));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;
    case PPC_INST_VPKUWUS:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_packus_epi32(simde_mm_min_epu32(" HC09_LD ", simde_mm_set1_epi32(0xFFFF)), simde_mm_min_epu32(" HC09_LD ", simde_mm_set1_epi32(0xFFFF))));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;
    case PPC_INST_VPKUWUM:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_packus_epi32(simde_mm_and_si128(" HC09_LD ", simde_mm_set1_epi32(0xFFFF)), simde_mm_and_si128(" HC09_LD ", simde_mm_set1_epi32(0xFFFF))));", v(insn.operands[0]), v(insn.operands[2]), v(insn.operands[1]));
        break;

    // Per-element shifts and rotates (shift count from the low bits of the matching element)
    case PPC_INST_VSLH:
    case PPC_INST_VSRH:
    case PPC_INST_VSRAH:
    case PPC_INST_VRLH:
    {
        const auto a = v(insn.operands[1]), b = v(insn.operands[2]);
        for (size_t i = 0; i < 8; i++)
        {
            switch (insn.opcode->id)
            {
            case PPC_INST_VSLH: println("\t{}.u16[{}] = uint16_t({}.u16[{}] << ({}.u16[{}] & 0xF));", vTemp(), i, a, i, b, i); break;
            case PPC_INST_VSRH: println("\t{}.u16[{}] = uint16_t({}.u16[{}] >> ({}.u16[{}] & 0xF));", vTemp(), i, a, i, b, i); break;
            case PPC_INST_VSRAH: println("\t{}.s16[{}] = int16_t({}.s16[{}] >> ({}.u16[{}] & 0xF));", vTemp(), i, a, i, b, i); break;
            default: println("\t{}.u16[{}] = uint16_t(({}.u16[{}] << ({}.u16[{}] & 0xF)) | ({}.u16[{}] >> ((16 - ({}.u16[{}] & 0xF)) & 0xF)));", vTemp(), i, a, i, b, i, a, i, b, i); break;
            }
        }
        println("\t{} = {};", v(insn.operands[0]), vTemp());
        break;
    }

    case PPC_INST_VRLW:
        for (size_t i = 0; i < 4; i++)
            println("\t{}.u32[{}] = ({}.u32[{}] << ({}.u32[{}] & 0x1F)) | ({}.u32[{}] >> ((32 - ({}.u32[{}] & 0x1F)) & 0x1F));", vTemp(), i, v(insn.operands[1]), i, v(insn.operands[2]), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
        println("\t{} = {};", v(insn.operands[0]), vTemp());
        break;

    case PPC_INST_VSRB:
    case PPC_INST_VSRAB:
        for (size_t i = 0; i < 16; i++)
        {
            if (insn.opcode->id == PPC_INST_VSRB)
                println("\t{}.u8[{}] = uint8_t({}.u8[{}] >> ({}.u8[{}] & 0x7));", vTemp(), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
            else
                println("\t{}.s8[{}] = int8_t({}.s8[{}] >> ({}.u8[{}] & 0x7));", vTemp(), i, v(insn.operands[1]), i, v(insn.operands[2]), i);
        }
        println("\t{} = {};", v(insn.operands[0]), vTemp());
        break;

    case PPC_INST_VSPLTISH:
        println("\tsimde_mm_store_si128((simde__m128i*){}.u16, simde_mm_set1_epi16(short(0x{:X})));", v(insn.operands[0]), uint16_t(insn.operands[1]));
        break;

    // Whole-register shifts. PPC "left" is toward host high bytes.
    case PPC_INST_VSL:
        println("\t{}.u64 = {}.u8[0] & 0x7;", temp(), v(insn.operands[2]));
        println("\t{}.u64[1] = {}.u64 ? ({}.u64[1] << {}.u64) | ({}.u64[0] >> (64 - {}.u64)) : {}.u64[1];",
            vTemp(), temp(), v(insn.operands[1]), temp(), v(insn.operands[1]), temp(), v(insn.operands[1]));
        println("\t{}.u64[0] = {}.u64[0] << {}.u64;", vTemp(), v(insn.operands[1]), temp());
        println("\t{} = {};", v(insn.operands[0]), vTemp());
        break;

    case PPC_INST_VSLO:
    case PPC_INST_VSLO128:
        println("\t{}.u64 = ({}.u8[0] >> 3) & 0xF;", temp(), v(insn.operands[2]));
        println("\tfor (int i = 0; i < 16; i++) {}.u8[i] = i >= int({}.u64) ? {}.u8[i - {}.u64] : 0;", vTemp(), temp(), v(insn.operands[1]), temp());
        println("\t{} = {};", v(insn.operands[0]), vTemp());
        break;

#undef HC09_LD
    // ---- end NFL Head Coach 09 additions --------------------------------------------------

