/* The executor's decision table, without a machine and without a journal.
 *
 * Every outcome here is reachable with fd == -1, which is the point: the
 * checks that make an irreversible action tolerable must be exercisable on a
 * developer's laptop, not only against real hardware. */

#include "geistshell/device_executor.h"

#include <stdio.h>
#include <string.h>

/* The model output the recommendation spans point into. */
static const char                 OUTPUT[]    = "heater nosuch";
static const struct spg_text_span SPAN_HEATER = {.offset = 0u, .length = 6u};
static const struct spg_text_span SPAN_NOSUCH = {.offset = 7u, .length = 6u};

static void build_device(struct spg_device *dev) {
    spg_device_init(dev);
    const struct spg_device_channel heater = {.name     = "heater",
                                              .reg      = 1u,
                                              .min      = 0,
                                              .max      = 100,
                                              .writable = true,
                                              .safe     = 0};
    (void)spg_device_add_channel(dev, &heater);
}

static enum spg_status run(struct spg_device *dev, const int64_t value,
                           const struct spg_text_span         target,
                           const bool                         execution_enabled,
                           const uint64_t                     timestamp_ns,
                           struct spg_device_executor_result *out) {
    const struct spg_device_executor_state  state  = {.device  = dev,
                                                      .journal = nullptr};
    const struct spg_device_executor_config config = {
        .timestamp_ns      = timestamp_ns,
        .write_journal     = false,
        .execution_enabled = execution_enabled,
    };
    struct spg_recommendation rec = {
        .action_kind      = SPG_ACTION_DEVICE_WRITE,
        .target           = target,
        .has_target       = true,
        .device_value     = value,
        .has_device_value = true,
    };
    const struct spg_policy_decision allow = {.kind =
                                                  SPG_POLICY_DECISION_ALLOW};
    return spg_device_executor_step(&state, &config, sizeof OUTPUT - 1u, OUTPUT,
                                    &rec, &allow, out);
}

static int test_no_device(void) {
    struct spg_device_executor_result out = {};
    if (run(nullptr, 50, SPAN_HEATER, true, 0u, &out) != SPG_OK) {
        return 1;
    }
    /* Not a refusal. "There is no machine" and "the machine said no" are
     * different facts and must not share an outcome. */
    if (out.outcome != SPG_DEVICE_OUTCOME_NO_DEVICE) {
        return 1;
    }
    if (strcmp(out.channel, "heater") != 0 || out.value != 50) {
        return 1; /* the attempt is still described, so it can be audited */
    }
    return 0;
}

static int test_unknown_channel(void) {
    struct spg_device                 dev = {};
    struct spg_device_executor_result out = {};
    build_device(&dev);
    if (run(&dev, 50, SPAN_NOSUCH, true, 0u, &out) != SPG_OK ||
        out.outcome != SPG_DEVICE_OUTCOME_UNKNOWN_CHANNEL) {
        return 1;
    }
    return 0;
}

static int test_dry_run(void) {
    struct spg_device                 dev = {};
    struct spg_device_executor_result out = {};
    build_device(&dev);
    if (run(&dev, 50, SPAN_HEATER, false, 0u, &out) != SPG_OK ||
        out.outcome != SPG_DEVICE_OUTCOME_NOT_EXECUTED) {
        return 1;
    }
    return 0;
}

static int test_refused_out_of_range(void) {
    struct spg_device                 dev = {};
    struct spg_device_executor_result out = {};
    build_device(&dev);
    /* 150 is outside 0..100, so it is refused by the table before the socket
     * is ever consulted — which is why this returns REFUSED and not
     * IO_FAILED even though nothing is connected. */
    if (run(&dev, 150, SPAN_HEATER, true, 0u, &out) != SPG_OK ||
        out.outcome != SPG_DEVICE_OUTCOME_REFUSED ||
        out.write_status != SPG_E_LIMIT) {
        return 1;
    }
    return 0;
}

/* The property this file exists for: once contact is lost, the next command is
 * NOT the one the model asked for. */
static int test_watchdog_precedes_the_write(void) {
    struct spg_device                 dev = {};
    struct spg_device_executor_result out = {};
    build_device(&dev);
    spg_device_arm_watchdog(&dev, 1000u, 0u);

    /* Inside the deadline: the watchdog does not interfere. With no socket the
     * write fails as I/O, which is exactly what distinguishes this from the
     * expired case below. */
    if (run(&dev, 50, SPAN_HEATER, true, 500u, &out) != SPG_OK ||
        out.outcome != SPG_DEVICE_OUTCOME_IO_FAILED || out.safe_state_driven) {
        return 1;
    }

    /* Past the deadline: the setpoint is abandoned and the safe state is
     * driven instead. */
    if (run(&dev, 50, SPAN_HEATER, true, 5000u, &out) != SPG_OK ||
        out.outcome != SPG_DEVICE_OUTCOME_WATCHDOG_EXPIRED ||
        !out.safe_state_driven) {
        return 1;
    }
    /* The safe state could not be reached either — there is no machine. That
     * has to be visible: a failed safe state is the worst case in this file
     * and must never be silent. */
    if (out.safe_state_status == SPG_OK) {
        return 1;
    }
    return 0;
}

/* A denied decision, or a recommendation of another kind, must never reach the
 * machine — the gate is upstream, and the executor must not second-guess it in
 * either direction. */
static int test_not_our_action(void) {
    struct spg_device dev = {};
    build_device(&dev);
    const struct spg_device_executor_state  state  = {.device  = &dev,
                                                      .journal = nullptr};
    const struct spg_device_executor_config config = {.execution_enabled =
                                                          true};

    struct spg_recommendation rec = {
        .action_kind      = SPG_ACTION_DEVICE_WRITE,
        .target           = SPAN_HEATER,
        .has_target       = true,
        .device_value     = 50,
        .has_device_value = true,
    };
    const struct spg_policy_decision  deny = {.kind = SPG_POLICY_DECISION_DENY};
    struct spg_device_executor_result out  = {};
    if (spg_device_executor_step(&state, &config, sizeof OUTPUT - 1u, OUTPUT,
                                 &rec, &deny, &out) != SPG_OK ||
        out.outcome != SPG_DEVICE_OUTCOME_NOT_EXECUTED) {
        return 1;
    }

    rec.action_kind                        = SPG_ACTION_MACHINE_PAUSE;
    const struct spg_policy_decision allow = {.kind =
                                                  SPG_POLICY_DECISION_ALLOW};
    if (spg_device_executor_step(&state, &config, sizeof OUTPUT - 1u, OUTPUT,
                                 &rec, &allow, &out) != SPG_OK ||
        out.outcome != SPG_DEVICE_OUTCOME_NOT_EXECUTED) {
        return 1;
    }
    return 0;
}

/* A span that points outside the model output is rejected rather than copied.
 * It is whatever the model emitted, so it gets checked against the buffer it
 * claims to point into. */
static int test_span_bounds(void) {
    struct spg_device dev = {};
    build_device(&dev);
    struct spg_device_executor_result out  = {};
    const struct spg_text_span        past = {.offset = 100u, .length = 6u};
    if (run(&dev, 50, past, true, 0u, &out) != SPG_E_SCHEMA) {
        return 1;
    }
    const struct spg_text_span empty = {.offset = 0u, .length = 0u};
    if (run(&dev, 50, empty, true, 0u, &out) != SPG_E_SCHEMA) {
        return 1;
    }
    return 0;
}

int main(void) {
    struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"no_device", test_no_device},
        {"unknown_channel", test_unknown_channel},
        {"dry_run", test_dry_run},
        {"refused_out_of_range", test_refused_out_of_range},
        {"watchdog_precedes_the_write", test_watchdog_precedes_the_write},
        {"not_our_action", test_not_our_action},
        {"span_bounds", test_span_bounds},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            (void)fprintf(stderr, "test_device_executor: FAIL — %s\n",
                          cases[i].name);
            return 1;
        }
    }
    (void)printf("test_device_executor: PASS\n");
    return 0;
}
