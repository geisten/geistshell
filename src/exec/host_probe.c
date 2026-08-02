#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#    define _DARWIN_C_SOURCE 1
#endif

#include "geistshell/host_probe.h"

#include <string.h>
#include <sys/utsname.h>

/* Copy a NUL-terminated source into a fixed destination, truncating to fit and
 * always terminating. cap is guaranteed > 0 by the struct definition. */
static void copy_field(char *dst, const size_t cap, const char *src) {
    size_t i = 0u;
    if (src != nullptr) {
        for (; i + 1u < cap && src[i] != '\0'; i += 1u) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

static enum spg_host_os classify(const char *sysname) {
    if (sysname == nullptr) {
        return SPG_HOST_OS_UNKNOWN;
    }
    if (strcmp(sysname, "Linux") == 0) {
        return SPG_HOST_OS_LINUX;
    }
    if (strcmp(sysname, "Darwin") == 0) {
        return SPG_HOST_OS_MACOS;
    }
    if (strcmp(sysname, "FreeBSD") == 0) {
        return SPG_HOST_OS_FREEBSD;
    }
    if (strcmp(sysname, "OpenBSD") == 0) {
        return SPG_HOST_OS_OPENBSD;
    }
    if (strcmp(sysname, "NetBSD") == 0) {
        return SPG_HOST_OS_NETBSD;
    }
    return SPG_HOST_OS_UNKNOWN;
}

enum spg_status spg_host_probe(struct spg_host_info *out) {
    if (out == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *out = (struct spg_host_info){};

    struct utsname uts;
    if (uname(&uts) != 0) {
        return SPG_E_IO;
    }
    copy_field(out->sysname, sizeof out->sysname, uts.sysname);
    copy_field(out->release, sizeof out->release, uts.release);
    copy_field(out->version, sizeof out->version, uts.version);
    copy_field(out->machine, sizeof out->machine, uts.machine);
    copy_field(out->nodename, sizeof out->nodename, uts.nodename);
    out->os = classify(out->sysname);
    return SPG_OK;
}

const char *spg_host_os_to_string(const enum spg_host_os os) {
    switch (os) {
    case SPG_HOST_OS_UNKNOWN:
        return "unknown";
    case SPG_HOST_OS_LINUX:
        return "linux";
    case SPG_HOST_OS_MACOS:
        return "macos";
    case SPG_HOST_OS_FREEBSD:
        return "freebsd";
    case SPG_HOST_OS_OPENBSD:
        return "openbsd";
    case SPG_HOST_OS_NETBSD:
        return "netbsd";
    }
    return "unknown";
}
