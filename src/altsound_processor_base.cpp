// ---------------------------------------------------------------------------
// altsound_processor_base.cpp
//
// Base class implementation for AltsoundProcessor, which encapsulates
// command execution, and specialization in support of all AltSound formats
// ---------------------------------------------------------------------------
// license:BSD-3-Clause
// copyright-holders: Dave Roscoe
// ---------------------------------------------------------------------------

#include "altsound_processor_base.hpp"
#include "altsound_logger.hpp"
#include "miniaudio_bass_compat.hpp"

#include <iomanip>
#include <chrono>
#include <cfloat>
#include <fstream>
#include <algorithm>

extern AltsoundLogger alog;
extern StreamArray channel_stream;
extern std::mutex io_mutex;

// initialize static data members
float AltsoundProcessorBase::global_vol = 1.0f;
float AltsoundProcessorBase::master_vol = 1.0f;
std::array<float, OVERLAY + 1> AltsoundProcessorBase::group_gain = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

// reference to sound command recording status
extern bool rec_snd_cmds;

// DAR@20230721
// ALTSOUND_STANDALONE is a compile-time flag set only when building the
// altsound processor as an executable.  It prevents the executable from
// trying to record a cmdlog.txt during playback, corrupting the source
// file
//
#ifndef ALTSOUND_STANDALONE
	std::ofstream logFile;
	std::chrono::high_resolution_clock::time_point lastCmdTime;
#endif

// ---------------------------------------------------------------------------
// CTOR/DTOR
// ---------------------------------------------------------------------------

AltsoundProcessorBase::AltsoundProcessorBase(const std::string& _game_name,
	                                         const std::filesystem::path& _vpm_path)
: game_name(_game_name),
  vpm_path(_vpm_path),
  skip_count(0)
{
}

AltsoundProcessorBase::~AltsoundProcessorBase()
{
#ifndef ALTSOUND_STANDALONE
	// close sound command recording log
	if (logFile.is_open())
		logFile.close();
#endif

	// clean up stored steam objects
	for (auto& stream : channel_stream) {
		delete stream;
		stream = nullptr;
	}
}

// ---------------------------------------------------------------------------

bool AltsoundProcessorBase::handleCmd(const unsigned int cmd_in)
{
#ifndef ALTSOUND_STANDALONE
	if (rec_snd_cmds) {
		// sound command recording is enabled

		// Get the current time.
		const auto currentTime = std::chrono::high_resolution_clock::now();

		// Compute the difference from the last command time.
		const auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastCmdTime).count();

		// Log the time and command.
		logFile << std::setw(10) << std::setfill('0') << std::dec << deltaTime;
		logFile << ", 0x" << std::setw(4) << std::setfill('0') << std::hex << cmd_in;
		logFile << ", " << "<user comments here>" << std::endl;

		// Store the current time for the next command.
		lastCmdTime = currentTime;
	}
#endif
	return true;
}

// ---------------------------------------------------------------------------

void AltsoundProcessorBase::setDataEastMusic(const bool enable)
{
	std::lock_guard<std::mutex> guard(io_mutex);
	de_music = enable;
	de_modes.clear();
	de_main = ~0u;
}

// ---------------------------------------------------------------------------

bool AltsoundProcessorBase::isMusicPlaying(const unsigned int cmd_combined_in)
{
	for (const AltsoundStreamInfo* stream : channel_stream)
		if (stream && stream->stream_type == MUSIC && stream->cmd == cmd_combined_in)
			return true;
	return false;
}

// ---------------------------------------------------------------------------

bool AltsoundProcessorBase::filterDataEastMusic(unsigned int& cmd_inout)
{
	if (!de_music)
		return true;

	const unsigned int cmd = cmd_inout;

	// Music: 0x01-0x11 and 0x40 and up are main themes, 0x12-0x3F mode (mission) music
	if (isMusicSample(cmd)) {
		if (isMusicPlaying(cmd)) {
			ALT_INFO(0, "DE music %04X already playing", cmd);
			return false;
		}
		const bool main_theme = (cmd >= 0x01 && cmd <= 0x11) || cmd >= 0x40;
		if (!main_theme) {
			de_modes.erase(std::remove(de_modes.begin(), de_modes.end(), cmd), de_modes.end());
			de_modes.push_back(cmd);
			return true;
		}
		de_main = cmd;
		if (!de_modes.empty()) {
			ALT_INFO(0, "DE main theme %04X kept for after mode music %04X", cmd, de_modes.back());
			return false;
		}
		return true;
	}

	if (hasSample(cmd))
		return true;

	// The command after a mode's music ends that mode: resume the previous mode or the theme
	if ((cmd & 1) && std::find(de_modes.begin(), de_modes.end(), cmd - 1) != de_modes.end()) {
		de_modes.erase(std::remove(de_modes.begin(), de_modes.end(), cmd - 1), de_modes.end());
		const unsigned int resume = de_modes.empty() ? de_main : de_modes.back();
		if (resume == ~0u || isMusicPlaying(resume))
			return true;
		ALT_INFO(0, "DE mode %04X ended, resuming music %04X", cmd - 1, resume);
		cmd_inout = resume;
		return true;
	}

	// 0x18 and 0x23 stop the music (altsound_postprocess_commands), 0x00 stops everything
	if (cmd == 0x0018 || cmd == 0x0023 || cmd == 0x0000)
		de_modes.clear();

	return true;
}

// ---------------------------------------------------------------------------

void AltsoundProcessorBase::init()
{
#ifndef ALTSOUND_STANDALONE
	// If recording sound commands, initialize output file
	if (rec_snd_cmds) {
		if (!ALT_CALL(startLogging(AltsoundProcessorBase::getGameName()))) {
			ALT_ERROR(0, "FAILED startLogging()");
		}
		else {
			ALT_INFO(1, "SUCCESS startLogging()");
		}
	}
#endif
}

// ---------------------------------------------------------------------------

bool AltsoundProcessorBase::startLogging(const std::string& gameName) {
#ifndef ALTSOUND_STANDALONE
	ALT_DEBUG(0, "BEGIN startLogging()");

	// Check if gameName is not empty
	if (gameName.empty()) {
		ALT_ERROR(1, "Game name is empty. Cannot start logging.");
		ALT_DEBUG(0, "END startLogging()");
		return false;
	}

	const std::filesystem::path recording_fname = vpm_path / "altsound" / "cmdlog.txt";
	logFile.open(recording_fname);
	if (!logFile.is_open()) {
		ALT_ERROR(1, "Failed to open log file: %s", recording_fname.string().c_str());
		ALT_DEBUG(0, "END startLogging()");
		return false;
	}

	ALT_INFO(1, "Creating sound command log: %s", recording_fname.string().c_str());

	const std::filesystem::path game_altsound_path = vpm_path / "altsound" / gameName;

	// Check if game_altsound_path is valid
	if (!dir_exists(game_altsound_path)) {
		ALT_ERROR(1, "Altsound path does not exist: %s", game_altsound_path.string().c_str());
		ALT_DEBUG(0, "END startLogging()");
		return false;
	}

	logFile << "altsound_path: " << game_altsound_path.generic_string() << std::endl;
	logFile << "hardware_gen: 0x" << 0 << std::endl;

	lastCmdTime = std::chrono::high_resolution_clock::now();

	ALT_DEBUG(0, "END startLogging()");
#endif
	return true;
}

// ---------------------------------------------------------------------------

bool AltsoundProcessorBase::findFreeChannel(unsigned int& channel_out)
{
	ALT_INFO(0, "BEGIN: AltsoundProcessorBase::findFreeChannel()");
	ALT_INDENT;

	const auto it = std::find(channel_stream.begin(), channel_stream.end(), nullptr);
	if (it != channel_stream.end()) {
		channel_out = static_cast<unsigned int>(std::distance(channel_stream.begin(), it));
		ALT_INFO(1, "Found free channel: %02u", channel_out);

		ALT_OUTDENT;
		ALT_DEBUG(0, "END: AltsoundProcessorBase::findFreeChannel()");
		return true;
	}
	ALT_ERROR(1, "No free channels available!");

	ALT_OUTDENT;
	ALT_DEBUG(0, "END AltsoundProcessorBase::findFreeChannel()");
	return false;
}

// ----------------------------------------------------------------------------

bool AltsoundProcessorBase::setStreamVolume(unsigned int stream_in, const float vol_in)
{
	ALT_DEBUG(0, "BEGIN: AltsoundProcessorBase::setVolume()");
	ALT_INDENT;

	if (stream_in == MINIAUDIO_NO_STREAM)
		return true;

	AltsoundStreamInfo* const stream = findStream(stream_in);
	if (stream)
		stream->processor_vol = vol_in;
	const float group_vol = stream ? getGroupGain(stream->stream_type) : 1.0f;
	const float new_vol = vol_in * global_vol * master_vol * group_vol;
	ALT_INFO(1, "Setting volume for stream %u", stream_in);
	ALT_DEBUG(1, "SAMPLE_VOL:%.02f  GLOBAL_VOL:%.02f  MASTER_VOL:%.02f  GROUP_VOL:%.02f", vol_in,
	          global_vol, master_vol, group_vol);
	const bool success = MiniAudio_ChannelSetVolume(stream_in, new_vol);

	if (!success) {
		ALT_ERROR(1, "FAILED MiniAudio_ChannelSetVolume()");
	}
	else {
		ALT_INFO(1, "SUCCESS MiniAudio_ChannelSetVolume()");
	}

	ALT_OUTDENT;
	ALT_DEBUG(0, "END: AltsoundProcessorBase::setVolume()");
	return success;
}

// ----------------------------------------------------------------------------

float AltsoundProcessorBase::getStreamVolume(unsigned int stream_in)
{
	if (stream_in == MINIAUDIO_NO_STREAM)
		return -FLT_MAX;

	float vol;
	if (!MiniAudio_ChannelGetVolume(stream_in, vol))
		return -FLT_MAX;
	else {
		const AltsoundStreamInfo* const stream = findStream(stream_in);
		return vol/(global_vol * master_vol * (stream ? getGroupGain(stream->stream_type) : 1.0f));
	}
}

// ----------------------------------------------------------------------------

AltsoundStreamInfo* AltsoundProcessorBase::findStream(unsigned int stream_in)
{
	for (const auto stream : channel_stream) {
		if (stream && stream->hstream == stream_in)
			return stream;
	}
	return nullptr;
}

// ----------------------------------------------------------------------------

void AltsoundProcessorBase::setGroupGain(AltsoundSampleType type, const float gain_in)
{
	if (type <= UNDEFINED || type > OVERLAY)
		return;

	group_gain[type] = gain_in;

	// Re-set each playing stream of the group from the volume the processor last gave it (its
	// gain after ducking), which setStreamVolume recorded; the new factor goes on top of that.
	for (const auto stream : channel_stream) {
		if (stream && stream->stream_type == type)
			setStreamVolume(stream->hstream, stream->processor_vol);
	}
}

// ----------------------------------------------------------------------------

bool AltsoundProcessorBase::createStream(void* syncproc_in, AltsoundStreamInfo* stream_out)
{
	ALT_DEBUG(0, "BEGIN AltsoundProcessorBase::createStream()");
	ALT_INDENT;

	const std::string short_path = getShortPath(stream_out->sample_path);
	unsigned int ch_idx;

	if (!ALT_CALL(findFreeChannel(ch_idx))) {
		ALT_ERROR(1, "FAILED AltsoundProcessorBase::findFreeChannel()");

		ALT_OUTDENT;
		ALT_DEBUG(0, "END AltsoundProcessorBase::createStream()");
		return false;
	}

	stream_out->channel_idx = ch_idx; // store channel assignment
	const bool loop = stream_out->loop;

	// Create playback stream
	unsigned int hstream = MiniAudio_StreamCreateFile(false, stream_out->sample_path, 0, loop);

	if (hstream == MINIAUDIO_NO_STREAM) {
		// Failed to create stream
		ALT_ERROR(1, "FAILED MiniAudio_StreamCreateFile(%s): %s", short_path.c_str(), get_miniaudio_err());

		ALT_OUTDENT;
		ALT_DEBUG(0, "END: AltsoundProcessorBase::createStream()");
		return false;
	}

	// Set callback to execute when sample playback ends
	SYNCPROC* callback = reinterpret_cast<SYNCPROC*>(syncproc_in);
	unsigned int hsync = 0;

	if (callback) {
		// Set sync to execute callback when sample playback ends
		hsync = MiniAudio_ChannelSetSync(hstream, MINIAUDIO_SYNC_END | MINIAUDIO_SYNC_ONETIME,
		                                 callback, stream_out);
		if (!hsync) {
			// Failed to set sync
			ALT_ERROR(1, "FAILED MiniAudio_ChannelSetSync(): STREAM: %u ERROR: %s", hstream, get_miniaudio_err());
			freeStream(hstream);

			ALT_OUTDENT;
			ALT_DEBUG(0, "END: AltsoundProcessorBase::createStream()");
			return false;
		}
	}
	ALT_INFO(1, "Successfully created stream(%u) on channel(%02d)", hstream, ch_idx);

	stream_out->hstream = hstream; // store hstream
	stream_out->hsync = hsync; // store hsync

	ALT_OUTDENT;
	ALT_DEBUG(0, "END: AltsoundProcessorBase::createStream()");
	return true;
}

// ----------------------------------------------------------------------------

bool AltsoundProcessorBase::freeStream(const unsigned int hstream_in)
{
	ALT_INFO(0, "BEGIN AltsoundProcessorBase::freeStream()");
	ALT_INDENT;

	const bool success = MiniAudio_StreamFree(hstream_in);
	if (!success) {
		ALT_ERROR(1, "Failed to free stream(%u)", hstream_in);
	} else {
		ALT_INFO(1, "Successfully free'd stream(%u)", hstream_in);
	}

	ALT_OUTDENT;
	ALT_DEBUG(0, "END AltsoundProcessorBase::freeStream()");
	return success;
}

// ----------------------------------------------------------------------------

bool AltsoundProcessorBase::stopStream(unsigned int hstream_in)
{
	ALT_DEBUG(0, "BEGIN: AltsoundProcessorBase::stopStream()");
	ALT_INDENT;

	bool success = false;

	if (hstream_in != MINIAUDIO_NO_STREAM) {
		if (MiniAudio_ChannelStop(hstream_in)) {
			success = freeStream(hstream_in);
			if (!success) {
				ALT_ERROR(0, "FAILED AltsoundProcessorBase::freeStream()");
			}
			else {
				ALT_INFO(0, "Successfully stopped stream(%u)", hstream_in);
			}
		}
		else {
			ALT_ERROR(0, "FAILED MiniAudio_ChannelStop(%u): %s", hstream_in, get_miniaudio_err());
		}
	}

	ALT_OUTDENT;
	ALT_DEBUG(0, "END: AltsoundProcessorBase::stopStream()");
	return success;
}

// ----------------------------------------------------------------------------

bool AltsoundProcessorBase::stopAllStreams()
{
	ALT_DEBUG(0, "BEGIN AltsoundProcessorBase::stopAllStreams()");
	ALT_INDENT;

	bool success = true;

	for (auto stream : channel_stream) {
		if (!stream)
			continue;

		if (!stopStream(stream->hstream)) {
			success = false;
			ALT_ERROR(0, "FAILED stopStream(%u)", stream->hstream);
		}
	}

	ALT_DEBUG(0, "END AltsoundProcessorBase::stopAllStreams()");
	return success;
}

// ---------------------------------------------------------------------------
// Helper function to remove major path from filenames.  Returns just:
// <ROM shortname>/<rest of path>
// ---------------------------------------------------------------------------

std::string AltsoundProcessorBase::getShortPath(const std::string& path_in)
{
	const std::size_t pos = path_in.find(game_name);
	if (pos != std::string::npos)
	{
		return path_in.substr(pos);
	}
	else
	{
		return path_in;
	}
}
