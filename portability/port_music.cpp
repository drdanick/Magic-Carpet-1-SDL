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

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#if REMC1_MUSIC
#include <adlmidi.h>
#include "midi_sequencer.hpp"
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

// --- Player: sequencer + synth, driven by the SDL_mixer music hook ------------------------------------
std::mutex audioMutex; // guards everything below against the audio callback
std::unique_ptr<Synth> synth;
std::unique_ptr<BW_MidiSequencer> sequencer; // the current song (null: none / stopped)
BW_MidiRtInterface rtInterface;
bool sequencerPlaying = false;
int deviceRate = 44100;
FILE* wavDump = nullptr;

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
	// General MIDI / MT-32 songs: OPL3 with a General MIDI bank for now
	std::unique_ptr<Synth> s;
	auto adl = std::make_unique<AdlSynth>(rate, device == PortMusicDevice::AdLib);
	if (adl->Ok())
		s = std::move(adl);
	if (!s) {
		fprintf(stderr, "music: cannot create the synthesizer, music is silent\n");
		return;
	}
	{
		std::lock_guard<std::mutex> lock(audioMutex);
		deviceRate = rate;
		synth = std::move(s);
		InitInterface();
		OpenWavDump();
	}
	Mix_HookMusic(MusicHook, nullptr);
#endif
}

void PortMusic_Shutdown()
{
#if REMC1_MUSIC
	Mix_HookMusic(nullptr, nullptr);
	std::lock_guard<std::mutex> lock(audioMutex);
	sequencer.reset();
	synth.reset();
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
