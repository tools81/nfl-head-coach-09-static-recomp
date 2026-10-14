"""Patch thirdparty/XenonRecomp/XenonRecomp/recompiler.cpp with the instructions HC09 needs.

Idempotent: does nothing if the patch is already applied.
"""
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
TARGET = os.path.join(ROOT, "thirdparty", "XenonRecomp", "XenonRecomp", "recompiler.cpp")
MARKER = "// ---- Added for NFL Head Coach 09"

s = open(TARGET, encoding="utf-8").read()
if MARKER in s:
    print("already patched")
    raise SystemExit(0)


def replace_once(text, old, new):
    assert text.count(old) == 1, f"anchor not found exactly once: {old[:60]!r}"
    return text.replace(old, new)


inc = open(os.path.join(ROOT, "patches", "hc09_ops.inc"), encoding="utf-8").read()

# New instruction cases
s = replace_once(s, "    case PPC_INST_VADDSHS:\n", inc + "    case PPC_INST_VADDSHS:\n")

# vcmpgtuh moved into the new block (now also sets CR6 for the dotted form)
s = replace_once(s, """    case PPC_INST_VCMPGTUH:
        println("\\tsimde_mm_store_si128((simde__m128i*){}.u8, simde_mm_cmpgt_epu16(simde_mm_load_si128((simde__m128i*){}.u16), simde_mm_load_si128((simde__m128i*){}.u16)));", v(insn.operands[0]), v(insn.operands[1]), v(insn.operands[2]));
        break;

""", "")

# vsel128 has the same operand layout as vsel (vD, vA, vB, mask)
s = replace_once(s, "    case PPC_INST_VSEL:\n", "    case PPC_INST_VSEL:\n    case PPC_INST_VSEL128:\n")

# bdnzf: decode the real CR bit instead of assuming eq
s = replace_once(s, """        // NOTE: assuming eq here as a shortcut because all the instructions in the game do that
        println("\\t--{}.u64;", ctr());
        println("\\tif ({}.u32 != 0 && !{}.eq) goto loc_{:X};", ctr(), cr(insn.operands[0] / 4), insn.operands[1]);""",
"""    {
        constexpr std::string_view fields[] = { "lt", "gt", "eq", "so" };
        println("\\t--{}.u64;", ctr());
        println("\\tif ({}.u32 != 0 && !{}.{}) goto loc_{:X};", ctr(), cr(insn.operands[0] / 4), fields[insn.operands[0] % 4], insn.operands[1]);
    }""")

# Integer division: PPC leaves x/0 and MIN/-1 undefined without trapping, but x86 traps.
# Emit guarded divisions that produce 0 in those cases (as Xenia does).
s = replace_once(s, 'println("\\t{}.s64 = {}.s64 / {}.s64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));',
    'println("\\t{0}.s64 = ({2}.s64 == 0 || ({1}.s64 == INT64_MIN && {2}.s64 == -1)) ? 0 : {1}.s64 / {2}.s64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));')
s = replace_once(s, 'println("\\t{}.u64 = {}.u64 / {}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));',
    'println("\\t{0}.u64 = {2}.u64 == 0 ? 0 : {1}.u64 / {2}.u64;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));')
s = replace_once(s, 'println("\\t{}.s32 = {}.s32 / {}.s32;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));',
    'println("\\t{0}.s32 = ({2}.s32 == 0 || ({1}.s32 == INT_MIN && {2}.s32 == -1)) ? 0 : {1}.s32 / {2}.s32;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));')
s = replace_once(s, 'println("\\t{}.u32 = {}.u32 / {}.u32;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));',
    'println("\\t{0}.u32 = {2}.u32 == 0 ? 0 : {1}.u32 / {2}.u32;", r(insn.operands[0]), r(insn.operands[1]), r(insn.operands[2]));')

# vupkd3d128: formats 2-5 (XenonRecomp only handles D3DCOLOR and two shorts)
vupkd3d = open(os.path.join(ROOT, "patches", "hc09_vupkd3d.inc"), encoding="utf-8").read()
s = replace_once(s, """            println("\\t{}.f32[0] = 1.0f;", vTemp());
            println("\\t{} = {};", v(insn.operands[0]), vTemp());
            break;

        default:
            println("\\t__builtin_debugtrap();");""", """            println("\\t{}.f32[0] = 1.0f;", vTemp());
            println("\\t{} = {};", v(insn.operands[0]), vTemp());
            break;

""" + vupkd3d + """        default:
            println("\\t__builtin_debugtrap();");""")

# vpkd3d128: formats 1-4 (XenonRecomp only handles D3DCOLOR and float16_4)
vpkd3d = open(os.path.join(ROOT, "patches", "hc09_vpkd3d.inc"), encoding="utf-8").read()
s = replace_once(s, """            break;

        default:
            println("\\t__builtin_debugtrap();");
            break;
        }
        break;

    case PPC_INST_VPKSHUS:""", """            break;

""" + vpkd3d + """        default:
            println("\\t__builtin_debugtrap();");
            break;
        }
        break;

    case PPC_INST_VPKSHUS:""")

# Jump tables: the guest bounds-checks and indexes with the low 32 bits of the register
# (cmplwi + rlwinm), so switch on those. A 64-bit switch sends values such as
# 0xFFFFFFFF + 1 = 0x100000000 (index 0 on hardware) to the unreachable default and
# from there to an arbitrary host address. An index outside the analysed table traps.
s = replace_once(s, 'println("\\tswitch ({}.u64) {{", r(switchTable->second.r));',
                 'println("\\tswitch ({}.u32) {{", r(switchTable->second.r));')
s = replace_once(s, """            println("\\tdefault:");
            println("\\t\\t__builtin_unreachable();");""", """            println("\\tdefault:");
            println("\\t\\t__builtin_debugtrap();");""")

# blrl: call the address in LR, linking to the next instruction
s = replace_once(s, """    case PPC_INST_BLRL:
        println("__builtin_debugtrap();");
        break;""", """    case PPC_INST_BLRL:
        println("\\t{}.u64 = ctx.lr;", temp());
        println("\\tctx.lr = 0x{:X};", base + 4);
        println("\\tPPC_CALL_INDIRECT_FUNC({}.u32);", temp());
        break;""")

# vcmpbfp: bounds compare. Bit 31: a > b; bit 30: a < -b (both set for NaN). The dotted
# form sets CR6.eq when every element is within bounds.
s = replace_once(s, """    case PPC_INST_VCMPBFP:
    case PPC_INST_VCMPBFP128:
        println("\\t__builtin_debugtrap();");
        break;""", """    case PPC_INST_VCMPBFP:
    case PPC_INST_VCMPBFP128:
        for (size_t i = 0; i < 4; i++)
            println("\\t{0}.u32[{1}] = ({2}.f32[{1}] <= {3}.f32[{1}] ? 0u : 0x80000000u) | ({2}.f32[{1}] >= -{3}.f32[{1}] ? 0u : 0x40000000u);",
                vTemp(), i, v(insn.operands[1]), v(insn.operands[2]));
        println("\\t{} = {};", v(insn.operands[0]), vTemp());
        if (strchr(insn.opcode->name, '.'))
        {
            println("\\t{}.lt = false;", cr(6));
            println("\\t{}.gt = false;", cr(6));
            println("\\t{0}.eq = ({1}.u32[0] | {1}.u32[1] | {1}.u32[2] | {1}.u32[3]) == 0;", cr(6), v(insn.operands[0]));
            println("\\t{}.so = false;", cr(6));
        }
        break;""")

open(TARGET, "w", encoding="utf-8").write(s)
print("patched", TARGET)

# Helpers used by the generated code live in ppc_context.h (copied into the output).
CONTEXT = os.path.join(ROOT, "thirdparty", "XenonRecomp", "XenonUtils", "ppc_context.h")
c = open(CONTEXT, encoding="utf-8").read()
if "PPCHalfToFloat" not in c:
    helpers = open(os.path.join(ROOT, "patches", "hc09_context_helpers.inc"), encoding="utf-8").read()
    c = replace_once(c, "inline simde__m128i simde_mm_vsr(", helpers + "inline simde__m128i simde_mm_vsr(")
    open(CONTEXT, "w", encoding="utf-8").write(c)
    print("patched", CONTEXT)
