// Music (portability/port_music): HMP song length, the tick-driven "song playing" state the game polls, the
// HMI timer events used by the danger-music fade, and (with REMC1_DATA_DIR) every song shipped with the game.
#include <gtest/gtest.h>

#include "support/test_support.h"
#include "portability/port_music.h"
#include "portability/port_timer.h"
#include "portability/Settings.h"

#include <cstdlib>
#include <fstream>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;
using namespace remc1test;

int RncUnpack_62B60_63070(uint8_t* input, uint8_t* output);

namespace {

void Put32(std::vector<uint8_t>& d, size_t at, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		d[at + i] = (uint8_t)(v >> (8 * i));
}

// HMP delta: 7 bits per byte, low group first, bit 7 marks the last byte
void PutDelta(std::vector<uint8_t>& t, uint32_t v)
{
	while (v >= 0x80) {
		t.push_back((uint8_t)(v & 0x7F));
		v >>= 7;
	}
	t.push_back((uint8_t)(v | 0x80));
}

std::vector<uint8_t> MakeHmp(const std::vector<std::vector<uint8_t>>& tracks, uint32_t rate = 120)
{
	std::vector<uint8_t> d(0x308, 0);
	memcpy(d.data(), "HMIMIDIP", 8);
	Put32(d, 0x30, (uint32_t)tracks.size());
	Put32(d, 0x34, 120);
	Put32(d, 0x38, rate);
	for (size_t i = 0; i < tracks.size(); i++) {
		size_t at = d.size();
		d.resize(at + 12);
		Put32(d, at, (uint32_t)i);
		Put32(d, at + 4, (uint32_t)(12 + tracks[i].size()));
		d.insert(d.end(), tracks[i].begin(), tracks[i].end());
	}
	return d;
}

// one note held for `ticks`, with a running-status note off
std::vector<uint8_t> NoteTrack(uint32_t ticks)
{
	std::vector<uint8_t> t;
	PutDelta(t, 0);
	t.insert(t.end(), { 0x92, 60, 100 });
	PutDelta(t, ticks);
	t.insert(t.end(), { 60, 0 }); // running status: note on, velocity 0
	PutDelta(t, 0);
	t.insert(t.end(), { 0xFF, 0x2F, 0x00 });
	return t;
}

struct DeterministicClock {
	DeterministicClock() { PortTimer_SetDeterministic(true, 5); }
	~DeterministicClock() { PortTimer_SetDeterministic(false, 5); }
};

} // namespace

TEST(Music, SongTicksIsTheLongestTrack)
{
	auto hmp = MakeHmp({ NoteTrack(100), NoteTrack(240), NoteTrack(20000) });
	EXPECT_EQ(PortMusic_SongTicks(hmp.data(), hmp.size(), 120.0), 20000u);
	EXPECT_EQ(PortMusic_SongTicks(hmp.data(), hmp.size(), 60.0), 10000u); // ticks of another rate
	auto slow = MakeHmp({ NoteTrack(240) }, 60);
	EXPECT_EQ(PortMusic_SongTicks(slow.data(), slow.size(), 120.0), 480u);
}

TEST(Music, SongTicksRejectsOtherData)
{
	std::vector<uint8_t> junk(0x400, 0x55);
	EXPECT_EQ(PortMusic_SongTicks(junk.data(), junk.size(), 120.0), 0u);
	auto hmp = MakeHmp({ NoteTrack(240) });
	EXPECT_EQ(PortMusic_SongTicks(hmp.data(), 0x300, 120.0), 0u); // truncated header
	EXPECT_EQ(PortMusic_SongTicks(hmp.data(), hmp.size() - 4, 120.0), 0u); // truncated track: not counted
}

TEST(Music, PlayingStateFollowsTimerTicks)
{
	DeterministicClock clock;
	auto hmp = MakeHmp({ NoteTrack(240) });
	PortMusic_Load(hmp.data(), hmp.size());
	EXPECT_FALSE(PortMusic_IsPlaying()); // loaded, not started
	const uint64_t ticks = PortMusic_SongTicks(hmp.data(), hmp.size(), PortTimer_TickHz());
	ASSERT_GT(ticks, 100u);

	PortMusic_Start();
	for (uint64_t i = 0; i < 50; i++)
		PortTimer_Yield();
	EXPECT_TRUE(PortMusic_IsPlaying());
	PortMusic_Stop(); // pauses: start resumes with the rest of the song
	EXPECT_FALSE(PortMusic_IsPlaying());
	for (int i = 0; i < 1000; i++)
		PortTimer_Yield();
	PortMusic_Start();
	for (uint64_t i = 50; i < ticks; i++)
		PortTimer_Yield();
	EXPECT_TRUE(PortMusic_IsPlaying());
	PortTimer_Yield();
	PortTimer_Yield();
	EXPECT_FALSE(PortMusic_IsPlaying()); // done

	PortMusic_Rewind(); // the game's restart: reset + start
	PortMusic_Start();
	EXPECT_TRUE(PortMusic_IsPlaying());
	PortMusic_Stop();
}

TEST(PortTimer, EventsRunAtTheirRate)
{
	DeterministicClock clock;
	static int calls = 0;
	calls = 0;
	const int handle = PortTimer_AddEvent(20, [] { calls++; });
	ASSERT_GE(handle, 0);
	const int ticks = (int)std::lround(PortTimer_TickHz() * 3); // 3 s of ticks
	for (int i = 0; i < ticks; i++)
		PortTimer_Yield();
	PortTimer_RemoveEvent(handle);
	EXPECT_NEAR(calls, 60, 1);
	for (int i = 0; i < 100; i++)
		PortTimer_Yield();
	EXPECT_NEAR(calls, 60, 1); // removed
}

// Every song in DATA/MUSIC*.DAT: a valid HMP whose length matches the duration in its header and, when
// the sequencer is built in, the length the sequencer plays.
TEST(GameData, MusicSongsParse)
{
	std::string data = DataDir();
	if (data.empty())
		GTEST_SKIP() << "set REMC1_DATA_DIR to the extracted game data";
	auto load = [](const fs::path& p) {
		std::vector<uint8_t> raw = ReadFile(p);
		if (raw.size() < 18 || memcmp(raw.data(), "RNC\x01", 4) != 0)
			return raw;
		const uint32_t size = (uint32_t(raw[4]) << 24) | (uint32_t(raw[5]) << 16) | (uint32_t(raw[6]) << 8) | raw[7];
		GameBytes in(raw.begin(), raw.end()), out(size);
		EXPECT_EQ((uint32_t)RncUnpack_62B60_63070(in.data(), out.data()), size) << p;
		return std::vector<uint8_t>(out.begin(), out.end());
	};
	int songs = 0;
	for (auto& e : fs::directory_iterator(fs::path(data) / "CD" / "DATA")) {
		std::string name = e.path().filename().string();
		std::transform(name.begin(), name.end(), name.begin(), ::toupper);
		if (name.rfind("MUSIC", 0) != 0 || e.path().extension().string().size() != 4 || name.find(".DAT") == std::string::npos)
			continue;
		fs::path tabPath = e.path();
		tabPath.replace_extension(e.path().extension() == ".DAT" ? ".TAB" : ".tab");
		auto dat = load(e.path()), tab = load(tabPath);
		for (size_t entry = 32; entry + 32 <= tab.size(); entry += 32, songs++) {
			uint32_t offset, length;
			memcpy(&offset, &tab[entry + 18], 4);
			memcpy(&length, &tab[entry + 26], 4);
			ASSERT_LE((size_t)offset + length, dat.size()) << name << " entry " << entry / 32;
			const uint8_t* song = dat.data() + offset;
			const uint64_t ticks = PortMusic_SongTicks(song, length, 120.0);
			uint32_t headerSeconds;
			memcpy(&headerSeconds, song + 0x3C, 4);
			EXPECT_GT(ticks, 0u) << name << " entry " << entry / 32;
			EXPECT_NEAR(ticks / 120.0, headerSeconds, 2.0) << name << " entry " << entry / 32;
			const double seq = PortMusic_SequencerSeconds(song, length);
			if (seq >= 0) // the sequencer adds a 1 s wait for the release tails after the last event
				EXPECT_NEAR(seq, ticks / 120.0 + 1.0, 0.01) << name << " entry " << entry / 32;
		}
	}
	EXPECT_GE(songs, 20);
}

// The Roland card needs a complete MT-32 / CM-32L ROM set: none without a folder or with other files in it;
// with REMC1_MT32_ROMS (a folder with the ROMs) in an MT-32 build, the set is found.
TEST(Music, Mt32NeedsRoms)
{
	const std::string savedRoms = g_settings.mt32Roms, savedModel = g_settings.mt32Model;
	g_settings.mt32Model = "auto";
	g_settings.mt32Roms = "";
	EXPECT_FALSE(PortMusic_Mt32Available());
	const fs::path dir = fs::temp_directory_path() / "remc1_test_mt32_roms";
	fs::create_directories(dir);
	std::ofstream(dir / "MT32_CONTROL.ROM", std::ios::binary) << std::string(65536, 'x');
	g_settings.mt32Roms = dir.string();
	EXPECT_FALSE(PortMusic_Mt32Available()); // the right size and name, but not a ROM Munt knows
	fs::remove(dir / "MT32_CONTROL.ROM");
	fs::remove(dir);
	const char* roms = getenv("REMC1_MT32_ROMS");
	if (roms && *roms) {
		g_settings.mt32Roms = roms;
#if REMC1_MT32
		EXPECT_TRUE(PortMusic_Mt32Available()) << roms;
#else
		EXPECT_FALSE(PortMusic_Mt32Available()); // built without Munt
#endif
	}
	g_settings.mt32Roms = savedRoms;
	g_settings.mt32Model = savedModel;
}
