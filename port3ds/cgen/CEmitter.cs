using System.Text;
using RecompOne.Recompiler.Analysis;
using RecompOne.Recompiler.Disasm;

namespace CGen;

// Emits C instead of C# for the 3DS runtime. Mirrors the semantics of
// RecompOne.Recompiler.CodeGen.InstructionEmitter / FunctionEmitter.
public sealed class CFuncContext
{
    public uint FuncStart;
    public uint FuncEnd;
    public Dictionary<uint, string> KnownFunctions = [];
    public HashSet<uint> Labels = [];
    public Dictionary<uint, JumpTable> JumpTablesByJr = [];
    public HashSet<uint> RaReturnJrs = [];
    public MipsInstruction[] AllInstructions = [];
    public HashSet<string> Callees = [];
    public bool Comments;

    public string Trail(MipsInstruction i, string line)
    {
        if (!Comments) return line;
        return line.PadRight(60) + $"/* {i.Vram:X8}: {i.Disassemble()} */";
    }

    public uint SkipNopPadding(uint addr)
    {
        if (AllInstructions.Length == 0) return addr;
        uint baseAddr = AllInstructions[0].Vram;
        if (addr < baseAddr) return addr;
        int i = (int)((addr - baseAddr) / 4);
        while (i >= 0 && i < AllInstructions.Length && AllInstructions[i].IsNop)
        {
            addr += 4;
            i++;
        }
        return addr;
    }
}

public static class CEmitter
{
    static string R(int r) => r == 0 ? "0u" : $"c->r[{r}]";

    static string Hex(uint v) => $"0x{v:X}u";

    static string Addr(int rs, short imm)
    {
        if (rs == 0) return $"0x{(uint)(int)imm:X8}u";
        if (imm == 0) return R(rs);
        if (imm > 0) return $"({R(rs)} + 0x{(uint)imm:X}u)";
        return $"({R(rs)} - 0x{unchecked((uint)(-(int)imm)):X}u)";
    }

    static string Cop0Read(int rd) => rd switch
    {
        8 => "c->badvaddr",
        12 => "c->sr",
        13 => "c->cause",
        14 => "c->epc",
        15 => "c->prid",
        _ => "0u"
    };

    static string Cop0Write(int rd, string val) => rd switch
    {
        8 => $"c->badvaddr = {val};",
        12 => $"c->sr = {val};",
        13 => $"c->cause = {val};",
        14 => $"c->epc = {val};",
        15 => $"c->prid = {val};",
        _ => ""
    };

    public static string EmitSingle(MipsInstruction i)
    {
        uint op = i.Word >> 26;
        uint fn = i.Word & 0x3F;
        int rs = i.Rs, rt = i.Rt, rd = i.Rd, sa = i.Sa;
        short imm = i.ImmS;
        ushort immU = i.ImmU;
        string RS = R(rs), RT = R(rt), RD = R(rd);

        if (op == 0)
        {
            return (int)fn switch
            {
                0 => rd == 0 ? "" : sa == 0 ? $"{RD} = {RT};" : $"{RD} = {RT} << {sa};",
                2 => rd == 0 ? "" : $"{RD} = {RT} >> {sa};",
                3 => rd == 0 ? "" : $"{RD} = (u32)((s32){RT} >> {sa});",
                4 => rd == 0 ? "" : $"{RD} = {RT} << ({RS} & 31u);",
                6 => rd == 0 ? "" : $"{RD} = {RT} >> ({RS} & 31u);",
                7 => rd == 0 ? "" : $"{RD} = (u32)((s32){RT} >> ({RS} & 31u));",
                8 => "",
                9 => "",
                12 => "",
                13 => "",
                16 => rd == 0 ? "" : $"{RD} = c->hi;",
                17 => $"c->hi = {RS};",
                18 => rd == 0 ? "" : $"{RD} = c->lo;",
                19 => $"c->lo = {RS};",
                24 => $"{{ s64 _r = (s64)(s32){RS} * (s32){RT}; c->lo = (u32)_r; c->hi = (u32)((u64)_r >> 32); }}",
                25 => $"{{ u64 _r = (u64){RS} * {RT}; c->lo = (u32)_r; c->hi = (u32)(_r >> 32); }}",
                26 => rt == 0 ? "c->lo = 0u; c->hi = 0u;" : $"if ({RT} != 0u) {{ if ((s32){RS} == (s32)0x80000000 && (s32){RT} == -1) {{ c->lo = 0x80000000u; c->hi = 0u; }} else {{ s32 _a = (s32){RS}, _b = (s32){RT}; c->lo = (u32)(_a / _b); c->hi = (u32)(_a % _b); }} }}",
                27 => rt == 0 ? "c->lo = 0u; c->hi = 0u;" : $"if ({RT} != 0u) {{ u32 _a = {RS}, _b = {RT}; c->lo = _a / _b; c->hi = _a % _b; }}",
                32 or 33 => rd == 0 ? "" : $"{RD} = {RS} + {RT};",
                34 or 35 => rd == 0 ? "" : $"{RD} = {RS} - {RT};",
                36 => rd == 0 ? "" : $"{RD} = {RS} & {RT};",
                37 => rd == 0 ? "" : rs == 0 ? $"{RD} = {RT};" : rt == 0 ? $"{RD} = {RS};" : $"{RD} = {RS} | {RT};",
                38 => rd == 0 ? "" : $"{RD} = {RS} ^ {RT};",
                39 => rd == 0 ? "" : $"{RD} = ~({RS} | {RT});",
                42 => rd == 0 ? "" : $"{RD} = (s32){RS} < (s32){RT} ? 1u : 0u;",
                43 => rd == 0 ? "" : $"{RD} = {RS} < {RT} ? 1u : 0u;",
                _ => Unknown(i, $"SPECIAL fn=0x{fn:X2}")
            };
        }

        if (op == 1) return "";

        if (op == 16)
        {
            uint cop0rs = (i.Word >> 21) & 0x1F;
            if (cop0rs == 0) return rt == 0 ? "" : $"{RT} = {Cop0Read(rd)};";
            if (cop0rs == 4) return Cop0Write(rd, RT);
            if (cop0rs == 16 && fn == 16) return "c->sr = (c->sr & ~0xFu) | ((c->sr >> 2) & 0xFu);";
            return "";
        }

        if (op == 18)
        {
            uint cop2rs = (i.Word >> 21) & 0x1F;
            if (cop2rs == 8) return "";
            if (((i.Word >> 25) & 1) == 1) return $"gte_execute(0x{i.Word:X8}u);";
            return cop2rs switch
            {
                0 => rt == 0 ? "" : $"{RT} = gte_read({rd});",
                2 => rt == 0 ? "" : $"{RT} = gte_read_ctrl({rd});",
                4 => $"gte_write({rd}, {RT});",
                6 => $"gte_write_ctrl({rd}, {RT});",
                _ => ""
            };
        }

        if (op is 2 or 3 or 4 or 5 or 6 or 7) return "";

        return (int)op switch
        {
            8 or 9 => rt == 0 ? "" : rs == 0 ? $"{RT} = 0x{unchecked((uint)(int)imm):X8}u;" : imm >= 0 ? $"{RT} = {RS} + 0x{(uint)imm:X}u;" : $"{RT} = {RS} - 0x{unchecked((uint)(-(int)imm)):X}u;",
            10 => rt == 0 ? "" : $"{RT} = (s32){RS} < {(int)imm} ? 1u : 0u;",
            11 => rt == 0 ? "" : $"{RT} = {RS} < 0x{(uint)(int)imm:X8}u ? 1u : 0u;",
            12 => rt == 0 ? "" : $"{RT} = {RS} & 0x{immU:X4}u;",
            13 => rt == 0 ? "" : immU == 0 ? $"{RT} = {RS};" : $"{RT} = {RS} | 0x{immU:X4}u;",
            14 => rt == 0 ? "" : $"{RT} = {RS} ^ 0x{immU:X4}u;",
            15 => rt == 0 ? "" : $"{RT} = 0x{(uint)immU << 16:X8}u;",
            32 => rt == 0 ? "" : $"{RT} = (u32)(s32)(s8)RD8({Addr(rs, imm)});",
            33 => rt == 0 ? "" : $"{RT} = (u32)(s32)(s16)RD16({Addr(rs, imm)});",
            34 => rt == 0 ? "" : $"{RT} = mem_lwl({RT}, {Addr(rs, imm)});",
            35 => rt == 0 ? "" : $"{RT} = RD32({Addr(rs, imm)});",
            36 => rt == 0 ? "" : $"{RT} = RD8({Addr(rs, imm)});",
            37 => rt == 0 ? "" : $"{RT} = RD16({Addr(rs, imm)});",
            38 => rt == 0 ? "" : $"{RT} = mem_lwr({RT}, {Addr(rs, imm)});",
            40 => $"WR8({Addr(rs, imm)}, (u8){RT});",
            41 => $"WR16({Addr(rs, imm)}, (u16){RT});",
            42 => $"mem_swl({Addr(rs, imm)}, {RT});",
            43 => $"WR32({Addr(rs, imm)}, {RT});",
            46 => $"mem_swr({Addr(rs, imm)}, {RT});",
            50 => $"gte_write({rt}, RD32({Addr(rs, imm)}));",
            58 => $"WR32({Addr(rs, imm)}, gte_read({rt}));",
            _ => Unknown(i, $"op=0x{op:X2}")
        };
    }

    public static int UnknownCount;

    static string Unknown(MipsInstruction i, string desc)
    {
        UnknownCount++;
        return $"/* unknown {desc} word=0x{i.Word:X8} @ 0x{i.Vram:X8} */";
    }

    public static bool SkipDelaySlot(MipsInstruction ctrl)
    {
        uint op = ctrl.Word >> 26;
        uint fn = ctrl.Word & 0x3F;
        if (op is 2 or 3) return true;
        if (op == 0 && fn is 8 or 9) return true;
        if (op == 4 && ctrl.Rs == ctrl.Rt) return true;
        if (op == 1 && (uint)ctrl.Rt is 0x10 or 0x11) return true;
        return false;
    }

    // destination GPR of an instruction (or -1)
    static int DestReg(MipsInstruction i)
    {
        uint op = i.Word >> 26, fn = i.Word & 0x3F;
        switch (op)
        {
            case 0:
                return fn switch
                {
                    0x08 or 0x0C or 0x0D or 0x11 or 0x13 or 0x18 or 0x19 or 0x1A or 0x1B => -1,
                    _ => i.Rd,
                };
            case 0x08: case 0x09: case 0x0A: case 0x0B:
            case 0x0C: case 0x0D: case 0x0E: case 0x0F:
            case 0x20: case 0x21: case 0x22: case 0x23:
            case 0x24: case 0x25: case 0x26: return i.Rt;
            case 0x10: case 0x12: return i.Rs is 0 or 2 ? i.Rt : -1;
            default: return -1;
        }
    }

    public static void EmitWithDelaySlot(StringBuilder sb, MipsInstruction ctrl, MipsInstruction? ds, CFuncContext ctx, string indent)
    {
        uint op = ctrl.Word >> 26;
        uint fn = ctrl.Word & 0x3F;
        int rs = ctrl.Rs, rt = ctrl.Rt, rd = ctrl.Rd;
        uint pc = ctrl.Vram;
        string RS = R(rs), RT = R(rt);
        string ind2 = indent + "    ";

        void Ds()
        {
            if (ds == null) return;
            string line = EmitSingle(ds);
            if (!string.IsNullOrEmpty(line)) sb.AppendLine(ctx.Trail(ds, $"{indent}{line}"));
        }

        void DsInline()
        {
            if (ds == null) return;
            string line = EmitSingle(ds);
            if (!string.IsNullOrEmpty(line)) sb.AppendLine(ctx.Trail(ds, $"{ind2}{line}"));
        }

        void CallOrDispatch(uint addr, string ind)
        {
            if (ctx.KnownFunctions.TryGetValue(addr, out var name))
            {
                ctx.Callees.Add(name);
                sb.AppendLine(ctx.Trail(ctrl, $"{ind}{name}(c);"));
            }
            else
                sb.AppendLine(ctx.Trail(ctrl, $"{ind}dispatch_call(c, 0x{addr:X8}u);"));
        }

        bool InFunc(uint target) => target >= ctx.FuncStart && target < ctx.FuncEnd;

        void Conditional(string cond, uint target)
        {
            sb.AppendLine(ctx.Trail(ctrl, $"{indent}if ({cond}) {{"));
            DsInline();
            if (InFunc(target))
                sb.AppendLine($"{ind2}goto L{target:X8};");
            else
            {
                CallOrDispatch(target, ind2);
                sb.AppendLine($"{ind2}return;");
            }
            sb.AppendLine($"{indent}}}");
        }

        // the jump register must be read before the delay slot runs
        string JumpReg()
        {
            if (ds != null && rs != 0 && DestReg(ds) == rs)
            {
                sb.AppendLine($"{indent}u32 _jr{pc:X8} = {RS};");
                return $"_jr{pc:X8}";
            }
            return RS;
        }

        if (op is 4 or 5 or 6 or 7)
        {
            uint target = ctrl.BranchTarget;
            if (op == 4 && rs == rt)
            {
                Ds();
                if (InFunc(target)) sb.AppendLine(ctx.Trail(ctrl, $"{indent}goto L{target:X8};"));
                else { CallOrDispatch(target, indent); sb.AppendLine($"{indent}return;"); }
                return;
            }
            if (op == 5 && rs == rt) return;
            string cond = op switch
            {
                4 => $"{RS} == {RT}",
                5 => $"{RS} != {RT}",
                6 => $"(s32){RS} <= 0",
                _ => $"(s32){RS} > 0",
            };
            Conditional(cond, target);
            return;
        }

        if (op == 1)
        {
            uint rtField = (uint)rt;
            uint target = ctrl.BranchTarget;
            bool link = rtField is 0x10 or 0x11;
            string cond = rtField switch
            {
                0x00 or 0x10 => $"(s32){RS} < 0",
                0x01 or 0x11 => $"(s32){RS} >= 0",
                _ => "0"
            };
            if (link)
            {
                sb.AppendLine($"{indent}{{ int _cnd = {cond};");
                Ds();
                sb.AppendLine($"{indent}c->r[31] = 0x{pc + 8:X8}u;");
                sb.AppendLine($"{indent}if (_cnd) {{");
                if (InFunc(target)) sb.AppendLine($"{ind2}goto L{target:X8};");
                else CallOrDispatch(target, ind2);
                sb.AppendLine($"{indent}}} }}");
            }
            else Conditional(cond, target);
            return;
        }

        if (op == 3)
        {
            uint target = ctrl.JumpTarget;
            Ds();
            sb.AppendLine($"{indent}c->r[31] = 0x{pc + 8:X8}u;");
            CallOrDispatch(target, indent);
            return;
        }
        if (op == 2)
        {
            uint target = ctrl.JumpTarget;
            Ds();
            if (InFunc(target)) sb.AppendLine(ctx.Trail(ctrl, $"{indent}goto L{target:X8};"));
            else { CallOrDispatch(target, indent); sb.AppendLine($"{indent}return;"); }
            return;
        }
        if (op == 0 && fn == 8)
        {
            if (rs == 31 || ctx.RaReturnJrs.Contains(pc))
            {
                Ds();
                sb.AppendLine(ctx.Trail(ctrl, $"{indent}return;"));
                return;
            }
            string jr = JumpReg();
            Ds();
            if (ctx.JumpTablesByJr.TryGetValue(pc, out var jtbl))
            {
                sb.AppendLine(ctx.Trail(ctrl, $"{indent}switch ({jr}) {{"));
                foreach (uint entry in jtbl.Entries.Distinct())
                {
                    if (InFunc(entry))
                        sb.AppendLine($"{indent}    case 0x{entry:X8}u: goto L{entry:X8};");
                }
                sb.AppendLine($"{indent}    default: dispatch_call(c, {jr}); return;");
                sb.AppendLine($"{indent}}}");
            }
            else
            {
                sb.AppendLine(ctx.Trail(ctrl, $"{indent}dispatch_call(c, {jr});"));
                sb.AppendLine($"{indent}return;");
            }
            return;
        }
        if (op == 0 && fn == 9)
        {
            string jr = JumpReg();
            Ds();
            if (rd != 0) sb.AppendLine($"{indent}{R(rd)} = 0x{pc + 8:X8}u;");
            sb.AppendLine(ctx.Trail(ctrl, $"{indent}dispatch_call(c, {jr});"));
            return;
        }
        if (op == 18 && ((ctrl.Word >> 21) & 0x1F) == 8)
        {
            uint target = ctrl.BranchTarget;
            string cond = rt == 1 ? "gte_condition()" : "!gte_condition()";
            Conditional(cond, target);
            return;
        }
    }

    public static string EmitFunction(MipsFunction func, CFuncContext ctx, string cname,
        string? patchTarget, List<string> pre, List<string> post)
    {
        var sb = new StringBuilder();
        var instrs = func.Instructions;

        if (func.IsStub)
        {
            sb.AppendLine($"void {cname}(Cpu *restrict c) {{ (void)c; }}");
            return sb.ToString();
        }

        bool hooked = pre.Count > 0 || post.Count > 0;

        void Hooks(string body)
        {
            foreach (var p in pre) sb.AppendLine($"    if (!{p}(c)) return;");
            sb.AppendLine(body);
            foreach (var p in post) sb.AppendLine($"    {p}(c);");
        }

        if (patchTarget != null)
        {
            sb.AppendLine($"void {cname}(Cpu *restrict c) {{");
            if (hooked) Hooks($"    {patchTarget}(c);");
            else sb.AppendLine($"    {patchTarget}(c);");
            sb.AppendLine("}");
            return sb.ToString();
        }

        string name = cname;
        if (hooked)
        {
            sb.AppendLine($"static void {cname}_impl(Cpu *restrict c);");
            sb.AppendLine($"void {cname}(Cpu *restrict c) {{");
            Hooks($"    {cname}_impl(c);");
            sb.AppendLine("}");
            name = cname + "_impl";
            sb.AppendLine($"static void {name}(Cpu *restrict c) {{");
        }
        else
            sb.AppendLine($"void {name}(Cpu *restrict c) {{");

        var dsIdx = new HashSet<int>();
        for (int i = 0; i < instrs.Length - 1; i++)
            if (instrs[i].HasDelaySlot && SkipDelaySlot(instrs[i]) && !ctx.Labels.Contains(instrs[i + 1].Vram))
                dsIdx.Add(i + 1);

        const string ind = "    ";
        for (int i = 0; i < instrs.Length; i++)
        {
            if (dsIdx.Contains(i)) continue;
            var instr = instrs[i];
            if (ctx.Labels.Contains(instr.Vram))
                sb.AppendLine($"L{instr.Vram:X8}: ;");

            if (instr.HasDelaySlot)
            {
                var delaySlot = i + 1 < instrs.Length ? instrs[i + 1] : null;
                EmitWithDelaySlot(sb, instr, delaySlot, ctx, ind);
            }
            else
            {
                string line = EmitSingle(instr);
                if (!string.IsNullOrEmpty(line)) sb.AppendLine(ctx.Trail(instr, ind + line));
            }
        }

        if (FallsThrough(instrs))
        {
            uint target = ctx.SkipNopPadding(func.End);
            if (ctx.KnownFunctions.TryGetValue(target, out var ft))
            {
                ctx.Callees.Add(ft);
                sb.AppendLine($"{ind}{ft}(c);");
            }
            else
                sb.AppendLine($"{ind}dispatch_call(c, 0x{target:X8}u);");
        }

        sb.AppendLine("}");
        return sb.ToString();
    }

    static bool FallsThrough(MipsInstruction[] instrs)
    {
        if (instrs.Length == 0) return false;
        int idx = instrs.Length - 1;
        if (instrs.Length >= 2 && instrs[idx - 1].HasDelaySlot) idx--;
        var ctrl = instrs[idx];
        if (ctrl.IsReturn || ctrl.IsJump || ctrl.IsRegisterJump || ctrl.IsUnconditionalBranch) return false;
        if (ctrl.IsFunctionCall) return false;
        return true;
    }
}
