#include "geistshell/cmd_menu.h"

#include <string.h>

#include <stdio.h>
#include <string.h>

static int test_find_known(void) {
    const struct spg_cmd_menu_entry *uname = spg_cmd_menu_find("uname");
    if (uname == nullptr || strcmp(uname->name, "uname") != 0 ||
        uname->uses_network) {
        return 1;
    }
    const struct spg_cmd_menu_entry *ssh = spg_cmd_menu_find("ssh");
    if (ssh == nullptr || !ssh->uses_network) {
        return 1;
    }
    return 0;
}

static int test_find_unknown(void) {
    if (spg_cmd_menu_find("definitely_not_a_real_command") != nullptr) {
        return 1;
    }
    return spg_cmd_menu_find(nullptr) == nullptr ? 0 : 1;
}

static int test_availability(void) {
    const struct spg_cmd_menu_entry *uname = spg_cmd_menu_find("uname");
    if (uname == nullptr) {
        return 1;
    }
    if (!spg_cmd_menu_available(uname, SPG_HOST_OS_LINUX) ||
        !spg_cmd_menu_available(uname, SPG_HOST_OS_MACOS) ||
        !spg_cmd_menu_available(uname, SPG_HOST_OS_FREEBSD)) {
        return 1;
    }
    /* Null descriptor is never available; UNKNOWN os has no bit set. */
    if (spg_cmd_menu_available(nullptr, SPG_HOST_OS_LINUX) ||
        spg_cmd_menu_available(uname, SPG_HOST_OS_UNKNOWN)) {
        return 1;
    }
    return 0;
}

static int test_table_well_formed(void) {
    const size_t count = spg_cmd_menu_count();
    if (count == 0u || spg_cmd_menu_at(count) != nullptr) {
        return 1;
    }
    for (size_t i = 0u; i < count; i += 1u) {
        const struct spg_cmd_menu_entry *d = spg_cmd_menu_at(i);
        if (d == nullptr || d->name == nullptr || d->name[0] == '\0' ||
            d->summary == nullptr || d->os_mask == 0u) {
            return 1;
        }
        /* Names must be unique. */
        for (size_t j = i + 1u; j < count; j += 1u) {
            const struct spg_cmd_menu_entry *e = spg_cmd_menu_at(j);
            if (e != nullptr && strcmp(d->name, e->name) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* #56: the menu is rendered for the model — the half that never existed. */
static int test_render(void) {
    char         buf[4096];
    const size_t n = spg_cmd_menu_render(SPG_HOST_OS_LINUX, sizeof buf, buf);
    if (n == 0u || buf[n] != '\0') {
        return 1;
    }
    /* summary and common_flags are useless to the executor and meaningful only
     * to a model; if they are not in the render, the table is dead weight
     * again. */
    if (strstr(buf, "(ls ") == nullptr ||
        strstr(buf, "list directory contents") == nullptr ||
        strstr(buf, "-l -a") == nullptr) {
        return 1;
    }
    /* Truncation yields a SHORTER menu, never a torn line — the context is
     * bounded and a half-written entry would corrupt the s-expression. */
    char   small[40];
    size_t m = spg_cmd_menu_render(SPG_HOST_OS_LINUX, sizeof small, small);
    if (m >= sizeof small || small[m] != '\0') {
        return 1;
    }
    for (size_t i = 0u; i < m; i += 1u) {
        if (small[i] == '\0') {
            return 1;
        }
    }
    if (m > 0u && small[m - 1u] != '\n') {
        return 1; /* stopped mid-entry */
    }
    if (spg_cmd_menu_render(SPG_HOST_OS_LINUX, 0u, buf) != 0u ||
        spg_cmd_menu_render(SPG_HOST_OS_LINUX, sizeof buf, nullptr) != 0u) {
        return 1;
    }
    return 0;
}

static int test_load(void) {
    static struct spg_cmd_menu m;
    const char *src =
        "(command_menu"
        " ((name \"ls\") (summary \"list files\") (flags \"-l\"))"
        " ((name \"date\") (summary \"print the date\")))";
    if (spg_cmd_menu_load(strlen(src), src, &m) != SPG_OK) {
        return 1;
    }
    if (m.count != 2u || strcmp(m.entries[0].name, "ls") != 0 ||
        strcmp(m.entries[0].summary, "list files") != 0 ||
        strcmp(m.entries[0].common_flags, "-l") != 0) {
        return 1;
    }
    /* flags are optional; a missing one is an empty hint, not a null deref */
    if (strcmp(m.entries[1].name, "date") != 0 ||
        m.entries[1].common_flags == nullptr ||
        m.entries[1].common_flags[0] != '\0') {
        return 1;
    }
    /* A file menu may name commands the built-in table never heard of — that
     * is the whole point of it being loadable. */
    if (spg_cmd_menu_find("date") != nullptr) {
        return 1; /* not in the built-in table */
    }

    char buf[512];
    if (spg_cmd_menu_render_of(&m, sizeof buf, buf) == 0u ||
        strstr(buf, "(date ") == nullptr) {
        return 1;
    }
    const char *names[SPG_CMD_MENU_MAX];
    if (spg_cmd_menu_names(&m, SPG_CMD_MENU_MAX, names) != 2u ||
        strcmp(names[1], "date") != 0) {
        return 1;
    }
    return 0;
}

static int test_load_rejects(void) {
    static struct spg_cmd_menu m;
    /* A malformed menu is an error, never a silently shorter menu: a dropped
     * row changes what the agent will try, quietly. */
    static const char *const bad[] = {
        "(command_menu)",                                  /* no entries      */
        "(command_menu ((summary \"no name\")))",           /* nameless       */
        "(command_menu ((name \"ls\")))",                   /* undescribed    */
        "(command_menu ((name \"\") (summary \"x\")))",      /* empty name     */
        "(tools ((name \"ls\") (summary \"x\")))",          /* wrong form     */
        "(command_menu ((name \"ls\") (summary \"x\"))",     /* unbalanced     */
        "",
    };
    for (size_t i = 0u; i < sizeof bad / sizeof bad[0]; i += 1u) {
        if (spg_cmd_menu_load(strlen(bad[i]), bad[i], &m) == SPG_OK) {
            return 1;
        }
    }
    return 0;
}

/* The rendered menu goes into the context, and the context is hashed by
 * test_cli_baseline.sh. If the render ever varies by host OS, that hash becomes
 * platform-dependent and the determinism canary quietly stops being one — on
 * the machine that does not run it.
 *
 * Every built-in entry is SPG_CMD_OS_ALL today, so this holds. It is asserted
 * rather than assumed, because the person who adds the first platform-specific
 * command will not be thinking about a journal hash. */
static int test_render_is_host_independent(void) {
    static const enum spg_host_os every[] = {
        SPG_HOST_OS_LINUX, SPG_HOST_OS_MACOS, SPG_HOST_OS_FREEBSD,
        SPG_HOST_OS_OPENBSD, SPG_HOST_OS_NETBSD,
    };
    char first[4096];
    if (spg_cmd_menu_render(every[0], sizeof first, first) == 0u) {
        return 1;
    }
    for (size_t i = 1u; i < sizeof every / sizeof every[0]; i += 1u) {
        char other[4096];
        (void)spg_cmd_menu_render(every[i], sizeof other, other);
        if (strcmp(first, other) != 0) {
            fprintf(stderr,
                    "the menu differs between hosts, so the baseline journal "
                    "hash is now platform-dependent.\n"
                    "Either give the new entry SPG_CMD_OS_ALL, or decide "
                    "deliberately that test_cli_baseline.sh is per-platform.\n");
            return 1;
        }
    }
    return 0;
}

int main(void) {
    if (test_find_known() != 0) {
        fprintf(stderr, "test_find_known failed\n");
        return 1;
    }
    if (test_find_unknown() != 0) {
        fprintf(stderr, "test_find_unknown failed\n");
        return 1;
    }
    if (test_availability() != 0) {
        fprintf(stderr, "test_availability failed\n");
        return 1;
    }
    if (test_render() != 0) {
        fprintf(stderr, "test_render failed\n");
        return 1;
    }
    if (test_load() != 0) {
        fprintf(stderr, "test_load failed\n");
        return 1;
    }
    if (test_load_rejects() != 0) {
        fprintf(stderr, "test_load_rejects failed\n");
        return 1;
    }
    if (test_render_is_host_independent() != 0) {
        fprintf(stderr, "test_render_is_host_independent failed\n");
        return 1;
    }
    if (test_table_well_formed() != 0) {
        fprintf(stderr, "test_table_well_formed failed\n");
        return 1;
    }
    return 0;
}
