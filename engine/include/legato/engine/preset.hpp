#ifndef LEGATO_ENGINE_PRESET_HPP
#define LEGATO_ENGINE_PRESET_HPP

#include <cstring>

namespace legato {

struct PresetDesc {
    const char* name;
    double buffer_start_s;
    double buffer_resume_s;
    double buffer_max_s;
    double live_target_latency_s;
    bool exact_seek;
    int preload_ahead;
    int preload_behind;
    bool video;
};

extern const PresetDesc kPresets[];
extern const int kPresetCount;

const PresetDesc* find_preset(const char* name);
const PresetDesc* default_preset();

}  /* namespace legato */

#endif
