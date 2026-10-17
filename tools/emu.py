#!/usr/bin/env python3
"""Unicorn machine holding a port memory snapshot, the original CARPET.EXE and the port's own code.

Both the ORIGINAL function (IDA address) and the PORT function (remc1.exe, relocated to the snapshot's
image base) can be called on the same memory; every write is logged so a call can be undone. Port
imports (CRT) are served by Python stubs. Used by tools/diff_tick.py and tools/diff_funcs.py.
"""
import bisect
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UC_HOOK_INTR, UC_HOOK_MEM_INVALID, \
    UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, \
    UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP
from le_image import LeImage
from port_symbols import PortSymbols, SymTagFunction
from audit_data import PortAddrMap, name_addresses

PAGE = 0x1000
IDA_LO, IDA_HI = 0x10000, 0x150000
STACK_BASE, STACK_SIZE = 0x7F000000, 0x100000
TRAP = 0x7E000000
STUBS = 0x7D000000
HEAP_BASE, HEAP_SIZE = 0x7A000000, 0x2000000

SKIP_GLOBALS = {'tempScreenBuffer_A0000', 'unk_93AE0', 'off_96864', 'videoModes_12F080', 'pathStrArray_F1EF0',
                'unk_A1D34', 'str_254DBC', 'dword_999B8', 'off_9EF10', 'off_99A84', 'off_99B68'}
CLIP_GLOBALS = {'dword_9334C': 1024}
# sound: the original drives the HMI/SOS driver, the port SDL_mixer - neither is game logic
SKIP_FUNCS = ['sub_55370_558A0']


def read_dump(path):
    regions = []
    with open(path, 'rb') as f:
        assert f.read(8) == b'MC1SNAP1'
        while True:
            base, size = struct.unpack('<II', f.read(8))
            if size == 0:
                break
            regions.append((base, f.read(size)))
    return regions


def read_meta(path):
    meta = {}
    for line in open(path):
        k, *v = line.split()
        meta[k] = [int(x, 0) for x in v]
    return meta


class Dump:
    def __init__(self, regions):
        self.regions = sorted(regions)
        self.bases = [r[0] for r in self.regions]

    def read(self, addr, n):
        """Bytes [addr, addr+n) - may span adjacent regions; None if any byte is missing."""
        out = bytearray()
        while n > 0:
            i = bisect.bisect_right(self.bases, addr) - 1
            if i < 0:
                return None
            base, data = self.regions[i]
            o = addr - base
            if o >= len(data):
                return None
            take = min(n, len(data) - o)
            out += data[o:o + take]
            addr += take
            n -= take
        return bytes(out)


class Stop(Exception):
    pass


class Machine:
    def __init__(self, snapdir, exe='C:/mc1data/CD/CARPET.EXE', port='build/win-x86/RelWithDebInfo/remc1.exe',
                 load_port_code=True):
        self.meta = read_meta(os.path.join(snapdir, 'meta.txt'))
        self.image_base = self.meta['image_base'][0]
        self.state_addr, self.state_size = self.meta['state']
        self.before = read_dump(os.path.join(snapdir, 'before.bin'))
        self.bdump = Dump(self.before)
        a_path = os.path.join(snapdir, 'after.bin')
        self.after = Dump(read_dump(a_path)) if os.path.exists(a_path) else None
        self.img = LeImage(exe)
        self.omem = self.img.memory()
        self.ps = PortSymbols(port)
        self.amap = PortAddrMap(self.ps, self.img)
        self.sym_by_name = self.ps.by_name()
        ib = self.image_base
        self.image_lo = ib
        self.image_hi = ib + max(s[1] + max(s[2], s[4]) for s in self.ps.sections)
        self.mu = mu = Uc(UC_ARCH_X86, UC_MODE_32)
        self.mapped = []
        for base, data in sorted(self.before):
            lo, hi = base, base + len(data)
            if (hi <= IDA_LO or lo >= IDA_HI) and lo < HEAP_BASE:
                mu.mem_map(lo, hi - lo, UC_PROT_ALL)
                mu.mem_write(lo, data)
                self.mapped.append((lo, hi))
        mu.mem_map(IDA_LO, IDA_HI - IDA_LO, UC_PROT_ALL)
        for (obase, vsize, flags, pidx, pcnt) in self.img.objects:
            mu.mem_write(obase, bytes(self.omem[obase]))
        mu.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
        mu.mem_map(TRAP, PAGE, UC_PROT_ALL)
        mu.mem_write(TRAP, b'\xf4' * 16)
        mu.mem_map(STUBS, 0x10000, UC_PROT_ALL)
        mu.mem_write(STUBS, b'\xc3' * 0x10000)
        mu.mem_map(HEAP_BASE, HEAP_SIZE, UC_PROT_ALL)
        self.heap_ptr = HEAP_BASE
        self.stub_funcs = {}
        if load_port_code:
            self._load_port_code()
        self._copy_globals()
        self.wlog = []
        self.logging = False
        mu.hook_add(UC_HOOK_INTR, self._on_intr)
        mu.hook_add(UC_HOOK_MEM_INVALID, self._on_invalid)
        mu.hook_add(UC_HOOK_CODE, self._on_trap, begin=TRAP, end=TRAP + 16)
        mu.hook_add(UC_HOOK_CODE, self._on_stub, begin=STUBS, end=STUBS + 0xFFFF)
        mu.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        self.stop_reason = None
        # functions skipped on BOTH sides (platform code the port replaced): original address + port name
        self.skipped = {}
        for name in SKIP_FUNCS:
            from port_index import addr_from_name
            self.skip_function(addr_from_name(name), 0)
            if name in self.sym_by_name:
                self.skip_function(self.port_addr(name), 0)
        self.ctx = mu.context_save()  # clean CPU state (segments!) restored before every call

    def skip_function(self, addr, retval=0):
        """Make every call to addr return immediately with eax = retval."""
        def hook(uc, address, size, user):
            esp = uc.reg_read(UC_X86_REG_ESP)
            ret = struct.unpack('<I', bytes(uc.mem_read(esp, 4)))[0]
            uc.reg_write(UC_X86_REG_ESP, esp + 4)
            uc.reg_write(UC_X86_REG_EAX, retval)
            uc.reg_write(UC_X86_REG_EIP, ret)
        self.skipped[addr] = self.mu.hook_add(UC_HOOK_CODE, hook, begin=addr, end=addr)

    # ---- setup -----------------------------------------------------------------------------------
    def _load_port_code(self):
        ps, mu, ib = self.ps, self.mu, self.image_base
        pe = ps.pe
        delta = ib - ps.image_base
        relocs = sorted(ps.reloc_sites)
        for name, va, vsize, rawptr, rawsize in ps.sections:
            if name not in ('.text', '.rdata', '.idata', '.00cfg', '.tls'):
                continue
            lo = ib + va
            size = (max(vsize, rawsize) + PAGE - 1) & ~(PAGE - 1)
            if any(not (hi <= lo or l >= lo + size) for l, hi in self.mapped):
                # already part of the dump (writable section) - keep runtime contents
                continue
            data = bytearray(pe[rawptr:rawptr + min(rawsize, vsize)])
            data += b'\0' * (size - len(data))
            i = bisect.bisect_left(relocs, va)
            while i < len(relocs) and relocs[i] < va + size:
                o = relocs[i] - va
                if o + 4 <= len(data):
                    v = struct.unpack_from('<I', data, o)[0]
                    struct.pack_into('<I', data, o, (v + delta) & 0xFFFFFFFF)
                i += 1
            mu.mem_map(lo, size, UC_PROT_ALL)
            mu.mem_write(lo, bytes(data))
            self.mapped.append((lo, lo + size))
        self._bind_imports()

    def _bind_imports(self):
        ps, pe, mu, ib = self.ps, self.ps.pe, self.mu, self.image_base
        e = struct.unpack_from('<I', pe, 0x3C)[0]
        dd = e + 24 + 96
        imp_rva = struct.unpack_from('<I', pe, dd + 8)[0]
        p = ps.rva_to_off(imp_rva)
        k = 0
        while True:
            oft, ts, fc, name_rva, ft = struct.unpack_from('<IIIII', pe, p)
            if name_rva == 0:
                break
            q = ps.rva_to_off(oft or ft)
            j = 0
            while True:
                v = struct.unpack_from('<I', pe, q + 4 * j)[0]
                if v == 0:
                    break
                fname = f'ord{v & 0xFFFF}'
                if not v & 0x80000000:
                    ho = ps.rva_to_off(v)
                    fname = pe[ho + 2:pe.index(b'\0', ho + 2)].decode()
                stub = STUBS + 16 * k
                self.stub_funcs[stub] = fname
                mu.mem_write(ib + ft + 4 * j, struct.pack('<I', stub))
                k += 1
                j += 1
            p += 20

    def _copy_globals(self):
        """Port globals named after original addresses -> original addresses (pointers translated)."""
        mu = self.mu
        self.global_map = []  # (ida, size, port_addr, name)
        # bigger globals first: port arrays that were enlarged (or IDA "tables" that are really several
        # variables) cover other globals; the more specific (smaller) global must win
        owner = {}  # ida byte -> (static match, global) of the global currently owning it
        self.dropped = []
        for s in sorted(self.ps.data_symbols(), key=lambda s: -s.size):
            if s.name in SKIP_GLOBALS or s.size == 0:
                continue
            cands = name_addresses(s.name, self.img)
            if not cands:
                continue
            ida = cands[0]
            obase = self.img.object_at(ida)
            oend = obase + [o for o in self.img.objects if o[0] == obase][0][1]
            size = min(s.size, oend - ida, CLIP_GLOBALS.get(s.name, s.size))
            data = self.bdump.read(self.image_base + s.rva, size)
            if data is None:
                continue
            # does the port's static image agree with the original data at that address? A global whose
            # name carries a wrong address (other build) disagrees with the bytes it would cover.
            pinit = self.ps.initial_bytes(s.rva, size)
            oinit = bytes(self.omem[obase][ida - obase:ida - obase + size])
            nz = [i for i in range(size) if oinit[i] or pinit[i]]
            match = 1.0 if not nz else sum(1 for i in nz if oinit[i] == pinit[i]) / len(nz)
            overlapped = [owner[ida + i] for i in range(size) if ida + i in owner]
            if overlapped and any(m0 > match + 0.2 for m0, _ in overlapped):
                self.dropped.append((s.name, ida, size, match, overlapped[0][1]))
                continue
            for i in range(size):
                owner[ida + i] = (match, s.name)
            mu.mem_write(ida, self.translate_block(data, s.rva))
            self.global_map.append((ida, size, self.image_base + s.rva, s.name))
        self.global_map.sort()
        self.gm_pages = {}
        for g in self.global_map:
            for pg in range(g[0] >> 12, ((g[0] + g[1] - 1) >> 12) + 1):
                self.gm_pages.setdefault(pg, []).append(g)

    def translate_value(self, v):
        """port pointer -> original address (or None)."""
        if self.image_lo <= v < self.image_hi:
            sym, tida = self.amap.lookup(v - self.image_base)
            if tida is not None and (sym.tag == SymTagFunction or sym.name not in SKIP_GLOBALS):
                return tida
        return None

    def translate_block(self, data, rva=None):
        """Translate port pointers inside a global's bytes. Only dwords that are relocation sites of
        the port image, 4-byte aligned, or exact function starts - scanning every byte offset turns
        neighbouring table entries into fake pointers."""
        buf = bytearray(data)
        relocs = self.ps.reloc_sites
        o = 0
        while o < len(buf) - 3:
            v = struct.unpack_from('<I', buf, o)[0]
            ok = (rva is not None and (rva + o) in relocs) or o % 4 == 0
            # (unaligned non-relocated dwords are never translated: table bytes can form fake pointers,
            # e.g. two trig entries spelled the address of a port function)
            t = self.translate_value(v) if ok and self.image_lo <= v < self.image_hi else None
            if t is not None:
                struct.pack_into('<I', buf, o, t)
                o += 4
                continue
            o += 1
        return bytes(buf)

    def ida_to_port(self, a):
        best = None
        for g in self.gm_pages.get(a >> 12, ()):
            if g[0] <= a < g[0] + g[1] and (best is None or g[1] < best[1]):
                best = g
        if best:
            ida, size, paddr, name = best
            return paddr + (a - ida), name
        return None, None

    # ---- hooks -----------------------------------------------------------------------------------
    def _on_intr(self, uc, intno, user):
        self.stop_reason = f'interrupt {intno:#x} at {uc.reg_read(UC_X86_REG_EIP):#x}'
        uc.emu_stop()

    def _on_invalid(self, uc, access, address, size, value, user):
        self.stop_reason = f'invalid memory access (type {access}) at {address:#x}, eip {uc.reg_read(UC_X86_REG_EIP):#x}'
        return False

    def _on_trap(self, uc, address, size, user):
        uc.emu_stop()

    def _on_write(self, uc, access, address, size, value, user):
        if self.logging:
            self.wlog.append((address, bytes(uc.mem_read(address, size))))

    def arg(self, n):
        esp = self.mu.reg_read(UC_X86_REG_ESP)
        return struct.unpack('<I', bytes(self.mu.mem_read(esp + 4 + 4 * n, 4)))[0]

    def _on_stub(self, uc, address, size, user):
        name = self.stub_funcs.get(address, '?')
        a = self.arg
        r = 0
        if name in ('memcpy', 'memmove'):
            d, s, n = a(0), a(1), a(2)
            if n:
                data = bytes(uc.mem_read(s, n))
                self._logged_write(d, data)
            r = d
        elif name == 'memset':
            d, c, n = a(0), a(1), a(2)
            if n:
                self._logged_write(d, bytes([c & 0xFF]) * n)
            r = d
        elif name in ('malloc', 'calloc'):
            n = a(0) * (a(1) if name == 'calloc' else 1)
            r = self.heap_ptr
            self.heap_ptr += (n + 15) & ~15
        elif name in ('free', '__stdio_common_vfprintf', '__stdio_common_vsprintf', 'fflush', 'fputs', 'fputc',
                      '__acrt_iob_func', 'fwrite'):
            r = 0
        elif name == 'rand':
            self.stop_reason = 'port called rand()'
            uc.emu_stop()
        else:
            self.stop_reason = f'port called import {name}'
            uc.emu_stop()
        uc.reg_write(UC_X86_REG_EAX, r & 0xFFFFFFFF)

    def _logged_write(self, addr, data):
        if self.logging:
            self.wlog.append((addr, bytes(self.mu.mem_read(addr, len(data)))))
        self.mu.mem_write(addr, data)

    # ---- calls -----------------------------------------------------------------------------------
    def call(self, addr, args=(), max_insns=50_000_000):
        mu = self.mu
        mu.context_restore(self.ctx)
        esp = STACK_BASE + STACK_SIZE - 0x1000
        frame = struct.pack('<I', TRAP) + b''.join(struct.pack('<I', x & 0xFFFFFFFF) for x in args)
        mu.mem_write(esp, frame)
        mu.reg_write(UC_X86_REG_ESP, esp)
        for r in (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI,
                  UC_X86_REG_EBP):
            mu.reg_write(r, 0)
        self.stop_reason = None
        try:
            mu.emu_start(addr, TRAP, count=max_insns)
        except UcError as e:
            self.stop_reason = self.stop_reason or f'{e} at eip {mu.reg_read(UC_X86_REG_EIP):#x}'
        if self.stop_reason is None and mu.reg_read(UC_X86_REG_EIP) != TRAP:
            self.stop_reason = f'did not return (eip {mu.reg_read(UC_X86_REG_EIP):#x})'
        return mu.reg_read(UC_X86_REG_EAX)

    def begin(self):
        self.wlog = []
        self.logging = True

    def end(self):
        """Stop logging; return {addr: final byte} for every written byte (stack excluded)."""
        self.logging = False
        out = {}
        mu = self.mu
        for addr, old in self.wlog:
            if STACK_BASE <= addr < STACK_BASE + STACK_SIZE:
                continue
            for i in range(len(old)):
                out.setdefault(addr + i, None)
        if out:
            for a in list(out):
                out[a] = mu.mem_read(a, 1)[0]
        return out

    def undo(self):
        for addr, old in reversed(self.wlog):
            self.mu.mem_write(addr, old)
        self.wlog = []

    def port_addr(self, name):
        s = self.sym_by_name[name]
        return self.image_base + s.rva
