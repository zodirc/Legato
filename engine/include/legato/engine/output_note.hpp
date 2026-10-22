#ifndef LEGATO_ENGINE_OUTPUT_NOTE_HPP
#define LEGATO_ENGINE_OUTPUT_NOTE_HPP

#include <cstdint>

namespace legato {

/* 输出线程写给控制线程的通知。必须能放进单生产者单消费者环。 */
struct OutputNote {
    enum Kind : std::uint8_t { None = 0, FirstFrame = 1, Stall = 2, Resume = 3, Failed = 4, Ended = 5 };
    Kind kind;
    std::int64_t position_us;
    std::int64_t extra_us;
    char name[32];
    char message[48];
};

}  /* namespace legato */

#endif
