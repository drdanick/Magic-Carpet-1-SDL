#pragma once
// Replacement for the DOS timer interrupt (INT 8 / PIT channel 0).
//
// The game reprograms the PIT (divisor 9903 -> ~120.5 Hz) and installs an ISR that increments
// tick counters. Here the ISR body is called either
//   * real-time:     from a background thread paced by std::chrono::steady_clock, or
//   * deterministic: synchronously, a fixed number of ticks per presented frame and one tick per
//                    PortTimer_Yield() (used by replay tests).
//
// Game code that busy-waits on a tick counter must call PortTimer_Yield() inside the loop.

#include <cstdint>

using PortTimerCallback = void (*)();

// divisor: value programmed into PIT channel 0 (1193182 / divisor Hz). Restarts if already running.
void PortTimer_Start(uint32_t divisor, PortTimerCallback isr);
void PortTimer_Stop();
bool PortTimer_Running();

// Deterministic mode must be selected before PortTimer_Start.
void PortTimer_SetDeterministic(bool enabled, int ticksPerFrame);
bool PortTimer_IsDeterministic();

// Called once per presented frame by the video layer.
void PortTimer_OnFrame();
// Called from busy-wait loops in game code.
void PortTimer_Yield();
