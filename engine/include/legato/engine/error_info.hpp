#ifndef LEGATO_ENGINE_ERROR_INFO_HPP
#define LEGATO_ENGINE_ERROR_INFO_HPP

#include <cstring>

namespace legato {

struct ErrorInfo {
    const char* name;
    int code;
    const char* category;
    const char* hint;
};

const ErrorInfo* error_info(const char* name);

}  /* namespace legato */

#endif
