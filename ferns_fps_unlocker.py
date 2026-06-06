import ctypes, ctypes.wintypes as W, struct, subprocess, sys, os
from ctypes import c_void_p, c_ulonglong, c_size_t, byref, create_string_buffer, WinDLL

VERSION = "1.1"
TARGET  = "Ferns.exe"

k32 = WinDLL("kernel32", use_last_error=True)

PROCESS_ALL = 0x1F0FFF
MEM_COMMIT  = 0x1000

PAGE_READWRITE         = 0x04
PAGE_WRITECOPY         = 0x08
PAGE_EXECUTE_READWRITE = 0x40
PAGE_EXECUTE_WRITECOPY = 0x80
PAGE_GUARD             = 0x100

WRITABLE = {PAGE_READWRITE, PAGE_WRITECOPY, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY}

CAPS = [
    (struct.pack("<f", 1/60.), struct.pack("<f", 1/9999.)),
    (struct.pack("<d", 1/60.), struct.pack("<d", 1/9999.)),
    (struct.pack("<f", 1/30.), struct.pack("<f", 1/9999.)),
    (struct.pack("<d", 1/30.), struct.pack("<d", 1/9999.)),
]


class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress",       c_ulonglong), ("AllocationBase",    c_ulonglong),
        ("AllocationProtect", W.DWORD),     ("__p1",              W.DWORD),
        ("RegionSize",        c_ulonglong), ("State",             W.DWORD),
        ("Protect",           W.DWORD),     ("Type",              W.DWORD),
        ("__p2",              W.DWORD),
    ]


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

def scan_and_patch(h):
    mbi   = MBI()
    addr  = 0
    total = 0

    while k32.VirtualQueryEx(h, c_void_p(addr), byref(mbi), ctypes.sizeof(mbi)):
        if mbi.RegionSize == 0:
            break
        if mbi.State == MEM_COMMIT and (mbi.Protect & ~PAGE_GUARD) in WRITABLE:
            chunk = read_mem(h, addr, mbi.RegionSize)
            for needle, rep in CAPS:
                pos = 0
                while True:
                    idx = chunk.find(needle, pos)
                    if idx == -1:
                        break
                    if write_mem(h, addr + idx, rep):
                        total += 1
                    pos = idx + 1
        addr += mbi.RegionSize

    return total

def bail(msg):
    print(f"\n  error: {msg}")
    input("\n  press enter to exit ")
    sys.exit(1)


def main():
    os.system("cls" if os.name == "nt" else "clear")
    print(f"\n  ferns fps unlocker  v{VERSION}")
    print(f"  {'─' * 30}\n")

    pid = pid_of(TARGET)
    if not pid:
        bail(f"{TARGET} not running — open the game first")

    print(f"  pid      {pid}")
    h = open_proc(pid)

    print("  scanning memory...\n")
    n = scan_and_patch(h)

    if n:
        print(f"  patched  {n} value(s)")
        print(f"  fps cap removed")
    else:
        print("  nothing found — try running as administrator")

    k32.CloseHandle(h)
    print()
    input("  press enter to exit ")


if __name__ == "__main__":
    main()
