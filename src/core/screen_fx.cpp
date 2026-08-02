#include "screen_fx.h"

#include <dlfcn.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>

#include "loader.h"
#include "log.h"

#if defined(__ANDROID__)
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif

namespace mcbe::fx {
namespace {

// ---------------------------------------------------------------------------
// What can be turned on
// ---------------------------------------------------------------------------

enum Effect {
    kPixelate,
    kFisheye,
    kWave,
    kGlitch,
    kChroma,
    kCrt,
    kVignette,
    kGray,
    kInvert,
    kEffectCount,
};

struct Named {
    const char* name;
    const char* uniform;
};

const Named kNames[kEffectCount] = {
    {"pixelate", "uPixelate"}, {"fisheye", "uFisheye"},   {"wave", "uWave"},
    {"glitch", "uGlitch"},     {"chroma", "uChroma"},     {"crt", "uCrt"},
    {"vignette", "uVignette"}, {"grayscale", "uGray"},    {"invert", "uInvert"},
};

// Written from whichever thread runs the mod, read on the render thread.
std::atomic<float> g_amounts[kEffectCount];
std::atomic<bool> g_any{false};
std::atomic<bool> g_running{false};

void clear_all() {
    for (size_t index = 0; index < kEffectCount; ++index) g_amounts[index].store(0.0f);
    g_any.store(false);
}

#if defined(__ANDROID__)

// ---------------------------------------------------------------------------
// The shader
// ---------------------------------------------------------------------------

constexpr const char* kVertexSource = R"GLSL(
attribute vec2 aPos;
varying vec2 vUv;
void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

constexpr const char* kFragmentSource = R"GLSL(
precision mediump float;

varying vec2 vUv;

uniform sampler2D uTex;
uniform vec2 uRes;
uniform float uTime;

uniform float uPixelate;
uniform float uFisheye;
uniform float uWave;
uniform float uGlitch;
uniform float uChroma;
uniform float uCrt;
uniform float uVignette;
uniform float uGray;
uniform float uInvert;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    vec2 uv = vUv;

    // Chunky pixels: the coordinate is snapped to a coarse grid, so whole
    // squares of the picture take one colour.
    if (uPixelate > 0.001) {
        vec2 cells = max(vec2(16.0), uRes / mix(1.0, 24.0, clamp(uPixelate, 0.0, 1.0)));
        uv = (floor(uv * cells) + 0.5) / cells;
    }

    // A lens: the further from the middle, the more the picture is pushed out.
    if (uFisheye > 0.001) {
        vec2 centred = uv - 0.5;
        float radius = length(centred);
        centred *= 1.0 + uFisheye * (radius * radius - 0.25);
        uv = centred + 0.5;
    }

    if (uWave > 0.001) {
        uv.x += sin(uv.y * 24.0 + uTime * 3.0) * 0.012 * uWave;
        uv.y += cos(uv.x * 18.0 + uTime * 2.0) * 0.008 * uWave;
    }

    // Bands slide sideways, a few of them at a time, changing several times a
    // second: a picture that has lost its hold.
    if (uGlitch > 0.001) {
        float band = floor(uv.y * 28.0);
        float jump = hash(vec2(band, floor(uTime * 12.0)));
        if (jump > 1.0 - 0.35 * uGlitch) {
            uv.x += (jump - 0.5) * 0.08 * uGlitch;
        }
    }

    vec3 colour;
    if (uChroma > 0.001) {
        // Red and blue are read a little to either side of green, the way a
        // cheap lens fails to bring the colours to the same place.
        vec2 shift = (uv - 0.5) * 0.014 * uChroma;
        colour = vec3(texture2D(uTex, uv + shift).r,
                      texture2D(uTex, uv).g,
                      texture2D(uTex, uv - shift).b);
    } else {
        colour = texture2D(uTex, uv).rgb;
    }

    // Distortion can push a sample past the edge, where there is nothing to
    // show; black reads as the edge of a screen rather than a smear.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) colour = vec3(0.0);

    if (uCrt > 0.001) {
        float line = sin(uv.y * uRes.y * 1.4) * 0.5 + 0.5;
        colour *= mix(1.0, 0.62 + 0.38 * line, uCrt);

        float stripe = mod(floor(uv.x * uRes.x), 3.0);
        vec3 tint = vec3(stripe == 0.0 ? 1.18 : 0.9,
                         stripe == 1.0 ? 1.18 : 0.9,
                         stripe == 2.0 ? 1.18 : 0.9);
        colour *= mix(vec3(1.0), tint, uCrt * 0.6);
        colour += (hash(uv * uRes + uTime) - 0.5) * 0.05 * uCrt;
    }

    if (uVignette > 0.001) {
        float edge = length(uv - 0.5) * 1.4142;
        colour *= mix(1.0, smoothstep(1.0, 0.25, edge), uVignette);
    }

    if (uGray > 0.001) {
        float grey = dot(colour, vec3(0.299, 0.587, 0.114));
        colour = mix(colour, vec3(grey), uGray);
    }

    if (uInvert > 0.001) colour = mix(colour, vec3(1.0) - colour, uInvert);

    gl_FragColor = vec4(colour, 1.0);
}
)GLSL";

// ---------------------------------------------------------------------------
// EGL, reached without a header of its own
// ---------------------------------------------------------------------------

constexpr int kEglWidth = 0x3057;
constexpr int kEglHeight = 0x3056;

using SwapBuffers = unsigned (*)(void*, void*);
using QuerySurface = unsigned (*)(void*, void*, int, int*);

SwapBuffers g_original_swap = nullptr;
QuerySurface g_query_surface = nullptr;

// GLES 3 keeps vertex array state in an object, and while one is bound the
// arrays this code sets up would go into it. Reached by name because the
// loader targets GLES 2 for the devices that only have that.
using BindVertexArray = void (*)(GLuint);
BindVertexArray g_bind_vertex_array = nullptr;
constexpr GLenum kVertexArrayBinding = 0x85B5;

// ---------------------------------------------------------------------------
// What lives on the GPU
// ---------------------------------------------------------------------------

struct Resources {
    GLuint program = 0;
    GLuint texture = 0;
    GLuint buffer = 0;
    GLint width = 0;
    GLint height = 0;
    GLint position_attribute = -1;
    GLint texture_uniform = -1;
    GLint resolution_uniform = -1;
    GLint time_uniform = -1;
    GLint amount_uniforms[kEffectCount] = {};
    bool broken = false;
};

Resources g_gpu;
double g_started_at = 0.0;
uint64_t g_frames = 0;

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (shader == 0) return 0;

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
        char message[512] = {};
        glGetShaderInfoLog(shader, sizeof(message) - 1, nullptr, message);
        MCBE_LOGE("screen effect shader did not compile: %s", message);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool build_program(Resources& gpu) {
    GLuint vertex = compile(GL_VERTEX_SHADER, kVertexSource);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragmentSource);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        return false;
    }

    gpu.program = glCreateProgram();
    glAttachShader(gpu.program, vertex);
    glAttachShader(gpu.program, fragment);
    glLinkProgram(gpu.program);

    // The shaders belong to the program now.
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = 0;
    glGetProgramiv(gpu.program, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
        char message[512] = {};
        glGetProgramInfoLog(gpu.program, sizeof(message) - 1, nullptr, message);
        MCBE_LOGE("screen effect program did not link: %s", message);
        glDeleteProgram(gpu.program);
        gpu.program = 0;
        return false;
    }

    gpu.position_attribute = glGetAttribLocation(gpu.program, "aPos");
    gpu.texture_uniform = glGetUniformLocation(gpu.program, "uTex");
    gpu.resolution_uniform = glGetUniformLocation(gpu.program, "uRes");
    gpu.time_uniform = glGetUniformLocation(gpu.program, "uTime");
    for (size_t index = 0; index < kEffectCount; ++index) {
        gpu.amount_uniforms[index] = glGetUniformLocation(gpu.program, kNames[index].uniform);
    }
    return true;
}

bool prepare(Resources& gpu, GLint width, GLint height) {
    if (gpu.broken) return false;

    if (gpu.program == 0 && !build_program(gpu)) {
        gpu.broken = true;
        return false;
    }

    if (gpu.buffer == 0) {
        // One triangle large enough to cover the screen: fewer vertices than a
        // quad and no seam down the diagonal.
        const GLfloat corners[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
        glGenBuffers(1, &gpu.buffer);
        glBindBuffer(GL_ARRAY_BUFFER, gpu.buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    }

    if (gpu.texture == 0) {
        glGenTextures(1, &gpu.texture);
        glBindTexture(GL_TEXTURE_2D, gpu.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gpu.width = 0;
        gpu.height = 0;
    }

    if (gpu.width != width || gpu.height != height) {
        glBindTexture(GL_TEXTURE_2D, gpu.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE,
                     nullptr);
        gpu.width = width;
        gpu.height = height;
    }
    return true;
}

/**
 * Everything the game left set, put back exactly as it was.
 *
 * The game does not expect anyone else to touch the context, so anything
 * changed here and not restored shows up as a corrupted frame later, in a
 * place with no obvious connection to this code.
 */
struct SavedState {
    GLint program = 0;
    GLint active_texture = 0;
    GLint texture_2d = 0;
    GLint array_buffer = 0;
    GLint framebuffer = 0;
    GLint vertex_array = 0;
    GLint viewport[4] = {};
    GLboolean colour_mask[4] = {};
    GLboolean depth_test = GL_FALSE;
    GLboolean blend = GL_FALSE;
    GLboolean cull = GL_FALSE;
    GLboolean scissor = GL_FALSE;
    GLboolean stencil = GL_FALSE;
    GLboolean dither = GL_FALSE;

    GLint attribute = 0;
    GLint attribute_enabled = 0;
    GLint attribute_size = 0;
    GLint attribute_type = 0;
    GLint attribute_normalised = 0;
    GLint attribute_stride = 0;
    GLint attribute_buffer = 0;
    void* attribute_pointer = nullptr;

    void save(GLint attribute_index) {
        attribute = attribute_index;

        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetBooleanv(GL_COLOR_WRITEMASK, colour_mask);

        depth_test = glIsEnabled(GL_DEPTH_TEST);
        blend = glIsEnabled(GL_BLEND);
        cull = glIsEnabled(GL_CULL_FACE);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        dither = glIsEnabled(GL_DITHER);

        if (g_bind_vertex_array != nullptr) glGetIntegerv(kVertexArrayBinding, &vertex_array);

        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d);

        if (attribute >= 0) {
            glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attribute_enabled);
            glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attribute_size);
            glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attribute_type);
            glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                                &attribute_normalised);
            glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attribute_stride);
            glGetVertexAttribiv(attribute, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                                &attribute_buffer);
            glGetVertexAttribPointerv(attribute, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                      &attribute_pointer);
        }
    }

    void restore() {
        if (attribute >= 0) {
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(attribute_buffer));
            glVertexAttribPointer(attribute, attribute_size, static_cast<GLenum>(attribute_type),
                                  attribute_normalised == GL_TRUE, attribute_stride,
                                  attribute_pointer);
            if (attribute_enabled == GL_TRUE) {
                glEnableVertexAttribArray(attribute);
            } else {
                glDisableVertexAttribArray(attribute);
            }
        }

        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_2d));
        glActiveTexture(static_cast<GLenum>(active_texture));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer));
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        glUseProgram(static_cast<GLuint>(program));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glColorMask(colour_mask[0], colour_mask[1], colour_mask[2], colour_mask[3]);

        if (g_bind_vertex_array != nullptr) {
            g_bind_vertex_array(static_cast<GLuint>(vertex_array));
        }

        auto set = [](GLenum capability, GLboolean was) {
            if (was == GL_TRUE) {
                glEnable(capability);
            } else {
                glDisable(capability);
            }
        };
        set(GL_DEPTH_TEST, depth_test);
        set(GL_BLEND, blend);
        set(GL_CULL_FACE, cull);
        set(GL_SCISSOR_TEST, scissor);
        set(GL_STENCIL_TEST, stencil);
        set(GL_DITHER, dither);
    }
};

double now_seconds() {
    timespec time = {};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1e9;
}

void draw(void* display, void* surface) {
    if (!g_any.load()) return;
    if (g_query_surface == nullptr) return;

    int width = 0;
    int height = 0;
    if (g_query_surface(display, surface, kEglWidth, &width) == 0 ||
        g_query_surface(display, surface, kEglHeight, &height) == 0) {
        return;
    }
    if (width <= 0 || height <= 0) return;

    // Anything the game left behind would be blamed on this code, so the slate
    // starts clean and any error raised here is swallowed at the end.
    while (glGetError() != GL_NO_ERROR) {
    }

    SavedState saved;
    saved.save(g_gpu.program == 0 ? 0 : g_gpu.position_attribute);

    if (g_bind_vertex_array != nullptr) g_bind_vertex_array(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!prepare(g_gpu, width, height)) {
        saved.restore();
        return;
    }

    // The finished picture is read back out of the buffer about to be shown.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_gpu.texture);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, width, height);

    glUseProgram(g_gpu.program);
    glUniform1i(g_gpu.texture_uniform, 0);
    glUniform2f(g_gpu.resolution_uniform, static_cast<float>(width), static_cast<float>(height));
    glUniform1f(g_gpu.time_uniform, static_cast<float>(now_seconds() - g_started_at));
    for (size_t index = 0; index < kEffectCount; ++index) {
        if (g_gpu.amount_uniforms[index] < 0) continue;
        glUniform1f(g_gpu.amount_uniforms[index], g_amounts[index].load());
    }

    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.buffer);
    glEnableVertexAttribArray(static_cast<GLuint>(g_gpu.position_attribute));
    glVertexAttribPointer(static_cast<GLuint>(g_gpu.position_attribute), 2, GL_FLOAT, GL_FALSE, 0,
                          nullptr);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    saved.restore();
    while (glGetError() != GL_NO_ERROR) {
    }

    if (g_frames++ == 0) {
        g_running.store(true);
        MCBE_LOGI("screen effects are drawing at %dx%d", width, height);
    }
}

unsigned detour_swap(void* display, void* surface) {
    draw(display, surface);
    return g_original_swap == nullptr ? 0 : g_original_swap(display, surface);
}

#endif  // __ANDROID__

}  // namespace

// ---------------------------------------------------------------------------
// The public side
// ---------------------------------------------------------------------------

std::string names() {
    std::string result;
    for (size_t index = 0; index < kEffectCount; ++index) {
        if (index != 0) result += ",";
        result += kNames[index].name;
    }
    return result;
}

size_t set(const std::string& spec) {
    clear_all();

    size_t recognised = 0;
    std::istringstream reader(spec);
    std::string pair;

    while (std::getline(reader, pair, ',')) {
        const size_t equals = pair.find('=');
        if (equals == std::string::npos) continue;

        std::string name = pair.substr(0, equals);
        const std::string value = pair.substr(equals + 1);

        // Whitespace around a hand-written config line is not an error.
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();

        float amount = 0.0f;
        if (sscanf(value.c_str(), "%f", &amount) != 1) continue;
        if (amount < 0.0f) amount = 0.0f;
        if (amount > 1.0f) amount = 1.0f;

        bool known = false;
        for (size_t index = 0; index < kEffectCount; ++index) {
            if (name != kNames[index].name) continue;
            g_amounts[index].store(amount);
            if (amount > 0.0f) g_any.store(true);
            known = true;
            ++recognised;
            break;
        }
        if (!known) MCBE_LOGW("unknown screen effect: %s", name.c_str());
    }

    MCBE_LOGI("screen effects set to: %s", describe().c_str());
    return recognised;
}

std::string describe() {
    std::string result;
    for (size_t index = 0; index < kEffectCount; ++index) {
        const float amount = g_amounts[index].load();
        if (amount <= 0.0f) continue;

        char pair[64] = {};
        snprintf(pair, sizeof(pair), "%s=%.3f", kNames[index].name, amount);
        if (!result.empty()) result += ",";
        result += pair;
    }
    return result;
}

bool running() { return g_running.load(); }

#if defined(__ANDROID__)

bool install(Loader& loader) {
    clear_all();
    g_started_at = now_seconds();

    void* swap = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    g_query_surface = reinterpret_cast<QuerySurface>(dlsym(RTLD_DEFAULT, "eglQuerySurface"));
    g_bind_vertex_array =
        reinterpret_cast<BindVertexArray>(dlsym(RTLD_DEFAULT, "glBindVertexArray"));

    if (swap == nullptr || g_query_surface == nullptr) {
        MCBE_LOGW("no OpenGL frame handover in this process, screen effects are off");
        return false;
    }

    void* original = nullptr;
    if (loader.install_hook(swap, reinterpret_cast<void*>(&detour_swap), &original) !=
        MCBE_OK) {
        MCBE_LOGW("could not hook the frame handover, screen effects are off");
        return false;
    }

    g_original_swap = reinterpret_cast<SwapBuffers>(original);
    MCBE_LOGI("screen effects ready (%s)", names().c_str());
    return true;
}

#else

bool install(Loader&) { return false; }

#endif

}  // namespace mcbe::fx
