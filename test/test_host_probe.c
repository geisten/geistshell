#include "geistshell/host_probe.h"

#include <stdio.h>
#include <string.h>

static int test_probe_basic(void) {
    struct spg_host_info info = {};
    if (spg_host_probe(&info) != SPG_OK) {
        return 1;
    }
    /* uname always yields a sysname; on supported hosts it classifies. */
    if (info.sysname[0] == '\0') {
        return 1;
    }
    /* Every field must be NUL-terminated within its buffer. */
    if (memchr(info.sysname, '\0', SPG_HOST_FIELD_CAP) == nullptr ||
        memchr(info.release, '\0', SPG_HOST_FIELD_CAP) == nullptr ||
        memchr(info.version, '\0', SPG_HOST_FIELD_CAP) == nullptr ||
        memchr(info.machine, '\0', SPG_HOST_FIELD_CAP) == nullptr ||
        memchr(info.nodename, '\0', SPG_HOST_FIELD_CAP) == nullptr) {
        return 1;
    }
    if (spg_host_os_to_string(info.os) == nullptr) {
        return 1;
    }
    return 0;
}

static int test_probe_null(void) {
    return spg_host_probe(nullptr) == SPG_E_INVALID_ARG ? 0 : 1;
}

static int test_os_strings(void) {
    const enum spg_host_os all[] = {
        SPG_HOST_OS_UNKNOWN, SPG_HOST_OS_LINUX,   SPG_HOST_OS_MACOS,
        SPG_HOST_OS_FREEBSD, SPG_HOST_OS_OPENBSD, SPG_HOST_OS_NETBSD,
    };
    for (size_t i = 0u; i < sizeof all / sizeof all[0]; i += 1u) {
        const char *s = spg_host_os_to_string(all[i]);
        if (s == nullptr || s[0] == '\0') {
            return 1;
        }
    }
    if (strcmp(spg_host_os_to_string(SPG_HOST_OS_UNKNOWN), "unknown") != 0 ||
        strcmp(spg_host_os_to_string(SPG_HOST_OS_LINUX), "linux") != 0) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_probe_basic() != 0) {
        fprintf(stderr, "test_probe_basic failed\n");
        return 1;
    }
    if (test_probe_null() != 0) {
        fprintf(stderr, "test_probe_null failed\n");
        return 1;
    }
    if (test_os_strings() != 0) {
        fprintf(stderr, "test_os_strings failed\n");
        return 1;
    }
    return 0;
}
