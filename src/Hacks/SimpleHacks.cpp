#include "Common.h"
#include "Settings.hpp"

#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/ShaderLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GameLevelManager.hpp>

using namespace geode::prelude;

class $modify(HardStreak)
{
	void updateStroke(float dt)
	{
		bool enabled = Settings::get<bool>("player/trail/enabled", false);

		if(enabled)
		{
			float trailSize = Settings::get<float>("player/trail/size", 1.f);
			MBO(float, this, 0x168) = trailSize;

			bool color = Settings::get<bool>("player/trail/color/enabled", false);

			if(color)
			{
				float trailColor[3]{
					Settings::get<float>("player/trail/color/r", 1.f),
					Settings::get<float>("player/trail/color/g", 1.f),
					Settings::get<float>("player/trail/color/b", 1.f)
				};

				this->setColor({(byte)(trailColor[0]*255), (byte)(trailColor[1]*255), (byte)(trailColor[2]*255)});
			}
		}

		HardStreak::updateStroke(dt);
	}
};

class $modify(PlayLayer)
{
	geode::Patch* AnticheatPatch;

	void resetLevel()
	{
		// skips the whole if check that checks for time in level and a bunch of other stuff
		m_fields->AnticheatPatch = Mod::get()->patch(reinterpret_cast<void*>(base::get() + 0x2DDC03), { 0xEB, 0x53 }).unwrap();
		m_fields->AnticheatPatch->enable();

		PlayLayer::resetLevel();

		if (Settings::get<bool>("level/instant_complete", false))
			this->playPlatformerEndAnimationToPos({ .0f, 105.f }, true);

		m_fields->AnticheatPatch->disable();
	}
};

class $modify(ShaderLayer)
{
	void visit()
	{
		bool enabled = Settings::get<bool>("level/no_shaders", false);
		if(enabled)
		{
			CCNode::visit();
			return;
		}

		ShaderLayer::visit();
	}
};