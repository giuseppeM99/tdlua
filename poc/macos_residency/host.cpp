#include <cerrno>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {
using Entry = int (*)(void*);
using SetSink = void (*)(void (*)(int));
using BumpState = int (*)();
using TouchTls = void (*)();
using IntFunction = int (*)();

enum class ImagePresence { loaded, absent, unidentifiable };

std::string marker_path;

ImagePresence image_is_loaded(const char* path, std::string* loaded_name) {
    struct stat expected{};
    if (stat(path, &expected) != 0)
        return ImagePresence::unidentifiable;

    bool found_unstatable_image = false;
    for (uint32_t index = 0; index < _dyld_image_count(); ++index) {
        const char* image = _dyld_get_image_name(index);
        if (!image) {
            found_unstatable_image = true;
            continue;
        }

        struct stat loaded{};
        if (stat(image, &loaded) != 0) {
            found_unstatable_image = true;
            continue;
        }
        if (expected.st_dev == loaded.st_dev && expected.st_ino == loaded.st_ino) {
            if (loaded_name)
                *loaded_name = image;
            return ImagePresence::loaded;
        }
    }
    return found_unstatable_image ? ImagePresence::unidentifiable : ImagePresence::absent;
}

void record_event(int event) {
    const int fd = open(marker_path.c_str(), O_WRONLY | O_APPEND);
    if (fd < 0)
        return;
    const char value = static_cast<char>('0' + event);
    (void)write(fd, &value, 1);
    (void)write(fd, "\n", 1);
    close(fd);
}

template <typename Function> Function symbol(void* handle, const char* name) {
    dlerror();
    auto* address = dlsym(handle, name);
    const char* error = dlerror();
    if (error) {
        std::fprintf(stderr, "dlsym(%s): %s\n", name, error);
        return nullptr;
    }
    return reinterpret_cast<Function>(address);
}

int run_pinned_child(const char* module_path, const char* dependency_path, const char* event_path) {
    marker_path = event_path;
    void* first = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
    void* second = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
    if (!first || !second) {
        std::fprintf(stderr, "pinned scenario module load failed: %s\n", dlerror());
        return 10;
    }

    const auto entry = symbol<Entry>(first, "luaopen_tdlua");
    const auto set_sink = symbol<SetSink>(first, "poc_set_event_sink");
    const auto bump = symbol<BumpState>(first, "poc_bump_state");
    const auto touch_tls = symbol<TouchTls>(first, "poc_touch_tls");
    const auto tls_count = symbol<IntFunction>(first, "poc_tls_finalizer_count");
    const auto pin_count = symbol<IntFunction>(first, "poc_pin_call_count");
    const auto dependency_value = symbol<IntFunction>(first, "poc_dependency_value_from_module");
    if (!entry || !set_sink || !bump || !touch_tls || !tls_count || !pin_count || !dependency_value)
        return 11;

    set_sink(record_event);
    if (entry(nullptr) != 0 || entry(nullptr) != 0 || pin_count() != 2)
        return 12;
    std::puts("PASS pinned: dladdr and NODELETE pin executed twice with balanced handles");
    if (bump() != 41 || dependency_value() != 7)
        return 13;
    std::puts("PASS pinned: module state and dynamic dependency call verified");

    std::thread worker([touch_tls] { touch_tls(); });
    worker.join();
    if (tls_count() != 1) {
        std::fprintf(stderr, "pinned scenario TLS destructor did not run at thread exit\n");
        return 14;
    }
    std::puts("PASS pinned: worker TLS destructor ran at thread exit");

    if (dlclose(first) != 0 || dlclose(second) != 0)
        return 15;
    std::puts("PASS pinned: both original module handles released");

    dlerror();
    void* module_probe = dlopen(module_path, RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
    if (!module_probe) {
        std::fprintf(stderr, "pinned module unavailable to RTLD_NOLOAD: %s\n", dlerror());
        return 16;
    }
    const auto resident_bump = symbol<BumpState>(module_probe, "poc_bump_state");
    const auto resident_tls_count = symbol<IntFunction>(module_probe, "poc_tls_finalizer_count");
    const auto module_finalizer_count =
        symbol<IntFunction>(module_probe, "poc_module_finalizer_count");
    if (!resident_bump || !resident_tls_count || !module_finalizer_count || resident_bump() != 42 ||
        resident_tls_count() != 1 || module_finalizer_count() != 0)
        return 17;
    std::puts("PASS pinned: state persists and module finalizer remains pending");

    dlerror();
    void* dependency_probe = dlopen(dependency_path, RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
    if (!dependency_probe) {
        std::fprintf(stderr, "pinned dependency unavailable to RTLD_NOLOAD: %s\n", dlerror());
        return 18;
    }
    const auto dependency_finalizer_count =
        symbol<IntFunction>(dependency_probe, "poc_dependency_finalizer_count");
    if (!dependency_finalizer_count || dependency_finalizer_count() != 0)
        return 19;
    std::puts("PASS pinned: dependency finalizer remains pending");

    if (dlclose(dependency_probe) != 0 || dlclose(module_probe) != 0)
        return 20;
    std::string loaded_module;
    std::string loaded_dependency;
    const auto module_presence = image_is_loaded(module_path, &loaded_module);
    const auto dependency_presence = image_is_loaded(dependency_path, &loaded_dependency);
    if (module_presence != ImagePresence::loaded || dependency_presence != ImagePresence::loaded) {
        std::fprintf(
            stderr,
            "pinned zero-handle residency inconclusive or failed: module=%d dependency=%d\n",
            static_cast<int>(module_presence), static_cast<int>(dependency_presence));
        return 21;
    }
    std::printf("PASS pinned: zero-handle module image identified as %s\n", loaded_module.c_str());
    std::printf("PASS pinned: zero-handle dependency identified as %s\n",
                loaded_dependency.c_str());

    // The parent requires static finalizers to occur after this zero-handle check.
    record_event(9);
    return 0;
}

int run_unpinned_child(const char* module_path, const char* dependency_path,
                       const char* event_path) {
    marker_path = event_path;
    void* first = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
    void* second = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
    if (!first || !second) {
        std::fprintf(stderr, "unpinned scenario module load failed: %s\n", dlerror());
        return 30;
    }

    const auto set_sink = symbol<SetSink>(first, "poc_set_event_sink");
    const auto bump = symbol<BumpState>(first, "poc_bump_state");
    const auto touch_tls = symbol<TouchTls>(first, "poc_touch_tls");
    const auto tls_count = symbol<IntFunction>(first, "poc_tls_finalizer_count");
    const auto module_finalizer_count = symbol<IntFunction>(first, "poc_module_finalizer_count");
    const auto pin_count = symbol<IntFunction>(first, "poc_pin_call_count");
    const auto dependency_value = symbol<IntFunction>(first, "poc_dependency_value_from_module");
    if (!set_sink || !bump || !touch_tls || !tls_count || !module_finalizer_count || !pin_count ||
        !dependency_value)
        return 31;

    set_sink(record_event);
    if (pin_count() != 0) {
        std::fprintf(stderr, "unpinned scenario unexpectedly called luaopen_tdlua\n");
        return 32;
    }
    if (bump() != 41 || dependency_value() != 7)
        return 33;
    std::puts("PASS unpinned: module and dependency loaded without invoking the pin entry");

    std::thread worker([touch_tls] { touch_tls(); });
    worker.join();
    if (tls_count() != 1 || module_finalizer_count() != 0)
        return 34;

    dlerror();
    void* dependency_probe = dlopen(dependency_path, RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
    if (!dependency_probe) {
        std::fprintf(stderr, "unpinned dependency probe failed: %s\n", dlerror());
        return 35;
    }
    const auto dependency_finalizer_count =
        symbol<IntFunction>(dependency_probe, "poc_dependency_finalizer_count");
    if (!dependency_finalizer_count || dependency_finalizer_count() != 0)
        return 36;
    if (dlclose(dependency_probe) != 0 || dlclose(first) != 0 || dlclose(second) != 0)
        return 37;

    // Record after releasing every handle. A subsequent image-list query does
    // not open another handle and therefore cannot force a residency result.
    record_event(8);
    std::string loaded_module;
    std::string loaded_dependency;
    const auto module_presence = image_is_loaded(module_path, &loaded_module);
    const auto dependency_presence = image_is_loaded(dependency_path, &loaded_dependency);
    if (module_presence == ImagePresence::unidentifiable ||
        dependency_presence == ImagePresence::unidentifiable) {
        record_event(5);
        std::puts("INFO unpinned: image identity unavailable; negative check is inconclusive");
    } else if (module_presence == ImagePresence::loaded) {
        record_event(7);
        std::puts("INFO unpinned: module remains mapped; negative check is non-discriminating");
    } else {
        record_event(6);
        std::puts("INFO unpinned: module is absent after final dlclose");
    }
    std::printf("INFO unpinned image states: module=%d dependency=%d\n",
                static_cast<int>(module_presence), static_cast<int>(dependency_presence));
    return 0;
}

int run_child(const char* scenario, const char* module_path, const char* dependency_path,
              const char* event_path) {
    if (std::strcmp(scenario, "pinned") == 0)
        return run_pinned_child(module_path, dependency_path, event_path);
    if (std::strcmp(scenario, "unpinned") == 0)
        return run_unpinned_child(module_path, dependency_path, event_path);
    return 2;
}

bool spawn_child(const char* executable, const char* scenario, const char* module_path,
                 const char* dependency_path, const char* event_path) {
    pid_t child = 0;
    char* const child_argv[] = {const_cast<char*>(executable),
                                const_cast<char*>("--child"),
                                const_cast<char*>(scenario),
                                const_cast<char*>(module_path),
                                const_cast<char*>(dependency_path),
                                const_cast<char*>(event_path),
                                nullptr};
    const int spawn_error = posix_spawn(&child, executable, nullptr, nullptr, child_argv, environ);
    if (spawn_error != 0) {
        std::fprintf(stderr, "posix_spawn failed: %s\n", std::strerror(spawn_error));
        return false;
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "%s child failed (wait status=%d)\n", scenario, status);
        return false;
    }
    return true;
}

bool read_events(const char* path, std::vector<int>* events) {
    FILE* file = std::fopen(path, "r");
    if (!file)
        return false;
    int event = 0;
    while (std::fscanf(file, "%d", &event) == 1)
        events->push_back(event);
    const bool read_ok = std::ferror(file) == 0;
    std::fclose(file);
    return read_ok;
}

bool verify_pinned_events(const std::vector<int>& events) {
    return events.size() == 4 && events[0] == 3 && events[1] == 9 &&
           ((events[2] == 1 && events[3] == 2) || (events[2] == 2 && events[3] == 1));
}

bool verify_unpinned_events(const std::vector<int>& events) {
    int counts[10] = {};
    std::size_t close_marker = events.size();
    std::size_t image_marker = events.size();
    for (std::size_t index = 0; index < events.size(); ++index) {
        const int event = events[index];
        if (event < 1 || event > 8)
            return false;
        ++counts[event];
        if (event == 8)
            close_marker = index;
        if (event >= 5 && event <= 7)
            image_marker = index;
    }
    return counts[1] == 1 && counts[2] == 1 && counts[3] == 1 && counts[8] == 1 &&
           counts[5] + counts[6] + counts[7] == 1 && close_marker < image_marker &&
           std::find(events.begin(), events.end(), 3) <
               events.begin() + static_cast<std::ptrdiff_t>(close_marker);
}

int run_scenario(const char* executable, const char* scenario, const char* module_path,
                 const char* dependency_path) {
    char event_path[] = "/tmp/tdlua-macos-residency-XXXXXX";
    const int marker_fd = mkstemp(event_path);
    if (marker_fd < 0) {
        std::perror("mkstemp");
        return 40;
    }
    close(marker_fd);

    const bool child_ok =
        spawn_child(executable, scenario, module_path, dependency_path, event_path);
    std::vector<int> events;
    const bool events_read = read_events(event_path, &events);
    unlink(event_path);
    if (!child_ok || !events_read)
        return 41;

    const bool events_ok = std::strcmp(scenario, "pinned") == 0 ? verify_pinned_events(events)
                                                                : verify_unpinned_events(events);
    if (!events_ok) {
        std::fprintf(stderr, "%s finalizer/event sequence invalid:", scenario);
        for (int event : events)
            std::fprintf(stderr, " %d", event);
        std::fputc('\n', stderr);
        return 42;
    }

    if (std::strcmp(scenario, "pinned") == 0) {
        std::puts("PASS pinned: TLS finalizer ran at thread exit; static finalizers ran after "
                  "handle release");
    } else {
        const auto image_event = std::find_if(events.begin(), events.end(),
                                              [](int event) { return event >= 5 && event <= 7; });
        const auto close_marker = std::find(events.begin(), events.end(), 8);
        const auto image_marker = std::find_if(events.begin(), events.end(),
                                               [](int event) { return event >= 5 && event <= 7; });
        const auto module_finalizer = std::find(events.begin(), events.end(), 1);
        const auto dependency_finalizer = std::find(events.begin(), events.end(), 2);
        if (*image_event == 7)
            std::puts("PASS unpinned diagnostics: dyld retained the image; this result does not "
                      "prove pin effectiveness");
        else if (*image_event == 6)
            std::puts(
                "PASS unpinned diagnostics: dyld reported at least one image absent after dlclose");
        else
            std::puts("PASS unpinned diagnostics: image identity was inconclusive; no residency "
                      "claim made");
        const auto finalizer_phase = [&](std::vector<int>::const_iterator finalizer) {
            if (finalizer < close_marker)
                return "during dlclose";
            if (finalizer > image_marker)
                return "during process exit";
            return "after dlclose, before image observation";
        };
        std::printf("INFO unpinned module finalizer: %s\n", finalizer_phase(module_finalizer));
        std::printf("INFO unpinned dependency finalizer: %s\n",
                    finalizer_phase(dependency_finalizer));
        std::printf("INFO unpinned event order:");
        for (int event : events)
            std::printf(" %d", event);
        std::fputc('\n', stdout);
        std::puts("PASS unpinned: TLS and both static finalizers were observed exactly once");
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 6 && std::strcmp(argv[1], "--child") == 0)
        return run_child(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 4 &&
        (std::strcmp(argv[1], "--pinned") == 0 || std::strcmp(argv[1], "--unpinned") == 0))
        return run_scenario(argv[0], argv[1] + 2, argv[2], argv[3]);
    std::fprintf(stderr, "usage: poc_host (--pinned|--unpinned) <module> <dependency>\n");
    return 2;
}
