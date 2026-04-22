#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <basalt/proxy/config/ProxyConfig.hpp>
#include <basalt/proxy/runtime/Admission.hpp>

#include <doctest.h>

#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

using namespace basalt::proxy;
using runtime::Admission;
using runtime::AdmissionResult;
using namespace std::chrono_literals;

TEST_CASE("origin and global buckets retain fractional monotonic credit") {
    config::AdmissionConfig settings;
    settings.globalRate = 2;
    settings.globalBurst = 3;
    settings.originRate = 2;
    settings.originBurst = 1;
    const Admission::Clock::time_point start{};
    Admission gate{settings, start};
    CHECK(gate.attempt("192.0.2.1", start) == AdmissionResult::Accepted);
    CHECK(gate.attempt("192.0.2.1", start + 250ms) == AdmissionResult::OriginRate);
    CHECK(gate.attempt("192.0.2.1", start + 500ms) == AdmissionResult::Accepted);
    CHECK(gate.attempt("192.0.2.2", start + 500ms) == AdmissionResult::Accepted);
    CHECK(gate.attempt("192.0.2.3", start + 500ms) == AdmissionResult::GlobalRate);
    CHECK(gate.attempt("192.0.2.3", start) == AdmissionResult::GlobalRate);
    CHECK(gate.attempt("192.0.2.3", start + 1s) == AdmissionResult::Accepted);
}

TEST_CASE("origin capacity fails closed and expiry cannot reset an exhausted bucket") {
    config::AdmissionConfig settings;
    settings.maxOrigins = 1;
    settings.originRate = 1;
    settings.originBurst = 3;
    settings.originExpiryMs = 1000;
    const Admission::Clock::time_point start{};
    Admission gate{settings, start};
    for (int i = 0; i < 3; ++i)
        REQUIRE(gate.attempt("192.0.2.1", start) == AdmissionResult::Accepted);
    CHECK(gate.attempt("192.0.2.2", start + 1s) == AdmissionResult::OriginCapacity);
    CHECK(gate.trackedOrigins() == 1);
    CHECK(gate.attempt("192.0.2.2", start + 3s) == AdmissionResult::Accepted);
    CHECK(gate.trackedOrigins() == 1);
}

TEST_CASE("parallel owners share one global burst and bounded origin storage") {
    config::AdmissionConfig settings;
    settings.globalBurst = 4;
    settings.originBurst = 4;
    settings.maxOrigins = 2;
    const Admission::Clock::time_point start{};
    Admission gate{settings, start};
    std::barrier barrier{8};
    std::atomic<int> accepted{0};
    std::vector<std::jthread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&] {
            barrier.arrive_and_wait();
            if (gate.attempt("192.0.2.1", start) == AdmissionResult::Accepted)
                accepted.fetch_add(1);
        });
    }
    workers.clear();
    CHECK(accepted.load() == 4);
    CHECK(gate.trackedOrigins() == 1);
}
