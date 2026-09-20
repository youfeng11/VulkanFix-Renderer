#include <EGL/egl.h>
#include <dlfcn.h>
#include <android/log.h>

#define LOG_TAG "VulkanLayer_EGL"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static void* g_real_egl = NULL;

static void* get_real_egl(void) {
    if (!g_real_egl) {
        g_real_egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_real_egl) {
#if defined(__aarch64__) || defined(__x86_64__)
            g_real_egl = dlopen("/system/lib64/libEGL.so", RTLD_NOW | RTLD_LOCAL);
#else
            g_real_egl = dlopen("/system/lib/libEGL.so", RTLD_NOW | RTLD_LOCAL);
#endif
        }
    }
    return g_real_egl;
}

#define EGL_FORWARD(ret, name, args, params) \
    __attribute__((visibility("default"))) ret name args { \
        typedef ret (*fn_type) args; \
        static fn_type s_fn = NULL; \
        if (__builtin_expect(!s_fn, 0)) { \
            void* handle = get_real_egl(); \
            if (handle) { \
                s_fn = (fn_type) dlsym(handle, #name); \
            } \
        } \
        if (__builtin_expect(s_fn != NULL, 1)) return s_fn params; \
        return (ret)0; \
    }

EGL_FORWARD(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType display_id), (display_id))
EGL_FORWARD(EGLBoolean, eglInitialize, (EGLDisplay dpy, EGLint *major, EGLint *minor), (dpy, major, minor))
EGL_FORWARD(EGLBoolean, eglTerminate, (EGLDisplay dpy), (dpy))
EGL_FORWARD(const char*, eglQueryString, (EGLDisplay dpy, EGLint name), (dpy, name))
EGL_FORWARD(EGLBoolean, eglGetConfigs, (EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config), (dpy, configs, config_size, num_config))
EGL_FORWARD(EGLBoolean, eglChooseConfig, (EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config), (dpy, attrib_list, configs, config_size, num_config))
EGL_FORWARD(EGLBoolean, eglGetConfigAttrib, (EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value), (dpy, config, attribute, value))
EGL_FORWARD(EGLSurface, eglCreateWindowSurface, (EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list), (dpy, config, win, attrib_list))
EGL_FORWARD(EGLSurface, eglCreatePbufferSurface, (EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list), (dpy, config, attrib_list))
EGL_FORWARD(EGLBoolean, eglDestroySurface, (EGLDisplay dpy, EGLSurface surface), (dpy, surface))
EGL_FORWARD(EGLContext, eglCreateContext, (EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list), (dpy, config, share_context, attrib_list))
EGL_FORWARD(EGLBoolean, eglDestroyContext, (EGLDisplay dpy, EGLContext ctx), (dpy, ctx))
EGL_FORWARD(EGLBoolean, eglMakeCurrent, (EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx), (dpy, draw, read, ctx))
EGL_FORWARD(EGLBoolean, eglSwapBuffers, (EGLDisplay dpy, EGLSurface surface), (dpy, surface))
EGL_FORWARD(EGLBoolean, eglSwapInterval, (EGLDisplay dpy, EGLint interval), (dpy, interval))
EGL_FORWARD(EGLBoolean, eglBindAPI, (EGLenum api), (api))
EGL_FORWARD(EGLenum, eglQueryAPI, (void), ())
EGL_FORWARD(EGLBoolean, eglWaitClient, (void), ())
EGL_FORWARD(EGLBoolean, eglReleaseThread, (void), ())
EGL_FORWARD(EGLContext, eglGetCurrentContext, (void), ())
EGL_FORWARD(EGLSurface, eglGetCurrentSurface, (EGLint readdraw), (readdraw))
EGL_FORWARD(EGLDisplay, eglGetCurrentDisplay, (void), ())
EGL_FORWARD(EGLBoolean, eglQueryContext, (EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value), (dpy, ctx, attribute, value))
EGL_FORWARD(EGLBoolean, eglWaitGL, (void), ())
EGL_FORWARD(EGLBoolean, eglWaitNative, (EGLint engine), (engine))
EGL_FORWARD(EGLint, eglGetError, (void), ())
EGL_FORWARD(__eglMustCastToProperFunctionPointerType, eglGetProcAddress, (const char *procname), (procname))
