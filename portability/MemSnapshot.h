#pragma once
#include <cstddef>

// Memory snapshots for the differential test harness (tools/diff_tick.py): with --snapshot-frame N the
// game dumps its writable memory right before and right after the entity tick of presented frame N,
// then exits. The harness replays the tick with the ORIGINAL machine code (CARPET.EXE under Unicorn) on
// the "before" image and compares the result with the "after" image.

// Called at the start (before=true) and end of UpdateEntities_41780_41AC0. state/settings are the game
// state (str_AE400_AE3F0) and settings (str_AE408_AE3F8) blocks, recorded in meta.txt.
void MemSnapshot_Tick(bool before, const void* state, size_t stateSize, const void* settings, size_t settingsSize);
