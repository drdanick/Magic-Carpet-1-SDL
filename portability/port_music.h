#pragma once
// Music: replacement for the HMI SOS MIDI driver (hmimdrv.386).
//
// The game's songs are HMI HMP files (DATA/MUSIC<set>-<variant>.DAT, one variant per card type). They are
// sequenced by BW_MidiSequencer (from libADLMIDI) and played by a synth chosen from the sound setup:
//   AdLib family -> libADLMIDI (OPL3 emulation) with the game's own INST.BNK / DRUM.BNK instruments;
//   General MIDI -> FluidSynth on a SoundFont, the Windows MIDI synth, or libADLMIDI with its General MIDI bank;
//   Roland -> Munt (MT-32 emulation) with the user's MT-32 / CM-32L ROMs; without them the General MIDI songs.
// The output is mixed into the SDL_mixer device opened by port_sound.
//
// Game-visible state (whether a song is still playing) is driven by timer ticks against the song length,
// never by the audio device, so live play, headless runs and replays behave identically - also with no
// audio device and in builds without music support (REMC1_MUSIC off).

#include <cstddef>
#include <cstdint>

enum class PortMusicDevice { None, AdLib, GeneralMidi, MT32 };

// Card name from sndsetup.inf / BF_MUSIC ("ADLIB", "SB16FM", "GENERAL", "ROLAND", ...), overridden by the
// music settings (--no-music, --music-device). The result only depends on the setup and the settings.
PortMusicDevice PortMusic_SelectDevice(const char* card);
// A complete MT-32 / CM-32L ROM set is in --mt32-roms (and the build has Munt): the Roland songs can play.
bool PortMusic_Mt32Available();
const char* PortMusic_DeviceName(PortMusicDevice device);

// Opens the audio output for the device (shared with port_sound). Without an audio device the songs are
// still "played" silently, so the game behaves the same.
void PortMusic_Init(PortMusicDevice device);
void PortMusic_Shutdown();

// AdLib instrument banks: unpacked AdLib BNK files (INST.BNK melodic, DRUM.BNK percussion).
bool PortMusic_LoadBanks(const uint8_t* melodic, size_t melodicSize, const uint8_t* drums, size_t drumsSize);

// Songs. The HMP data is parsed on load (it need not stay loaded); one song at a time, like the game uses
// the HMI driver.
void PortMusic_Load(const uint8_t* hmp, size_t size); // sosMIDIInitSong (stops the current song)
void PortMusic_Start();                              // sosMIDIStartSong
void PortMusic_Rewind();                             // sosMIDIResetSong
void PortMusic_Stop();                               // sosMIDIStopSong
bool PortMusic_IsPlaying();                           // !sosMIDISongDone (tick driven)
// Raw MIDI messages sent by the game (sosMIDISendMIDIData), e.g. the danger-music channel fades.
void PortMusic_SendMidi(const uint8_t* data, size_t size);
// Silences all channels and resets their controllers (sosMIDIResetDriver).
void PortMusic_ResetChannels();

// Song length in timer ticks of the given rate (max over the tracks), 0 if the data is not an HMP song.
uint64_t PortMusic_SongTicks(const uint8_t* hmp, size_t size, double tickHz);
// Song length in seconds as the sequencer plays it (tests); -1 if it cannot load the song or without REMC1_MUSIC.
double PortMusic_SequencerSeconds(const uint8_t* hmp, size_t size);
