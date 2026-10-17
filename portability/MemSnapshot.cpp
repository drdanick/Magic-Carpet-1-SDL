#include "MemSnapshot.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "Settings.h"
#include "port_sdl_vga_mouse.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
bool g_inTick = false;

std::string Dir()
{
	return g_settings.snapshotDir.empty() ? std::string("snapshot") : g_settings.snapshotDir;
}

#ifdef _WIN32
// File format: "MC1SNAP1", then records { uint32 base, uint32 size, bytes }, terminated by {0, 0}.
// Only committed, readable+writable memory that belongs to the process heap/allocations (MEM_PRIVATE)
// or to remc1.exe's own image (its .data/.bss) is written.
bool WriteDump(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "wb");
	if (!f)
		return false;
	fwrite("MC1SNAP1", 1, 8, f);
	HMODULE exe = GetModuleHandleA(nullptr);
	uint8_t* p = nullptr;
	MEMORY_BASIC_INFORMATION mbi;
	while (VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		uint8_t* next = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
		bool rw = (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
		bool keep = mbi.State == MEM_COMMIT && rw && !(mbi.Protect & PAGE_GUARD) &&
			(mbi.Type == MEM_PRIVATE || (mbi.Type == MEM_IMAGE && mbi.AllocationBase == (void*)exe));
		if (keep)
		{
			uint32_t hdr[2] = { (uint32_t)(uintptr_t)mbi.BaseAddress, (uint32_t)mbi.RegionSize };
			fwrite(hdr, 4, 2, f);
			fwrite(mbi.BaseAddress, 1, mbi.RegionSize, f);
		}
		if (next <= p)
			break;
		p = next;
	}
	uint32_t end[2] = { 0, 0 };
	fwrite(end, 4, 2, f);
	fclose(f);
	return true;
}
#else
bool WriteDump(const std::string&) { return false; }
#endif
} // namespace

void MemSnapshot_Tick(bool before, const void* state, size_t stateSize, const void* settings, size_t settingsSize)
{
	if (g_settings.snapshotFrame < 0)
		return;
	if (before)
	{
		if ((int)VGA_PresentedFrames() < g_settings.snapshotFrame || g_inTick)
			return;
		g_inTick = true;
		std::string dir = Dir();
#ifdef _WIN32
		CreateDirectoryA(dir.c_str(), nullptr);
#endif
		FILE* m = fopen((dir + "/meta.txt").c_str(), "w");
		if (m)
		{
#ifdef _WIN32
			fprintf(m, "image_base %#x\n", (unsigned)(uintptr_t)GetModuleHandleA(nullptr));
#endif
			fprintf(m, "frame %u\nstate %#x %u\nsettings %#x %u\n", VGA_PresentedFrames(), (unsigned)(uintptr_t)state,
				(unsigned)stateSize, (unsigned)(uintptr_t)settings, (unsigned)settingsSize);
			fclose(m);
		}
		if (!WriteDump(dir + "/before.bin"))
			fprintf(stderr, "snapshot: cannot write %s/before.bin\n", dir.c_str());
	}
	else if (g_inTick)
	{
		std::string dir = Dir();
		WriteDump(dir + "/after.bin");
		fprintf(stderr, "snapshot: frame %u written to %s\n", VGA_PresentedFrames(), dir.c_str());
		fflush(stderr);
		exit(0);
	}
}
