#include "geistshell/status.h"

#include <stdio.h>

static int test_status_strings(void) {
    if (spg_status_to_string(SPG_OK) == nullptr) {
        return 1;
    }
    if (spg_status_to_string((enum spg_status)9999) == nullptr) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_status_strings() != 0) {
        fprintf(stderr, "test_status_strings failed\n");
        return 1;
    }
    return 0;
}
