// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

#include <android/native_window_jni.h>
#include <glad/glad.h>

#include "common/logging/log.h"
#include "core/settings.h"
#include "input_common/main.h"
#include "jni/button_manager.h"
#include "jni/emu_window/emu_window.h"
#include "jni/id_cache.h"
#include "network/network.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

static constexpr std::array<EGLint, 15> egl_attribs{EGL_SURFACE_TYPE,
                                                    EGL_WINDOW_BIT,
                                                    EGL_RENDERABLE_TYPE,
                                                    EGL_OPENGL_ES3_BIT_KHR,
                                                    EGL_BLUE_SIZE,
                                                    8,
                                                    EGL_GREEN_SIZE,
                                                    8,
                                                    EGL_RED_SIZE,
                                                    8,
                                                    EGL_DEPTH_SIZE,
                                                    0,
                                                    EGL_STENCIL_SIZE,
                                                    0,
                                                    EGL_NONE};
static constexpr std::array<EGLint, 5> egl_empty_attribs{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
static constexpr std::array<EGLint, 4> egl_context_attribs{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};

SharedContext_Android::SharedContext_Android(EGLDisplay egl_display, EGLConfig egl_config,
                                             EGLContext egl_share_context)
    : egl_display{egl_display}, egl_surface{eglCreatePbufferSurface(egl_display, egl_config,
                                                                    egl_empty_attribs.data())},
      egl_context{eglCreateContext(egl_display, egl_config, egl_share_context,
                                   egl_context_attribs.data())} {
    ASSERT_MSG(egl_surface, "eglCreatePbufferSurface() failed!");
    ASSERT_MSG(egl_context, "eglCreateContext() failed!");
}

SharedContext_Android::~SharedContext_Android() {
    if (!eglDestroySurface(egl_display, egl_surface)) {
        LOG_CRITICAL(Frontend, "eglDestroySurface() failed");
    }

    if (!eglDestroyContext(egl_display, egl_context)) {
        LOG_CRITICAL(Frontend, "eglDestroySurface() failed");
    }
}

void SharedContext_Android::MakeCurrent() {
    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
}

void SharedContext_Android::DoneCurrent() {
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static bool IsPortraitMode() {
    return JNI_FALSE != IDCache::GetEnvForThread()->CallStaticBooleanMethod(
                            IDCache::GetNativeLibraryClass(), IDCache::GetIsPortraitMode());
}

static void UpdateLandscapeScreenLayout() {
    Settings::values.layout_option =
        static_cast<Settings::LayoutOption>(IDCache::GetEnvForThread()->CallStaticIntMethod(
            IDCache::GetNativeLibraryClass(), IDCache::GetLandscapeScreenLayout()));
}

void EmuWindow_Android::OnSurfaceChanged(ANativeWindow* surface) {
    render_window = surface;
}

void EmuWindow_Android::OnTouchEvent(int x, int y, bool pressed) {
    if (pressed) {
        TouchPressed((unsigned)std::max(x, 0), (unsigned)std::max(y, 0));
    } else {
        TouchReleased();
    }
}

void EmuWindow_Android::OnTouchMoved(int x, int y) {
    TouchMoved((unsigned)std::max(x, 0), (unsigned)std::max(y, 0));
}

void EmuWindow_Android::OnFramebufferSizeChanged() {
    UpdateLandscapeScreenLayout();
    UpdateCurrentFramebufferLayout(window_width, window_height, IsPortraitMode());
}

EmuWindow_Android::EmuWindow_Android(ANativeWindow* surface) {
    LOG_DEBUG(Frontend, "Initializing EmuWindow_Android");

    if (!surface) {
        LOG_CRITICAL(Frontend, "surface is nullptr");
        return;
    }

    Network::Init();

    host_window = surface;

    if (egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY); egl_display == EGL_NO_DISPLAY) {
        LOG_CRITICAL(Frontend, "eglGetDisplay() failed");
        return;
    }
    if (eglInitialize(egl_display, 0, 0) != EGL_TRUE) {
        LOG_CRITICAL(Frontend, "eglInitialize() failed");
        return;
    }
    if (EGLint egl_num_configs{}; eglChooseConfig(egl_display, egl_attribs.data(), &egl_config, 1,
                                                  &egl_num_configs) != EGL_TRUE) {
        LOG_CRITICAL(Frontend, "eglChooseConfig() failed");
        return;
    }

    CreateWindowSurface();

    if (egl_context = eglCreateContext(egl_display, egl_config, 0, egl_context_attribs.data());
        egl_context == EGL_NO_CONTEXT) {
        LOG_CRITICAL(Frontend, "eglCreateContext() failed");
        return;
    }
    if (eglSurfaceAttrib(egl_display, egl_surface, EGL_SWAP_BEHAVIOR, EGL_BUFFER_DESTROYED) !=
        EGL_TRUE) {
        LOG_CRITICAL(Frontend, "eglSurfaceAttrib() failed");
        return;
    }
    if (core_context = CreateSharedContext(); !core_context) {
        LOG_CRITICAL(Frontend, "CreateSharedContext() failed");
        return;
    }
    if (eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context) != EGL_TRUE) {
        LOG_CRITICAL(Frontend, "eglMakeCurrent() failed");
        return;
    }
    if (!gladLoadGLES2Loader((GLADloadproc)eglGetProcAddress)) {
        LOG_CRITICAL(Frontend, "gladLoadGLES2Loader() failed");
        return;
    }
    if (!eglSwapInterval(egl_display, Settings::values.use_vsync_new ? 1 : 0)) {
        LOG_CRITICAL(Frontend, "eglSwapInterval() failed");
        return;
    }

    OnFramebufferSizeChanged();
}

bool EmuWindow_Android::CreateWindowSurface() {
    if (!host_window) {
        return true;
    }

    EGLint format{};
    eglGetConfigAttrib(egl_display, egl_config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(host_window, 0, 0, format);
    window_width = ANativeWindow_getWidth(host_window);
    window_height = ANativeWindow_getHeight(host_window);
    UpdateCurrentFramebufferLayout(window_width, window_height);

    if (egl_surface = eglCreateWindowSurface(egl_display, egl_config, host_window, 0);
        egl_surface == EGL_NO_SURFACE) {
        return {};
    }

    return !!egl_surface;
}

void EmuWindow_Android::DestroyWindowSurface() {
    if (!egl_surface) {
        return;
    }
    if (eglGetCurrentSurface(EGL_DRAW) == egl_surface) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (!eglDestroySurface(egl_display, egl_surface)) {
        LOG_CRITICAL(Frontend, "eglDestroySurface() failed");
    }
    egl_surface = EGL_NO_SURFACE;
}

void EmuWindow_Android::DestroyContext() {
    if (!egl_context) {
        return;
    }
    if (eglGetCurrentContext() == egl_context) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (!eglDestroyContext(egl_display, egl_context)) {
        LOG_CRITICAL(Frontend, "eglDestroySurface() failed");
    }
    if (!eglTerminate(egl_display)) {
        LOG_CRITICAL(Frontend, "eglTerminate() failed");
    }
    egl_context = EGL_NO_CONTEXT;
    egl_display = EGL_NO_DISPLAY;
}

EmuWindow_Android::~EmuWindow_Android() {
    StopPresenting();
    DestroyWindowSurface();
    DestroyContext();
}

std::unique_ptr<Frontend::GraphicsContext> EmuWindow_Android::CreateSharedContext() const {
    return std::make_unique<SharedContext_Android>(egl_display, egl_config, egl_context);
}

void EmuWindow_Android::StartPresenting() {
    ASSERT(!presentation_thread);
    is_presenting = true;
    presentation_thread =
        std::make_unique<std::thread>([emu_window{this}] { emu_window->Present(); });
}

void EmuWindow_Android::StopPresenting() {
    is_presenting = false;
    if (presentation_thread) {
        presentation_thread->join();
        presentation_thread.reset();
    }
}

bool EmuWindow_Android::IsPresenting() const {
    return is_presenting;
}

void EmuWindow_Android::Present() {
    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    eglSwapInterval(egl_display, Settings::values.use_vsync_new ? 1 : 0);
    while (IsPresenting()) {
        VideoCore::g_renderer->TryPresent(100);
        eglSwapBuffers(egl_display, egl_surface);
    }
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void EmuWindow_Android::PollEvents() {
    if (!render_window) {
        return;
    }

    host_window = render_window;
    render_window = nullptr;

    if (IsPresenting()) {
        StopPresenting();
    }

    DestroyWindowSurface();
    CreateWindowSurface();
    OnFramebufferSizeChanged();
    StartPresenting();
}

void EmuWindow_Android::MakeCurrent() {
    core_context->MakeCurrent();
}

void EmuWindow_Android::DoneCurrent() {
    core_context->DoneCurrent();
}
