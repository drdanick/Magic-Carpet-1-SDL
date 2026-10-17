// Tier 1: port/platform behaviour that needs no game data.
#include <gtest/gtest.h>

#include "sub_main.h"
#include "portability/InputScript.h"
#include "portability/port_timer.h"
#include "portability/Settings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

// Defined in sub_main.cpp / port_sdl_vga_mouse.cpp without public declarations.
extern std::string gameDataPath;
extern std::string gameDataPath2;
char* FixPath(char* inPath);
uint16_t fixchar(uint16_t loclastchar);

namespace {

std::string Fix(const char* p)
{
	char buf[260];
	snprintf(buf, sizeof(buf), "%s", p);
	return FixPath(buf);
}

} // namespace

// --- DOS path mapping (sub_main.cpp FixPath) -------------------------------------------------

TEST(FixPath, CdRelativePathsGoToCdFolder)
{
	gameDataPath = "/data/CD";
	gameDataPath2 = "/data";
	EXPECT_EQ(Fix("data/build0-0.dat"), "/data/CD/data/build0-0.dat");
	EXPECT_EQ(Fix("data\\screens\\language.pal"), "/data/CD/data\\screens\\language.pal");
}

TEST(FixPath, InstallFolderAnyCaseAnySeparator)
{
	gameDataPath = "/data/CD";
	gameDataPath2 = "/data";
	EXPECT_EQ(Fix("C:\\carpet.cd\\language.inf"), "/data/carpet.cd\\language.inf");
	EXPECT_EQ(Fix("c:/CARPET.CD/save/carpdd00.gam"), "/data/CARPET.CD/save/carpdd00.gam");
	// lowercase form used by the save-scan code; was mapped into the CD folder before the port fix
	EXPECT_EQ(Fix("c:/carpet.cd/save/scanned.rmd"), "/data/carpet.cd/save/scanned.rmd");
}

// --- Keyboard: SDL scancode -> BIOS scan/ASCII (port_sdl_vga_mouse.cpp fixchar) -------------

TEST(Keyboard, ScancodeTranslation)
{
	auto tr = [](SDL_Scancode sc) { return fixchar((uint16_t)(sc << 8)); };
	EXPECT_EQ(tr(SDL_SCANCODE_ESCAPE), 0x011b);
	EXPECT_EQ(tr(SDL_SCANCODE_RETURN), 0x1c0d);
	EXPECT_EQ(tr(SDL_SCANCODE_SPACE), 0x3920);
	EXPECT_EQ(tr(SDL_SCANCODE_A), 0x1e61);
	EXPECT_EQ(tr(SDL_SCANCODE_1), 0x0231);
	EXPECT_EQ(tr(SDL_SCANCODE_UP), 0x4800);
	EXPECT_EQ(tr(SDL_SCANCODE_DOWN), 0x5000);
	EXPECT_EQ(tr(SDL_SCANCODE_LEFT), 0x4b00);
	EXPECT_EQ(tr(SDL_SCANCODE_RIGHT), 0x4d00);
	EXPECT_EQ(tr(SDL_SCANCODE_F1), 0x3b00);
	EXPECT_EQ(tr(SDL_SCANCODE_LALT), 0x3800);
}

TEST(Keyboard, WasdMapsToArrowsWhenEnabled)
{
	const bool saved = g_settings.wasdAsArrows;
	g_settings.wasdAsArrows = true;
	EXPECT_EQ(Port_RemapKey(SDL_SCANCODE_W), SDL_SCANCODE_UP);
	EXPECT_EQ(Port_RemapKey(SDL_SCANCODE_S), SDL_SCANCODE_DOWN);
	EXPECT_EQ(Port_RemapKey(SDL_SCANCODE_A), SDL_SCANCODE_LEFT);
	EXPECT_EQ(Port_RemapKey(SDL_SCANCODE_D), SDL_SCANCODE_RIGHT);
	EXPECT_EQ(Port_RemapKey(SDL_SCANCODE_Q), SDL_SCANCODE_Q);
	g_settings.wasdAsArrows = false;
	EXPECT_EQ(Port_RemapKey(SDL_SCANCODE_W), SDL_SCANCODE_W);
	g_settings.wasdAsArrows = saved;
}

TEST(Keyboard, TabMapsToEnterWhenEnabled)
{
	const bool saved = g_settings.tabAsEnter;
	g_settings.tabAsEnter = true;
	EXPECT_EQ(Port_RemapKey(SDL_SCANCODE_TAB), SDL_SCANCODE_RETURN);
	EXPECT_EQ(Port_RemapKey(SDL_SCANCODE_RETURN), SDL_SCANCODE_RETURN);
	g_settings.tabAsEnter = false;
	EXPECT_EQ(Port_RemapKey(SDL_SCANCODE_TAB), SDL_SCANCODE_TAB);
	g_settings.tabAsEnter = saved;
}

TEST(Keyboard, InjectKeyFillsPressedTable)
{
	VGA_mouse_clear_keys();
	VGA_InjectKey(true, SDL_SCANCODE_UP);
	EXPECT_EQ(pressedKeys_12EEF0_12EEE0[0x48], 0x48);
	EXPECT_EQ(lastPressedKey_12EF70_12EF60, 0x48);
	VGA_InjectKey(false, SDL_SCANCODE_UP);
	EXPECT_EQ(pressedKeys_12EEF0_12EEE0[0x48], 0);
}

// --- Timer -----------------------------------------------------------------------------------

namespace {
int g_ticks = 0;
void CountTick() { g_ticks++; }
} // namespace

TEST(PortTimer, DeterministicTicksPerFrameAndYield)
{
	PortTimer_SetDeterministic(true, 5);
	g_ticks = 0;
	PortTimer_Start(9903, CountTick);
	EXPECT_EQ(g_ticks, 0);
	PortTimer_OnFrame();
	EXPECT_EQ(g_ticks, 5);
	PortTimer_Yield();
	EXPECT_EQ(g_ticks, 6);
	PortTimer_Stop();
	PortTimer_OnFrame();
	EXPECT_EQ(g_ticks, 6) << "no ticks after stop";
	PortTimer_SetDeterministic(false, 5);
}

TEST(PortTimer, RealTimeRateIsAboutPitRate)
{
	PortTimer_SetDeterministic(false, 5);
	g_ticks = 0;
	PortTimer_Start(9903, CountTick); // ~120.5 Hz
	PortTimer_OnFrame(); // first sync point starts the clock
	g_ticks = 0;
	SDL_Delay(500);
	EXPECT_EQ(g_ticks, 0) << "ticks must only be applied at sync points (game thread)";
	PortTimer_OnFrame();
	PortTimer_Stop();
	EXPECT_GT(g_ticks, 45);
	EXPECT_LT(g_ticks, 75);
}

// --- Input scripts ---------------------------------------------------------------------------

TEST(InputScript, KeyEventsAreAppliedAtTheirFrame)
{
	auto path = std::filesystem::temp_directory_path() / "remc1_test_script.txt";
	{
		std::ofstream f(path);
		f << "# comment\n"
		  << "10 keydown Up\n"
		  << "12 keyup Up\n"
		  << "20 key Space   # auto release 3 frames later\n";
	}
	VGA_mouse_clear_keys();
	ASSERT_TRUE(InputScript_Load(path.string()));
	InputScript_OnFrame(9);
	EXPECT_EQ(pressedKeys_12EEF0_12EEE0[0x48], 0);
	InputScript_OnFrame(10);
	EXPECT_EQ(pressedKeys_12EEF0_12EEE0[0x48], 0x48);
	InputScript_OnFrame(12);
	EXPECT_EQ(pressedKeys_12EEF0_12EEE0[0x48], 0);
	InputScript_OnFrame(20);
	EXPECT_EQ(pressedKeys_12EEF0_12EEE0[0x39], 0x39);
	InputScript_OnFrame(23);
	EXPECT_EQ(pressedKeys_12EEF0_12EEE0[0x39], 0);
	std::filesystem::remove(path);
}
