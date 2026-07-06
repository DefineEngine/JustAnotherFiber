// Measures raw fiber context-switch latency: a fiber that yields forever, driven
// by a tight resume() loop. Each resume() is one full round trip
// (resumer -> fiber -> resumer), i.e. two context switches.

#include <jaf/Jaf.hpp>

#include "Bench.hpp"

#include <cstdint>
#include <cstdio>

int main() {
    constexpr std::uint64_t kIters = 5'000'000;

    // A fiber that simply yields back on every resume, indefinitely.
    jaf::Fiber pingpong([] {
        for (;;) {
            jaf::this_fiber::yield();
        }
    });

    std::printf("JustAnotherFiber -- context switch benchmark\n");
    std::printf("--------------------------------------------\n");

    double const ns_round_trip =
        jaf::bench::run("resume+yield (2 switches)", kIters, [&] {
            pingpong.resume();
        });

    std::printf("%-36s %25.2f ns/switch\n", "=> per single context switch", ns_round_trip / 2.0);

    // pingpong is left Suspended and is intentionally never resumed again; its
    // stack is reclaimed by the destructor at scope exit.
    return 0;
}
