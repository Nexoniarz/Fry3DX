#pragma once
// OpenGL 4.6 entry points, loaded through SDL at runtime (no GL library linked, no extra
// dependency): gl::Enable(...), gl::DrawElements(...). Types and constants come from SDL's
// copy of the Khronos headers. Add a function to KEEL_GL_FUNCTIONS to use it.
#include <SDL3/SDL_opengl.h>

#define KEEL_GL_FUNCTIONS(X)                                                                                            \
    X(const GLubyte*, GetString, (GLenum name))                                                                         \
    X(const GLubyte*, GetStringi, (GLenum name, GLuint index))                                                          \
    X(void, GetIntegerv, (GLenum pname, GLint* data))                                                                   \
    X(GLenum, GetError, (void))                                                                                         \
    X(void, Enable, (GLenum cap))                                                                                       \
    X(void, Disable, (GLenum cap))                                                                                      \
    X(void, ClipControl, (GLenum origin, GLenum depth))                                                                 \
    X(void, FrontFace, (GLenum mode))                                                                                   \
    X(void, Viewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                                         \
    X(void, Scissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                                          \
    X(void, DepthFunc, (GLenum func))                                                                                   \
    X(void, DepthMask, (GLboolean flag))                                                                                \
    X(void, BlendFuncSeparate, (GLenum sc, GLenum dc, GLenum sa, GLenum da))                                            \
    X(void, PolygonOffset, (GLfloat factor, GLfloat units))                                                             \
    X(void, PixelStorei, (GLenum pname, GLint param))                                                                   \
    X(void, CreateBuffers, (GLsizei n, GLuint* buffers))                                                                \
    X(void, DeleteBuffers, (GLsizei n, const GLuint* buffers))                                                          \
    X(void, NamedBufferStorage, (GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags))                    \
    X(void, NamedBufferSubData, (GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data))                     \
    X(void, BindBufferBase, (GLenum target, GLuint index, GLuint buffer))                                               \
    X(void, CreateTextures, (GLenum target, GLsizei n, GLuint* textures))                                               \
    X(void, DeleteTextures, (GLsizei n, const GLuint* textures))                                                        \
    X(void, TextureStorage2D, (GLuint texture, GLsizei levels, GLenum format, GLsizei w, GLsizei h))                     \
    X(void, TextureSubImage2D, (GLuint texture, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,    \
                                GLenum type, const void* pixels))                                                       \
    X(void, CompressedTextureSubImage2D, (GLuint texture, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,         \
                                          GLenum format, GLsizei size, const void* data))                               \
    X(void, CreateSamplers, (GLsizei n, GLuint* samplers))                                                              \
    X(void, DeleteSamplers, (GLsizei n, const GLuint* samplers))                                                        \
    X(void, SamplerParameteri, (GLuint sampler, GLenum pname, GLint param))                                             \
    X(void, SamplerParameterf, (GLuint sampler, GLenum pname, GLfloat param))                                           \
    X(GLuint64, GetTextureSamplerHandleARB, (GLuint texture, GLuint sampler))                                           \
    X(void, MakeTextureHandleResidentARB, (GLuint64 handle))                                                            \
    X(void, MakeTextureHandleNonResidentARB, (GLuint64 handle))                                                         \
    X(void, CreateFramebuffers, (GLsizei n, GLuint* framebuffers))                                                      \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))                                                \
    X(void, NamedFramebufferTexture, (GLuint fb, GLenum attachment, GLuint texture, GLint level))                       \
    X(void, NamedFramebufferDrawBuffer, (GLuint fb, GLenum buf))                                                        \
    X(GLenum, CheckNamedFramebufferStatus, (GLuint fb, GLenum target))                                                  \
    X(void, BindFramebuffer, (GLenum target, GLuint fb))                                                                \
    X(void, NamedFramebufferDrawBuffers, (GLuint fb, GLsizei n, const GLenum* bufs))                                   \
    X(void, ColorMaski, (GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a))                             \
    X(void, ClearNamedFramebufferfv, (GLuint fb, GLenum buffer, GLint drawbuffer, const GLfloat* value))                \
    X(GLuint, CreateShader, (GLenum type))                                                                              \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length))             \
    X(void, CompileShader, (GLuint shader))                                                                             \
    X(void, GetShaderiv, (GLuint shader, GLenum pname, GLint* params))                                                  \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei max, GLsizei* length, GLchar* log))                               \
    X(void, DeleteShader, (GLuint shader))                                                                              \
    X(GLuint, CreateProgram, (void))                                                                                    \
    X(void, AttachShader, (GLuint program, GLuint shader))                                                              \
    X(void, LinkProgram, (GLuint program))                                                                              \
    X(void, GetProgramiv, (GLuint program, GLenum pname, GLint* params))                                                \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei max, GLsizei* length, GLchar* log))                             \
    X(void, DeleteProgram, (GLuint program))                                                                            \
    X(void, UseProgram, (GLuint program))                                                                               \
    X(void, CreateVertexArrays, (GLsizei n, GLuint* arrays))                                                            \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint* arrays))                                                      \
    X(void, BindVertexArray, (GLuint array))                                                                            \
    X(void, VertexArrayElementBuffer, (GLuint vao, GLuint buffer))                                                      \
    X(void, DrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices))                               \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                                      \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels))              \
    X(void, ReadBuffer, (GLenum src))                                                                                   \
    X(GLsync, FenceSync, (GLenum condition, GLbitfield flags))                                                          \
    X(GLenum, ClientWaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout))                                        \
    X(void, DeleteSync, (GLsync sync))                                                                                  \
    X(void, BindBuffer, (GLenum target, GLuint buffer))                                                                 \
    X(void*, MapNamedBufferRange, (GLuint buffer, GLintptr offset, GLsizeiptr length, GLbitfield access))               \
    X(GLboolean, UnmapNamedBuffer, (GLuint buffer))                                                                     \
    X(void, DebugMessageCallback, (GLDEBUGPROC callback, const void* user))

namespace keel::gl {
#define KEEL_GL_DECLARE(ret, name, params) using name##_t = ret(APIENTRY*) params; inline name##_t name = nullptr;
KEEL_GL_FUNCTIONS(KEEL_GL_DECLARE)
#undef KEEL_GL_DECLARE

// Loads every entry point from the current context; false (and logs which) if any is missing.
bool load();
} // namespace keel::gl
