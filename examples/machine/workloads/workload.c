/* Reproducible load for machine experiments (roadmap phase 8, #68).
 *
 * One program, five modes, rather than five programs: the modes differ in what
 * they consume, not in how they are bounded, started or stopped — and that
 * shared part is the part that has to be right.
 *
 * Every mode is bounded three ways: a wall-clock deadline, a hard ceiling on
 * what it may allocate, and a signal handler that ends it early. It forks
 * nothing, opens no sockets, needs no privileges, and frees what it took.
 *
 *   workload --mode cpu|memory|mixed|batch|critical [--seconds N] [--mb N]
 *
 * `batch` and `critical` are `cpu` at different nice levels — the profile in
 * examples/eval/machine decides what they MEAN, which is the whole point of
 * phase 2. A workload does not get to declare its own importance. */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

/* Ceilings, not defaults. A CI machine and a Pi must both survive a mistake in
 * an argument, so the arguments cannot ask for more than this. */
enum {
    MAX_SECONDS = 600,
    MAX_MB      = 512,
    CHUNK_MB    = 8,
};

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static uint64_t now_ms(void) {
    struct timespec ts = {};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Enough arithmetic to keep a core busy without the compiler removing it. */
static uint64_t burn(uint64_t seed) {
    for (int i = 0; i < 200000; i += 1) {
        seed = seed * 6364136223846793005u + 1442695040888963407u;
        seed ^= seed >> 33;
    }
    return seed;
}

/* Allocate up to cap_mb in chunks, touching every page so the pages are really
 * resident — an untouched mapping does not show up as RSS and would make the
 * memory scenario a lie. Stops early on a failed allocation instead of
 * retrying: the point is to occupy memory, not to trigger the OOM killer. */
static size_t grow(char *blocks[], const size_t max_blocks, size_t held,
                   const size_t cap_mb) {
    const size_t chunk = (size_t)CHUNK_MB * 1024u * 1024u;
    while (held < max_blocks && held * (size_t)CHUNK_MB < cap_mb) {
        char *block = malloc(chunk);
        if (block == nullptr) {
            break; /* the machine said no; that is an answer, not an error */
        }
        memset(block, (int)(held & 0xffu), chunk);
        blocks[held] = block;
        held += 1u;
    }
    return held;
}

static int usage(void) {
    (void)fprintf(stderr,
                  "usage: workload --mode cpu|memory|mixed|batch|critical "
                  "[--seconds N] [--mb N]\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *mode    = nullptr;
    long        seconds = 5;
    long        mb      = 64;

    for (int i = 1; i < argc; i += 1) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[i + 1];
            i += 1;
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = strtol(argv[i + 1], nullptr, 10);
            i += 1;
        } else if (strcmp(argv[i], "--mb") == 0 && i + 1 < argc) {
            mb = strtol(argv[i + 1], nullptr, 10);
            i += 1;
        } else {
            return usage();
        }
    }
    if (mode == nullptr) {
        return usage();
    }
    /* Clamped rather than rejected: an experiment script that asks for an hour
     * by accident should still finish, and it should say so in its own
     * behaviour rather than fail at startup. */
    if (seconds < 1) {
        seconds = 1;
    }
    if (seconds > MAX_SECONDS) {
        seconds = MAX_SECONDS;
    }
    if (mb < 0) {
        mb = 0;
    }
    if (mb > MAX_MB) {
        mb = MAX_MB;
    }

    struct sigaction sa = {.sa_handler = on_signal};
    (void)sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGTERM, &sa, nullptr);
    (void)sigaction(SIGINT, &sa, nullptr);

    const bool wants_cpu = strcmp(mode, "cpu") == 0 ||
                           strcmp(mode, "mixed") == 0 ||
                           strcmp(mode, "batch") == 0 ||
                           strcmp(mode, "critical") == 0;
    const bool wants_memory =
        strcmp(mode, "memory") == 0 || strcmp(mode, "mixed") == 0;
    if (!wants_cpu && !wants_memory) {
        return usage();
    }
    /* A batch job asks to be preempted; a critical one does not. The nice
     * value is the only difference, and it is what makes "pause the batch job"
     * a sensible remedy rather than an arbitrary one. */
    if (strcmp(mode, "batch") == 0) {
        /* setpriority rather than nice(): nice() is XSI, so under a plain
         * _POSIX_C_SOURCE it is undeclared on glibc while macOS exposes it
         * anyway. The Pi found that; the laptop could not. */
        (void)setpriority(PRIO_PROCESS, 0, 10);
    }

    enum { MAX_BLOCKS = (size_t)MAX_MB / CHUNK_MB };
    char  *blocks[MAX_BLOCKS] = {};
    size_t held               = 0u;
    if (wants_memory) {
        held = grow(blocks, MAX_BLOCKS, 0u, (size_t)mb);
    }

    (void)printf("workload %s pid=%ld seconds=%ld mb=%ld\n", mode,
                 (long)getpid(), seconds, wants_memory ? mb : 0);
    (void)fflush(stdout);

    const uint64_t deadline = now_ms() + (uint64_t)seconds * 1000u;
    uint64_t       sink     = 1u;
    while (!stop_requested && now_ms() < deadline) {
        if (wants_cpu) {
            sink = burn(sink);
        } else {
            /* A memory-only workload must not also be a CPU workload, or the
             * scenario measures two things at once. */
            struct timespec nap = {.tv_sec = 0, .tv_nsec = 50000000};
            (void)nanosleep(&nap, nullptr);
        }
    }

    for (size_t i = 0u; i < held; i += 1u) {
        free(blocks[i]);
    }
    /* Printed so the sink cannot be optimised away, and so a stopped-and-never-
     * resumed workload is visible by the absence of this line. */
    (void)printf("workload %s done sink=%llu\n", mode,
                 (unsigned long long)(sink & 0xffffu));
    return 0;
}
