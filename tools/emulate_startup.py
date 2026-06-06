#!/usr/bin/env python3
"""
emulate_startup.py -- run the QuickBASIC self-relocating startup offline.

The PKLITE-unpacked image keeps its DGROUP/reloc data byte-LZ-packed; the QB
startup (a) self-moves, (b) decompresses that data, (c) applies relocations, then
(d) `ljmp cs:[0]` to the real program entry. Static analysis can't see the reloc
table or the real entry because they only exist after step (b). So we emulate the
startup (a minimal 8086 interpreter, enough for the instructions it uses) on a
DOS-style memory image and report the resolved entry + the decompressed memory.

Output: the real entry CS:IP (and its image offset) for the recomp to dispatch to.
"""
import sys
import capstone

IMG = "work/BOLO3_image.bin"
MEM_SIZE = 0x110000           # 1 MB + 64 KB (8086 addressable)

# DOS load layout: PSP at segment 0, load module at 0x10 (image offset 0 == 0x100)
PSP = 0x0000
LM = 0x0010
ENTRY_CS = LM + 0x2011
ENTRY_IP = 0x0010
ENTRY_SS = LM + 0x2348
ENTRY_SP = 0x0080


class CPU8086:
    def __init__(self, mem):
        self.m = mem
        self.r = {k: 0 for k in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
                                 "cs", "ds", "es", "ss", "ip")}
        self.cf = self.zf = self.sf = self.of = self.pf = self.df = 0
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
        self.md.detail = True

    # --- register access (8/16-bit) ---
    def get(self, name):
        if name in self.r:
            return self.r[name]
        base = {"al": "ax", "bl": "bx", "cl": "cx", "dl": "dx"}
        hi = {"ah": "ax", "bh": "bx", "ch": "cx", "dh": "dx"}
        if name in base:
            return self.r[base[name]] & 0xFF
        if name in hi:
            return (self.r[hi[name]] >> 8) & 0xFF
        raise KeyError(name)

    def setr(self, name, val):
        if name in self.r:
            self.r[name] = val & 0xFFFF
            return
        base = {"al": "ax", "bl": "bx", "cl": "cx", "dl": "dx"}
        hi = {"ah": "ax", "bh": "bx", "ch": "cx", "dh": "dx"}
        if name in base:
            self.r[base[name]] = (self.r[base[name]] & 0xFF00) | (val & 0xFF)
            return
        if name in hi:
            self.r[hi[name]] = (self.r[hi[name]] & 0x00FF) | ((val & 0xFF) << 8)
            return
        raise KeyError(name)

    # --- memory ---
    def lin(self, seg, off):
        return ((self.r[seg] << 4) + (off & 0xFFFF)) & 0xFFFFF

    def rd8(self, a):
        return self.m[a]

    def rd16(self, a):
        return self.m[a] | (self.m[a + 1] << 8)

    def wr8(self, a, v):
        self.m[a] = v & 0xFF

    def wr16(self, a, v):
        self.m[a] = v & 0xFF
        self.m[a + 1] = (v >> 8) & 0xFF

    # --- effective address for a capstone mem operand ---
    def ea(self, op):
        seg = "ds"
        if op.mem.segment != 0:
            seg = self.md.reg_name(op.mem.segment)
        off = op.mem.disp
        if op.mem.base != 0:
            off += self.get(self.md.reg_name(op.mem.base))
        if op.mem.index != 0:
            off += self.get(self.md.reg_name(op.mem.index)) * op.mem.scale
        return seg, off & 0xFFFF

    def rdop(self, ins, op, size):
        if op.type == capstone.x86.X86_OP_REG:
            return self.get(ins.reg_name(op.reg))
        if op.type == capstone.x86.X86_OP_IMM:
            return op.imm & 0xFFFF
        seg, off = self.ea(op)
        a = self.lin(seg, off)
        return self.rd16(a) if size == 2 else self.rd8(a)

    def wrop(self, ins, op, val, size):
        if op.type == capstone.x86.X86_OP_REG:
            self.setr(ins.reg_name(op.reg), val)
            return
        seg, off = self.ea(op)
        a = self.lin(seg, off)
        if size == 2:
            self.wr16(a, val)
        else:
            self.wr8(a, val)

    def opsize(self, ins, op):
        if op.type == capstone.x86.X86_OP_REG:
            n = ins.reg_name(op.reg)
            return 1 if n in ("al", "ah", "bl", "bh", "cl", "ch", "dl", "dh") else 2
        return op.size

    def setflags_add(self, a, b, r, size):
        m = 0xFFFF if size == 2 else 0xFF
        sb = 0x8000 if size == 2 else 0x80
        self.cf = 1 if (r & ~m) else 0
        rr = r & m
        self.zf = 1 if rr == 0 else 0
        self.sf = 1 if rr & sb else 0
        self.of = 1 if ((a ^ rr) & (b ^ rr) & sb) else 0

    def setflags_sub(self, a, b, r, size):
        m = 0xFFFF if size == 2 else 0xFF
        sb = 0x8000 if size == 2 else 0x80
        self.cf = 1 if (a & m) < (b & m) else 0
        rr = r & m
        self.zf = 1 if rr == 0 else 0
        self.sf = 1 if rr & sb else 0
        self.of = 1 if ((a ^ b) & (a ^ rr) & sb) else 0

    def setflags_logic(self, r, size):
        m = 0xFFFF if size == 2 else 0xFF
        rr = r & m
        self.cf = 0
        self.of = 0
        self.zf = 1 if rr == 0 else 0
        self.sf = 1 if rr & (0x8000 if size == 2 else 0x80) else 0


def run(cpu, max_steps=20_000_000, trace=False):
    md = cpu.md
    X = capstone.x86
    for step in range(max_steps):
        pc = cpu.lin("cs", cpu.r["ip"])
        code = bytes(cpu.m[pc:pc + 16])
        try:
            ins = next(md.disasm(code, cpu.r["ip"], count=1))
        except StopIteration:
            print(f"decode fail at {cpu.r['cs']:04x}:{cpu.r['ip']:04x}")
            return None
        m = ins.mnemonic
        ops = ins.operands
        nxt = (cpu.r["ip"] + ins.size) & 0xFFFF
        if trace:
            print(f"{cpu.r['cs']:04x}:{cpu.r['ip']:04x} {m} {ins.op_str}")

        if m == "ljmp" or (m == "jmp" and ops and ops[0].type == X.X86_OP_MEM and ops[0].size == 4):
            seg, off = cpu.ea(ops[0])
            a = cpu.lin(seg, off)
            return (cpu.rd16(a + 2), cpu.rd16(a))      # (cs, ip) far target
        if m in ("jmp",):
            if ops[0].type == X.X86_OP_IMM:
                cpu.r["ip"] = ops[0].imm & 0xFFFF
            else:                                       # jmp reg/mem near
                cpu.r["ip"] = cpu.rdop(ins, ops[0], 2)
            continue
        if m == "retf":
            sp = cpu.r["sp"]
            ip = cpu.rd16(cpu.lin("ss", sp))
            cs = cpu.rd16(cpu.lin("ss", (sp + 2) & 0xFFFF))
            cpu.r["sp"] = (sp + 4) & 0xFFFF
            cpu.r["cs"] = cs
            cpu.r["ip"] = ip
            continue

        # conditional jumps
        cc = {"je": cpu.zf, "jz": cpu.zf, "jne": not cpu.zf, "jnz": not cpu.zf,
              "jb": cpu.cf, "jc": cpu.cf, "jnae": cpu.cf,
              "jae": not cpu.cf, "jnb": not cpu.cf, "jnc": not cpu.cf,
              "js": cpu.sf, "jns": not cpu.sf, "jp": cpu.pf, "jnp": not cpu.pf,
              "jbe": cpu.cf or cpu.zf, "ja": not (cpu.cf or cpu.zf),
              "jl": cpu.sf != cpu.of, "jge": cpu.sf == cpu.of,
              "jle": cpu.zf or (cpu.sf != cpu.of), "jg": (not cpu.zf) and (cpu.sf == cpu.of)}
        if m in cc:
            cpu.r["ip"] = (ops[0].imm & 0xFFFF) if cc[m] else nxt
            continue
        if m == "jcxz":
            cpu.r["ip"] = (ops[0].imm & 0xFFFF) if cpu.r["cx"] == 0 else nxt
            continue
        if m == "loop":
            cpu.r["cx"] = (cpu.r["cx"] - 1) & 0xFFFF
            cpu.r["ip"] = (ops[0].imm & 0xFFFF) if cpu.r["cx"] != 0 else nxt
            continue

        cpu.r["ip"] = nxt

        if m == "mov":
            cpu.wrop(ins, ops[0], cpu.rdop(ins, ops[1], cpu.opsize(ins, ops[1])),
                     cpu.opsize(ins, ops[0]))
        elif m == "xchg":
            s = cpu.opsize(ins, ops[0])
            a = cpu.rdop(ins, ops[0], s); b = cpu.rdop(ins, ops[1], s)
            cpu.wrop(ins, ops[0], b, s); cpu.wrop(ins, ops[1], a, s)
        elif m in ("add", "sub", "cmp", "and", "or", "xor", "test", "adc", "sbb"):
            s = cpu.opsize(ins, ops[0])
            a = cpu.rdop(ins, ops[0], s); b = cpu.rdop(ins, ops[1], cpu.opsize(ins, ops[1]))
            if m in ("add", "adc"):
                c = cpu.cf if m == "adc" else 0
                r = a + b + c; cpu.setflags_add(a, b + c, r, s); cpu.wrop(ins, ops[0], r, s)
            elif m in ("sub", "sbb"):
                c = cpu.cf if m == "sbb" else 0
                r = a - b - c; cpu.setflags_sub(a, b + c, r, s); cpu.wrop(ins, ops[0], r, s)
            elif m == "cmp":
                cpu.setflags_sub(a, b, a - b, s)
            elif m == "test":
                cpu.setflags_logic(a & b, s)
            else:
                r = {"and": a & b, "or": a | b, "xor": a ^ b}[m]
                cpu.setflags_logic(r, s); cpu.wrop(ins, ops[0], r, s)
        elif m in ("inc", "dec"):
            s = cpu.opsize(ins, ops[0]); a = cpu.rdop(ins, ops[0], s)
            r = a + 1 if m == "inc" else a - 1
            ocf = cpu.cf
            if m == "inc": cpu.setflags_add(a, 1, r, s)
            else: cpu.setflags_sub(a, 1, r, s)
            cpu.cf = ocf                                # inc/dec preserve CF
            cpu.wrop(ins, ops[0], r, s)
        elif m == "not":
            s = cpu.opsize(ins, ops[0]); cpu.wrop(ins, ops[0], ~cpu.rdop(ins, ops[0], s), s)
        elif m == "neg":
            s = cpu.opsize(ins, ops[0]); a = cpu.rdop(ins, ops[0], s)
            cpu.setflags_sub(0, a, -a, s); cpu.wrop(ins, ops[0], -a, s)
        elif m in ("shr", "shl", "sal", "sar", "rol", "ror"):
            s = cpu.opsize(ins, ops[0]); a = cpu.rdop(ins, ops[0], s)
            cnt = cpu.rdop(ins, ops[1], 1) if len(ops) > 1 else 1
            mask = 0xFFFF if s == 2 else 0xFF
            r = a
            for _ in range(cnt & 0x1F):
                if m in ("shl", "sal"):
                    cpu.cf = 1 if r & (0x8000 if s == 2 else 0x80) else 0; r = (r << 1) & mask
                elif m == "shr":
                    cpu.cf = r & 1; r >>= 1
                elif m == "sar":
                    cpu.cf = r & 1; sb = (0x8000 if s == 2 else 0x80); r = (r >> 1) | (r & sb)
                elif m == "rol":
                    hb = 1 if r & (0x8000 if s == 2 else 0x80) else 0; r = ((r << 1) | hb) & mask; cpu.cf = hb
                elif m == "ror":
                    lb = r & 1; r = (r >> 1) | (lb * (0x8000 if s == 2 else 0x80)); cpu.cf = lb
            if cnt: cpu.setflags_logic(r, s); cpu.wrop(ins, ops[0], r, s)
        elif m == "push":
            cpu.r["sp"] = (cpu.r["sp"] - 2) & 0xFFFF
            cpu.wr16(cpu.lin("ss", cpu.r["sp"]), cpu.rdop(ins, ops[0], 2))
        elif m == "pop":
            cpu.wrop(ins, ops[0], cpu.rd16(cpu.lin("ss", cpu.r["sp"])), 2)
            cpu.r["sp"] = (cpu.r["sp"] + 2) & 0xFFFF
        elif m in ("cld",): cpu.df = 0
        elif m in ("std",): cpu.df = 1
        elif m in ("cli", "sti", "nop", "wait"): pass
        elif m == "lodsb":
            cpu.setr("al", cpu.rd8(cpu.lin("ds", cpu.r["si"]))); cpu.r["si"] = (cpu.r["si"] + (-1 if cpu.df else 1)) & 0xFFFF
        elif m == "lodsw":
            cpu.r["ax"] = cpu.rd16(cpu.lin("ds", cpu.r["si"])); cpu.r["si"] = (cpu.r["si"] + (-2 if cpu.df else 2)) & 0xFFFF
        elif m == "stosb":
            cpu.wr8(cpu.lin("es", cpu.r["di"]), cpu.get("al")); cpu.r["di"] = (cpu.r["di"] + (-1 if cpu.df else 1)) & 0xFFFF
        elif m == "stosw":
            cpu.wr16(cpu.lin("es", cpu.r["di"]), cpu.r["ax"]); cpu.r["di"] = (cpu.r["di"] + (-2 if cpu.df else 2)) & 0xFFFF
        elif m in ("movsb", "rep movsb"):
            n = cpu.r["cx"] if m.startswith("rep") else 1
            d = -1 if cpu.df else 1
            for _ in range(n):
                cpu.wr8(cpu.lin("es", cpu.r["di"]), cpu.rd8(cpu.lin("ds", cpu.r["si"])))
                cpu.r["si"] = (cpu.r["si"] + d) & 0xFFFF; cpu.r["di"] = (cpu.r["di"] + d) & 0xFFFF
            if m.startswith("rep"): cpu.r["cx"] = 0
        elif m in ("stosb", "rep stosb") or m == "rep stosb":
            n = cpu.r["cx"] if m.startswith("rep") else 1
            d = -1 if cpu.df else 1
            for _ in range(n):
                cpu.wr8(cpu.lin("es", cpu.r["di"]), cpu.get("al")); cpu.r["di"] = (cpu.r["di"] + d) & 0xFFFF
            if m.startswith("rep"): cpu.r["cx"] = 0
        elif m in ("repe scasb", "repz scasb", "scasb"):
            n = cpu.r["cx"] if m.startswith("rep") else 1
            d = -1 if cpu.df else 1
            while n > 0:
                v = cpu.rd8(cpu.lin("es", cpu.r["di"])); cpu.setflags_sub(cpu.get("al"), v, cpu.get("al") - v, 1)
                cpu.r["di"] = (cpu.r["di"] + d) & 0xFFFF; n -= 1
                if m.startswith("rep"):
                    cpu.r["cx"] -= 1
                    if cpu.zf == 0: break
        else:
            print(f"UNSUPPORTED {cpu.r['cs']:04x}:{cpu.r['ip']:04x} {m} {ins.op_str}")
            return None
    print("max steps reached")
    return None


def main():
    img = open(IMG, "rb").read()
    mem = bytearray(MEM_SIZE)
    mem[LM * 16:LM * 16 + len(img)] = img        # load module at 0x100
    # minimal PSP
    mem[PSP * 16] = 0xCD; mem[PSP * 16 + 1] = 0x20
    mem[PSP * 16 + 2] = 0xFF; mem[PSP * 16 + 3] = 0xFF      # top of memory (paras)
    cpu = CPU8086(mem)
    cpu.r.update(cs=ENTRY_CS, ip=ENTRY_IP, ss=ENTRY_SS, sp=ENTRY_SP, ds=PSP, es=PSP)
    trace = "--trace" in sys.argv
    res = run(cpu, trace=trace)
    if res:
        cs, ip = res
        lin = (cs << 4) + ip
        img_off = lin - LM * 16
        print(f"\nRESOLVED ENTRY: {cs:04x}:{ip:04x}  linear {lin:#07x}  image_off {img_off:#07x}")
        print(f"bytes at entry: {bytes(mem[lin:lin+16]).hex(' ')}")
    else:
        print("\nstartup did not reach the final far jump")


if __name__ == "__main__":
    main()
