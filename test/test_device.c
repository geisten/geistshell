/* Every refusal here runs without a single fork. That is the design
 * constraint, not a convenience: the refusals below are the safety layer, and
 * a safety layer that needs a program to exist before it is exercised is one
 * that gets tested on the day it is needed.
 *
 * The exec contract itself is tested with real scripts in a temp dir — a
 * sensor is a program that prints a number, and the cheapest honest test of
 * that sentence is a program that prints a number. */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIT(s) (sizeof(s) - 1u), (s)

/* Write an executable script and return 0 on success. */
static int write_script(const char *dir, const char *name, const char *body,
                        char out_path[], const size_t cap) {
    (void)snprintf(out_path, cap, "%s/%s", dir, name);
    FILE *f = fopen(out_path, "wb");
    if (f == nullptr) {
        return 1;
    }
    (void)fputs("#!/bin/sh\n", f);
    (void)fputs(body, f);
    (void)fclose(f);
    return chmod(out_path, 0755) == 0 ? 0 : 1;
}

static int test_parse_channel(void) {
    struct spg_device_channel channel = {};

    if (spg_device_parse_channel(
            LIT("(channel (name \"heater\") (program \"/opt/p/heater\") "
                "(range 0 100) (safe 0))"),
            &channel) != SPG_OK) {
        return 1;
    }
    if (strcmp(channel.name, "heater") != 0 ||
        strcmp(channel.program, "/opt/p/heater") != 0 || channel.min != 0 ||
        channel.max != 100 || !channel.writable || channel.safe != 0) {
        return 1;
    }

    /* No (safe ...) means read-only — one statement, not two. */
    if (spg_device_parse_channel(
            LIT("(channel (name \"temp\") (program \"/opt/p/temp\") "
                "(range -400 9000))"),
            &channel) != SPG_OK ||
        channel.min != -400 || channel.writable || channel.network) {
        return 1; /* absence of (network ...) means local */
    }

    /* #119: (network true) is operator trust, parsed only as the two
     * symbols — a typo must not silently change a trust decision. */
    if (spg_device_parse_channel(
            LIT("(channel (name \"mq\") (program \"/opt/p/mq\") "
                "(range 0 100) (safe 0) (network true))"),
            &channel) != SPG_OK ||
        !channel.network || !channel.writable) {
        return 1;
    }
    if (spg_device_parse_channel(
            LIT("(channel (name \"mq\") (program \"/opt/p/mq\") "
                "(range 0 100) (network false))"),
            &channel) != SPG_OK ||
        channel.network) {
        return 1;
    }
    if (spg_device_parse_channel(
            LIT("(channel (name \"mq\") (program \"/opt/p/mq\") "
                "(range 0 100) (network yes))"),
            &channel) != SPG_E_SCHEMA) {
        return 1;
    }

    /* Rejected: a missing name, program or range, a range with one bound, an
     * unknown field. The last one matters most — (writeable ...) silently
     * ignored would accept an operator's typo as a decision. */
    const char *bad[] = {
        "(channel (program \"/p\") (range 0 1))",
        "(channel (name \"x\") (range 0 1))",
        "(channel (name \"x\") (program \"/p\"))",
        "(channel (name \"x\") (program \"/p\") (range 0))",
        "(channel (name \"x\") (program \"/p\") (range 0 1) (writeable 1))",
        "(kanal (name \"x\") (program \"/p\") (range 0 1))",
        "not a form at all",
    };
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i += 1u) {
        if (spg_device_parse_channel(strlen(bad[i]), bad[i], &channel) ==
            SPG_OK) {
            (void)fprintf(stderr, "  accepted: %s\n", bad[i]);
            return 1;
        }
    }
    return 0;
}

static int test_load(void) {
    struct spg_device dev = {};
    spg_device_init(&dev);
    const char *table =
        "(device\n"
        "  (channel (name \"temp\")   (program \"/p/temp\")"
        " (range -400 9000))\n"
        "  (channel (name \"heater\") (program \"/p/heater\")"
        " (range 0 100) (safe 0)))\n";
    size_t bad = 99u;
    if (spg_device_load(strlen(table), table, &dev, &bad) != SPG_OK ||
        dev.n_channels != 2u || bad != 99u) {
        return 1;
    }
    if (spg_device_find(&dev, "heater") == nullptr ||
        !spg_device_find(&dev, "heater")->writable) {
        return 1;
    }

    /* The second channel is broken (range inside out): the load reports WHICH
     * form failed, because "invalid config" on a 30-channel table is a
     * treasure hunt. */
    struct spg_device dev2 = {};
    spg_device_init(&dev2);
    const char *broken =
        "(device\n"
        "  (channel (name \"a\") (program \"/p/a\") (range 0 1))\n"
        "  (channel (name \"b\") (program \"/p/b\") (range 9 1)))\n";
    if (spg_device_load(strlen(broken), broken, &dev2, &bad) == SPG_OK ||
        bad != 1u) {
        return 1;
    }
    return 0;
}

static int test_table(void) {
    struct spg_device dev = {};
    spg_device_init(&dev);
    if (dev.n_channels != 0u) {
        return 1;
    }

    struct spg_device_channel heater = {.name     = "heater",
                                        .program  = "/p/heater",
                                        .min      = 0,
                                        .max      = 100,
                                        .writable = true,
                                        .safe     = 0};
    struct spg_device_channel temp   = {
        .name = "temp", .program = "/p/temp", .min = -400, .max = 9000};

    if (spg_device_add_channel(&dev, &heater) != SPG_OK ||
        spg_device_add_channel(&dev, &temp) != SPG_OK) {
        return 1;
    }
    /* A duplicate name would make a write land on whichever program the table
     * happened to list first. */
    if (spg_device_add_channel(&dev, &heater) == SPG_OK) {
        return 1;
    }
    /* A channel without a program is a channel that cannot answer. */
    struct spg_device_channel silent = {.name = "mute", .max = 1};
    if (spg_device_add_channel(&dev, &silent) == SPG_OK) {
        return 1;
    }
    if (spg_device_find(&dev, "heater") == nullptr ||
        spg_device_find(&dev, "nope") != nullptr) {
        return 1;
    }

    struct spg_device full = {};
    spg_device_init(&full);
    for (size_t i = 0u; i < SPG_DEVICE_MAX_CHANNELS; i += 1u) {
        struct spg_device_channel ch = {.program = "/p/x", .max = 1};
        ch.name[0]                   = 'a';
        ch.name[1]                   = (char)('0' + (int)(i % 10u));
        ch.name[2]                   = (char)('0' + (int)(i / 10u));
        if (spg_device_add_channel(&full, &ch) != SPG_OK) {
            return 1;
        }
    }
    struct spg_device_channel overflow = {.name = "zz", .program = "/p/x",
                                          .max  = 1};
    if (spg_device_add_channel(&full, &overflow) != SPG_E_LIMIT) {
        return 1;
    }
    return 0;
}

/* The refusals, all decided before any fork. If any of these ever returns
 * SPG_E_IO instead of its own code, the check has drifted behind the exec
 * and stopped being testable without a program. */
static int test_write_refusals(void) {
    struct spg_device dev = {};
    spg_device_init(&dev);

    struct spg_device_channel heater = {.name     = "heater",
                                        .program  = "/nonexistent/heater",
                                        .min      = 0,
                                        .max      = 100,
                                        .writable = true,
                                        .safe     = 0};
    struct spg_device_channel temp   = {.name    = "temp",
                                        .program = "/nonexistent/temp",
                                        .min     = -400,
                                        .max     = 9000};
    if (spg_device_add_channel(&dev, &heater) != SPG_OK ||
        spg_device_add_channel(&dev, &temp) != SPG_OK) {
        return 1;
    }

    if (spg_device_write(&dev, "nope", 1) != SPG_E_NOT_FOUND) {
        return 1;
    }
    if (spg_device_write(&dev, "temp", 100) != SPG_E_POLICY_DENIED) {
        return 1;
    }
    /* Out of range is refused, never clamped to 100. */
    if (spg_device_write(&dev, "heater", 101) != SPG_E_LIMIT ||
        spg_device_write(&dev, "heater", -1) != SPG_E_LIMIT) {
        return 1;
    }
    /* In range, so it gets past every refusal and only then finds that the
     * program does not exist. */
    if (spg_device_write(&dev, "heater", 100) != SPG_E_IO) {
        return 1;
    }
    return 0;
}

/* The exec contract with real programs: a number on stdout is a reading, a
 * value as argv[1] is a write, and everything else is a failure that never
 * becomes a 0. */
static int test_exec_contract(void) {
    char dir[] = "/tmp/spg_device_XXXXXX";
    if (mkdtemp(dir) == nullptr) {
        return 1;
    }
    char temp_prog[256], fail_prog[256], text_prog[256];
    char heater_prog[256], liar_prog[256], heater_file[256];
    (void)snprintf(heater_file, sizeof heater_file, "%s/heater.value", dir);
    if (write_script(dir, "temp", "echo 2350\n", temp_prog,
                     sizeof temp_prog) != 0 ||
        write_script(dir, "fail", "exit 3\n", fail_prog,
                     sizeof fail_prog) != 0 ||
        write_script(dir, "text", "echo warm\n", text_prog,
                     sizeof text_prog) != 0 ||
        write_script(dir, "liar", "echo 39\n", liar_prog,
                     sizeof liar_prog) != 0) {
        return 1;
    }
    char heater_body[512];
    (void)snprintf(heater_body, sizeof heater_body,
                   "printf %%s \"$1\" > %s\necho \"$1\"\n", heater_file);
    if (write_script(dir, "heater", heater_body, heater_prog,
                     sizeof heater_prog) != 0) {
        return 1;
    }

    struct spg_device dev = {};
    spg_device_init(&dev);
    struct spg_device_channel ch = {.name = "temp", .min = -400, .max = 9000};
    (void)snprintf(ch.program, sizeof ch.program, "%s", temp_prog);
    if (spg_device_add_channel(&dev, &ch) != SPG_OK) {
        return 1;
    }
    ch = (struct spg_device_channel){.name = "broken", .min = 0, .max = 1};
    (void)snprintf(ch.program, sizeof ch.program, "%s", fail_prog);
    if (spg_device_add_channel(&dev, &ch) != SPG_OK) {
        return 1;
    }
    ch = (struct spg_device_channel){.name = "chatty", .min = 0, .max = 1};
    (void)snprintf(ch.program, sizeof ch.program, "%s", text_prog);
    if (spg_device_add_channel(&dev, &ch) != SPG_OK) {
        return 1;
    }
    ch = (struct spg_device_channel){
        .name = "heater", .min = 0, .max = 100, .writable = true, .safe = 0};
    (void)snprintf(ch.program, sizeof ch.program, "%s", heater_prog);
    if (spg_device_add_channel(&dev, &ch) != SPG_OK) {
        return 1;
    }
    ch = (struct spg_device_channel){
        .name = "lying", .min = 0, .max = 100, .writable = true, .safe = 0};
    (void)snprintf(ch.program, sizeof ch.program, "%s", liar_prog);
    if (spg_device_add_channel(&dev, &ch) != SPG_OK) {
        return 1;
    }

    /* A reading is the printed number, and it counts as contact. */
    int64_t value = 0;
    if (spg_device_read(&dev, "temp", &value) != SPG_OK || value != 2350 ||
        !dev.contact_pending) {
        return 1;
    }
    /* Exit != 0 and non-numeric output are failures; value stays untouched. */
    value = 777;
    if (spg_device_read(&dev, "broken", &value) != SPG_E_IO || value != 777) {
        return 1;
    }
    if (spg_device_read(&dev, "chatty", &value) != SPG_E_FORMAT ||
        value != 777) {
        return 1;
    }

    /* A write hands the value as argv[1] — the machine got exactly 40. */
    if (spg_device_write(&dev, "heater", 40) != SPG_OK) {
        return 1;
    }
    char  file_content[16] = {};
    FILE *f                = fopen(heater_file, "rb");
    if (f == nullptr ||
        fread(file_content, 1u, sizeof file_content - 1u, f) == 0u) {
        if (f != nullptr) {
            (void)fclose(f);
        }
        return 1;
    }
    (void)fclose(f);
    if (strcmp(file_content, "40") != 0) {
        (void)fprintf(stderr, "  heater got: %s\n", file_content);
        return 1;
    }
    /* A device that acknowledges a different value than it was told is a
     * failed write, not a warning. */
    if (spg_device_write(&dev, "lying", 40) != SPG_E_IO) {
        return 1;
    }

    /* A mixed sample: the dead sensors render unknown, the live ones their
     * value, and the first failure is reported without blinding the rest. */
    struct spg_device_state state = {};
    if (spg_device_sample(&dev, &state) == SPG_OK || state.n != 5u) {
        return 1;
    }
    if (!state.readings[0].known || state.readings[0].value != 2350 ||
        state.readings[1].known || state.readings[2].known) {
        return 1;
    }
    return 0;
}

static int test_watchdog(void) {
    struct spg_device dev = {};
    spg_device_init(&dev);

    /* Unarmed: never fires, whatever the clock says. */
    if (spg_device_watchdog_check(&dev, 1000000000u) != SPG_WATCHDOG_DISABLED) {
        return 1;
    }

    spg_device_arm_watchdog(&dev, 1000u, 100u);
    if (spg_device_watchdog_check(&dev, 1100u) != SPG_WATCHDOG_OK) {
        return 1; /* exactly at the deadline is not past it */
    }
    if (spg_device_watchdog_check(&dev, 1101u) != SPG_WATCHDOG_EXPIRED) {
        return 1;
    }

    /* Sticky: still expired on the next check, because nothing has answered. */
    if (spg_device_watchdog_check(&dev, 1102u) != SPG_WATCHDOG_EXPIRED) {
        return 1;
    }

    /* A successful transaction clears it — simulated here by the flag the
     * transport sets, which is what a real reply would have done. */
    dev.contact_pending = true;
    if (spg_device_watchdog_check(&dev, 2000u) != SPG_WATCHDOG_OK) {
        return 1;
    }
    if (spg_device_watchdog_check(&dev, 2500u) != SPG_WATCHDOG_OK) {
        return 1; /* the contact reset the deadline, it did not merely skip one
                     check */
    }
    if (spg_device_watchdog_check(&dev, 3001u) != SPG_WATCHDOG_EXPIRED) {
        return 1;
    }

    /* Time going backwards is a caller bug, not a reason to trip a machine. */
    dev.contact_pending = true;
    if (spg_device_watchdog_check(&dev, 5000u) != SPG_WATCHDOG_OK ||
        spg_device_watchdog_check(&dev, 4000u) != SPG_WATCHDOG_OK) {
        return 1;
    }
    return 0;
}

static int test_safe_state(void) {
    char dir[] = "/tmp/spg_device_XXXXXX";
    if (mkdtemp(dir) == nullptr) {
        return 1;
    }
    char heater_prog[256], heater_file[256];
    (void)snprintf(heater_file, sizeof heater_file, "%s/heater.value", dir);
    char body[512];
    (void)snprintf(body, sizeof body, "printf %%s \"$1\" > %s\n", heater_file);
    if (write_script(dir, "heater", body, heater_prog, sizeof heater_prog) !=
        0) {
        return 1;
    }

    struct spg_device dev = {};
    spg_device_init(&dev);
    /* Deliberate order: the unreachable valve FIRST, so the test fails if one
     * failure aborts the channels after it. */
    struct spg_device_channel valve = {.name     = "valve",
                                       .program  = "/nonexistent/valve",
                                       .min      = 0,
                                       .max      = 1,
                                       .writable = true,
                                       .safe     = 1};
    struct spg_device_channel heater = {
        .name = "heater", .min = 0, .max = 100, .writable = true, .safe = 0};
    (void)snprintf(heater.program, sizeof heater.program, "%s", heater_prog);
    struct spg_device_channel temp = {
        .name = "temp", .program = "/nonexistent/temp", .min = -400,
        .max  = 9000};
    if (spg_device_add_channel(&dev, &valve) != SPG_OK ||
        spg_device_add_channel(&dev, &heater) != SPG_OK ||
        spg_device_add_channel(&dev, &temp) != SPG_OK) {
        return 1;
    }

    /* The valve fails, the heater still lands on its safe value, and the
     * first failure is what the caller hears. */
    if (spg_device_safe_state(&dev) != SPG_E_IO) {
        return 1;
    }
    char  file_content[16] = {};
    FILE *f                = fopen(heater_file, "rb");
    if (f == nullptr ||
        fread(file_content, 1u, sizeof file_content - 1u, f) == 0u ||
        strcmp(file_content, "0") != 0) {
        if (f != nullptr) {
            (void)fclose(f);
        }
        return 1;
    }
    (void)fclose(f);

    /* A safe value the channel would refuse is rejected when the table is
     * built, not discovered when the watchdog fires. */
    struct spg_device_channel impossible = {.name     = "bad",
                                            .program  = "/p/bad",
                                            .min      = 0,
                                            .max      = 10,
                                            .writable = true,
                                            .safe     = 99};
    if (spg_device_add_channel(&dev, &impossible) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

static int test_state_render(void) {
    struct spg_device_state state = {
        .n        = 3u,
        .readings = {{.name = "temp", .value = 2350, .known = true},
                     {.name = "heater", .value = 0, .known = true},
                     {.name = "druck", .value = 0, .known = false}}};
    char   buf[SPG_DEVICE_RENDER_CAP];
    size_t required = 0u;
    if (spg_device_state_render(&state, sizeof buf, buf, &required) != SPG_OK) {
        return 1;
    }
    if (strcmp(buf, "(device-state (temp 2350) (heater 0) (druck unknown))") !=
        0) {
        (void)fprintf(stderr, "  rendered: %s\n", buf);
        return 1;
    }
    if (required != strlen(buf) + 1u) {
        return 1;
    }
    /* An unknown reading must never come out as 0 — the whole point of the
     * flag. A controller cannot tell a dead sensor from a zero measurement. */
    if (strstr(buf, "(druck 0)") != nullptr) {
        return 1;
    }
    /* A negative value keeps its sign; a below-zero temperature is the common
     * case that a naive unsigned path gets wrong. */
    struct spg_device_state cold = {
        .n = 1u, .readings = {{.name = "temp", .value = -400, .known = true}}};
    if (spg_device_state_render(&cold, sizeof buf, buf, &required) != SPG_OK ||
        strcmp(buf, "(device-state (temp -400))") != 0) {
        return 1;
    }
    /* Too small a buffer reports the need and writes no partial record. */
    char small[8] = {};
    if (spg_device_state_render(&state, sizeof small, small, &required) !=
        SPG_E_LIMIT) {
        return 1;
    }
    return 0;
}

static int test_sample_unreachable(void) {
    /* A sample of a plant whose programs are gone yields one unknown reading
     * per channel rather than an empty block. An agent must be able to tell
     * "the plant is unreachable" from "the plant has no channels". */
    struct spg_device dev = {};
    spg_device_init(&dev);
    const struct spg_device_channel temp = {.name    = "temp",
                                            .program = "/nonexistent/temp",
                                            .min     = -400,
                                            .max     = 9000};
    const struct spg_device_channel heater = {.name    = "heater",
                                              .program = "/nonexistent/heater",
                                              .min     = 0,
                                              .max     = 100,
                                              .writable = true,
                                              .safe     = 0};
    if (spg_device_add_channel(&dev, &temp) != SPG_OK ||
        spg_device_add_channel(&dev, &heater) != SPG_OK) {
        return 1;
    }
    struct spg_device_state state = {};
    if (spg_device_sample(&dev, &state) != SPG_E_IO) {
        return 1;
    }
    if (state.n != 2u || state.readings[0].known || state.readings[1].known) {
        return 1;
    }
    if (strcmp(state.readings[0].name, "temp") != 0 ||
        strcmp(state.readings[1].name, "heater") != 0) {
        return 1;
    }
    char   buf[SPG_DEVICE_RENDER_CAP];
    size_t required = 0u;
    if (spg_device_state_render(&state, sizeof buf, buf, &required) != SPG_OK ||
        strcmp(buf, "(device-state (temp unknown) (heater unknown))") != 0) {
        return 1;
    }
    return 0;
}

int main(void) {
    struct {
        const char *name;
        int (*fn)(void);
    } cases[] = {
        {"parse_channel", test_parse_channel},
        {"load", test_load},
        {"table", test_table},
        {"write_refusals", test_write_refusals},
        {"exec_contract", test_exec_contract},
        {"watchdog", test_watchdog},
        {"safe_state", test_safe_state},
        {"state_render", test_state_render},
        {"sample_unreachable", test_sample_unreachable},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i += 1u) {
        if (cases[i].fn() != 0) {
            (void)fprintf(stderr, "test_device: FAIL — %s\n", cases[i].name);
            return 1;
        }
    }
    (void)printf("test_device: PASS\n");
    return 0;
}
