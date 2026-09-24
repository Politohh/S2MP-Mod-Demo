"""Offline reader for the .dmp minidumps Call of Duty WWII drops next to the exe.

No game, no debugger, no symbols needed. Answers the only questions that matter
after a fault:

    * what was the exception, and at which address
    * what was that address in IDA terms  (IDA = VA - module_base, no 0x1000 fixup)
    * what module return addresses are on the faulting thread's stack

Usage:
    python tools/s2_crash_dump.py                 # newest dump in the game dir
    python tools/s2_crash_dump.py <file.dmp>
    python tools/s2_crash_dump.py --list          # all dumps, newest first
    python tools/s2_crash_dump.py --all           # summarise every dump

Caveat kept honest: the stack walk is a HEURISTIC scan for qwords that land inside
the module, exactly like tools/dump_stacks.py. Stale slots survive on a stack, so
treat the SET of frames as evidence and the ORDER as unreliable.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

GAME_DIR = Path(r"E:\SteamLibrary\steamapps\common\Call of Duty WWII")

# Stream types we care about (MINIDUMP_STREAM_TYPE).
THREAD_LIST_STREAM = 3
MODULE_LIST_STREAM = 4
EXCEPTION_STREAM = 6

# x64 CONTEXT field offsets.
CONTEXT_RIP = 0xF8
CONTEXT_RSP = 0x98
CONTEXT_RAX = 0x78
CONTEXT_RCX = 0x80
CONTEXT_RDX = 0x88
CONTEXT_RBX = 0x90
CONTEXT_RBP = 0xA0
CONTEXT_RSI = 0xA8
CONTEXT_RDI = 0xB0
CONTEXT_R8 = 0xB8
CONTEXT_R9 = 0xC0

EXCEPTION_NAMES = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC000001D: "ILLEGAL_INSTRUCTION",
    0xC0000025: "NONCONTINUABLE_EXCEPTION",
    0xC0000026: "INVALID_DISPOSITION",
    0xC000008C: "ARRAY_BOUNDS_EXCEEDED",
    0xC0000094: "INT_DIVIDE_BY_ZERO",
    0xC0000096: "PRIV_INSTRUCTION",
    0xC00000FD: "STACK_OVERFLOW",
    0xC0000409: "STACK_BUFFER_OVERRUN / __fastfail",
    0xC0000374: "HEAP_CORRUPTION",
    0x80000003: "BREAKPOINT",
    0xE06D7363: "C++ EXCEPTION",
}

AV_KIND = {0: "READ from", 1: "WRITE to", 8: "EXECUTE at"}


class Minidump:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        if len(self.data) < 32 or self.data[:4] != b"MDMP":
            raise ValueError(f"{path.name}: not a minidump (size {len(self.data)})")
        _sig, _ver, n_streams, dir_rva = struct.unpack_from("<4sIII", self.data, 0)
        self.streams: dict[int, tuple[int, int]] = {}
        for i in range(n_streams):
            stype, size, rva = struct.unpack_from("<III", self.data, dir_rva + 12 * i)
            self.streams[stype] = (size, rva)

    # ---- modules --------------------------------------------------------
    def _string(self, rva: int) -> str:
        (length,) = struct.unpack_from("<I", self.data, rva)
        return self.data[rva + 4: rva + 4 + length].decode("utf-16-le", "replace")

    def modules(self) -> list[tuple[int, int, str]]:
        if MODULE_LIST_STREAM not in self.streams:
            return []
        _size, rva = self.streams[MODULE_LIST_STREAM]
        (count,) = struct.unpack_from("<I", self.data, rva)
        out = []
        for i in range(count):
            off = rva + 4 + 108 * i
            base, size = struct.unpack_from("<QI", self.data, off)
            name_rva = struct.unpack_from("<I", self.data, off + 20)[0]
            out.append((base, size, self._string(name_rva)))
        return out

    def main_module(self) -> tuple[int, int, str] | None:
        for base, size, name in self.modules():
            if name.lower().endswith("s2_mp64_ship.exe"):
                return base, size, name
        return None

    def module_for(self, addr: int) -> tuple[int, int, str] | None:
        for base, size, name in self.modules():
            if base <= addr < base + size:
                return base, size, name
        return None

    # ---- exception ------------------------------------------------------
    def exception(self) -> dict | None:
        if EXCEPTION_STREAM not in self.streams:
            return None
        _size, rva = self.streams[EXCEPTION_STREAM]
        tid = struct.unpack_from("<I", self.data, rva)[0]
        code, flags, _rec, addr, n_params = struct.unpack_from("<IIQQI", self.data, rva + 8)
        params = list(struct.unpack_from("<15Q", self.data, rva + 8 + 32))[:n_params]
        ctx_size, ctx_rva = struct.unpack_from("<II", self.data, rva + 8 + 32 + 120)
        return {
            "thread_id": tid, "code": code, "flags": flags, "address": addr,
            "params": params, "ctx_size": ctx_size, "ctx_rva": ctx_rva,
        }

    # ---- threads --------------------------------------------------------
    def threads(self) -> list[dict]:
        if THREAD_LIST_STREAM not in self.streams:
            return []
        _size, rva = self.streams[THREAD_LIST_STREAM]
        (count,) = struct.unpack_from("<I", self.data, rva)
        out = []
        for i in range(count):
            off = rva + 4 + 48 * i
            # MINIDUMP_THREAD is 48 bytes:
            #   +0  ThreadId          +4  SuspendCount
            #   +8  PriorityClass     +12 Priority
            #   +16 Teb (u64)
            #   +24 Stack.StartOfMemoryRange (u64)
            #   +32 Stack.Memory {DataSize, Rva}
            #   +40 ThreadContext     {DataSize, Rva}
            tid = struct.unpack_from("<I", self.data, off)[0]
            stack_start = struct.unpack_from("<Q", self.data, off + 24)[0]
            stack_size, stack_rva = struct.unpack_from("<II", self.data, off + 32)
            ctx_size, ctx_rva = struct.unpack_from("<II", self.data, off + 40)
            out.append({
                "id": tid, "stack_start": stack_start, "stack_size": stack_size,
                "stack_rva": stack_rva, "ctx_size": ctx_size, "ctx_rva": ctx_rva,
            })
        return out

    def context_reg(self, ctx_rva: int, ctx_size: int, off: int) -> int | None:
        if ctx_size < off + 8:
            return None
        return struct.unpack_from("<Q", self.data, ctx_rva + off)[0]


def ida(addr: int, base: int) -> str:
    """Runtime VA -> IDA address. base + IDA, with NO 0x1000 adjustment (RULE A1/A2)."""
    return f"IDA_0x{addr - base:X}"


def report(path: Path, verbose: bool = True) -> None:
    dump = Minidump(path)
    main = dump.main_module()
    if not main:
        print(f"{path.name}: s2_mp64_ship.exe not in the module list")
        return
    base, size, _name = main

    exc = dump.exception()
    print(f"=== {path.name}")
    print(f"    module base = 0x{base:X}  size = 0x{size:X}")
    if not exc:
        print("    no exception stream")
        return

    code = exc["code"]
    name = EXCEPTION_NAMES.get(code, "?")
    print(f"    exception  = 0x{code:08X}  {name}")

    addr = exc["address"]
    owner = dump.module_for(addr)
    where = ida(addr, base) if owner and owner[0] == base else (
        f"in {Path(owner[2]).name}" if owner else "OUTSIDE any module")
    print(f"    fault at   = 0x{addr:X}   {where}")

    if code == 0xC0000005 and len(exc["params"]) >= 2:
        kind = AV_KIND.get(exc["params"][0], f"op{exc['params'][0]}")
        target = exc["params"][1]
        t_owner = dump.module_for(target)
        t_where = ida(target, base) if t_owner and t_owner[0] == base else (
            f"in {Path(t_owner[2]).name}" if t_owner else "unmapped")
        print(f"    tried to   = {kind} 0x{target:X}   ({t_where})")
        if target < 0x10000:
            print(f"    ^^ NULL-ish: this is a null base + offset 0x{target:X} "
                  f"({target} decimal) -- a field read off a NULL struct pointer")

    # MINIDUMP_THREAD.Context is frequently the crash handler's later
    # MiniDumpWriteDump call. The exception stream holds the original fault
    # context; using the thread context misattributes the failing registers.
    faulting = next((t for t in dump.threads() if t["id"] == exc["thread_id"]), None)
    if not faulting or not verbose:
        return

    ctx_rva, ctx_size = exc["ctx_rva"], exc["ctx_size"]
    regs = {
        "rip": CONTEXT_RIP, "rsp": CONTEXT_RSP, "rax": CONTEXT_RAX,
        "rcx": CONTEXT_RCX, "rdx": CONTEXT_RDX, "rbx": CONTEXT_RBX,
        "rbp": CONTEXT_RBP, "rsi": CONTEXT_RSI, "rdi": CONTEXT_RDI,
        "r8": CONTEXT_R8, "r9": CONTEXT_R9,
    }
    print(f"    thread {exc['thread_id']} exception registers:")
    for reg, off in regs.items():
        val = dump.context_reg(ctx_rva, ctx_size, off)
        if val is None:
            continue
        tag = ""
        if base <= val < base + size:
            tag = f"   <- {ida(val, base)}"
        elif val < 0x10000:
            tag = "   <- NULL-ish"
        print(f"        {reg:>3} = 0x{val:016X}{tag}")

    stack_rva, stack_size = faulting["stack_rva"], faulting["stack_size"]
    if not stack_size:
        return
    blob = dump.data[stack_rva: stack_rva + stack_size]
    print(f"    stack scan ({stack_size} bytes) -- HEURISTIC, order unreliable:")
    seen: list[int] = []
    for i in range(0, len(blob) - 8, 8):
        (val,) = struct.unpack_from("<Q", blob, i)
        if base <= val < base + size and val not in seen:
            seen.append(val)
    for val in seen[:400]:
        print(f"        {ida(val, base)}")
    if len(seen) > 40:
        print(f"        ... {max(0,len(seen) - 400)} more")


def dumps() -> list[Path]:
    files = [p for p in GAME_DIR.glob("*.dmp") if p.stat().st_size > 0]
    return sorted(files, key=lambda p: p.stat().st_mtime, reverse=True)


def main() -> int:
    args = sys.argv[1:]
    if "--list" in args:
        for p in dumps():
            print(f"{p.stat().st_mtime:.0f}  {p.name}  {p.stat().st_size} bytes")
        return 0
    if "--all" in args:
        for p in dumps():
            try:
                report(p, verbose=False)
            except Exception as exc:  # noqa: BLE001 - a bad dump must not stop the sweep
                print(f"=== {p.name}: {exc}")
            print()
        return 0

    targets = [Path(a) for a in args if not a.startswith("--")]
    if not targets:
        found = dumps()
        if not found:
            print(f"no non-empty .dmp files in {GAME_DIR}")
            return 1
        targets = [found[0]]
    for p in targets:
        report(p)
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
