#!/usr/bin/env python3
"""Global symbols of the built port (remc1.exe + remc1.pdb) and their initial bytes.

Uses dbghelp (Windows) to enumerate the PDB's data symbols (name, RVA, size) and reads the initial
contents from the PE image (.data/.rdata; uninitialised .bss reads as zeros). PE base relocations tell
which dwords are pointers.

    ps = PortSymbols('build/win-x86/RelWithDebInfo/remc1.exe')
    for s in ps.data_symbols(): s.name, s.rva, s.size
    ps.initial_bytes(rva, size)
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys

SYMOPT_UNDNAME = 0x2
SYMOPT_DEFERRED_LOADS = 0x4
SYMOPT_LOAD_LINES = 0x10
SymTagData = 7
SymTagFunction = 5
TI_GET_LENGTH = 2
TI_GET_TYPE = 4


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 1024)]


class Sym:
    __slots__ = ('name', 'rva', 'size', 'tag', 'type_index')

    def __init__(self, name, rva, size, tag, ti):
        self.name, self.rva, self.size, self.tag, self.type_index = name, rva, size, tag, ti

    def __repr__(self):
        return f'Sym({self.name}, rva={self.rva:#x}, size={self.size})'


class PortSymbols:
    BASE = 0x10000000

    def __init__(self, exe):
        self.exe = os.path.abspath(exe)
        self.pe = open(self.exe, 'rb').read()
        self._parse_pe()
        self._load_syms()

    # ---- PE ----------------------------------------------------------------------------------------
    def _parse_pe(self):
        pe = self.pe
        e_lfanew = struct.unpack_from('<I', pe, 0x3C)[0]
        nsec = struct.unpack_from('<H', pe, e_lfanew + 6)[0]
        optsize = struct.unpack_from('<H', pe, e_lfanew + 20)[0]
        opt = e_lfanew + 24
        self.image_base = struct.unpack_from('<I', pe, opt + 28)[0]
        dd = opt + 96
        reloc_rva, reloc_size = struct.unpack_from('<II', pe, dd + 5 * 8)
        self.sections = []
        so = opt + optsize
        for i in range(nsec):
            name = pe[so:so + 8].rstrip(b'\0').decode()
            vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', pe, so + 8)
            self.sections.append((name, va, vsize, rawptr, rawsize))
            so += 40
        self.reloc_sites = set()
        if reloc_rva:
            p = self.rva_to_off(reloc_rva)
            end = p + reloc_size
            while p < end:
                page, blk = struct.unpack_from('<II', pe, p)
                if blk == 0:
                    break
                for k in range((blk - 8) // 2):
                    e = struct.unpack_from('<H', pe, p + 8 + 2 * k)[0]
                    if e >> 12 == 3:  # HIGHLOW
                        self.reloc_sites.add(page + (e & 0xFFF))
                p += blk

    def rva_to_off(self, rva):
        for name, va, vsize, rawptr, rawsize in self.sections:
            if va <= rva < va + max(vsize, rawsize):
                o = rva - va
                return rawptr + o if o < rawsize else None
        return None

    def section_of(self, rva):
        for name, va, vsize, rawptr, rawsize in self.sections:
            if va <= rva < va + max(vsize, rawsize):
                return name
        return None

    def initial_bytes(self, rva, size):
        out = bytearray(size)
        for i in range(size):
            o = self.rva_to_off(rva + i)
            if o is not None and o < len(self.pe):
                out[i] = self.pe[o]
        return bytes(out)

    # ---- PDB ---------------------------------------------------------------------------------------
    def _load_syms(self):
        dbg = ctypes.WinDLL('dbghelp.dll')
        self.dbg = dbg
        self.h = wt.HANDLE(0x7357)
        dbg.SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS)
        dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
        if not dbg.SymInitialize(self.h, os.path.dirname(self.exe).encode(), False):
            raise OSError('SymInitialize failed')
        dbg.SymLoadModuleEx.restype = ctypes.c_uint64
        dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
                                        wt.DWORD, ctypes.c_void_p, wt.DWORD]
        base = dbg.SymLoadModuleEx(self.h, None, self.exe.encode(), None, self.BASE, len(self.pe), None, 0)
        if not base:
            raise OSError('SymLoadModuleEx failed')
        self.syms = []
        CB = ctypes.WINFUNCTYPE(wt.BOOL, ctypes.POINTER(SYMBOL_INFO), wt.ULONG, ctypes.c_void_p)

        def cb(pinfo, size, ctx):
            si = pinfo.contents
            name = si.Name[:si.NameLen].decode('latin-1')
            self.syms.append(Sym(name, si.Address - self.BASE, si.Size, si.Tag, si.TypeIndex))
            return True

        self._cb = CB(cb)
        dbg.SymEnumSymbols.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_char_p, CB, ctypes.c_void_p]
        dbg.SymEnumSymbols(self.h, self.BASE, b'*', self._cb, None)
        dbg.SymGetTypeInfo.argtypes = [wt.HANDLE, ctypes.c_uint64, wt.ULONG, ctypes.c_int, ctypes.c_void_p]
        for s in self.syms:
            if s.tag == SymTagData and s.size == 0 and s.type_index:
                ln = ctypes.c_uint64(0)
                if dbg.SymGetTypeInfo(self.h, self.BASE, s.type_index, TI_GET_LENGTH, ctypes.byref(ln)):
                    s.size = ln.value

    def data_symbols(self):
        return [s for s in self.syms if s.tag == SymTagData]

    def by_name(self):
        return {s.name: s for s in self.syms}


if __name__ == '__main__':
    ps = PortSymbols(sys.argv[1] if len(sys.argv) > 1 else 'build/win-x86/RelWithDebInfo/remc1.exe')
    d = ps.data_symbols()
    print(len(ps.syms), 'symbols,', len(d), 'data;', len(ps.reloc_sites), 'relocations')
    bn = ps.by_name()
    for n in ('creatureTypes_98F38', 'str_9688C', 'perfFont_96894', 'dword_999B8', 'mapHeightmap_DC1E0_DC1D0'):
        s = bn.get(n)
        print(n, s, s and ps.section_of(s.rva))
