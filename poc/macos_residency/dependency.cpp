#include <atomic>

namespace {
using EventSink = void (*)(int);
std::atomic<EventSink> event_sink{nullptr};
std::atomic<int> finalizer_count{0};

struct DependencyFinalizer {
    ~DependencyFinalizer() {
        finalizer_count.fetch_add(1);
        if (const auto sink = event_sink.load())
            sink(2);
    }
};

DependencyFinalizer dependency_finalizer;
} // namespace

extern "C" void poc_dependency_set_sink(void (*sink)(int)) {
    event_sink.store(sink);
}

extern "C" int poc_dependency_value() {
    return 7;
}

extern "C" int poc_dependency_finalizer_count() {
    return finalizer_count.load();
}
