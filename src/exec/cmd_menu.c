#include "geistshell/cmd_menu.h"

#include "geistshell/sexpr.h"

#include <stdio.h>
#include <string.h>

/* The built-in menu. NOT an allowlist — see the header. Add a command by
 * appending one entry, or ship a menu file instead. Keep names unique; lookup
 * is linear (the table is small) and order does not matter.
 *
 * `summary` and `common_flags` are here for exactly one consumer: the model's
 * context. They are meaningless to the executor. For most of this file's life
 * nothing rendered them, which is how it came to look like a security layer
 * that does nothing rather than a tool menu that was never plugged in. */
static const struct spg_cmd_menu_entry k_menu[] = {
    {.name               = "uname",
     .common_flags       = "-a -s -r -m -p",
     .summary            = "print system / kernel information",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 2000u,
     .uses_network       = false},
    {.name               = "hostname",
     .common_flags       = "-s -f",
     .summary            = "print the host name",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 2000u,
     .uses_network       = false},
    {.name               = "id",
     .common_flags       = "-u -g -n -G",
     .summary            = "print user and group identity",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 2000u,
     .uses_network       = false},
    {.name               = "whoami",
     .common_flags       = "",
     .summary            = "print the effective user name",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 2000u,
     .uses_network       = false},
    {.name               = "echo",
     .common_flags       = "-n",
     .summary            = "write arguments to standard output",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 2000u,
     .uses_network       = false},
    {.name               = "ls",
     .common_flags       = "-l -a -h -R",
     .summary            = "list directory contents",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 5000u,
     .uses_network       = false},
    {.name               = "cat",
     .common_flags       = "-n",
     .summary            = "concatenate and print files",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 5000u,
     .uses_network       = false},
    /* ps flags differ across kernels: BSD-style "aux" is widely accepted, but
     * Linux procps also supports "-ef". The flag hint reflects both. */
    {.name               = "ps",
     .common_flags       = "aux -ef",
     .summary            = "report process status",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 5000u,
     .uses_network       = false},
    {.name               = "df",
     .common_flags       = "-h -k",
     .summary            = "report file system disk usage",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 5000u,
     .uses_network       = false},
    {.name               = "ssh",
     .common_flags       = "-p -i -o -l",
     .summary            = "OpenSSH remote login client",
     .os_mask            = SPG_CMD_OS_ALL,
     .default_timeout_ms = 10000u,
     .uses_network       = true},
};

static const size_t k_menu_count = sizeof k_menu / sizeof k_menu[0];

const struct spg_cmd_menu_entry *spg_cmd_menu_find(const char *name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (size_t i = 0u; i < k_menu_count; i += 1u) {
        if (strcmp(k_menu[i].name, name) == 0) {
            return &k_menu[i];
        }
    }
    return nullptr;
}

bool spg_cmd_menu_available(const struct spg_cmd_menu_entry *desc,
                                const enum spg_host_os           os) {
    if (desc == nullptr) {
        return false;
    }
    const uint32_t bit = (uint32_t)1u << (unsigned)os;
    return (desc->os_mask & bit) != 0u;
}

size_t spg_cmd_menu_count(void) {
    return k_menu_count;
}

const struct spg_cmd_menu_entry *spg_cmd_menu_at(const size_t index) {
    if (index >= k_menu_count) {
        return nullptr;
    }
    return &k_menu[index];
}

size_t spg_cmd_menu_render(const enum spg_host_os os, const size_t cap,
                           char out[]) {
    if (out == nullptr || cap == 0u) {
        return 0u;
    }
    out[0]      = '\0';
    size_t used = 0u;
    for (size_t i = 0u; i < k_menu_count; i += 1u) {
        const struct spg_cmd_menu_entry *e = &k_menu[i];
        if (!spg_cmd_menu_available(e, os)) {
            continue;
        }
        const int n = snprintf(out + used, cap - used, "  (%s \"%s\"%s%s%s)\n",
                               e->name, e->summary,
                               e->common_flags != nullptr && e->common_flags[0] != '\0'
                                   ? " \"" : "",
                               e->common_flags != nullptr ? e->common_flags : "",
                               e->common_flags != nullptr && e->common_flags[0] != '\0'
                                   ? "\"" : "");
        if (n < 0 || (size_t)n >= cap - used) {
            out[used] = '\0'; /* a truncated menu is a shorter menu, never a
                               * malformed one */
            break;
        }
        used += (size_t)n;
    }
    return used;
}

/* ---- menu file (#56) ----------------------------------------------------- */

/* Copy a string span into the menu's own text store and return the borrowed
 * pointer, or null when it does not fit. */
static const char *intern(struct spg_cmd_menu *m, const size_t n,
                          const char in[], const struct spg_text_span sp) {
    if (sp.offset > n || sp.length > n - sp.offset ||
        sp.length + 1u > sizeof m->text - m->text_used) {
        return nullptr;
    }
    char *dst = m->text + m->text_used;
    memcpy(dst, in + sp.offset, sp.length);
    dst[sp.length] = '\0';
    m->text_used += sp.length + 1u;
    return dst;
}

/* String value of field `name` inside `entry`, interned. Null when absent. */
static const char *entry_field(struct spg_cmd_menu *m, const size_t n,
                               const char in[],
                               const struct spg_sexpr_node nodes[static 1],
                               const uint32_t entry, const char *name) {
    for (uint32_t f = spg_sexpr_first_child(nodes, entry);
         f != SPG_SEXPR_INVALID_INDEX; f = nodes[f].next_sibling) {
        const uint32_t fn = spg_sexpr_first_child(nodes, f);
        if (fn == SPG_SEXPR_INVALID_INDEX ||
            nodes[fn].kind != SPG_SEXPR_NODE_SYMBOL ||
            !spg_sexpr_span_eq_cstr(n, in, nodes[fn].span, name)) {
            continue;
        }
        const uint32_t v = spg_sexpr_second_child(nodes, f);
        struct spg_text_span sp;
        if (v == SPG_SEXPR_INVALID_INDEX ||
            !spg_sexpr_string_payload_span(&nodes[v], &sp)) {
            return nullptr;
        }
        return intern(m, n, in, sp);
    }
    return nullptr;
}

enum spg_status spg_cmd_menu_load(const size_t input_n,
                                  const char input[static 1],
                                  struct spg_cmd_menu *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_cmd_menu){};

    static struct spg_sexpr_token toks[2048];
    static struct spg_sexpr_node  nodes[2048];
    size_t                        tn = 0u;
    size_t                        nn = 0u;
    struct spg_sexpr_error        se = {};
    if (spg_sexpr_parse_text(input_n, input, 2048u, toks, 2048u, nodes, &tn, &nn,
                             &se) != SPG_OK ||
        nn == 0u || nodes[0].kind != SPG_SEXPR_NODE_LIST) {
        return SPG_E_FORMAT;
    }
    const uint32_t head = spg_sexpr_first_child(nodes, 0u);
    if (head == SPG_SEXPR_INVALID_INDEX ||
        !spg_sexpr_span_eq_cstr(input_n, input, nodes[head].span,
                                "command_menu")) {
        return SPG_E_SCHEMA;
    }
    for (uint32_t e = nodes[head].next_sibling; e != SPG_SEXPR_INVALID_INDEX;
         e = nodes[e].next_sibling) {
        if (nodes[e].kind != SPG_SEXPR_NODE_LIST) {
            return SPG_E_SCHEMA;
        }
        if (out->count >= SPG_CMD_MENU_MAX) {
            return SPG_E_LIMIT;
        }
        const char *name    = entry_field(out, input_n, input, nodes, e, "name");
        const char *summary = entry_field(out, input_n, input, nodes, e, "summary");
        const char *flags   = entry_field(out, input_n, input, nodes, e, "flags");
        /* A nameless or undescribed entry is useless to the model, which is the
         * only consumer — so it is an error, not a skipped row. */
        if (name == nullptr || name[0] == '\0' || summary == nullptr) {
            return SPG_E_SCHEMA;
        }
        out->entries[out->count] = (struct spg_cmd_menu_entry){
            .name               = name,
            .common_flags       = flags != nullptr ? flags : "",
            .summary            = summary,
            .os_mask            = SPG_CMD_OS_ALL,
            .default_timeout_ms = 5000u,
            .uses_network       = false,
        };
        out->count += 1u;
    }
    return out->count > 0u ? SPG_OK : SPG_E_SCHEMA;
}

size_t spg_cmd_menu_render_of(const struct spg_cmd_menu *menu, const size_t cap,
                              char out[]) {
    if (menu == nullptr || out == nullptr || cap == 0u) {
        return 0u;
    }
    out[0]      = '\0';
    size_t used = 0u;
    for (size_t i = 0u; i < menu->count; i += 1u) {
        const struct spg_cmd_menu_entry *e = &menu->entries[i];
        const bool has_flags = e->common_flags != nullptr && e->common_flags[0] != '\0';
        const int  n = snprintf(out + used, cap - used, "  (%s \"%s\"%s%s%s)\n",
                                e->name, e->summary, has_flags ? " \"" : "",
                                has_flags ? e->common_flags : "",
                                has_flags ? "\"" : "");
        if (n < 0 || (size_t)n >= cap - used) {
            out[used] = '\0';
            break;
        }
        used += (size_t)n;
    }
    return used;
}

size_t spg_cmd_menu_names(const struct spg_cmd_menu *menu, const size_t cap,
                          const char *names[]) {
    if (menu == nullptr || names == nullptr) {
        return 0u;
    }
    size_t n = 0u;
    for (size_t i = 0u; i < menu->count && n < cap; i += 1u) {
        if (menu->entries[i].name != nullptr) {
            names[n] = menu->entries[i].name;
            n += 1u;
        }
    }
    return n;
}

/* The built-in table as a loaded menu, so callers have one shape to handle. */
size_t spg_cmd_menu_builtin_names(const enum spg_host_os os, const size_t cap,
                                  const char *names[]) {
    if (names == nullptr) {
        return 0u;
    }
    size_t n = 0u;
    for (size_t i = 0u; i < k_menu_count && n < cap; i += 1u) {
        if (spg_cmd_menu_available(&k_menu[i], os)) {
            names[n] = k_menu[i].name;
            n += 1u;
        }
    }
    return n;
}
