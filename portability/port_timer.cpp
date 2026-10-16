#include "port_timer.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace {

constexpr double kPitHz = 1193181.666;

std::thread worker;
std::atomic<bool> running{ false };
std::atomic<PortTimerCallback> callback{ nullptr };
double tickSeconds = 1.0 / 120.0;

bool deterministic = false;
int ticksPerFrame = 5;

void Worker()
{
	using clock = std::chrono::steady_clock;
	auto next = clock::now();
	const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(tickSeconds));
	while (running.load()) {
		next += period;
		auto now = clock::now();
		if (next > now)
			std::this_thread::sleep_until(next);
		else if (now - next > period * 30)
			next = now; // fell far behind (debugger break, etc.): don't burst hundreds of ticks
		if (auto cb = callback.load())
			cb();
	}
}

} // namespace

void PortTimer_SetDeterministic(bool enabled, int perFrame)
{
	deterministic = enabled;
	ticksPerFrame = perFrame > 0 ? perFrame : 1;
}

bool PortTimer_IsDeterministic()
{
	return deterministic;
}

void PortTimer_Start(uint32_t divisor, PortTimerCallback isr)
{
	PortTimer_Stop();
	if (divisor == 0)
		divisor = 0x10000; // PIT semantics: 0 means 65536
	tickSeconds = divisor / kPitHz;
	callback = isr;
	running = true;
	if (!deterministic)
		worker = std::thread(Worker);
}

void PortTimer_Stop()
{
	running = false;
	if (worker.joinable())
		worker.join();
	callback = nullptr;
}

bool PortTimer_Running()
{
	return running.load();
}

void PortTimer_OnFrame()
{
	if (!deterministic || !running)
		return;
	if (auto cb = callback.load())
		for (int i = 0; i < ticksPerFrame; i++)
			cb();
}

void PortTimer_Yield()
{
	if (deterministic) {
		if (running)
			if (auto cb = callback.load())
				cb();
	}
	else {
		std::this_thread::sleep_for(std::chrono::microseconds(500));
	}
}
