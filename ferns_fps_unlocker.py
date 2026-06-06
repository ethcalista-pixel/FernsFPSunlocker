import ctypes, ctypes.wintypes as W, struct, subprocess, sys, time, os
from ctypes import c_void_p, c_ulonglong, c_size_t, byref, create_string_buffer, WinDLL

VERSION = "2.0"
TARGET  = "Ferns.exe"

k32   = WinDLL("kernel32", use_last_error=True)
psapi = WinDLL("psapi",    use_last_error=True)

PROCESS_ALL = 0x1F0FFF
MEM_COMMIT  = 0x1000
PAGE_RW     = 0x04
PAGE_WC     = 0x08
PAGE_GUARD  = 0x100
WRITABLE    = {PAGE_RW, PAGE_WC}

# frame times for common caps  (float + double, little-endian)
CAPS = [
    (struct.pack("<f", 1/60.), struct.pack("<f", 1/9999.), "60  f32"),
    (struct.pack("<d", 1/60.), struct.pack("<d", 1/9999.), "60  f64"),
    (struct.pack("<f", 1/30.), struct.pack("<f", 1/9999.), "30  f32"),
    (struct.pack("<d", 1/30.), struct.pack("<d", 1/9999.), "30  f64"),
    (struct.pack("<f", 1/20.), struct.pack("<f", 1/9999.), "20  f32"),
    (struct.pack("<d", 1/20.), struct.pack("<d", 1/9999.), "20  f64"),
]


# ── structs ───────────────────────────────────────────────────────────────────

class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress",       c_ulonglong), ("AllocationBase",    c_ulonglong),
        ("AllocationProtect", W.DWORD),     ("__p1",              W.DWORD),
        ("RegionSize",        c_ulonglong), ("State",             W.DWORD),
        ("Protect",           W.DWORD),     ("Type",              W.DWORD),
        ("__p2",              W.DWORD),
    ]

class MODINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", c_void_p), ("SizeOfImage", W.DWORD), ("EntryPoint", c_void_p)]


# ── win32 helpers ─────────────────────────────────────────────────────────────

def pid_of(name):
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
            text=True, stderr=subprocess.DEVNULL)
        for line in out.strip().splitlines():
            if name.lower() in line.lower():
                p = line.split(",")
                if len(p) >= 2:
                    return int(p[1].strip().strip('"'))
    except Exception:
        pass
    return None

def open_proc(pid):
    h = k32.OpenProcess(PROCESS_ALL, False, pid)
    if not h:
        e = ctypes.get_last_error()
        bail("access denied — run as administrator" if e == 5 else f"OpenProcess failed ({e})")
    return h

def read_mem(h, addr, n):
    buf = create_string_buffer(n)
    rd  = c_size_t(0)
    k32.ReadProcessMemory(h, c_void_p(addr), buf, n, byref(rd))
    return buf.raw[:rd.value]

def write_mem(h, addr, data):
    buf = (ctypes.c_byte * len(data))(*data)
    wr  = c_size_t(0)
    return bool(k32.WriteProcessMemory(h, c_void_p(addr), buf, len(data), byref(wr)))

def get_module_info(h):
    mods   = (W.HMODULE * 512)()
    needed = W.DWORD(0)
    psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), byref(needed), 0x03)
    count = needed.value // ctypes.sizeof(W.HMODULE)
    if count == 0:
        return None, 0
    mi = MODINFO()
    psapi.GetModuleInformation(h, mods[0], byref(mi), ctypes.sizeof(mi))
    return mi.lpBaseOfDll, mi.SizeOfImage


# ── PE parser — pull .data section bounds from the live process ───────────────

def data_section(h, base):
    raw = read_mem(h, base, 0x1000)
    if len(raw) < 0x40 or raw[:2] != b"MZ":
        return None, 0

    pe = struct.unpack_from("<I", raw, 0x3C)[0]
    if pe + 6 > len(raw) or raw[pe:pe+4] != b"PE\0\0":
        return None, 0

    nsects   = struct.unpack_from("<H", raw, pe + 6)[0]
    optsz    = struct.unpack_from("<H", raw, pe + 20)[0]
    soff     = pe + 24 + optsz

    for i in range(nsects):
        s     = soff + i * 40
        name  = raw[s:s+8].rstrip(b"\x00")
        vrva  = struct.unpack_from("<I", raw, s + 12)[0]
        vsz   = struct.unpack_from("<I", raw, s + 16)[0]
        chars = struct.unpack_from("<I", raw, s + 36)[0]
        # writable + not executable = .data / .bss
        if (chars & 0x80000000) and not (chars & 0x20000000):
            return base + vrva, vsz

    return None, 0


# ── scanner ───────────────────────────────────────────────────────────────────

def scan_region(h, start, size, needle):
    chunk = read_mem(h, start, size)
    hits  = []
    pos   = 0
    while True:
        idx = chunk.find(needle, pos)
        if idx == -1:
            break
        hits.append(start + idx)
        pos = idx + 1
    return hits

def patch_region(h, start, size):
    total = 0
    for needle, rep, _ in CAPS:
        for addr in scan_region(h, start, size, needle):
            if write_mem(h, addr, rep):
                total += 1
    return total

def full_writable_scan(h):
    mbi  = MBI()
    addr = 0
    total = 0
    while k32.VirtualQueryEx(h, c_void_p(addr), byref(mbi), ctypes.sizeof(mbi)):
        if mbi.RegionSize == 0:
            break
        if mbi.State == MEM_COMMIT and (mbi.Protect & ~PAGE_GUARD) in WRITABLE:
            total += patch_region(h, addr, mbi.RegionSize)
        addr += mbi.RegionSize
    return total


# ── misc ──────────────────────────────────────────────────────────────────────

def bail(msg):
    print(f"\n  error: {msg}")
    input("\n  press enter to exit ")
    sys.exit(1)

def hr():
    print("  " + "─" * 38)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    os.system("cls" if os.name == "nt" else "clear")
    print(f"\n  ferns fps unlocker  v{VERSION}\n")
    hr()

    pid = pid_of(TARGET)
    if not pid:
        bail(f"{TARGET} not running — open the game first")

    h = open_proc(pid)

    base, img_size = get_module_info(h)
    ds, dsz = (None, 0)
    if base:
        ds, dsz = data_section(h, base)

    if base:
        print(f"  pid      {pid}")
        print(f"  module   0x{base:X}   ({img_size // 1024 // 1024} MB)")
        if ds:
            print(f"  .data    0x{ds:X}   ({dsz // 1024} KB)")
    else:
        print(f"  pid      {pid}  (module resolve failed, using full scan)")

    hr()
    print()

    cycle = 0
    try:
        while True:
            cycle += 1

            if ds:
                n = patch_region(h, ds, dsz)
            else:
                n = full_writable_scan(h)

            if cycle == 1:
                if n:
                    print(f"  patched  {n} value(s)  —  fps cap removed")
                    print(f"  looping  re-patching every 100ms  (ctrl+c to stop)\n")
                else:
                    print("  nothing patched")
                    print("  the cap value wasn't in .data — trying full scan next pass\n")
                    ds, dsz = None, 0  # fall back to full scan

            time.sleep(0.1)

    except KeyboardInterrupt:
        pass

    k32.CloseHandle(h)
    print("\n  stopped.")
    input("  press enter to exit ")


if __name__ == "__main__":
    main()
