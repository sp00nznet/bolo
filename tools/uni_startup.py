#!/usr/bin/env python3
"""
uni_startup.py -- run the Bolo QB startup under the Unicorn CPU emulator.

A fully-scriptable harness (no DOSBox GUI): load the PKLITE-unpacked image into a
DOS-style real-mode memory map, seed the entry registers, hook INT/code, and run.
Goal: observe what the self-relocating startup ACTUALLY does (register state at the
rep movsb, and the real entry CS:IP it finally jumps to) -- the ground truth that
settles the move-direction puzzle and unblocks the boot.
"""
import struct
import sys
from unicorn import *
from unicorn.x86_const import *

IMG = "work/BOLO3_image.bin"
TOTAL = 0x110000                 # 1 MB + 64 KB (covers seg:off up to FFFF:FFFF)

# High/wraparound load: the QB self-move only works when CS wraps past 0x10000
# (so top=LM+0x1B22 stays large > CS=LM+0x2011 wrapped) -> 8086 1 MB wraparound.
LM = 0xE000                      # load module segment (program straddles 1 MB)
PSP_SEG = (LM - 0x10) & 0xFFFF
ENTRY_CS = (LM + 0x2011) & 0xFFFF
ENTRY_IP = 0x0010
ENTRY_SS = (LM + 0x2348) & 0xFFFF
ENTRY_SP = 0x0080

R = {n: getattr(__import__("unicorn.x86_const", fromlist=[n]), n) for n in
     ("UC_X86_REG_AX", "UC_X86_REG_BX", "UC_X86_REG_CX", "UC_X86_REG_DX",
      "UC_X86_REG_SI", "UC_X86_REG_DI", "UC_X86_REG_BP", "UC_X86_REG_SP",
      "UC_X86_REG_CS", "UC_X86_REG_DS", "UC_X86_REG_ES", "UC_X86_REG_SS",
      "UC_X86_REG_IP")}


def regs(uc):
    return {k[11:].lower(): uc.reg_read(v) for k, v in R.items()}


def main():
    img = open(IMG, "rb").read()
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, TOTAL)
    # load with 8086 1 MB wraparound: image byte F -> linear (LM*16 + F) & 0xFFFFF
    base = LM * 16
    for f in range(0, len(img), 0x4000):
        chunk = img[f:f + 0x4000]
        a = (base + f) & 0xFFFFF
        if a + len(chunk) <= 0x100000:
            uc.mem_write(a, chunk)
        else:                                   # split across the 1 MB boundary
            n = 0x100000 - a
            uc.mem_write(a, chunk[:n]); uc.mem_write(0, chunk[n:])
    # mirror low 64 KB into the overflow region so seg:off > 1 MB reads wrap
    uc.mem_write(0x100000, bytes(uc.mem_read(0, TOTAL - 0x100000)))
    # minimal PSP
    uc.mem_write(PSP_SEG * 16, b"\xCD\x20")
    uc.mem_write(PSP_SEG * 16 + 2, struct.pack("<H", 0xFFFF))   # top of memory

    def hook_wrap(uc, access, address, size, value, _):
        # keep the 64 KB overflow mirror in sync with low memory on writes
        if address >= 0x100000:
            uc.mem_write(address - 0x100000, struct.pack("<I", value & 0xFFFFFFFF)[:size])
        elif address < 0x10000:
            uc.mem_write(address + 0x100000, struct.pack("<I", value & 0xFFFFFFFF)[:size])
    uc.hook_add(UC_HOOK_MEM_WRITE, hook_wrap)

    for r, val in ((UC_X86_REG_CS, ENTRY_CS), (UC_X86_REG_IP, ENTRY_IP),
                   (UC_X86_REG_SS, ENTRY_SS), (UC_X86_REG_SP, ENTRY_SP),
                   (UC_X86_REG_DS, PSP_SEG), (UC_X86_REG_ES, PSP_SEG),
                   (UC_X86_REG_AX, 0), (UC_X86_REG_BP, 0)):
        uc.reg_write(r, val)

    state = {"n": 0, "entry": None, "ljmp_lin": None, "stop": None,
             "last_cs": None, "transitions": []}
    TRACE = "--trace" in sys.argv
    SEGLOG = "--segs" in sys.argv
    NMAX = 5_000_000

    def hook_code(uc, address, size, _):
        state["n"] += 1
        cs = uc.reg_read(UC_X86_REG_CS); ip = uc.reg_read(UC_X86_REG_IP)
        code = uc.mem_read(address, min(size, 8))
        if SEGLOG and cs != state["last_cs"]:
            state["transitions"].append((state["n"], state["last_cs"], cs, ip, address))
            if len(state["transitions"]) <= 60:
                print(f"[{state['n']:7}] CS {state['last_cs']} -> {cs:04x}:{ip:04x} "
                      f"(lin {address:#07x}, img {address-LM*16:#x})")
            state["last_cs"] = cs
        if TRACE and (state["n"] <= 20 or 50695 <= state["n"] <= 50760):
            try:
                import capstone
                md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
                i = next(md.disasm(bytes(code), ip, 1))
                txt = f"{i.mnemonic} {i.op_str}"
            except Exception:
                txt = code.hex()
            print(f"[{state['n']:5}] {cs:04x}:{ip:04x} (lin {address:#07x})  {txt}")
        # decisive measurement: state right after the rep movsb (ip==0x2f)
        if cs == ENTRY_CS and ip == 0x2f and "moved" not in state:
            state["moved"] = True
            es = uc.reg_read(UC_X86_REG_ES)
            print(f"\n=== after rep movsb (insn {state['n']}) ===")
            print(f"  ES(top)={es:04x}  CS={cs:04x}  top-CS={(es-cs)&0xFFFF:#x} "
                  f"({'top<CS: move DOWN' if es < cs else 'top>=CS: move UP'})")
            print(f"  bytes at CS:0x2f (the push/retf): "
                  f"{bytes(uc.mem_read((cs<<4)+0x2f, 8)).hex(' ')}")
            print(f"  bytes at ES:0x2f (the copy):      "
                  f"{bytes(uc.mem_read((es<<4)+0x2f, 8)).hex(' ')}")
        # detect the final handoff: jmp far cs:[bx] = 2E FF 2F
        if bytes(code[:3]) == b"\x2e\xff\x2f":
            bx = uc.reg_read(UC_X86_REG_BX)
            a = (cs << 4) + bx
            off = struct.unpack("<H", uc.mem_read(a, 2))[0]
            seg = struct.unpack("<H", uc.mem_read(a + 2, 2))[0]
            state["entry"] = (seg, off)
            state["ljmp_lin"] = address
            print(f"\n*** ljmp cs:[bx] at {cs:04x}:{ip:04x}: target {seg:04x}:{off:04x}"
                  f"  linear {(seg<<4)+off:#07x}  image_off {((seg<<4)+off)-LM*16:#07x}")
            uc.emu_stop()
        if state["n"] > NMAX:
            state["stop"] = "max insns"; uc.emu_stop()

    def hook_intr(uc, intno, _):
        ah = (uc.reg_read(UC_X86_REG_AX) >> 8) & 0xFF
        ax = uc.reg_read(UC_X86_REG_AX)
        if TRACE:
            print(f"  INT {intno:#x} AH={ah:#x} AX={ax:#x}")
        if intno == 0x21 and ah == 0x4C:
            state["stop"] = f"INT 21h/4C exit code {ax & 0xFF}"; uc.emu_stop()
        # other INTs: stub (return)

    uc.hook_add(UC_HOOK_CODE, hook_code)
    uc.hook_add(UC_HOOK_INTR, hook_intr)

    begin = ENTRY_CS * 16 + ENTRY_IP
    print(f"start {ENTRY_CS:04x}:{ENTRY_IP:04x} (lin {begin:#07x}), LM={LM:#x}")
    try:
        uc.emu_start(begin, TOTAL, count=NMAX)
    except UcError as e:
        r = regs(uc)
        print(f"\nUcError: {e}  after {state['n']} insns at {r['cs']:04x}:{r['ip']:04x}")
        print("  regs:", {k: hex(v) for k, v in r.items()})
        # dump bytes at fault PC
        pc = (r['cs'] << 4) + r['ip']
        try:
            print("  bytes@pc:", bytes(uc.mem_read(pc, 16)).hex(' '))
        except Exception:
            pass
        return
    print(f"\nstopped: {state['stop']}  after {state['n']} insns")
    if state["entry"]:
        print("RESOLVED ENTRY:", state["entry"])


if __name__ == "__main__":
    main()
