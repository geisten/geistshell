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
#    include <sys/wait.h>
#    include <time.h>
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

static struct spg_machine_pause_ledger probe_ledger;

static enum spg_machine_exec_outcome run(const struct spg_machine_state *m,
                                         const enum spg_action_kind      kind) {
    char                                     payload[512];
    const struct spg_machine_executor_state  st  = {.machine = m,
                                                    .ledger  = &probe_ledger};
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

/* Phase 6b (#80): the run dies between a pause and its resume. SIGKILL is not
 * catchable, so the ledger dies with the process — the journal is all that is
 * left, and the next start has to finish the job.
 *
 * Simulated by simply not releasing, rather than by killing an agent
 * mid-signal: that would be a race, and a race in a test is a flake. What is
 * under test is the recovery, not the timing of the crash. */
static int recovery_scenario(void) {
    const pid_t child = fork();
    if (child < 0) {
        return 1;
    }
    if (child == 0) {
        for (;;) {
            (void)pause();
        }
    }
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
    (void)nanosleep(&ts, nullptr);

    struct spg_machine_state m = {.n_processes = 1u};
    if (!read_identity((uint64_t)child, &m.processes[0])) {
        (void)kill(child, SIGKILL);
        return 1;
    }
    memcpy(m.processes[0].profile_id, "batch_job", sizeof "batch_job");

    const char               *path   = "build/machine-probe-recovery.sgj";
    struct spg_journal_writer writer = {};
    (void)remove(path);
    if (spg_journal_writer_open(&writer, path) != SPG_OK) {
        (void)kill(child, SIGKILL);
        return 1;
    }
    char                                    payload[512];
    struct spg_machine_pause_ledger         ledger = {};
    const struct spg_machine_executor_state st     = {
        .machine = &m, .journal = &writer, .ledger = &ledger};
    const struct spg_machine_executor_config cfg = {.actor_id          = 1u,
                                                    .timestamp_ns      = 1u,
                                                    .write_journal     = true,
                                                    .execution_enabled = true};
    const struct spg_machine_executor_workspace ws = {
        .payload_capacity = sizeof payload, .payload = payload};
    struct spg_machine_executor_result r  = {};
    int                                rc = 0;

    if (spg_machine_executor_step(&st, &cfg, SPG_ACTION_MACHINE_PAUSE,
                                  "batch_job", &ws, &r) != SPG_OK ||
        r.outcome != SPG_MACHINE_EXEC_OK) {
        printf("FAIL: recovery setup could not pause\n");
        rc = 1;
    }
    (void)nanosleep(&ts, nullptr);
    if (process_state((uint64_t)child) != 'T') {
        printf("FAIL: recovery setup left the child running\n");
        rc = 1;
    }
    /* The crash: the ledger is dropped on the floor, exactly as it would be if
     * the process had been killed. */
    (void)spg_journal_writer_close(&writer);

    size_t recovered = 0u;
    if (spg_machine_recover_journal(path, &cfg, &ws, nullptr, &recovered) !=
            SPG_OK ||
        recovered != 1u) {
        printf("FAIL: recovery resumed %zu processes, expected 1\n", recovered);
        rc = 1;
    }
    (void)nanosleep(&ts, nullptr);
    if (process_state((uint64_t)child) == 'T') {
        printf("FAIL: the child is still stopped after recovery\n");
        rc = 1;
    }

    /* Recovery must be safe to run twice. It does not have to be a no-op —
     * SIGCONT on a running process is harmless — but it must not fail and it
     * must not leave the child worse off. That is what makes it safe to run
     * unconditionally at every start. */
    size_t again = 0u;
    if (spg_machine_recover_journal(path, &cfg, &ws, nullptr, &again) !=
        SPG_OK) {
        printf("FAIL: repeating recovery errored\n");
        rc = 1;
    }
    (void)nanosleep(&ts, nullptr);
    if (process_state((uint64_t)child) == 'T') {
        printf("FAIL: repeating recovery stopped the child\n");
        rc = 1;
    }

    (void)kill(child, SIGKILL);
    int status = 0;
    (void)waitpid(child, &status, 0);
    if (rc == 0) {
        printf("machine_action_probe: recovery restored a stranded pause\n");
    }
    return rc;
}

/* The window the ledger cannot cover: the agent is SIGKILLed while a process
 * is paused. Nothing in the dying process gets to run, so the only thing that
 * can help is something that was already running — the guardian.
 *
 * Modelled by forking a stand-in agent that pauses a target and then dies
 * without releasing. If the guardian works, the target is running again a
 * moment later, with no next agent start involved. */
static int guardian_scenario(void) {
    const pid_t target = fork();
    if (target < 0) {
        return 1;
    }
    if (target == 0) {
        for (;;) {
            (void)pause();
        }
    }
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 150000000};
    (void)nanosleep(&ts, nullptr);

    const pid_t agent = fork();
    if (agent < 0) {
        (void)kill(target, SIGKILL);
        return 1;
    }
    if (agent == 0) {
        /* The stand-in agent: pause, then hang. It will be killed. */
        struct spg_machine_state m = {.n_processes = 1u};
        if (!read_identity((uint64_t)target, &m.processes[0])) {
            _exit(1);
        }
        memcpy(m.processes[0].profile_id, "batch_job", sizeof "batch_job");
        char                                     payload[512];
        struct spg_machine_pause_ledger          ledger = {};
        const struct spg_machine_executor_state  st     = {.machine = &m,
                                                           .ledger  = &ledger};
        const struct spg_machine_executor_config cfg    = {
            .actor_id = 1u, .execution_enabled = true};
        const struct spg_machine_executor_workspace ws = {
            .payload_capacity = sizeof payload, .payload = payload};
        struct spg_machine_executor_result r = {};
        if (spg_machine_executor_step(&st, &cfg, SPG_ACTION_MACHINE_PAUSE,
                                      "batch_job", &ws, &r) != SPG_OK ||
            r.outcome != SPG_MACHINE_EXEC_OK) {
            _exit(1);
        }
        for (;;) {
            (void)pause();
        }
    }

    (void)nanosleep(&ts, nullptr);
    int rc = 0;
    if (process_state((uint64_t)target) != 'T') {
        printf("FAIL: guardian setup did not pause the target\n");
        rc = 1;
    }
    /* The kill the ledger cannot survive. */
    (void)kill(agent, SIGKILL);
    int status = 0;
    (void)waitpid(agent, &status, 0);
    (void)nanosleep(&ts, nullptr);
    (void)nanosleep(&ts, nullptr);

    if (process_state((uint64_t)target) == 'T') {
        printf("FAIL: target still stopped after the agent was killed\n");
        rc = 1;
    }
    (void)kill(target, SIGKILL);
    (void)waitpid(target, &status, 0);
    if (rc == 0) {
        printf("machine_action_probe: guardian released a killed agent's "
               "pause\n");
    }
    return rc;
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
    if (rc != 0) {
        return rc;
    }
    printf("machine_action_probe: pause/resume/identity all correct\n");
    rc = recovery_scenario();
    if (rc != 0) {
        return rc;
    }
    return guardian_scenario();
}

#else
int main(void) {
    printf("machine_action_probe: no /proc, nothing to prove here\n");
    return 0;
}
#endif
