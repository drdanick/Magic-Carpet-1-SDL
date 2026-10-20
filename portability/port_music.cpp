#include "port_music.h"
#include "port_sound.h"
#include "port_timer.h"
#include "Settings.h"

#ifdef _MSC_VER
#include "SDL.h"
#include "SDL_mixer.h"
#else
#include "SDL2/SDL.h"
#include "SDL2/SDL_mixer.h"
#endif

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if REMC1_MUSIC
#include <adlmidi.h>
#include "midi_sequencer.hpp"
#endif
#if REMC1_FLUIDSYNTH
#include <fluidsynth.h>
#endif
#if REMC1_MUSIC && defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#endif

namespace {

bool EqualsNoCase(const char* a, const char* b)
{
	for (; *a && *b; a++, b++)
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	return *a == *b;
}

uint32_t ReadLE32(const uint8_t* p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// --- Game-visible song state (tick driven) -----------------------------------------------------------
PortMusicDevice currentDevice = PortMusicDevice::None;
bool songLoaded = false;
uint64_t songTicks = 0;     // length in timer ticks
uint64_t songPosition = 0;  // ticks played before the last stop (start resumes from there)
uint64_t songEndTick = 0;
bool songPlaying = false;
const bool debugMusic = getenv("REMC1_DEBUG_MUSIC") != nullptr; // log song commands and MIDI sent by the game

uint64_t SongTicksPlayed()
{
	if (!songPlaying)
		return songPosition;
	const uint64_t now = PortTimer_TickCount();
	return now >= songEndTick ? songTicks : songTicks - (songEndTick - now);
}

#if REMC1_MUSIC
// --- Synths --------------------------------------------------------------------------------------------
class Synth {
public:
	virtual ~Synth() = default;
	virtual void NoteOn(uint8_t channel, uint8_t note, uint8_t velocity) = 0;
	virtual void NoteOff(uint8_t channel, uint8_t note) = 0;
	virtual void NoteAfterTouch(uint8_t channel, uint8_t note, uint8_t value) = 0;
	virtual void ChannelAfterTouch(uint8_t channel, uint8_t value) = 0;
	virtual void ControllerChange(uint8_t channel, uint8_t type, uint8_t value) = 0;
	virtual void PatchChange(uint8_t channel, uint8_t patch) = 0;
	virtual void PitchBend(uint8_t channel, uint8_t msb, uint8_t lsb) = 0;
	virtual void SysEx(const uint8_t* msg, size_t size) = 0;
	virtual void Reset() = 0;
	// Interleaved stereo S16.
	virtual void Render(int16_t* out, int frames) = 0;
	// Plays on its own device (nothing to render): the sequencer runs on a thread of its own instead of
	// the SDL_mixer music hook.
	virtual bool Realtime() const { return false; }
};

class AdlSynth : public Synth {
public:
	// hmi: set up like the game's HMI FM driver (its banks follow with LoadBnk); otherwise libADLMIDI's
	// default General MIDI bank plays the General MIDI songs.
	AdlSynth(int rate, bool hmi)
	{
		player_ = adl_init(rate);
		if (!player_)
			return;
		const bool nuked = EqualsNoCase(g_settings.oplEmulator.c_str(), "nuked");
		adl_switchEmulator(player_, nuked ? ADLMIDI_EMU_NUKED : ADLMIDI_EMU_DOSBOX);
		adl_setNumChips(player_, hmi ? 1 : 2);
		if (hmi) {
			adl_setNumFourOpsChn(player_, 0);
			adl_setVolumeRangeModel(player_, ADLMIDI_VolumeModel_HMI);
			adl_setHTremolo(player_, 1);
			adl_setHVibrato(player_, 1);
		}
	}
	~AdlSynth() override
	{
		if (player_)
			adl_close(player_);
	}
	bool Ok() const { return player_ != nullptr; }

	// AdLib BNK (version 1.0, "ADLIB-"): header, 12-byte name records (data index, flags, name[9]) and
	// 30-byte instrument records (mode, voice, modulator[13], carrier[13], two waveforms). The game's banks
	// have 128 entries each: melodic = program number, percussion = MIDI note; for percussion the record's
	// flags byte is the note the drum sounds at (HMI convention, as in libADLMIDI's HMI bank loader).
	bool LoadBnk(const uint8_t* d, size_t size, bool percussive)
	{
		if (!player_ || size < 28 || memcmp(d + 2, "ADLIB-", 6) != 0)
			return false;
		const unsigned used = d[8] | (d[9] << 8);
		const uint32_t names = ReadLE32(d + 12), data = ReadLE32(d + 16);
		ADL_BankId id = { (ADL_UInt8)(percussive ? 1 : 0), 0, 0 };
		ADL_Bank bank;
		if (adl_getBank(player_, &id, ADLMIDI_Bank_Create, &bank) < 0)
			return false;
		for (unsigned n = 0; n < used && n < 128; n++) {
			const size_t rec = names + n * 12;
			if (rec + 12 > size)
				return false;
			const unsigned index = d[rec] | (d[rec + 1] << 8);
			const size_t ins = data + (size_t)index * 30;
			if (ins + 30 > size)
				return false;
			const uint8_t* mod = d + ins + 2;
			const uint8_t* car = d + ins + 15;
			auto op = [](const uint8_t* o, uint8_t wave) {
				// ksl, multiple, feedback, attack, sustain, eg, decay, release, level, am, vib, ksr, con
				ADL_Operator r;
				r.avekf_20 = (uint8_t)(((o[9] & 1) << 7) | ((o[10] & 1) << 6) | ((o[5] & 1) << 5) | ((o[11] & 1) << 4) | (o[1] & 15));
				r.ksl_l_40 = (uint8_t)(((o[0] & 3) << 6) | (o[8] & 63));
				r.atdec_60 = (uint8_t)(((o[3] & 15) << 4) | (o[6] & 15));
				r.susrel_80 = (uint8_t)(((o[4] & 15) << 4) | (o[7] & 15));
				r.waveform_E0 = (uint8_t)(wave & 7);
				return r;
			};
			ADL_Instrument in;
			memset(&in, 0, sizeof(in));
			in.version = ADLMIDI_InstrumentVersion;
			in.percussion_key_number = percussive ? d[rec + 2] : 0;
			in.inst_flags = ADLMIDI_Ins_2op;
			in.fb_conn1_C0 = (uint8_t)(((mod[2] & 7) << 1) | (mod[12] & 1));
			in.operators[0] = op(car, d[ins + 29]);
			in.operators[1] = op(mod, d[ins + 28]);
			if (adl_setInstrument(player_, &bank, n, &in) < 0)
				return false;
		}
		return true;
	}

	void NoteOn(uint8_t c, uint8_t n, uint8_t v) override { adl_rt_noteOn(player_, c, n, v); }
	void NoteOff(uint8_t c, uint8_t n) override { adl_rt_noteOff(player_, c, n); }
	void NoteAfterTouch(uint8_t c, uint8_t n, uint8_t v) override { adl_rt_noteAfterTouch(player_, c, n, v); }
	void ChannelAfterTouch(uint8_t c, uint8_t v) override { adl_rt_channelAfterTouch(player_, c, v); }
	void ControllerChange(uint8_t c, uint8_t t, uint8_t v) override { adl_rt_controllerChange(player_, c, t, v); }
	void PatchChange(uint8_t c, uint8_t p) override { adl_rt_patchChange(player_, c, p); }
	void PitchBend(uint8_t c, uint8_t msb, uint8_t lsb) override { adl_rt_pitchBendML(player_, c, msb, lsb); }
	void SysEx(const uint8_t* msg, size_t size) override { adl_rt_systemExclusive(player_, msg, size); }
	void Reset() override { adl_rt_resetState(player_); }
	void Render(int16_t* out, int frames) override { adl_generate(player_, frames * 2, out); }

private:
	ADL_MIDIPlayer* player_ = nullptr;
};

#if REMC1_FLUIDSYNTH
// General MIDI songs on a SoundFont.
class FluidSynth : public Synth {
public:
	FluidSynth(int rate, const std::string& soundfont)
	{
		fluid_set_log_function(FLUID_INFO, nullptr, nullptr);
		fluid_set_log_function(FLUID_DBG, nullptr, nullptr);
		settings_ = new_fluid_settings();
		if (!settings_)
			return;
		fluid_settings_setnum(settings_, "synth.sample-rate", rate);
		fluid_settings_setnum(settings_, "synth.gain", 0.3); // about as loud as the AdLib music, no clipping
		fluid_settings_setint(settings_, "synth.threadsafe-api", 0); // port_music serialises the calls
		synth_ = new_fluid_synth(settings_);
		if (synth_ && fluid_synth_sfload(synth_, soundfont.c_str(), 1) == FLUID_FAILED) {
			delete_fluid_synth(synth_);
			synth_ = nullptr;
		}
	}
	~FluidSynth() override
	{
		if (synth_)
			delete_fluid_synth(synth_);
		if (settings_)
			delete_fluid_settings(settings_);
	}
	bool Ok() const { return synth_ != nullptr; }

	void NoteOn(uint8_t c, uint8_t n, uint8_t v) override { fluid_synth_noteon(synth_, c, n, v); }
	void NoteOff(uint8_t c, uint8_t n) override { fluid_synth_noteoff(synth_, c, n); }
	void NoteAfterTouch(uint8_t c, uint8_t n, uint8_t v) override { fluid_synth_key_pressure(synth_, c, n, v); }
	void ChannelAfterTouch(uint8_t c, uint8_t v) override { fluid_synth_channel_pressure(synth_, c, v); }
	void ControllerChange(uint8_t c, uint8_t t, uint8_t v) override { fluid_synth_cc(synth_, c, t, v); }
	void PatchChange(uint8_t c, uint8_t p) override { fluid_synth_program_change(synth_, c, p); }
	void PitchBend(uint8_t c, uint8_t msb, uint8_t lsb) override { fluid_synth_pitch_bend(synth_, c, (msb << 7) | lsb); }
	void SysEx(const uint8_t* msg, size_t size) override
	{
		// FluidSynth takes the message without its F0 / F7 framing
		if (size && msg[0] == 0xF0) { msg++; size--; }
		if (size && msg[size - 1] == 0xF7) size--;
		if (size)
			fluid_synth_sysex(synth_, (const char*)msg, (int)size, nullptr, nullptr, nullptr, 0);
	}
	void Reset() override { fluid_synth_system_reset(synth_); }
	void Render(int16_t* out, int frames) override { fluid_synth_write_s16(synth_, frames, out, 0, 2, out, 1, 2); }

private:
	fluid_settings_t* settings_ = nullptr;
	fluid_synth_t* synth_ = nullptr;
};

// --soundfont / audio.soundfont, else the usual system locations (Linux distributions' GM SoundFonts).
std::string FindSoundFont()
{
	if (!g_settings.soundfont.empty())
		return g_settings.soundfont;
	static const char* const candidates[] = {
		"/usr/share/soundfonts/default.sf2",       // Fedora, Arch (symlink to the installed GM SoundFont)
		"/usr/share/soundfonts/FluidR3_GM.sf2",    // Arch soundfont-fluid
		"/usr/share/sounds/sf2/default-GM.sf2",    // Debian / Ubuntu
		"/usr/share/sounds/sf2/FluidR3_GM.sf2",    // Debian / Ubuntu fluid-soundfont-gm
		"/usr/local/share/soundfonts/default.sf2",
		"/opt/homebrew/share/soundfonts/default.sf2",
	};
	for (const char* path : candidates)
		if (FILE* f = fopen(path, "rb")) {
			fclose(f);
			return path;
		}
	return std::string();
}
#endif // REMC1_FLUIDSYNTH

#ifdef _WIN32
// General MIDI songs on the Windows MIDI synth (the default MIDI output, normally the Microsoft GS
// Wavetable Synth). The music volume scales the channel volumes (CC7): the device has no volume of its own.
class WinMidiSynth : public Synth {
public:
	WinMidiSynth()
	{
		if (midiOutOpen(&out_, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
			out_ = nullptr;
	}
	~WinMidiSynth() override
	{
		if (out_) {
			midiOutReset(out_);
			midiOutClose(out_);
		}
	}
	bool Ok() const { return out_ != nullptr; }

	void NoteOn(uint8_t c, uint8_t n, uint8_t v) override { Send(0x90 | c, n, v); }
	void NoteOff(uint8_t c, uint8_t n) override { Send(0x80 | c, n, 0); }
	void NoteAfterTouch(uint8_t c, uint8_t n, uint8_t v) override { Send(0xA0 | c, n, v); }
	void ChannelAfterTouch(uint8_t c, uint8_t v) override { Send(0xD0 | c, v, 0); }
	void ControllerChange(uint8_t c, uint8_t t, uint8_t v) override { Send(0xB0 | c, t, t == 7 ? Scaled(v) : v); }
	void PatchChange(uint8_t c, uint8_t p) override { Send(0xC0 | c, p, 0); }
	void PitchBend(uint8_t c, uint8_t msb, uint8_t lsb) override { Send(0xE0 | c, lsb, msb); }
	void SysEx(const uint8_t* msg, size_t size) override
	{
		std::vector<char> buf;
		if (!size || msg[0] != 0xF0)
			buf.push_back((char)0xF0);
		buf.insert(buf.end(), (const char*)msg, (const char*)msg + size);
		if ((uint8_t)buf.back() != 0xF7)
			buf.push_back((char)0xF7);
		MIDIHDR h;
		memset(&h, 0, sizeof(h));
		h.lpData = buf.data();
		h.dwBufferLength = h.dwBytesRecorded = (DWORD)buf.size();
		if (midiOutPrepareHeader(out_, &h, sizeof(h)) != MMSYSERR_NOERROR)
			return;
		midiOutLongMsg(out_, &h, sizeof(h));
		while (midiOutUnprepareHeader(out_, &h, sizeof(h)) == MIDIERR_STILLPLAYING)
			Sleep(1);
	}
	void Reset() override
	{
		midiOutReset(out_);
		static const uint8_t gmOn[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
		SysEx(gmOn, sizeof(gmOn));
		for (uint8_t c = 0; c < 16; c++)
			ControllerChange(c, 7, 100); // the GM default volume, scaled
	}
	void Render(int16_t* out, int frames) override { memset(out, 0, (size_t)frames * 4); }
	bool Realtime() const override { return true; }

private:
	void Send(int status, uint8_t a, uint8_t b) { midiOutShortMsg(out_, (DWORD)(status | (a << 8) | (b << 16))); }
	static uint8_t Scaled(uint8_t v) { return (uint8_t)(v * g_settings.musicVolume / 100); }
	HMIDIOUT out_ = nullptr;
};
#endif // _WIN32

// --- Player: sequencer + synth, driven by the SDL_mixer music hook ------------------------------------
std::mutex audioMutex; // guards everything below against the audio callback
std::unique_ptr<Synth> synth;
std::unique_ptr<BW_MidiSequencer> sequencer; // the current song (null: none / stopped)
BW_MidiRtInterface rtInterface;
bool sequencerPlaying = false;
int deviceRate = 44100;
FILE* wavDump = nullptr;
std::thread sequencerThread; // realtime synths
std::atomic<bool> sequencerThreadQuit{ false };

void InitInterface()
{
	memset(&rtInterface, 0, sizeof(rtInterface));
	rtInterface.rt_noteOn = [](void*, uint8_t c, uint8_t n, uint8_t v) { synth->NoteOn(c, n, v); };
	rtInterface.rt_noteOff = [](void*, uint8_t c, uint8_t n) { synth->NoteOff(c, n); };
	rtInterface.rt_noteAfterTouch = [](void*, uint8_t c, uint8_t n, uint8_t v) { synth->NoteAfterTouch(c, n, v); };
	rtInterface.rt_channelAfterTouch = [](void*, uint8_t c, uint8_t v) { synth->ChannelAfterTouch(c, v); };
	rtInterface.rt_controllerChange = [](void*, uint8_t c, uint8_t t, uint8_t v) { synth->ControllerChange(c, t, v); };
	rtInterface.rt_patchChange = [](void*, uint8_t c, uint8_t p) { synth->PatchChange(c, p); };
	rtInterface.rt_pitchBend = [](void*, uint8_t c, uint8_t msb, uint8_t lsb) { synth->PitchBend(c, msb, lsb); };
	rtInterface.rt_systemExclusive = [](void*, const uint8_t* m, size_t s) { synth->SysEx(m, s); };
	rtInterface.onPcmRender = [](void*, uint8_t* stream, size_t length) { synth->Render((int16_t*)stream, (int)(length / 4)); };
	rtInterface.pcmSampleRate = (uint32_t)deviceRate;
	rtInterface.pcmFrameSize = 4; // stereo S16
}

void SDLCALL MusicHook(void*, Uint8* stream, int len)
{
	std::lock_guard<std::mutex> lock(audioMutex);
	if (!synth) {
		memset(stream, 0, (size_t)len);
		return;
	}
	int done = 0;
	if (sequencer && sequencerPlaying) {
		done = sequencer->playStream(stream, (size_t)len);
		if (done < len)
			sequencerPlaying = false; // song end: the game restarts it (tick driven) if it wants to
	}
	if (done < len)
		synth->Render((int16_t*)(stream + done), (len - done) / 4); // release tails / silence
	const int volume = g_settings.musicVolume;
	if (volume < 100) {
		int16_t* s = (int16_t*)stream;
		for (int i = 0; i < len / 2; i++)
			s[i] = (int16_t)(s[i] * volume / 100);
	}
	if (wavDump)
		fwrite(stream, 1, (size_t)len, wavDump);
}

// The sequencer for realtime synths: plays the events due since the last round, then sleeps until the
// next one is due (at most 5 ms, so a song started meanwhile begins promptly).
void SequencerThread()
{
#ifdef _WIN32
	timeBeginPeriod(1);
#endif
	auto last = std::chrono::steady_clock::now();
	while (!sequencerThreadQuit) {
		double wait = 0.005;
		{
			std::lock_guard<std::mutex> lock(audioMutex);
			const auto now = std::chrono::steady_clock::now();
			const double elapsed = std::chrono::duration<double>(now - last).count();
			last = now;
			if (sequencer && sequencerPlaying) {
				wait = sequencer->Tick(elapsed, 0.001);
				if (sequencer->positionAtEnd())
					sequencerPlaying = false; // song end: the game restarts it (tick driven) if it wants to
			}
		}
		std::this_thread::sleep_for(std::chrono::duration<double>(wait < 0.001 ? 0.001 : wait > 0.005 ? 0.005 : wait));
	}
#ifdef _WIN32
	timeEndPeriod(1);
#endif
}

// Also runs at exit (the game exits without shutting the music down): stops the sequencer thread before
// the objects it uses are destroyed, and silences the device.
void StopPlayer()
{
	Mix_HookMusic(nullptr, nullptr);
	if (sequencerThread.joinable()) {
		sequencerThreadQuit = true;
		sequencerThread.join();
	}
	std::lock_guard<std::mutex> lock(audioMutex);
	sequencer.reset();
	synth.reset();
}

// REMC1_MUSIC_WAV=<file>: also write the music output to a WAV file (debugging without listening).
void OpenWavDump()
{
	const char* path = getenv("REMC1_MUSIC_WAV");
	if (!path || !*path || !(wavDump = fopen(path, "wb")))
		return;
	uint8_t header[44] = { 'R', 'I', 'F', 'F', 0xFF, 0xFF, 0xFF, 0x7F, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
		1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0xDB, 0xFF, 0xFF, 0x7F }; // open-ended sizes
	const uint32_t rate = (uint32_t)deviceRate, bytes = rate * 4;
	memcpy(header + 24, &rate, 4);
	memcpy(header + 28, &bytes, 4);
	fwrite(header, 1, sizeof(header), wavDump);
}

void AllNotesOff()
{
	if (!synth)
		return;
	for (uint8_t c = 0; c < 16; c++) {
		synth->ControllerChange(c, 64, 0);  // sustain off
		synth->ControllerChange(c, 123, 0); // all notes off
	}
}
#endif // REMC1_MUSIC

} // namespace

PortMusicDevice PortMusic_SelectDevice(const char* card)
{
	if (!g_settings.music)
		return PortMusicDevice::None;
	const char* force = g_settings.musicDevice.c_str();
	if (EqualsNoCase(force, "none"))
		return PortMusicDevice::None;
	if (EqualsNoCase(force, "adlib"))
		return PortMusicDevice::AdLib;
	if (EqualsNoCase(force, "gm"))
		return PortMusicDevice::GeneralMidi;
	if (EqualsNoCase(force, "mt32"))
		return PortMusicDevice::MT32;
	// "auto": the card chosen in the game's sound setup (sub_3C570's card list)
	static const char* const fm[] = { "ADLIB", "SBLAST", "SBPRO", "ADLIBG", "SB16FM", "GRAVIS", "PASFM", "COMPATIBLE" };
	for (const char* c : fm)
		if (EqualsNoCase(card, c))
			return PortMusicDevice::AdLib;
	if (EqualsNoCase(card, "ROLAND"))
		return PortMusicDevice::MT32;
	if (EqualsNoCase(card, "SBAWE32") || EqualsNoCase(card, "GENERAL") || EqualsNoCase(card, "WBLAST"))
		return PortMusicDevice::GeneralMidi;
	return PortMusicDevice::None;
}

const char* PortMusic_DeviceName(PortMusicDevice device)
{
	switch (device) {
	case PortMusicDevice::AdLib: return "AdLib (OPL3)";
	case PortMusicDevice::GeneralMidi: return "General MIDI";
	case PortMusicDevice::MT32: return "Roland MT-32";
	default: return "none";
	}
}

void PortMusic_Init(PortMusicDevice device)
{
	currentDevice = device;
	printf("music: %s\n", PortMusic_DeviceName(device));
#if REMC1_MUSIC
	// --no-music also silences replays, whose music card comes from the recording
	if (device == PortMusicDevice::None || !g_settings.music || !PortSound_Init())
		return;
	int rate = 44100, channels = 2;
	Uint16 format = AUDIO_S16SYS;
	Mix_QuerySpec(&rate, &format, &channels);
	if (format != AUDIO_S16SYS || channels != 2) {
		fprintf(stderr, "music: unsupported output format, music disabled\n");
		return;
	}
	StopPlayer(); // a second init replaces the player
	std::unique_ptr<Synth> s;
	// General MIDI songs (also chosen for Roland), --midi-synth: by default FluidSynth on a SoundFont,
	// else the Windows MIDI synth, else OPL3 with libADLMIDI's GM bank
	const char* midiSynth = g_settings.midiSynth.c_str();
	const bool anySynth = EqualsNoCase(midiSynth, "auto");
	if (device != PortMusicDevice::AdLib && !EqualsNoCase(midiSynth, "opl")) {
#if REMC1_FLUIDSYNTH
		if (anySynth || EqualsNoCase(midiSynth, "fluidsynth")) {
			const std::string soundfont = FindSoundFont();
			if (soundfont.empty())
				fprintf(stderr, "music: no SoundFont for FluidSynth (set --soundfont or audio.soundfont in config.json)\n");
			else {
				auto fluid = std::make_unique<FluidSynth>(rate, soundfont);
				if (fluid->Ok()) {
					printf("music: FluidSynth, %s\n", soundfont.c_str());
					s = std::move(fluid);
				}
				else
					fprintf(stderr, "music: cannot load the SoundFont %s\n", soundfont.c_str());
			}
		}
#endif
#ifdef _WIN32
		// not in headless runs or on SDL's dummy audio driver, which must stay silent
		const char* driver = SDL_GetCurrentAudioDriver();
		const bool silentRun = g_settings.headless || (driver && (!strcmp(driver, "dummy") || !strcmp(driver, "disk")));
		if (!s && !silentRun && (anySynth || EqualsNoCase(midiSynth, "system"))) {
			auto win = std::make_unique<WinMidiSynth>();
			if (win->Ok()) {
				printf("music: Windows MIDI synth\n");
				s = std::move(win);
			}
			else
				fprintf(stderr, "music: cannot open the Windows MIDI synth\n");
		}
#endif
		if (!s)
			printf("music: General MIDI on the OPL3 emulator\n");
	}
	// AdLib songs, and General MIDI without another synth: OPL3 emulation
	if (!s) {
		auto adl = std::make_unique<AdlSynth>(rate, device == PortMusicDevice::AdLib);
		if (adl->Ok())
			s = std::move(adl);
	}
	if (!s) {
		fprintf(stderr, "music: cannot create the synthesizer, music is silent\n");
		return;
	}
	const bool realtime = s->Realtime();
	{
		std::lock_guard<std::mutex> lock(audioMutex);
		deviceRate = rate;
		synth = std::move(s);
		InitInterface();
		if (realtime)
			synth->Reset(); // a device keeps the state an earlier program left
		else
			OpenWavDump();
	}
	if (realtime) {
		sequencerThreadQuit = false;
		sequencerThread = std::thread(SequencerThread);
		static const bool atExit = (atexit(StopPlayer), true);
		(void)atExit;
	}
	else
		Mix_HookMusic(MusicHook, nullptr);
#endif
}

void PortMusic_Shutdown()
{
#if REMC1_MUSIC
	StopPlayer();
#endif
	songPlaying = false;
	songLoaded = false;
}

bool PortMusic_LoadBanks(const uint8_t* melodic, size_t melodicSize, const uint8_t* drums, size_t drumsSize)
{
#if REMC1_MUSIC
	std::lock_guard<std::mutex> lock(audioMutex);
	auto* adl = dynamic_cast<AdlSynth*>(synth.get());
	if (!adl)
		return true;
	if (!adl->LoadBnk(melodic, melodicSize, false) || !adl->LoadBnk(drums, drumsSize, true)) {
		fprintf(stderr, "music: bad instrument bank, using libADLMIDI's default bank\n");
		return false;
	}
#else
	(void)melodic; (void)melodicSize; (void)drums; (void)drumsSize;
#endif
	return true;
}

void PortMusic_Load(const uint8_t* hmp, size_t size)
{
	songLoaded = true;
	songPlaying = false;
	songPosition = 0;
	songTicks = PortMusic_SongTicks(hmp, size, PortTimer_TickHz());
	if (debugMusic)
		fprintf(stderr, "music: load %zu bytes, %llu ticks, tick=%llu\n", size, (unsigned long long)songTicks,
			(unsigned long long)PortTimer_TickCount());
#if REMC1_MUSIC
	if (!synth)
		return;
	auto seq = std::make_unique<BW_MidiSequencer>();
	seq->setInterface(&rtInterface);
	seq->setLoopEnabled(false); // HMI loop controllers: the game restarts songs itself when they end
	if (!seq->loadMIDI(hmp, size)) {
		fprintf(stderr, "music: cannot load song: %s\n", seq->getErrorString());
		seq.reset();
	}
	std::lock_guard<std::mutex> lock(audioMutex);
	AllNotesOff();
	synth->Reset();
	sequencer = std::move(seq);
	sequencerPlaying = false;
#endif
}

void PortMusic_Start()
{
	if (!songLoaded || songPlaying)
		return;
	// +1: the current tick is already under way
	songEndTick = PortTimer_TickCount() + 1 + (songTicks - songPosition);
	songPlaying = true;
#if REMC1_MUSIC
	std::lock_guard<std::mutex> lock(audioMutex);
	sequencerPlaying = sequencer != nullptr && !sequencer->positionAtEnd();
#endif
}

void PortMusic_Rewind()
{
	const bool wasPlaying = PortMusic_IsPlaying();
	songPosition = 0;
	if (wasPlaying)
		songEndTick = PortTimer_TickCount() + 1 + songTicks;
#if REMC1_MUSIC
	std::lock_guard<std::mutex> lock(audioMutex);
	if (sequencer) {
		AllNotesOff();
		sequencer->rewind();
	}
#endif
}

void PortMusic_Stop()
{
	songPosition = SongTicksPlayed();
	songPlaying = false;
#if REMC1_MUSIC
	std::lock_guard<std::mutex> lock(audioMutex);
	sequencerPlaying = false;
	AllNotesOff();
#endif
}

bool PortMusic_IsPlaying()
{
	if (songPlaying && PortTimer_TickCount() >= songEndTick) {
		songPlaying = false;
		songPosition = songTicks;
	}
	return songPlaying;
}

void PortMusic_SendMidi(const uint8_t* data, size_t size)
{
	if (debugMusic)
		fprintf(stderr, "music: send %02x %02x %02x (%zu bytes) tick=%llu\n", size > 0 ? data[0] : 0, size > 1 ? data[1] : 0,
			size > 2 ? data[2] : 0, size, (unsigned long long)PortTimer_TickCount());
#if REMC1_MUSIC
	std::lock_guard<std::mutex> lock(audioMutex);
	if (!synth)
		return;
	uint8_t status = 0;
	for (size_t i = 0; i < size;) {
		if (data[i] & 0x80)
			status = data[i++];
		const uint8_t c = status & 15;
		const int n = (status & 0xE0) == 0xC0 ? 1 : 2;
		if (status < 0x80 || status >= 0xF0 || i + n > size)
			break;
		const uint8_t a = data[i], b = n > 1 ? data[i + 1] : 0;
		i += n;
		switch (status & 0xF0) {
		case 0x80: synth->NoteOff(c, a); break;
		case 0x90: if (b) synth->NoteOn(c, a, b); else synth->NoteOff(c, a); break;
		case 0xA0: synth->NoteAfterTouch(c, a, b); break;
		case 0xB0: synth->ControllerChange(c, a, b); break;
		case 0xC0: synth->PatchChange(c, a); break;
		case 0xD0: synth->ChannelAfterTouch(c, a); break;
		case 0xE0: synth->PitchBend(c, b, a); break;
		}
	}
#else
	(void)data; (void)size;
#endif
}

void PortMusic_ResetChannels()
{
#if REMC1_MUSIC
	std::lock_guard<std::mutex> lock(audioMutex);
	AllNotesOff();
	if (synth)
		for (uint8_t c = 0; c < 16; c++)
			synth->ControllerChange(c, 121, 0); // reset all controllers
#endif
}

double PortMusic_SequencerSeconds(const uint8_t* hmp, size_t size)
{
#if REMC1_MUSIC
	BW_MidiSequencer seq;
	BW_MidiRtInterface none; // the sequencer requires the channel callbacks
	memset(&none, 0, sizeof(none));
	none.rt_noteOn = [](void*, uint8_t, uint8_t, uint8_t) {};
	none.rt_noteOff = [](void*, uint8_t, uint8_t) {};
	none.rt_noteAfterTouch = [](void*, uint8_t, uint8_t, uint8_t) {};
	none.rt_channelAfterTouch = [](void*, uint8_t, uint8_t) {};
	none.rt_controllerChange = [](void*, uint8_t, uint8_t, uint8_t) {};
	none.rt_patchChange = [](void*, uint8_t, uint8_t) {};
	none.rt_pitchBend = [](void*, uint8_t, uint8_t, uint8_t) {};
	none.rt_systemExclusive = [](void*, const uint8_t*, size_t) {};
	seq.setInterface(&none);
	seq.setLoopEnabled(false);
	if (seq.loadMIDI(hmp, size))
		return seq.timeLength();
#else
	(void)hmp; (void)size;
#endif
	return -1;
}

// HMP: "HMIMIDIP" header (track count at 0x30, ticks per second - the driver's timer rate - at 0x38),
// tracks from 0x308 (0x388 for the "013195" revision), each a 12-byte chunk header (number, length, ?)
// and events with HMP's variable-length deltas (7 bits per byte, low group first, the last byte has bit 7
// set). The song ends when its longest track ends.
uint64_t PortMusic_SongTicks(const uint8_t* d, size_t size, double tickHz)
{
	if (!d || size < 0x308 || memcmp(d, "HMIMIDIP", 8) != 0)
		return 0;
	const uint32_t tracks = ReadLE32(d + 0x30), rate = ReadLE32(d + 0x38);
	if (!rate)
		return 0;
	size_t pos = memcmp(d + 8, "013195", 6) == 0 ? 0x388 : 0x308;
	uint64_t longest = 0;
	for (uint32_t t = 0; t < tracks && pos + 12 <= size; t++) {
		const uint32_t length = ReadLE32(d + pos + 4);
		if (length < 12 || length > size - pos)
			break;
		const size_t end = pos + length;
		size_t p = pos + 12;
		uint64_t ticks = 0;
		uint8_t status = 0;
		while (p < end) {
			uint64_t delta = 0;
			for (int shift = 0; p < end; shift += 7) {
				const uint8_t b = d[p++];
				if (shift < 56)
					delta |= (uint64_t)(b & 0x7F) << shift;
				if (b & 0x80)
					break;
			}
			ticks += delta;
			if (p >= end)
				break;
			if (d[p] & 0x80)
				status = d[p++];
			if (status == 0xFF) { // meta: type, length (one byte in HMP)
				if (p + 2 > end)
					break;
				const uint8_t type = d[p];
				p += 2 + d[p + 1];
				if (type == 0x2F)
					break;
			}
			else if (status == 0xF0 || status == 0xF7) { // SysEx: SMF variable-length size
				uint32_t n = 0;
				while (p < end) {
					const uint8_t b = d[p++];
					n = (n << 7) | (b & 0x7F);
					if (!(b & 0x80))
						break;
				}
				p += n;
			}
			else if (status >= 0x80)
				p += (status & 0xE0) == 0xC0 ? 1 : 2;
			else
				break; // data without a status: corrupt
		}
		if (ticks > longest)
			longest = ticks;
		pos = end;
	}
	return (uint64_t)((double)longest * tickHz / rate + 0.5);
}
