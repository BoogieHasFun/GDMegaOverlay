#pragma once

#include <string>
#include <vector>
#include "util.hpp"

namespace cocos2d
{
	class CCLayer;
}

namespace Common
{
	inline bool iconsLoaded = false;

	void calculateFramerate();
	void setPriority();
	void onAudioSpeedChange();
	void onAudioPitchChange();
	void saveIcons();
	void loadIcons();
};
