/* geistd_client.c — the one translation unit that compiles the vendored
 * geistd client (deps/geistd/geistd_client.h). It lives apart from
 * model_adapter.c because glibc hides getaddrinfo and struct addrinfo
 * under -std=c23 unless the POSIX feature macro precedes the first system
 * header, and a header included mid-file cannot arrange that. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#define GEISTD_CLIENT_IMPLEMENTATION
#include "geistd_client.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
