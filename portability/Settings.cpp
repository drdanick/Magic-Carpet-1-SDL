#include "Settings.h"

#include "fcaseopen.h"
#include "InputScript.h"
#include "port_timer.h"
#include "port_filesystem.h"
#include "../engine/CommandLineParser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

Remc1Settings g_settings;

static std::string CaseFixed(const std::string& path)
{
#if defined(_WIN32)
	return path;
#else
	return casepath(path);
#endif
}

bool IsValidDataPath(const std::string& path)
{
	if (path.empty())
		return false;
	std::error_code ec;
	return fs::is_directory(CaseFixed(path + "/CD/DATA"), ec);
}

std::string GetCdDataPath()
{
	return CaseFixed(g_settings.dataPath + "/CD");
}

std::string GetInstallRootPath()
{
	return g_settings.dataPath;
}

static std::vector<fs::path> UserDirs()
{
	std::vector<fs::path> dirs;
#if defined(_WIN32)
	if (const char* appdata = std::getenv("APPDATA"))
		dirs.emplace_back(fs::path(appdata) / "remc1");
	if (const char* profile = std::getenv("USERPROFILE"))
		dirs.emplace_back(fs::path(profile) / "remc1");
#elif defined(__APPLE__)
	if (const char* home = std::getenv("HOME"))
		dirs.emplace_back(fs::path(home) / "Library" / "Application Support" / "remc1");
#else
	if (const char* xdg = std::getenv("XDG_DATA_HOME"))
		dirs.emplace_back(fs::path(xdg) / "remc1");
	if (const char* home = std::getenv("HOME"))
		dirs.emplace_back(fs::path(home) / ".local" / "share" / "remc1");
#endif
	return dirs;
}

static std::vector<fs::path> ConfigCandidates()
{
	std::vector<fs::path> c;
#if !defined(_WIN32) && !defined(__APPLE__)
	if (const char* xdg = std::getenv("XDG_CONFIG_HOME"))
		c.emplace_back(fs::path(xdg) / "remc1" / "config.json");
	if (const char* home = std::getenv("HOME"))
		c.emplace_back(fs::path(home) / ".config" / "remc1" / "config.json");
#endif
	for (auto& d : UserDirs())
		c.emplace_back(d / "config.json");
	c.emplace_back(fs::path(get_exe_path()) / "config.json");
	return c;
}

static void LoadConfigFile(const fs::path& path)
{
	std::ifstream in(path);
	if (!in)
		return;
	nlohmann::json j;
	try {
		in >> j;
	}
	catch (const std::exception& e) {
		std::cerr << "remc1: ignoring malformed config " << path.string() << ": " << e.what() << "\n";
		return;
	}
	g_settings.configPath = path.string();
	auto& s = g_settings;
	if (j.contains("paths")) {
		auto& p = j["paths"];
		if (p.contains("data") && !p["data"].get<std::string>().empty()) {
			fs::path d = p["data"].get<std::string>();
			if (d.is_relative())
				d = path.parent_path() / d;
			s.dataPath = d.string();
		}
	}
	if (j.contains("graphics")) {
		auto& g = j["graphics"];
		s.fullscreen = g.value("fullscreen", s.fullscreen);
		s.windowWidth = g.value("windowWidth", s.windowWidth);
		s.windowHeight = g.value("windowHeight", s.windowHeight);
		s.maintainAspectRatio = g.value("maintainAspectRatio", s.maintainAspectRatio);
	}
	if (j.contains("input")) {
		s.grabMouse = j["input"].value("grabMouse", s.grabMouse);
	}
	if (j.contains("debug")) {
		s.debugLog = j["debug"].value("log", s.debugLog);
	}
}

static void PrintUsage()
{
	std::cout <<
		"remc1 - Magic Carpet 1 port\n"
		"Port options:\n"
		"  --data <dir>       game data folder (created by tools/extract_data.py)\n"
		"  --config <file>    config.json to use\n"
		"  --fullscreen       start fullscreen\n"
		"  --window WxH       window size (default 1280x960)\n"
		"  --no-grab          do not confine the mouse to the window\n"
		"  --debug-log        write debug.txt next to the executable\n"
		"  --dump-frames <dir> [--dump-every N]  save every Nth presented frame (default 30) as BMP\n"
		"  --exit-after-frames N                  quit after N presented frames\n"
		"  --deterministic [N]                    game timer advances N ticks (default 5) per frame instead of real time\n"
		"  --input-script <file>                 play scripted input (format: portability/InputScript.h)\n"
		"  --record-input <file>                  record live input in the same format\n"
		"Original game options (-level N, -cheat N, -detail N, -network, ...) are passed through.\n";
}

std::vector<std::string> InitSettings(int argc, char* argv[])
{
	CommandLineParams.Init(1, argv); // REMC2 legacy flags: defaults only

	std::vector<std::string> passthrough;
	passthrough.emplace_back(argc > 0 ? argv[0] : "remc1");

	std::string cliData, cliConfig;
	bool cliFullscreen = false, cliNoGrab = false, cliDebugLog = false;
	int cliW = 0, cliH = 0;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&](const char* what) -> std::string {
			if (i + 1 >= argc) {
				std::cerr << "remc1: " << a << " needs " << what << "\n";
				return {};
			}
			return argv[++i];
		};
		if (a == "--data") cliData = next("a directory");
		else if (a == "--config") cliConfig = next("a file");
		else if (a == "--fullscreen") cliFullscreen = true;
		else if (a == "--no-grab") cliNoGrab = true;
		else if (a == "--debug-log") cliDebugLog = true;
		else if (a == "--dump-frames") g_settings.dumpFramesDir = next("a directory");
		else if (a == "--dump-every") g_settings.dumpEvery = (std::max)(1, atoi(next("a number").c_str()));
		else if (a == "--exit-after-frames") g_settings.exitAfterFrames = atoi(next("a number").c_str());
		else if (a == "--deterministic") {
			int perFrame = 5;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				perFrame = atoi(argv[++i]);
			PortTimer_SetDeterministic(true, perFrame);
		}
		else if (a == "--input-script") { if (!InputScript_Load(next("a file"))) return {}; }
		else if (a == "--record-input") { if (!InputScript_StartRecording(next("a file"))) return {}; }
		else if (a == "--window") {
			std::string v = next("WxH");
			if (sscanf(v.c_str(), "%dx%d", &cliW, &cliH) != 2) { cliW = cliH = 0; }
		}
		else if (a == "--help" || a == "-h") { PrintUsage(); return {}; }
		else if (a.rfind("--", 0) == 0) {
			std::cerr << "remc1: unknown option " << a << " (see --help)\n";
			return {};
		}
		else passthrough.push_back(a);
	}

	if (!cliConfig.empty())
		LoadConfigFile(cliConfig);
	else
		for (auto& c : ConfigCandidates()) {
			std::error_code ec;
			if (fs::exists(c, ec)) { LoadConfigFile(c); break; }
		}

	if (const char* env = std::getenv("REMC1_DATA_DIR"))
		g_settings.dataPath = env;
	if (!cliData.empty())
		g_settings.dataPath = cliData;
	if (cliFullscreen) g_settings.fullscreen = true;
	if (cliNoGrab) g_settings.grabMouse = false;
	if (cliDebugLog) g_settings.debugLog = true;
	if (cliW > 0 && cliH > 0) { g_settings.windowWidth = cliW; g_settings.windowHeight = cliH; }

	if (g_settings.dataPath.empty()) {
		std::vector<fs::path> search;
		search.emplace_back(fs::path(get_exe_path()) / "data");
		for (auto& d : UserDirs())
			search.emplace_back(d / "data");
		for (auto& s : search)
			if (IsValidDataPath(s.string())) { g_settings.dataPath = s.string(); break; }
	}

	if (!IsValidDataPath(g_settings.dataPath)) {
		std::cerr << "remc1: no game data found"
			<< (g_settings.dataPath.empty() ? std::string() : " at '" + g_settings.dataPath + "'") << ".\n"
			"Extract it from your GOG install with:\n"
			"  python tools/extract_data.py --gog \"<GOG Magic Carpet Plus folder>\" --out <datadir>\n"
			"then run with --data <datadir> (or set REMC1_DATA_DIR, or paths.data in config.json).\n";
		return {};
	}
	// normalise separators; game code concatenates with '/'
	g_settings.dataPath = fs::path(g_settings.dataPath).generic_string();
	while (g_settings.dataPath.size() > 1 && g_settings.dataPath.back() == '/')
		g_settings.dataPath.pop_back();
	return passthrough;
}
