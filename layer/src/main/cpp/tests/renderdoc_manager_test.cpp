#include "core/driver_loader.h"
#include "core/renderdoc_manager.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace {
bool available = false, active = false, deny_start = false, deny_end = false;
bool add_file = true, missing_file = false, empty_file = false, deny_query = false;
unsigned starts = 0, ends = 0, discards = 0, failures = 0, checks = 0;
void *started = nullptr;
void *ended = nullptr;
std::vector<std::string> captures;
RENDERDOC_API_1_6_0 api{};
void check(bool condition, const char *message) {
    ++checks;
    printf("%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition)
        ++failures;
}
int RENDERDOC_CC option(RENDERDOC_CaptureOption, uint32_t) { return 1; }
void RENDERDOC_CC path(const char *) {}
uint32_t RENDERDOC_CC capturing() { return active; }
uint32_t RENDERDOC_CC count() { return uint32_t(captures.size()); }
void RENDERDOC_CC start(RENDERDOC_DevicePointer pointer, RENDERDOC_WindowHandle) {
    ++starts;
    started = pointer;
    if (!deny_start)
        active = true;
}
uint32_t RENDERDOC_CC end(RENDERDOC_DevicePointer pointer, RENDERDOC_WindowHandle) {
    ++ends;
    ended = pointer;
    if (deny_end)
        return 0;
    active = false;
    if (add_file) {
        auto name = "mock-capture-" + std::to_string(captures.size()) + ".rdc";
        if (!missing_file) {
            auto file = fopen(name.c_str(), "w");
            if (!file)
                return 0;
            if (!empty_file)
                fputs("mock only, not a replay capture", file);
            fclose(file);
        }
        captures.push_back(name);
    }
    return 1;
}
uint32_t RENDERDOC_CC discard(RENDERDOC_DevicePointer, RENDERDOC_WindowHandle) {
    ++discards;
    active = false;
    return 1;
}
uint32_t RENDERDOC_CC capture(uint32_t index, char *name, uint32_t *length, uint64_t *) {
    if (deny_query || index >= captures.size())
        return 0;
    if (name)
        strcpy(name, captures[index].c_str());
    *length = uint32_t(captures[index].size() + 1);
    return 1;
}
int RENDERDOC_CC get_api(RENDERDOC_Version, void **out) {
    if (!available)
        return 0;
    *out = &api;
    return 1;
}
} // namespace
extern "C" void *get_real_proc(VkInstance, VkDevice, const char *name) {
    return available && !strcmp(name, "RENDERDOC_GetAPI") ? (void *)get_api : nullptr;
}
int main() {
    setenv("VULKAN_FIX_CAPTURE_STATUS", "./manager-status.json", 1);
    setenv("VULKAN_FIX_CAPTURE_TRIGGER", "./manager-trigger", 1);
    setenv("VULKAN_FIX_LOG_PATH", "./manager-test.log", 1);
    unsetenv("VULKAN_FIX_RENDERDOC_LIB");
    auto &manager = RenderDocManager::get();
    manager.init();
    check(!manager.is_available(),
          "ordinary initialization does not load a capture library from disk");
    api.SetCaptureOptionU32 = option;
    api.SetCaptureFilePathTemplate = path;
    api.IsFrameCapturing = capturing;
    api.StartFrameCapture = start;
    api.EndFrameCapture = end;
    api.GetNumCaptures = count;
    api.GetCapture = capture;
    api.DiscardFrameCapture = discard;
    available = true;
    manager.init();
    check(manager.is_available(), "a layer discovered after startup can provide the control API");
    void *dispatch_a = (void *)(uintptr_t)0xabc0;
    void *dispatch_b = (void *)(uintptr_t)0xabc1;
    auto instance_a = (VkInstance)&dispatch_a, instance_b = (VkInstance)&dispatch_b;
    auto a = (VkDevice)(uintptr_t)0x100, b = (VkDevice)(uintptr_t)0x101;
    manager.on_device_created(a, instance_a);
    manager.on_device_created(b, instance_b);
    check(!manager.start_capture(), "multiple devices require an explicit capture target");
    check(!manager.start_capture((VkDevice)(uintptr_t)0x999),
          "unknown device handles are never dereferenced as instances");
    deny_start = true;
    check(!manager.start_capture(a) && !manager.is_capturing(),
          "a no-op StartFrameCapture does not report an active capture");
    deny_start = false;
    check(manager.start_capture(a) && started == dispatch_a,
          "capture uses the owning instance's dispatch pointer instead of VkDevice");
    const auto before_starts = starts;
    check(!manager.start_capture(b) && starts == before_starts,
          "overlapping captures do not reach RenderDoc");
    check(!manager.end_capture(b) && manager.is_capturing(),
          "another device cannot end the active capture");
    check(manager.end_capture(a) && ended == dispatch_a && !manager.last_capture_path().empty(),
          "success requires a new nonempty file and the matching capture pointer");
    const auto successful_path = manager.last_capture_path();
    const auto before_ends = ends;
    check(!manager.end_capture(a) && ends == before_ends,
          "ending an inactive capture does not call RenderDoc");
    manager.start_capture(a);
    deny_end = true;
    check(!manager.end_capture(a) && manager.is_capturing(),
          "failed EndFrameCapture preserves an actually active capture for retry");
    deny_end = false;
    check(manager.end_capture(a) && !manager.is_capturing(),
          "a failed end can be retried successfully");
    manager.start_capture(a);
    add_file = false;
    check(!manager.end_capture(a), "an old capture file cannot satisfy a new capture request");
    add_file = true;
    manager.start_capture(a);
    deny_query = true;
    check(!manager.end_capture(a), "missing new capture metadata is a failure");
    deny_query = false;
    manager.start_capture(a);
    missing_file = true;
    check(!manager.end_capture(a), "a reported path without a file is a failure");
    missing_file = false;
    manager.start_capture(a);
    empty_file = true;
    check(!manager.end_capture(a), "a zero-byte capture is a failure");
    empty_file = false;
    manager.trigger_capture(1);
    manager.on_post_present(a, VK_ERROR_OUT_OF_DATE_KHR);
    check(!manager.is_capturing(), "failed presentation does not start a pending capture");
    manager.on_post_present(a, VK_SUCCESS);
    check(manager.is_capturing(), "successful presentation starts the requested next frame");
    const auto auto_ends = ends;
    manager.on_post_present(b, VK_SUCCESS);
    check(manager.is_capturing() && ends == auto_ends,
          "another device's presentation does not close this capture");
    manager.on_post_present(a, VK_SUCCESS);
    check(!manager.is_capturing() && ends == auto_ends + 1,
          "the target frame includes presentation before capture ends");
    manager.start_capture(a);
    manager.on_post_present(a, VK_SUCCESS);
    check(manager.is_capturing(),
          "manual capture lifetime is not controlled by automatic frame boundaries");
    manager.on_instance_destroyed(instance_a);
    check(!manager.is_capturing() && !manager.start_capture(a),
          "instance destruction removes its capture targets");
    manager.on_device_created(a, instance_b);
    check(manager.start_capture(a) && started == dispatch_b,
          "a reused device handle uses its new instance mapping");
    deny_end = true;
    manager.on_device_destroyed(a);
    check(!manager.is_capturing() && discards == 1 && !active,
          "target destruction discards an unresolved active capture");
    deny_end = false;
    active = true;
    const auto external_ends = ends;
    check(!manager.start_capture(b) && !manager.end_capture() && ends == external_ends,
          "an externally owned capture is neither overlapped nor ended");
    active = false;
    auto c = (VkDevice)(uintptr_t)0x102;
    manager.on_device_created(c, instance_b);
    auto trigger = fopen("manager-trigger", "w");
    if (trigger)
        fclose(trigger);
    for (unsigned frame = 0; frame < 14; ++frame)
        manager.on_post_present(c, VK_SUCCESS);
    check(!manager.is_capturing() && access("manager-trigger", F_OK) == 0,
          "trigger polling respects its frame interval");
    manager.on_post_present(c, VK_SUCCESS);
    check(manager.is_capturing() && access("manager-trigger", F_OK) != 0,
          "the configured trigger file is consumed exactly once");
    manager.on_post_present(c, VK_SUCCESS);
    check(!manager.is_capturing(), "triggered capture finishes after the next target presentation");
    manager.on_device_destroyed(b);
    manager.on_device_destroyed(c);
    for (const auto &file : captures)
        unlink(file.c_str());
    printf("RenderDoc manager checks: %u, failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}
