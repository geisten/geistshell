/* Drives the machine executor against a real child process.
 *
 * Not a test_*.c file on purpose: it forks, and the Makefile's wildcard would
 * run it on every platform. The shell wrapper decides when it is meaningful.
 *
 * What it proves that a fixture cannot: SIGSTOP reaches the intended process
 * (its state in /proc becomes T), SIGCONT brings it back, and a snapshot whose
 * start identity no longer matches produces no signal at all. */

#define _POSIX_C_SOURCE 200809L

#include "geistshell/machine_executor.h"
#include "geistshell/machine_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#    include <signal.h>
#    include <time.h>
#    include <sys/wait.h>
#    include <unistd.h>

static bool read_identity(const uint64_t pid, struct spg_process_sample *out) {
    char path[64];
    (void)snprintf(path, sizeof path, "/proc/%llu/stat",
                   (unsigned long long)pid);
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    char         buf[2048];
    const size_t n = fread(buf, 1u, sizeof buf, f);
    (void)fclose(f);
    return n > 0u && spg_process_parse_stat(n, buf, 4096u, out) == SPG_OK;
}

static char process_state(const uint64_t pid) {
    struct spg_process_sample s = {};
    return read_identity(pid, &s) ? s.state : '?';
}

static enum spg_machine_exec_outcome run(const struct spg_machine_state *m,
                                         const enum spg_action_kind      kind) {
    char                                     payload[512];
    const struct spg_machine_executor_state  st  = {.machine = m};
    const struct spg_machine_executor_config cfg = {.actor_id          = 1u,
                                                    .execution_enabled = true};
    const struct spg_machine_executor_workspace ws = {
        .payload_capacity = sizeof payload, .payload = payload};
    struct spg_machine_executor_result r = {};
    if (spg_machine_executor_step(&st, &cfg, kind, "batch_job", &ws, &r) !=
        SPG_OK) {
        return SPG_MACHINE_EXEC_REFUSED;
    }
    return r.outcome;
}

int main(void) {
    const pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        for (;;) {
            (void)pause();
        }
    }
    /* Give the child a moment to exist in /proc. */
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
    (void)nanosleep(&ts, nullptr);

    struct spg_machine_state m = {.n_processes = 1u};
    if (!read_identity((uint64_t)child, &m.processes[0])) {
        printf("FAIL: cannot read child identity\n");
        (void)kill(child, SIGKILL);
        return 1;
    }
    memcpy(m.processes[0].profile_id, "batch_job", sizeof "batch_job");

    int rc = 0;
    if (run(&m, SPG_ACTION_MACHINE_PAUSE) != SPG_MACHINE_EXEC_OK) {
        printf("FAIL: pause was not executed\n");
        rc = 1;
    }
    (void)nanosleep(&ts, nullptr);
    if (process_state((uint64_t)child) != 'T') {
        printf("FAIL: child is '%c', expected 'T' (stopped)\n",
               process_state((uint64_t)child));
        rc = 1;
    }

    if (run(&m, SPG_ACTION_MACHINE_RESUME) != SPG_MACHINE_EXEC_OK) {
        printf("FAIL: resume was not executed\n");
        rc = 1;
    }
    (void)nanosleep(&ts, nullptr);
    if (process_state((uint64_t)child) == 'T') {
        printf("FAIL: child still stopped after resume\n");
        rc = 1;
    }

    /* THE case. Same pid, a start identity that no longer matches: the
     * executor must recognise it is not the process that was observed and send
     * nothing at all. If this ever regresses, the agent signals strangers. */
    struct spg_machine_state stale = m;
    stale.processes[0].start_identity += 1u;
    const enum spg_machine_exec_outcome o =
        run(&stale, SPG_ACTION_MACHINE_PAUSE);
    if (o != SPG_MACHINE_EXEC_IDENTITY_CHANGED) {
        printf("FAIL: stale identity gave '%s', expected identity_changed\n",
               spg_machine_exec_outcome_to_string(o));
        rc = 1;
    }
    if (process_state((uint64_t)child) == 'T') {
        printf("FAIL: the child was stopped despite the identity mismatch\n");
        rc = 1;
    }

    (void)kill(child, SIGKILL);
    int status = 0;
    (void)waitpid(child, &status, 0);
    if (rc == 0) {
        printf("machine_action_probe: pause/resume/identity all correct\n");
    }
    return rc;
}

#else
int main(void) {
    printf("machine_action_probe: no /proc, nothing to prove here\n");
    return 0;
}
#endif
