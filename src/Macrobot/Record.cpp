#include "Record.h"
#include "../Common.h"
#include "../GUI/GUI.h"
#include "../Settings.hpp"

#include "AudioRecord.h"
#include "Clickpacks.h"
#include "Macrobot.h"
#include "sndfile.h"
#include <Geode/cocos/platform/CCGL.h>

#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/Geode.hpp>

using namespace geode::prelude;
using namespace Record;

bool levelDone = false;

class $modify(PlayLayer){void onQuit(){if (recorder.m_recording) recorder.stop();
PlayLayer::onQuit();
}

void levelComplete()
{
	PlayLayer::levelComplete();
	levelDone = true;
}

void resetLevel()
{
	levelDone = false;
	PlayLayer::resetLevel();
}
}
;

class $modify(GJBaseGameLayer){void update(float dt){if (recorder.m_recording) recorder.handle_recording(this, dt);

if (recorder.m_recording_audio)
	recorder.handle_recording_audio(this, dt);

GJBaseGameLayer::update(dt);
}
}
;

std::string narrow(const wchar_t *str)
{
	int size = WideCharToMultiByte(CP_UTF8, 0, str, -1, nullptr, 0, nullptr, nullptr);
	if (size <= 0)
	{ /* fuck */
	}
	auto buffer = new char[size];
	WideCharToMultiByte(CP_UTF8, 0, str, -1, buffer, size, nullptr, nullptr);
	std::string result(buffer, size_t(size) - 1);
	delete[] buffer;
	return result;
}
inline auto narrow(const std::wstring &str)
{
	return narrow(str.c_str());
}

std::wstring widen(const char *str)
{
	int size = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
	if (size <= 0)
	{ /* fuck */
	}
	auto buffer = new wchar_t[size];
	MultiByteToWideChar(CP_UTF8, 0, str, -1, buffer, size);
	std::wstring result(buffer, size_t(size) - 1);
	delete[] buffer;
	return result;
}
inline auto widen(const std::string &str)
{
	return widen(str.c_str());
}

Recorder::Recorder() : m_width(1280), m_height(720), m_fps(60)
{
}

void Recorder::start()
{
	levelDone = false;
	int resolution[2];
	resolution[0] = Settings::get<int>("recorder/resolution/x", 1920);
	resolution[1] = Settings::get<int>("recorder/resolution/y", 1080);

	int framerate = Settings::get<int>("recorder/fps", 60);

	m_width = resolution[0];
	m_height = resolution[1];
	m_fps = framerate;

	std::string codec = Settings::get<std::string>("recorder/codec", "h264_nvenc");
	if (codec != "")
		m_codec = codec;

	std::string extraArgs =
		Settings::get<std::string>("recorder/extraArgs", "-hwaccel cuda -hwaccel_output_format cuda");

	if (extraArgs != "")
		m_extra_args = extraArgs;
	// if (hacks.extraArgsAfter != "")
	m_extra_args_after = ""; // hacks.extraArgsAfter;
	m_recording = true;
	m_frame_has_data = false;
	m_current_frame.resize(m_width * m_height * 3, 0);
	m_finished_level = false;
	m_last_frame_t = m_extra_t = 0;

	int bitrate = Settings::get<int>("recorder/bitrate", 30);
	m_bitrate = std::to_string(bitrate) + "M";

	float afterEnd = Settings::get<float>("recorder/after_end", 5.f);

	m_after_end_extra_time = 0.f;
	m_after_end_duration = afterEnd; // hacks.afterEndDuration;
	m_renderer.m_width = m_width;
	m_renderer.m_height = m_height;
	m_renderer.begin();
	tfx = 0;

	m_song_start_offset = GameManager::get()->getPlayLayer()->m_levelSettings->m_songOffset;

	GameManager::get()->getPlayLayer()->resetLevel();

	std::string level_id = std::to_string(GameManager::get()->getPlayLayer()->m_level->m_levelID.value());
	auto bg_volume = 1;
	auto sfx_volume = 1;

	auto song_offset = m_song_start_offset;

	if (!ghc::filesystem::is_directory(Mod::get()->getSaveDir().string() + "/renders/" + level_id) ||
		!ghc::filesystem::exists(Mod::get()->getSaveDir().string() + "/renders/" + level_id))
	{
		ghc::filesystem::create_directory(Mod::get()->getSaveDir().string() + "/renders/" + level_id);
	}

	if (m_recording_audio)
		return;

	std::thread([&, bg_volume, sfx_volume, song_offset, level_id]()
				{
		auto finalpath = (Mod::get()->getSaveDir().string() + "/renders/" + level_id + "/final.mp4");
		auto notfinalpath = (Mod::get()->getSaveDir().string() + "/renders/" + level_id + "/rendered_video.mp4");
		auto clickpath = (Mod::get()->getSaveDir().string() + "/renders/" + level_id + "/rendered_clicks.wav");

		{
			std::stringstream stream;
			stream << '"' << m_ffmpeg_path << '"' << " -y -f rawvideo -pix_fmt rgb24 -s " << m_width << "x" << m_height
				   << " -r " << m_fps;
			if (!m_extra_args.empty())
			{
				stream << " " << m_extra_args;
			}

			stream << " -i - ";
			if (!m_codec.empty())
				stream << "-c:v " << m_codec << " ";
			if (!m_bitrate.empty())
				stream << "-b:v " << m_bitrate << " ";
			if (!m_extra_args_after.empty())
				stream << m_extra_args_after << " ";
			else
				stream << "-pix_fmt yuv420p ";

			stream << "-vf \"vflip\" -an \"" << notfinalpath << "\" "; // i hope just putting it in "" escapes it
			auto process = subprocess::Popen(stream.str());
			while (m_recording || m_frame_has_data)
			{
				m_lock.lock();
				if (m_frame_has_data)
				{
					const auto frame = m_current_frame; // copy it
					m_frame_has_data = false;
					m_lock.unlock();
					process.m_stdin.write(frame.data(), frame.size());
				}
				else
					m_lock.unlock();
			}

			try
			{
				process.close();
			}
			catch (const std::exception& e)
			{
				std::cout << e.what() << '\n';
			}
		}

		if (!Settings::get<bool>("recorder/clicks/enabled", false))
			return;

		generate_clicks(clickpath);

		{
			float clickVolume = Settings::get<float>("clickpacks/click/volume", 1.f);
			std::stringstream f;
			f << '"' << m_ffmpeg_path << '"' << " -y -i " << '"' << notfinalpath << '"' << " -i " << '"' << clickpath
			  << '"' << " -c:v copy -map 0:v -map 1:a " << '"' << finalpath << '"';
			std::cout << f.str() << std::endl;
			auto process = subprocess::Popen(f.str());
			try
			{
				process.close();
			}
			catch (const std::exception& e)
			{
				std::cout << e.what() << '\n';
			}
		} })
		.detach();
}

void Recorder::stop()
{
	m_renderer.end();
	m_recording = false;
}

void MyRenderTexture::begin()
{
	glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &m_old_fbo);

	m_texture = new CCTexture2D;
	{
		auto data = malloc(m_width * m_height * 3);
		memset(data, 0, m_width * m_height * 3);
		m_texture->initWithData(data, kCCTexture2DPixelFormat_RGB888, m_width, m_height,
								CCSize(static_cast<float>(m_width), static_cast<float>(m_height)));
		free(data);
	}

	glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &m_old_rbo);

	glGenFramebuffersEXT(1, &m_fbo);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_fbo);

	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, m_texture->getName(), 0);

	m_texture->setAliasTexParameters();

	// m_texture->autorelease();

	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, m_old_rbo);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_old_fbo);
}

void MyRenderTexture::capture(std::mutex &lock, std::vector<u8> &data, volatile bool &lul)
{
	glViewport(0, 0, m_width, m_height);

	glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &m_old_fbo);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_fbo);

	auto director = CCDirector::sharedDirector();
	GameManager::get()->getPlayLayer()->visit();

	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	lock.lock();
	lul = true;
	glReadPixels(0, 0, m_width, m_height, GL_RGB, GL_UNSIGNED_BYTE, data.data());
	lock.unlock();

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_old_fbo);
	director->setViewport();
}

void MyRenderTexture::end()
{
	m_texture->release();
}

void Recorder::capture_frame()
{
	while (m_frame_has_data)
	{
	}

	m_renderer.capture(m_lock, m_current_frame, m_frame_has_data);
}

void Recorder::stop_audio()
{
	AudioRecord::stop();
	m_recording_audio = false;

	GJGameLevel *level =
		GameManager::get()
			->getPlayLayer()
			->m_level; // MBO(gd::GJGameLevel*, GameManager::get()->getPlayLayer(), 1504); // found in playlayer_init

	std::string level_id = std::to_string(level->m_levelID.value());

	std::string video_path = Mod::get()->getSaveDir().string() + "/renders/" + level_id + "/final.mp4";

	bool clicks = ghc::filesystem::exists(video_path);

	if (!clicks)
		video_path = Mod::get()->getSaveDir().string() + "/renders/" + level_id + "/rendered_video.mp4";

	std::string temp_path = Mod::get()->getSaveDir().string() + "/renders/" + level_id + "/music.mp4";

	std::stringstream ss;

	if (clicks)
	{
		ss << m_ffmpeg_path << " -i " << video_path
		   << " -i fmodoutput.wav -filter_complex \"[0:a][1:a] amix = duration = longest[a]\" -map 0:v -map \"[a]\" "
			  "-c:v "
			  "copy "
		   << temp_path;
	}
	else
	{
		ss << m_ffmpeg_path << " -y -i " << video_path << " -i fmodoutput.wav -c:v copy -map 0:v -map 1:a "
		   << temp_path;
	}

	auto process = subprocess::Popen(ss.str());
	try
	{
		process.close();
	}
	catch (const std::exception &e)
	{
		std::cout << e.what() << '\n';
	}

	ghc::filesystem::remove("fmodoutput.wav");

	ghc::filesystem::remove(widen(video_path));
	ghc::filesystem::rename(temp_path, widen(video_path));
}

void Recorder::handle_recording_audio(GJBaseGameLayer *play_layer, float dt)
{
	if (!levelDone || m_after_end_extra_time < m_after_end_duration)
	{
		if (levelDone)
		{
			m_after_end_extra_time += dt;
			m_finished_level = true;
		}

		if (!AudioRecord::recording)
			AudioRecord::start();
	}
	else
	{
		if (AudioRecord::recording)
			stop_audio();
	}
}

void Recorder::handle_recording(GJBaseGameLayer *play_layer, float dt)
{
	if (!levelDone || m_after_end_extra_time < m_after_end_duration)
	{
		if (levelDone)
		{
			m_after_end_extra_time += dt;
			m_finished_level = true;
		}

		/* if (!play_layer->m_hasCompletedLevel)
			tfx = play_layer->timeForXPos(play_layer->m_pPlayer1->getPositionX());
		else */
		tfx += dt;
		float timewarp = MBO(float, GameManager::get()->getPlayLayer(), 724);

		auto frame_dt = 1. / static_cast<double>(m_fps) * timewarp;
		auto time = tfx + m_extra_t - m_last_frame_t;
		if (time >= frame_dt)
		{
			m_extra_t = time - frame_dt;
			m_last_frame_t = tfx;
			capture_frame();
		}
	}
	else
	{
		stop();
	}
}

bool Recorder::areAudioFilesValid(const std::vector<ghc::filesystem::path> &files, const std::string& dirName)
{
	if(files.size() <= 0)
	{
		FLAlertLayer::create("Info", "Make sure your clickpacks contains " + dirName + "!", "Ok")->show();
		return false;
	}

	for (auto filename : Clickpacks::currentClickpack.clicks)
	{
		SF_INFO inputSfInfoDummy;
		SNDFILE *inputSndFileDummy = sf_open(filename.string().c_str(), SFM_READ, &inputSfInfoDummy);

		if (sampleRate != 0 && inputSfInfoDummy.samplerate != sampleRate)
		{
			FLAlertLayer::create("Info", "Make sure all of your files have the same sample rate! " + dirName + "/" +
										 filename.string() + ".wav", "Ok")->show();
			sf_close(inputSndFileDummy);
			return false;
		}

		if (numChannels != 0 && inputSfInfoDummy.channels != numChannels)
		{
			FLAlertLayer::create("Info", "Make sure all of your files have the same channel number! " + dirName + "/" +
										 filename.string() + ".wav", "Ok")->show();
			sf_close(inputSndFileDummy);
			return false;
		}

		if (sampleFormat != 0 && inputSfInfoDummy.format != sampleFormat)
		{
			FLAlertLayer::create("Info", "Make sure all of your files have the same sample format! " + dirName + "/" +
										 filename.string() + ".wav", "Ok")->show();
			sf_close(inputSndFileDummy);
			return false;
		}

		sampleRate = inputSfInfoDummy.samplerate;
		numChannels = inputSfInfoDummy.channels;
		sampleFormat = inputSfInfoDummy.format;

		sf_close(inputSndFileDummy);
	}

	return true;
}

bool Recorder::generate_clicks(const std::string& outputPath)
{
	std::string clickPath = Settings::get<std::string>("clickpacks/path", "");

	if (clickPath == "")
		return false;

	if (!areAudioFilesValid(Clickpacks::currentClickpack.clicks, "clicks"))
		return false;

	if (!areAudioFilesValid(Clickpacks::currentClickpack.softclicks, "softclicks"))
		return false;

	if (!areAudioFilesValid(Clickpacks::currentClickpack.releases, "releases"))
		return false;

	if (!areAudioFilesValid(Clickpacks::currentClickpack.platClicks, "platClicks"))
		return false;

	if (!areAudioFilesValid(Clickpacks::currentClickpack.platReleases, "platReleases"))
		return false;

	const char *outputFile = outputPath.c_str();

	ghc::filesystem::create_directories(ghc::filesystem::path(outputFile).parent_path());

	SF_INFO sfInfo;
	sfInfo.samplerate = sampleRate;
	sfInfo.channels = numChannels;
	sfInfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	SNDFILE *sndFile = sf_open(outputFile, SFM_WRITE, &sfInfo);

	if (!sndFile)
		return false;

	double previousTimestamp = 0.0;

	std::vector<int16_t> mixedBuffer;

	float softclickAt = Settings::get<float>("clickpacks/softclicks_at", 0.1f);

	bool wasLastActionPressP1 = false;
	bool wasLastActionPressP2 = false;

	bool wasLastPlatActionPressP1 = false;
	bool wasLastPlatActionPressP2 = false;

	for (const Macrobot::Action &ac : Macrobot::macro.inputs)
	{
		double timestamp = ac.time;

		if (timestamp == previousTimestamp)
			continue;

		ghc::filesystem::path soundPath;
		if (ac.button <= 1)
		{
			if (!ac.player2)
			{
				if (ac.down == wasLastActionPressP1)
					continue;

				wasLastActionPressP1 = ac.down;
			}
			else
			{
				if (ac.down == wasLastActionPressP2)
					continue;

				wasLastActionPressP2 = ac.down;
			}

			soundPath =
				ac.down ? (timestamp - previousTimestamp <= softclickAt
							   ? Clickpacks::currentClickpack
									 .softclicks[util::randomInt(0, Clickpacks::currentClickpack.softclicks.size() - 1)]
							   : Clickpacks::currentClickpack
									 .clicks[util::randomInt(0, Clickpacks::currentClickpack.clicks.size() - 1)])
						: Clickpacks::currentClickpack
							  .releases[util::randomInt(0, Clickpacks::currentClickpack.releases.size() - 1)];
		}
		else
		{
			if (!ac.player2)
			{
				if (ac.down == wasLastPlatActionPressP1)
					continue;

				wasLastPlatActionPressP1 = ac.down;
			}
			else
			{
				if (ac.down == wasLastPlatActionPressP2)
					continue;

				wasLastPlatActionPressP2 = ac.down;
			}

			soundPath =
				ac.down ? Clickpacks::currentClickpack
							  .platClicks[util::randomInt(0, Clickpacks::currentClickpack.platClicks.size() - 1)]
						: Clickpacks::currentClickpack
							  .platReleases[util::randomInt(0, Clickpacks::currentClickpack.platReleases.size() - 1)];
		}

		SF_INFO inputSfInfo;
		SNDFILE *inputSndFile = sf_open(soundPath.string().c_str(), SFM_READ, &inputSfInfo);
		if (!inputSndFile)
		{
			std::cout << "Failed to open input file: " << soundPath << std::endl;
			continue;
		}

		double duration = inputSfInfo.frames / static_cast<double>(sampleRate);

		double timeDiff = timestamp - previousTimestamp - duration;

		if (timeDiff < 0)
			timeDiff = 0;

		previousTimestamp = timestamp;

		inputSfInfo.samplerate = sampleRate;

		int16_t *outputBuffer = new int16_t[inputSfInfo.frames * inputSfInfo.channels];

		sf_readf_short(inputSndFile, outputBuffer, inputSfInfo.frames);

		sf_close(inputSndFile);

		long long frameTime = static_cast<long long>(timestamp * sampleRate * numChannels);

		if (mixedBuffer.size() < frameTime + (inputSfInfo.frames * inputSfInfo.channels))
			mixedBuffer.resize(frameTime + (inputSfInfo.frames * inputSfInfo.channels), 0);

		for (long long i = 0; i < inputSfInfo.frames * inputSfInfo.channels; ++i)
			mixedBuffer[frameTime + i] += outputBuffer[i];

		delete[] outputBuffer;
	}

	sf_writef_short(sndFile, mixedBuffer.data(), mixedBuffer.size() / numChannels);

	sf_close(sndFile);

	return true;
}

void Record::renderWindow()
{
	if (Record::recorder.m_recording_audio)
		ImGui::BeginDisabled();

	if (GUI::button("Start Recording") && GameManager::get()->getPlayLayer())
	{
		if (!ghc::filesystem::exists("ffmpeg.exe"))
		{
			auto process = subprocess::Popen(string::wideToUtf8((Mod::get()->getResourcesDir() / "get_ffmpeg.exe").wstring()));
			try
			{
				process.close();
			}
			catch (const std::exception &e)
			{
				std::cout << e.what() << '\n';
			}
		}
		Record::recorder.start();
	}

	if (GUI::button("Stop Recording") && Record::recorder.m_recording && GameManager::get()->getPlayLayer())
		Record::recorder.stop();

	if (Record::recorder.m_recording_audio)
		ImGui::EndDisabled();

	if (Record::recorder.m_recording)
		ImGui::BeginDisabled();

	if (GUI::button("Start Audio") && GameManager::get()->getPlayLayer())
	{
		if (!ghc::filesystem::exists("ffmpeg.exe"))
		{
			auto process = subprocess::Popen(string::wideToUtf8((Mod::get()->getResourcesDir() / "get_ffmpeg.exe").wstring()));
			try
			{
				process.close();
			}
			catch (const std::exception &e)
			{
				std::cout << e.what() << '\n';
			}
		}
		recorder.m_recording_audio = true;
		Record::recorder.start();
		recorder.m_recording = false;
	}

	if (GUI::button("Stop Audio") && Record::recorder.m_recording_audio && GameManager::get()->getPlayLayer())
	{
		recorder.m_recording_audio = false;
		AudioRecord::stop();
	}

	if (Record::recorder.m_recording)
		ImGui::EndDisabled();

	int resolution[2];
	resolution[0] = Settings::get<int>("recorder/resolution/x", 1920);
	resolution[1] = Settings::get<int>("recorder/resolution/y", 1080);

	GUI::inputInt2("Resolution", resolution, 256, 7680, 144, 4320);
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		Mod::get()->setSavedValue<int>("recorder/resolution/x", resolution[0]);
		Mod::get()->setSavedValue<int>("recorder/resolution/y", resolution[1]);
	}

	int bitrate = Settings::get<int>("recorder/bitrate", 30);
	GUI::inputInt("Bitrate", &bitrate);

	if (ImGui::IsItemDeactivatedAfterEdit())
		Mod::get()->setSavedValue<int>("recorder/bitrate", bitrate);

	std::string codec = Settings::get<std::string>("recorder/codec", "");

	if (GUI::inputText("Codec", &codec))
		Mod::get()->setSavedValue<std::string>("recorder/codec", codec);

	std::string extraArgs = Settings::get<std::string>("recorder/extraArgs", "");
	if (GUI::inputText("Extra Args", &extraArgs))
		Mod::get()->setSavedValue<std::string>("recorder/extraArgs", extraArgs);

	int framerate = Settings::get<int>("recorder/fps", 60);
	GUI::inputInt("Framerate", &framerate);

	if (ImGui::IsItemDeactivatedAfterEdit())
		Mod::get()->setSavedValue<int>("recorder/fps", framerate);

	float afterEnd = Settings::get<float>("recorder/after_end", 5.f);
	GUI::inputFloat("Show Endscreen For", &afterEnd);

	if (ImGui::IsItemDeactivatedAfterEdit())
		Mod::get()->setSavedValue<float>("recorder/after_end", afterEnd);

	GUI::checkbox("Record clicks", "recorder/clicks/enabled");
	GUI::arrowButton("Clickpacks");
	Clickpacks::drawGUI();

	if (GUI::button("NVIDIA"))
	{
		Mod::get()->setSavedValue<std::string>("recorder/codec", "h264_nvenc");
		Mod::get()->setSavedValue<std::string>("recorder/extraArgs", "-hwaccel cuda -hwaccel_output_format cuda");
	}

	GUI::sameLine();

	if (GUI::button("AMD"))
	{
		Mod::get()->setSavedValue<std::string>("recorder/codec", "h264_amf");
		Mod::get()->setSavedValue<std::string>("recorder/extraArgs", "");
	}

	GUI::marker("[INFO]",
				"Press start recording to get a smooth recording of the level with optionally added clicks. "
				"To render music and sfx, press Start Music and wait for the level to finish again, then your "
				"rendered video will have music and sfx.");
}