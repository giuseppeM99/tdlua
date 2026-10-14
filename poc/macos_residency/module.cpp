#include <atomic>
#include <cerrno>
#include <cstdio>
#include <dlfcn.h>

#if !defined(RTLD_NOLOAD) || !defined(RTLD_NODELETE)
#error "This PoC requires RTLD_NOLOAD and RTLD_NODELETE from the macOS SDK"
#endif

namespace {
using EventSink = void (*)(int);
std::atomic<EventSink> event_sink{nullptr};
std::atomic<int> tls_finalizers{0};
std::atomic<int> module_finalizers{0};
std::atomic<int> pin_calls{0};
int retained_state = 40;

extern "C" void poc_dependency_set_sink(EventSink sink);
extern "C" int poc_dependency_value();

void emit(int event) {
    if (const auto sink = event_sink.load())
        sink(event);
}

struct ModuleFinalizer {
    ~ModuleFinalizer() {
        module_finalizers.fetch_add(1);
        emit(1);
    }
};

struct ThreadFinalizer {
    ~ThreadFinalizer() {
        tls_finalizers.fetch_add(1);
        emit(3);
    }
};

ModuleFinalizer module_finalizer;
thread_local ThreadFinalizer thread_finalizer;
} // namespace

extern "C" int luaopen_tdlua(void*) {
    pin_calls.fetch_add(1);
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&luaopen_tdlua), &info) == 0 || !info.dli_fname) {
        std::fprintf(stderr, "dladdr failed (errno=%d)\n", errno);
        return -1;
    }

    dlerror();
    void* pin = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
    if (!pin) {
        std::fprintf(stderr, "RTLD_NODELETE pin failed: %s\n", dlerror());
        return -2;
    }
    if (dlclose(pin) != 0) {
        std::fprintf(stderr, "balancing pin handle failed: %s\n", dlerror());
        return -3;
    }
    return 0;
}

extern "C" void poc_set_event_sink(void (*sink)(int)) {
    event_sink.store(sink);
    poc_dependency_set_sink(sink);
}

extern "C" int poc_bump_state() {
    return ++retained_state;
}

extern "C" void poc_touch_tls() {
    (void)thread_finalizer;
}

extern "C" int poc_tls_finalizer_count() {
    return tls_finalizers.load();
}

extern "C" int poc_module_finalizer_count() {
    return module_finalizers.load();
}

extern "C" int poc_pin_call_count() {
    return pin_calls.load();
}

extern "C" int poc_dependency_value_from_module() {
    return poc_dependency_value();
}
