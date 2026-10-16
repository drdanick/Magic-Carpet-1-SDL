#pragma once
// Port-level settings (not part of the original game). Filled from, in increasing priority:
//   built-in defaults -> config.json -> environment -> command line ("--xxx" options).
// Original DOS game options ("-level 3", "-cheat 1", ...) are passed through untouched to
// ProcessCommandLine_34DD0_35190.

#include <string>
#include <vector>

struct Remc1Settings {
	// Root of the extracted data (see tools/extract_data.py): contains CD/ and CARPET.CD/
	std::string dataPath;
	std::string configPath; // config.json actually loaded ("" if none)
	std::string cDrivePath; // overrides the folder standing in for DOS C: (default: dataPath); used by replay tests

	// Window / presentation
	bool fullscreen = false;
	int windowWidth = 1280;
	int windowHeight = 960;
	bool maintainAspectRatio = true;
	bool vsync = true;
	// Frame pacing. The game logic advances once per frame (the DOS original ran as fast as the CPU
	// allowed), so gameplay needs a cap; menus are paced like the VGA retrace (70 Hz in 320x200).
	int maxGameFps = 30;        // 0 = uncapped
	int menuFps = 70;           // menu/movie frame cap and emulated vertical retrace rate
	bool grabMouse = true;
	bool wasdAsArrows = true;   // testing convenience: W/A/S/D send the arrow keys (can't type them then)

	// Audio
	bool sound = true; // sound effects (SDL_mixer)
	bool music = true;

	// Diagnostics
	bool debugLog = false; // write debug.txt next to the executable
	std::string dumpFramesDir; // if set, every dumpEvery-th presented frame is saved as BMP here
	int dumpEvery = 30;
	int exitAfterFrames = 0;   // >0: quit after this many presented frames (automation)
	std::string frameHashFile; // if set, "<frame> <hash>" of framebuffer+palette is logged per frame
	bool frameStats = false;   // --frame-stats: print frame-interval percentiles every 5 s (pacing work)
	bool headless = false;     // SDL dummy video driver; live keyboard/mouse input is ignored
	int watchdogSeconds = -1;  // freeze watchdog; -1 = default (20 s for automated runs, off otherwise), 0 = off
};

extern Remc1Settings g_settings;

// Parses port options out of argv, loads config.json and resolves the data path.
// Returns the remaining arguments (argv[0] first) for the original game parser.
// On a fatal problem (e.g. no data found) prints a message and returns an empty vector.
std::vector<std::string> InitSettings(int argc, char* argv[]);

// Directory containing the read-only CD data (<dataPath>/CD) - replaces the original CD folder "\CARPET"
std::string GetCdDataPath();
// Directory that stands in for the DOS C: drive root (<dataPath>); the game itself appends "carpet.cd\..."
std::string GetInstallRootPath();

// True if <path>/CD/DATA exists (case-insensitive on Linux/macOS)
bool IsValidDataPath(const std::string& path);
