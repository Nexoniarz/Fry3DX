#include "rhi/gl/gl_loader.h"
#include "core/log.h"

#include <SDL3/SDL_video.h>

#include <string>

namespace keel::gl {

bool load() {
    std::string missing;
#define KEEL_GL_LOAD(ret, name, params)                                                       \
    name = reinterpret_cast<name##_t>(SDL_GL_GetProcAddress("gl" #name));                     \
    if (!name) missing += " gl" #name;
    KEEL_GL_FUNCTIONS(KEEL_GL_LOAD)
#undef KEEL_GL_LOAD
    if (!missing.empty()) log::warn("OpenGL: missing functions:{}", missing);
    return missing.empty();
}

} // namespace keel::gl
