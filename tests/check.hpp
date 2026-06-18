#pragma once

#include <cstdio>
#include <cstdlib>

// A check that runs in every configuration. The existing tests used assert(),
// which the Release build strips under NDEBUG, so the Release test binary was a
// no-op. AXIOM_CHECK aborts with a message regardless of NDEBUG.
#define AXIOM_CHECK(cond)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "CHECK FAILED: %s\n  at %s:%d\n", #cond,     \
                         __FILE__, __LINE__);                                 \
            std::abort();                                                     \
        }                                                                     \
    } while (0)
