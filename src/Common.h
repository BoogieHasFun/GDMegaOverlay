#pragma once

#include <string>
#include <vector>
#include "utils.hpp"

namespace cocos2d
{
    class CCLayer;
}

namespace Common
{
    inline void* gameManager = reinterpret_cast<void*(__stdcall*)()>(utils::gd_base + 0x121540)();
    inline void* FMODAudioEngine = reinterpret_cast<void*(__stdcall*)()>(utils::gd_base + 0x32BF0)();
    inline void* gjAccountManager = reinterpret_cast<void*(__stdcall*)()>(utils::gd_base + 0x18A510)();

    cocos2d::CCLayer* getBGL();

    inline const char*(__thiscall* splashString)();
    const char* __fastcall splashStringHook();
    inline bool(__thiscall* menuLayerInit)(int* self);
    bool __fastcall menuLayerInitHook(int* self, void*);

    void initHooks();
    void calculateFramerate();
    void setPriority();
    void onAudioSpeedChange();
    void onAudioPitchChange();
    void setIconValue(int valueOffset, int value);
    void saveIcons();
    void loadIcons();

    void checkForUpdate();

    int getRequest(std::string url, std::string* buffer, std::string userAgent = "");

    void openLink(const char* path);
};
