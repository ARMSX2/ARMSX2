// Host-build stub: replaces Host.h without the full PCSX2 host interface
#pragma once
#include <string>
#include <string_view>

#define TRANSLATE_NOOP(context, msg)   msg
#define TRANSLATE(context, msg)        (msg)
#define TRANSLATE_SV(context, msg)     std::string_view(msg)
#define TRANSLATE_STR(context, msg)    std::string(msg)
#define TRANSLATE_FS(context, msg)     std::string_view(msg)
#define TRANSLATE_PLURAL_FS(ctx, msg, dis, n) std::string_view(msg)

namespace Host
{
    static constexpr float OSD_INFO_DURATION  = 5.0f;
    static constexpr float OSD_QUICK_DURATION = 2.0f;
    static constexpr float OSD_ERROR_DURATION = 15.0f;

    inline void AddKeyedOSDMessage(std::string, std::string, float = OSD_INFO_DURATION) {}
    inline void RemoveKeyedOSDMessage(std::string) {}
    inline void AddOSDMessage(std::string, float = OSD_INFO_DURATION) {}

    inline std::string_view TranslateToStringView(const char*, const char* msg) { return msg; }
    inline const char*      TranslateToCString(const char*, const char* msg)    { return msg; }
    inline std::string      TranslateToString(const char*, const char* msg)     { return msg; }
    inline std::string      TranslatePluralToString(const char*, const char* msg, const char*, int) { return msg; }
}
