#!/usr/bin/env python3
"""Unicorn machine holding a port memory snapshot, the original CARPET.EXE and the port's own code.

Both the ORIGINAL function (IDA address) and the PORT function (remc1.exe, relocated to the snapshot's
image base) can be called on the same memory; every write is logged so a call can be undone. Port
imports (CRT) are served by Python stubs. Used by tools/diff_tick.py and tools/diff_funcs.py.
"""
import bisect
import os
import remc1_paths  # noqa: E402
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL, UC_PROT_READ, UC_HOOK_INTR, UC_HOOK_MEM_INVALID, \
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
                'unk_A1D34', 'str_254DBC', 'dword_999B8', 'off_9EF10', 'off_99A84', 'off_99B68',
                # pointer into sprite data whose header layout differs between the two programs; always set before use
                'sprData_B5CB0_B5CA0'}
CLIP_GLOBALS = {'dword_9334C': 1024}
# original bytes without a port counterpart that are no behaviour: the high words of the viewport width and
# height (dwords in the original, uint16 in the port)
# and the font table pointers at 0xAE3B8 (two dwords in the original, the struct array dword_AE3B8_2 in the port)
LAYOUT_IGNORE_IDA = [(0x93ADA, 0x93ADC), (0x93ADE, 0x93AE0), (0xAE3B8, 0xAE3C0)]
LAYOUT_IGNORE_PORT = ['dword_AE3B8_2']  # whole port symbols on the port side (12-byte font structs)
# sound: the original drives the HMI/SOS driver, the port SDL_mixer - neither is game logic
SKIP_FUNCS = ['sub_55370_558A0', 'sub_55890_55DC0',
              # port-only diagnostics called from game functions (the snapshot hooks themselves)
              'MemSnapshot_Call', 'MemSnapshot_Tick']


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
    def __init__(self, snapdir, exe=None, port=None,
                 load_port_code=True):
        exe = exe or remc1_paths.original_exe()
        port = port or os.environ.get('REMC1_PORT_EXE') or remc1_paths.find_port_exe()
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
        # port-only instrumentation globals (call counters etc.): writes to them are not differences
        self.port_only = set()
        for s_ in self.ps.data_symbols():
            if s_.name.startswith(('counter_', 'compare_', 'debugcounter')) or s_.name == 'DrawGameFrame_20FB0_index':
                self.port_only.update(range(self.image_base + s_.rva, self.image_base + s_.rva + max(s_.size, 1)))
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
        if not any(lo < PAGE for lo, _ in self.mapped):
            # DOS low memory (IVT/BIOS data) is readable under DOS/4GW, and the original reads it
            # through null entity pointers (docs/original-bugs.md #3): map it read-only, zeroed
            mu.mem_map(0, PAGE, UC_PROT_READ)
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
        self.watch = set()  # addresses whose writes are printed (diff_tick.py --watch)
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
            oa = addr_from_name(name)
            if oa is not None:
                self.skip_function(oa, 0)
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

    def _global_syms(self):
        """Data symbols, plus the address-named members of struct globals that have no address in their
        own name (e.g. strPal.fog_B7934_B7924: the renderer's fog/blend tables)."""
        from port_symbols import Sym, SymTagData
        out = list(self.ps.data_symbols())
        self.member_syms = set()
        for s in self.ps.data_symbols():
            if s.size < 64 or name_addresses(s.name, self.img) or s.name in SKIP_GLOBALS:
                continue
            for mname, off, mt, msize in self.ps.members(s.type_index):
                if msize and name_addresses(mname, self.img):
                    out.append(Sym(mname, s.rva + off, msize, SymTagData, mt))
                    self.member_syms.add(mname)
        return out

    def _copy_globals(self):
        """Port globals named after original addresses -> original addresses (pointers translated)."""
        mu = self.mu
        self.global_map = []  # (ida, size, port_addr, name)
        # bigger globals first: port arrays that were enlarged (or IDA "tables" that are really several
        # variables) cover other globals; the more specific (smaller) global must win
        owner = {}  # ida byte -> (static match, global) of the global currently owning it
        self.dropped = []
        # linker-like layout: a global ends where the next address-named global begins (port arrays
        # were often enlarged - word_CC070 ran into mapTerrainType_CC1E0 - and IDA "tables" such as
        # trig_9074C cover separately named variables). Static data cut off this way keeps the
        # original's own (identical) bytes.
        gsyms = self._global_syms()
        spans = []
        for s in gsyms:
            c = name_addresses(s.name, self.img)
            if c and s.size:
                spans.append((c[0], s.size, s.name))
        # an unk_ placeholder lying entirely inside another global (e.g. unk_110003 inside the 128 KB
        # mapEntityIndex) is IDA junk, not a variable: neither a boundary nor copied
        self.junk = set()
        # The same holds for a name that only carries a DOSBox-style 6-digit address (snds00Tab_2854A1 ->
        # 0xC74A1 lies inside the 64 KB blend table strPal.byte_BB934): it names other memory.
        import re
        for a0, n0, nm in spans:
            toks = [int(t, 16) for t in re.findall(r'_([0-9A-F]{5,6})(?=_|$)', nm)]
            dosbox_only = bool(toks) and all(t >= 0x200000 for t in toks)  # DOSBox = IDA + 0x1BE000
            # (only inside struct-member tables: enlarged port arrays would otherwise swallow e.g. begWscreen)
            inside = [n2 for a1, n1, n2 in spans if a1 < a0 and a0 + n0 <= a1 + n1]
            if (nm.startswith('unk_') and inside) or (dosbox_only and any(n2 in self.member_syms for n2 in inside)):
                self.junk.add(nm)
        # skipped globals are no boundary either (pathStrArray_F1EF0 would cut the 64 KB mapShading_EC1E0)
        starts = sorted({a0 for a0, n0, nm in spans if nm not in self.junk and nm not in SKIP_GLOBALS})
        self.clip_end = {}
        for i, a0 in enumerate(starts[:-1]):
            self.clip_end[a0] = starts[i + 1]
        for s in sorted(gsyms, key=lambda s: -s.size):
            if s.name in SKIP_GLOBALS or s.size == 0 or s.name in self.junk:
                continue
            cands = name_addresses(s.name, self.img)
            if not cands:
                continue
            ida = cands[0]
            obase = self.img.object_at(ida)
            oend = obase + [o for o in self.img.objects if o[0] == obase][0][1]
            size = min(s.size, oend - ida, CLIP_GLOBALS.get(s.name, s.size), self.clip_end.get(ida, ida + s.size) - ida)
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
            mu.mem_write(ida, self.translate_block(data, s.rva, s))
            self.global_map.append((ida, size, self.image_base + s.rva, s.name))
        self.global_map.sort()
        # skipped globals (scratch buffers, platform state): writes to them are no differences
        self.skip_ida, self.skip_port = list(LAYOUT_IGNORE_IDA), []
        for s in self.ps.data_symbols():
            if s.name in SKIP_GLOBALS and s.size:
                self.skip_port.append((self.image_base + s.rva, self.image_base + s.rva + s.size))
                c = name_addresses(s.name, self.img)
                if c:  # clipped like the copies (tempScreenBuffer_A0000 is far bigger in the port)
                    nxt = min([a0 for a0 in self.clip_end.values() if a0 > c[0]] or [c[0] + s.size])
                    self.skip_ida.append((c[0], min(c[0] + s.size, nxt)))
        self.gm_pages = {}
        for g in self.global_map:
            for pg in range(g[0] >> 12, ((g[0] + g[1] - 1) >> 12) + 1):
                self.gm_pages.setdefault(pg, []).append(g)
        # the port side of the known layout differences
        for n in LAYOUT_IGNORE_PORT:
            sym = self.sym_by_name.get(n)
            if sym:
                self.skip_port.append((self.image_base + sym.rva, self.image_base + sym.rva + sym.size))
        for lo, hi in LAYOUT_IGNORE_IDA:
            for a in range(lo, hi):
                pa, _ = self.ida_to_port(a)
                if pa is not None:
                    self.skip_port.append((pa, pa + 1))

    def translate_value(self, v):
        """port pointer -> original address (or None)."""
        if self.image_lo <= v < self.image_hi:
            sym, tida = self.amap.lookup(v - self.image_base)
            if tida is not None and (sym.tag == SymTagFunction or sym.name not in SKIP_GLOBALS):
                return tida
        return None

    def translate_block(self, data, rva=None, sym=None):
        """Translate port pointers inside a global's bytes to original addresses - only at the pointer
        fields the PDB type of the global declares (guessing by value turned packed table words and
        entity-index pairs into fake pointers). Falls back to relocation sites without a type."""
        buf = bytearray(data)
        if sym is not None and sym.type_index:
            offs = self.ps.pointer_offsets(sym.type_index)
        elif rva is not None:
            offs = [r - rva for r in self.ps.reloc_sites if rva <= r < rva + len(buf)]
        else:
            offs = []
        for o in offs:
            if o + 4 > len(buf):
                continue
            v = struct.unpack_from('<I', buf, o)[0]
            if self.image_lo <= v < self.image_hi:
                t = self.translate_value(v)
                if t is not None:
                    struct.pack_into('<I', buf, o, t)
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
        if self.watch and any(address <= w < address + size for w in self.watch):
            eip = uc.reg_read(UC_X86_REG_EIP)
            esp = uc.reg_read(UC_X86_REG_ESP)
            stack = struct.unpack('<64I', bytes(uc.mem_read(esp, 256)))
            rets = [hex(v) for v in stack if 0x10000 <= v < 0x90000][:8]
            if hasattr(self, 'watch_log'):
                self.watch_log.append((address, eip, value))
            print(f'  write {address:#x} size {size} value {value & ((1 << 8 * size) - 1):#x} at eip {eip:#x}; '
                  f'stack code addresses {" ".join(rets)}')

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
        elif name in ('_libm_sse2_pow_precise',):
            from unicorn.x86_const import UC_X86_REG_XMM0, UC_X86_REG_XMM1
            x0 = uc.reg_read(UC_X86_REG_XMM0) & ((1 << 64) - 1)
            x1 = uc.reg_read(UC_X86_REG_XMM1) & ((1 << 64) - 1)
            a_, b_ = struct.unpack('<d', struct.pack('<Q', x0))[0], struct.unpack('<d', struct.pack('<Q', x1))[0]
            try:
                res = a_ ** b_
            except (OverflowError, ZeroDivisionError, ValueError):
                res = float('nan')
            uc.reg_write(UC_X86_REG_XMM0, struct.unpack('<Q', struct.pack('<d', float(res)))[0])
            return
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
    def call(self, addr, args=(), max_insns=50_000_000, nested=False):
        """Call addr(args) (cdecl) until it returns. nested=True uses the lower half of the stack area
        (for calls made while an outer emulation, e.g. a tick, is paused)."""
        mu = self.mu
        mu.context_restore(self.ctx)
        esp = STACK_BASE + (STACK_SIZE // 2 if nested else STACK_SIZE) - 0x1000
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

    def trace_calls(self, addr, args, nested=True):
        """Call addr(args) logging every direct call made at depth 1: [(target name, args, eax)]. Undone."""
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        if not hasattr(self, '_fnames'):
            self._fnames = {self.image_base + s.rva: s.name for s in self.ps.syms if s.tag == SymTagFunction}
        out, pending = [], []
        start_esp = [None]

        def hook(uc, address, size, user):
            esp = uc.reg_read(UC_X86_REG_ESP)
            if start_esp[0] is None:
                start_esp[0] = esp
            if pending and address == pending[-1][0] and esp >= pending[-1][2]:
                out[pending[-1][1]][2] = uc.reg_read(UC_X86_REG_EAX)
                pending.pop()
            if pending:
                return
            ins = next(md.disasm(bytes(uc.mem_read(address, size)), address), None)
            if ins is None or ins.mnemonic != 'call':
                return
            tgt = ins.op_str
            try:
                t = int(tgt, 16)
                b = bytes(uc.mem_read(t, 5))
                if b[0] == 0xE9:
                    t = t + 5 + struct.unpack('<i', b[1:])[0]
                tgt = self._fnames.get(t, f'{t:#x}')
            except ValueError:
                pass
            a = struct.unpack('<4I', bytes(uc.mem_read(esp, 16)))
            out.append([tgt, a, None])
            pending.append((address + size, len(out) - 1, esp))

        h = self.mu.hook_add(UC_HOOK_CODE, hook)
        self.begin()
        self.call(addr, args, nested=nested)
        self.end()
        self.undo()
        self.mu.hook_del(h)
        return out

    def port_addr(self, name):
        s = self.sym_by_name[name]
        return self.image_base + s.rva
