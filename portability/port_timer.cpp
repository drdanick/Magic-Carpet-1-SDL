#include "port_timer.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

constexpr double kPitHz = 1193181.666;

std::thread worker;
std::atomic<bool> running{ false };
std::atomic<uint64_t> dueTicks{ 0 };   // incremented by the real-time worker
uint64_t appliedTicks = 0;             // applied on the game thread
PortTimerCallback callback = nullptr;
double tickSeconds = 1.0 / 120.0;

bool deterministic = false;
int ticksPerFrame = 5;

bool replaying = false;
std::map<uint64_t, uint32_t> replayLog;
FILE* recordFile = nullptr;
uint64_t syncIndex = 0;

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
		dueTicks.fetch_add(1);
	}
}

void SyncPoint(bool isFrame)
{
	uint32_t n = 0;
	if (running && callback) {
		if (replaying) {
			auto it = replayLog.find(syncIndex);
			n = it == replayLog.end() ? 0 : it->second;
		}
		else if (deterministic) {
			n = isFrame ? ticksPerFrame : 1;
		}
		else {
			uint64_t due = dueTicks.load();
			n = (uint32_t)(due - appliedTicks);
			appliedTicks = due;
		}
		for (uint32_t i = 0; i < n; i++)
			callback();
		if (recordFile && n)
			fprintf(recordFile, "T %llu %u\n", (unsigned long long)syncIndex, n);
	}
	syncIndex++;
}

} // namespace

void PortTimer_SetDeterministic(bool enabled, int perFrame)
{
	deterministic = enabled;
	ticksPerFrame = perFrame > 0 ? perFrame : 1;
}

bool PortTimer_IsDeterministic()
{
	return deterministic || replaying;
}

void PortTimer_SetReplayLog(std::map<uint64_t, uint32_t> log)
{
	replayLog = std::move(log);
	replaying = !replayLog.empty();
}

void PortTimer_SetRecordFile(FILE* f)
{
	recordFile = f;
}

void PortTimer_Start(uint32_t divisor, PortTimerCallback isr)
{
	PortTimer_Stop();
	if (divisor == 0)
		divisor = 0x10000; // PIT semantics: 0 means 65536
	tickSeconds = divisor / kPitHz;
	callback = isr;
	running = true;
	appliedTicks = dueTicks.load();
	if (!deterministic && !replaying)
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
	SyncPoint(true);
}

void PortTimer_Yield()
{
	SyncPoint(false);
	if (!deterministic && !replaying)
		std::this_thread::sleep_for(std::chrono::microseconds(500));
}
