#include "geistshell/cmd_registry.h"

#include <stdio.h>
#include <string.h>

static int test_find_known(void) {
    const struct spg_cmd_descriptor *uname = spg_cmd_registry_find("uname");
    if (uname == nullptr || strcmp(uname->name, "uname") != 0 ||
        uname->uses_network) {
        return 1;
    }
    const struct spg_cmd_descriptor *ssh = spg_cmd_registry_find("ssh");
    if (ssh == nullptr || !ssh->uses_network) {
        return 1;
    }
    return 0;
}

static int test_find_unknown(void) {
    if (spg_cmd_registry_find("definitely_not_a_real_command") != nullptr) {
        return 1;
    }
    return spg_cmd_registry_find(nullptr) == nullptr ? 0 : 1;
}

static int test_availability(void) {
    const struct spg_cmd_descriptor *uname = spg_cmd_registry_find("uname");
    if (uname == nullptr) {
        return 1;
    }
    if (!spg_cmd_registry_available(uname, SPG_HOST_OS_LINUX) ||
        !spg_cmd_registry_available(uname, SPG_HOST_OS_MACOS) ||
        !spg_cmd_registry_available(uname, SPG_HOST_OS_FREEBSD)) {
        return 1;
    }
    /* Null descriptor is never available; UNKNOWN os has no bit set. */
    if (spg_cmd_registry_available(nullptr, SPG_HOST_OS_LINUX) ||
        spg_cmd_registry_available(uname, SPG_HOST_OS_UNKNOWN)) {
        return 1;
    }
    return 0;
}

static int test_table_well_formed(void) {
    const size_t count = spg_cmd_registry_count();
    if (count == 0u || spg_cmd_registry_at(count) != nullptr) {
        return 1;
    }
    for (size_t i = 0u; i < count; i += 1u) {
        const struct spg_cmd_descriptor *d = spg_cmd_registry_at(i);
        if (d == nullptr || d->name == nullptr || d->name[0] == '\0' ||
            d->summary == nullptr || d->os_mask == 0u) {
            return 1;
        }
        /* Names must be unique. */
        for (size_t j = i + 1u; j < count; j += 1u) {
            const struct spg_cmd_descriptor *e = spg_cmd_registry_at(j);
            if (e != nullptr && strcmp(d->name, e->name) == 0) {
                return 1;
            }
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
    if (test_table_well_formed() != 0) {
        fprintf(stderr, "test_table_well_formed failed\n");
        return 1;
    }
    return 0;
}
