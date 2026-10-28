// ---------------------------------------------------------------------------
// altsound_processor_base.hpp
//
// Base class implementation for AltsoundProcessor, which encapsulates
// command execution, and specialization in support of all AltSound formats
// ---------------------------------------------------------------------------
// license:BSD-3-Clause
// copyright-holders: Dave Roscoe
// ---------------------------------------------------------------------------

#ifndef ALTSOUND_PROCESSOR_BASE_HPP
#define ALTSOUND_PROCESSOR_BASE_HPP
#if !defined(__GNUC__) || (__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || (__GNUC__ >= 4)	// GCC supports "pragma once" correctly since 3.4
#pragma once
#endif

#if _MSC_VER >= 1700
 #ifdef inline
  #undef inline
 #endif
#endif

#include "altsound_data.hpp"

#include "miniaudio_private.h"

#include <mutex>
#include <vector>

using std::string;

// ---------------------------------------------------------------------------
// AltsoundProcessorBase class definition
// ---------------------------------------------------------------------------

class AltsoundProcessorBase {
public:

	// Default constructor
	AltsoundProcessorBase() = delete;

	// Copy constructor
	AltsoundProcessorBase(AltsoundProcessorBase&) = delete;

	// Standard constructor
	AltsoundProcessorBase(const string& game_name, const std::filesystem::path& vpm_path);

	// Destructor
	virtual ~AltsoundProcessorBase();

	// Process ROM commands to the sound board
	virtual bool handleCmd(const unsigned int cmd_in);

	// external interface to stop playback of the current music stream
	virtual bool stopMusic() = 0;

	// ROM volume control accessor/mutator
	void romControlsVol(const bool use_rom_vol);
	bool romControlsVol();

	// command recording flag mutator
	void recordSoundCmds(const bool rec_sound_cmds);

	// initialize processing state
	virtual void init();

	// master volume accessor/mutator
	void setMasterVol(const float vol_in);
	static float getMasterVol();

	// Host gain per sample type (MUSIC, JINGLE, SFX, CALLOUT, SOLO, OVERLAY), on top of the
	// package's own gains and ducking. Re-applied to the streams already playing. Caller must
	// hold io_mutex for setGroupGain, as it walks channel_stream.
	static void setGroupGain(AltsoundSampleType type, const float gain_in);
	static float getGroupGain(AltsoundSampleType type);

	// global accessor/mutator
	void setGlobalVol(const float vol_in);
	static float getGlobalVol();

	// command skip count accessor/mutator
	void setSkipCount(const unsigned int skip_count_in);
	unsigned int getSkipCount() const;

	// whether the pack has a sample for this command
	bool hasSample(const unsigned int cmd_combined_in) { return getSample(cmd_combined_in) != ~0u; }

	// Data East sound board music handling (DE, DEDMD16/32/64), see filterDataEastMusic
	void setDataEastMusic(const bool enable);

public: // data

protected: // functions

	// populate sample data
	virtual bool loadSamples() = 0;

	// find sample matching provided command
	virtual unsigned int getSample(const unsigned int cmd_combined_in) = 0;

	// whether the pack's sample for this command is music
	virtual bool isMusicSample(const unsigned int cmd_combined_in) = 0;

	// whether a music stream started for this command is playing. Caller holds io_mutex.
	static bool isMusicPlaying(const unsigned int cmd_combined_in);

	// Data East music, applied by the processors after the skip count and before the sample
	// lookup (caller holds io_mutex). The DE sound board keeps the music itself: a mode's music
	// plays over the main theme and the CPU ends the mode with the next (odd) command, after
	// which the board resumes the theme on its own -- the CPU may send no music for a long
	// time (Tales from the Crypt: 42 s after 0x1f). The CPU also re-sends the music that is
	// playing. Returns false if nothing is to be played; may replace cmd_inout with the music
	// to resume.
	bool filterDataEastMusic(unsigned int& cmd_inout);

	// Create stream for miniaudio playback
	bool createStream(void* syncproc_in, AltsoundStreamInfo* stream_out);

	// get short path of current game <gamename>/subpath/filename
	string getShortPath(const string& path_in);

	// stop playback on all active streams
	bool stopAllStreams();

	// stop playback of provided stream handle
	static bool stopStream(unsigned int hstream);

	// free miniaudio resources of provided stream handle
	static bool freeStream(unsigned int hstream);

	// find available sound channel for sample playback
	static bool findFreeChannel(unsigned int& channel_out);

	// set volume on provided stream
	static bool setStreamVolume(unsigned int hstream, const float vol_in);

	// get volume on provided stream, -FLT_MAX on error
	static float getStreamVolume(unsigned int hstream);

	// stream registered on hstream in channel_stream, nullptr if none (yet)
	static AltsoundStreamInfo* findStream(unsigned int hstream);

	// Return ROM shortname
	const string& getGameName();

	// Return path to VPinMAME
	const std::filesystem::path& getVpmPath();

	std::filesystem::path getAltsoundPath() const;

	// Initialize log file
	bool startLogging(const string& gameName);

protected: // data

	string game_name;
	std::filesystem::path vpm_path;

private: // functions

private: // data

	bool rec_snd_cmds = false;
	bool use_rom_ctrl = true;
	bool de_music = false;
	std::vector<unsigned int> de_modes; // mode music, most recent last
	unsigned int de_main = ~0u;         // main theme to return to
	static float global_vol;
	static float master_vol;
	static std::array<float, OVERLAY + 1> group_gain;
	unsigned int skip_count;
};

// ----------------------------------------------------------------------------
// Inline functions
// ----------------------------------------------------------------------------

inline const string& AltsoundProcessorBase::getGameName() {
	return game_name;
}

// ----------------------------------------------------------------------------

inline const std::filesystem::path& AltsoundProcessorBase::getVpmPath() {
	return vpm_path;
}

// ----------------------------------------------------------------------------

inline std::filesystem::path AltsoundProcessorBase::getAltsoundPath() const {
	return vpm_path / "altsound" / game_name;
}

// ----------------------------------------------------------------------------

inline void AltsoundProcessorBase::romControlsVol(const bool use_rom_vol) {
	use_rom_ctrl = use_rom_vol;
}

// ----------------------------------------------------------------------------

inline bool AltsoundProcessorBase::romControlsVol() {
	return use_rom_ctrl;
}

// ----------------------------------------------------------------------------

inline void AltsoundProcessorBase::recordSoundCmds(const bool rec_sound_cmds) {
	rec_snd_cmds = rec_sound_cmds;
}

// ----------------------------------------------------------------------------

inline void AltsoundProcessorBase::setMasterVol(const float vol_in) {
	master_vol = vol_in;
}

// ----------------------------------------------------------------------------

inline float AltsoundProcessorBase::getMasterVol() {
	return master_vol;
}

// ----------------------------------------------------------------------------

inline float AltsoundProcessorBase::getGroupGain(AltsoundSampleType type) {
	return (type > UNDEFINED && type <= OVERLAY) ? group_gain[type] : 1.0f;
}

// ----------------------------------------------------------------------------

inline float AltsoundProcessorBase::getGlobalVol() {
	return global_vol;
}

// ----------------------------------------------------------------------------

inline unsigned int AltsoundProcessorBase::getSkipCount() const {
	return skip_count;
}

// ----------------------------------------------------------------------------

inline void AltsoundProcessorBase::setSkipCount(const unsigned int skip_count_in) {
	skip_count = skip_count_in;
}

#endif // ALTSOUND_PROCESSOR_BASE_HPP
