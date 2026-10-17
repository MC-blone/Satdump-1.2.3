#include "backend.h"
#include "core/exception.h"
#include <thread>
#include <chrono>

extern struct android_app *g_App;
extern EGLDisplay g_EglDisplay;
extern EGLSurface g_EglSurface;

float funcDeviceScale()
{
    JavaVM* java_vm = g_App->activity->vm;
    JNIEnv* java_env = NULL;

    jint jni_return = java_vm->GetEnv((void**)&java_env, JNI_VERSION_1_6);
    if (jni_return == JNI_ERR)
        throw satdump_exception("Could not get JNI environement");

    jni_return = java_vm->AttachCurrentThread(&java_env, NULL);
    if (jni_return != JNI_OK)
        throw satdump_exception("Could not attach to thread");

    jclass native_activity_clazz = java_env->GetObjectClass(g_App->activity->clazz);
    if (native_activity_clazz == NULL)
        throw satdump_exception("Could not get MainActivity class");

    jmethodID method_id = java_env->GetMethodID(native_activity_clazz, "get_dpi", "()F");
    if (method_id == NULL)
        throw satdump_exception("Could not get methode ID");

    jfloat jflt = java_env->CallFloatMethod(g_App->activity->clazz, method_id);

    jni_return = java_vm->DetachCurrentThread();
    if (jni_return != JNI_OK)
        throw satdump_exception("Could not detach from thread");

    return jflt;
}

void funcRebuildFonts()
{
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

void funcSetMousePos(int, int)
{
    // Not implemented on Android
}

std::pair<int, int> funcBeginFrame()
{
    // Get display dimensions
    EGLint width, height;
    eglQuerySurface(g_EglDisplay, g_EglSurface, EGL_WIDTH, &width);
    eglQuerySurface(g_EglDisplay, g_EglSurface, EGL_HEIGHT, &height);

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    return { (int)width, (int)height };
}

void funcEndFrame()
{
    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(style::theme.frame_bg.Value.x, style::theme.frame_bg.Value.y, style::theme.frame_bg.Value.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    eglSwapBuffers(g_EglDisplay, g_EglSurface);

    // ★ 核心修复：强制限制 UI 刷新率，防止空转吃满 CPU
    static auto last_frame_time = std::chrono::steady_clock::now();
    auto current_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_frame_time).count();

    // Android 里判断后台最安全的方式就是看 window 是否存活
    bool is_in_background = (g_App->window == NULL);

    if (is_in_background) {
        // 切到后台时，每秒只刷新 10 帧（100毫秒）节约电量
        if (elapsed < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100 - elapsed));
        }
    } else {
        // 正常前台显示，限制在 60 帧（每帧约 16 毫秒）
        if (elapsed < 16) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16 - elapsed));
        }
    }

    last_frame_time = std::chrono::steady_clock::now();
}

void funcSetIcon(uint8_t*, int, int)
{
    // Not implemented on Android
}

void bindBackendFunctions()
{
    backend::device_scale = funcDeviceScale();

    backend::rebuildFonts = funcRebuildFonts;
    backend::setMousePos = funcSetMousePos;
    backend::beginFrame = funcBeginFrame;
    backend::endFrame = funcEndFrame;
    backend::setIcon = funcSetIcon;
}