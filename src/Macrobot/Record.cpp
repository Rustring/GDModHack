#include "Record.h"
#include "../Common.h"
#include "../GUI/GUI.h"
#include "../Settings.hpp"

#include "AudioRecord.h"
#include "Macrobot.h"
#include <Geode/cocos/platform/CCGL.h>

#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/ShaderLayer.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/Geode.hpp>

using namespace geode::prelude;
using namespace Record;

bool levelDone = false;
uintptr_t addr = 0;

void glViewportHook(GLint a, GLint b, GLsizei c, GLsizei d)
{
	if(visiting && recorder.m_recording && inShader)
	{
		ImVec2 screenSize = ImGui::GetIO().DisplaySize;
		if(c == screenSize.x && d == screenSize.y)
		{
			c = Settings::get<int>("recorder/resolution/x", 1920);
			d = Settings::get<int>("recorder/resolution/y", 1080);
		}
	}
	reinterpret_cast<void(__stdcall *)(GLint, GLint, GLsizei, GLsizei)>(addr)(a, b, c, d);
}

$execute
{
	void (__stdcall *glViewportFunc)(GLint, GLint, GLsizei, GLsizei) = glViewport;
	addr = reinterpret_cast<uintptr_t>(glViewportFunc);
	Mod::get()->hook(reinterpret_cast<void *>(addr), &glViewportHook, "glViewport", tulip::hook::TulipConvention::Stdcall);
}

class $modify(ShaderLayer)
{
	void visit()
	{
		inShader = true;
		ShaderLayer::visit();
		inShader = false;
	}
};

class $modify(PlayLayer)
{
	void onQuit()
	{
		if (recorder.m_recording) recorder.stop();
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
};

class $modify(GJBaseGameLayer)
{
	void update(float dt)
	{
		if (recorder.m_recording) recorder.handle_recording(this, dt);

		if (recorder.m_recording_audio)
			recorder.handle_recording_audio(this, dt);

		GJBaseGameLayer::update(dt);
	}
};

Recorder::Recorder()
	: m_width(1280), m_height(720), m_fps(60)
{}

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

	std::string level_id = GameManager::get()->getPlayLayer()->m_level->m_levelName.c_str() + ("_" + std::to_string(GameManager::get()->getPlayLayer()->m_level->m_levelID.value()));
	auto bg_volume = 1;
	auto sfx_volume = 1;

	auto song_offset = m_song_start_offset;

	if (!ghc::filesystem::is_directory(Mod::get()->getSaveDir() / "renders" / level_id) ||
		!ghc::filesystem::exists(Mod::get()->getSaveDir() / "renders" / level_id))
	{
		ghc::filesystem::create_directory(Mod::get()->getSaveDir() / "renders" / level_id);
	}

	if (m_recording_audio)
		return;

	std::thread([&, bg_volume, sfx_volume, song_offset, level_id]()
				{
		auto renderedVideo = (Mod::get()->getSaveDir() / "renders" / level_id / "rendered_video.mp4");

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

			stream << "-vf \"vflip\" -an " << renderedVideo; // i hope just putting it in "" escapes it
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
		} })
		.detach();
}

void Recorder::stop()
{
	FLAlertLayer::create("Info", "Macro rendererd successfully!", "Ok")->show();
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
	auto director = CCDirector::sharedDirector();
	glViewport(0, 0, m_width, m_height);

	glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &m_old_fbo);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_fbo);

	visiting = true;
	GameManager::get()->getPlayLayer()->visit();
	visiting = false;

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
	FLAlertLayer::create("Info", "Sound recorded successfully!", "Ok")->show();
	AudioRecord::stop();
	m_recording_audio = false;

	GJGameLevel *level =
		GameManager::get()
			->getPlayLayer()
			->m_level; // MBO(gd::GJGameLevel*, GameManager::get()->getPlayLayer(), 1504); // found in playlayer_init

	std::string level_id = GameManager::get()->getPlayLayer()->m_level->m_levelName.c_str() + ("_" + std::to_string(GameManager::get()->getPlayLayer()->m_level->m_levelID.value()));

	ghc::filesystem::path video_path = Mod::get()->getSaveDir() / "renders" / level_id / "rendered_video.mp4";

	ghc::filesystem::path temp_path = Mod::get()->getSaveDir() / "renders" / level_id / "music.mp4";

	std::stringstream ss;

	ss << m_ffmpeg_path << " -y -i " << video_path << " -i fmodoutput.wav -c:v copy -map 0:v -map 1:a "
		   << temp_path;

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

	ghc::filesystem::remove(video_path);
	ghc::filesystem::rename(temp_path,video_path);
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

void Record::renderWindow()
{
	bool disabled = !GameManager::get()->getPlayLayer() || Record::recorder.m_recording_audio || Macrobot::macro.inputs.size() <= 0 || Macrobot::playerMode != 0;
	if (disabled)
		ImGui::BeginDisabled();

	if (GUI::button("Start Recording"))
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

	if(GUI::shouldRender() && disabled && ImGui::IsItemHovered())
		ImGui::SetTooltip("You need to be playing a macro to record");

	if (GUI::button("Stop Recording") && Record::recorder.m_recording)
		Record::recorder.stop();

	if (disabled)
		ImGui::EndDisabled();

	if (disabled)
		ImGui::BeginDisabled();

	if (GUI::button("Start Audio"))
	{
		std::string level_id = GameManager::get()->getPlayLayer()->m_level->m_levelName.c_str() + ("_" + std::to_string(GameManager::get()->getPlayLayer()->m_level->m_levelID.value()));
		auto path = Mod::get()->getSaveDir() / "renders" / level_id / "rendered_video.mp4";
		bool hasVideo = ghc::filesystem::exists(path);

		if(!hasVideo)
			FLAlertLayer::create("Error", ("The render for the level has not been found!\n" + string::wideToUtf8(path.wstring())).c_str(), "Ok")->show();
		else
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
	}

	if(GUI::shouldRender() && disabled && ImGui::IsItemHovered())
		ImGui::SetTooltip("You need to be playing a macro to record");

	if (GUI::button("Stop Audio") && Record::recorder.m_recording_audio)
	{
		recorder.m_recording_audio = false;
		AudioRecord::stop();
	}

	if (disabled)
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
				"Press start recording to get a smooth recording of the level. "
				"To render music, clicks and sfx, press Start Music and wait for the level to finish again, then your "
				"rendered video will have music, clicks and sfx.");
}