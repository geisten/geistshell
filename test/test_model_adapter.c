#include "geistshell/model_adapter.h"
#include "geistshell/policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_fake_generate(void) {
    struct spg_model_adapter adapter = {};
    const char response[] = "(action recommend)";
    const struct spg_model_adapter_config config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f},
        .fake_response_n = sizeof response - 1u,
        .fake_response   = response,
    };
    if (spg_model_adapter_init(&adapter, &config) != SPG_OK) {
        return 1;
    }
    const char prompt[] = "(context)";
    char output[64];
    struct spg_model_generate_result result = {
        .output_capacity = sizeof output,
        .output          = output,
    };
    const struct spg_model_generate_request request = {
        .prompt_n           = sizeof prompt - 1u,
        .prompt             = prompt,
        .reset_session      = true,
        .max_decode_tokens  = 8u,
    };
    if (spg_model_generate(&adapter, &request, &result) != SPG_OK) {
        spg_model_adapter_destroy(&adapter);
        return 1;
    }
    if (strcmp(output, response) != 0 || result.output_used != sizeof response - 1u ||
        result.tokens_decoded != 1u || result.output_truncated ||
        result.stopped_by_token_limit || result.stopped_by_eos) {
        spg_model_adapter_destroy(&adapter);
        return 1;
    }
    spg_model_adapter_destroy(&adapter);
    return adapter.initialized ? 1 : 0;
}

static int test_fake_output_limit(void) {
    struct spg_model_adapter adapter = {};
    const char response[] = "abcdef";
    const struct spg_model_adapter_config config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.top_p = 1.0f},
        .fake_response_n = sizeof response - 1u,
        .fake_response   = response,
    };
    if (spg_model_adapter_init(&adapter, &config) != SPG_OK) {
        return 1;
    }
    const char prompt[] = "p";
    char output[4];
    struct spg_model_generate_result result = {
        .output_capacity = sizeof output,
        .output          = output,
    };
    const struct spg_model_generate_request request = {
        .prompt_n           = sizeof prompt - 1u,
        .prompt             = prompt,
        .max_decode_tokens  = 1u,
    };
    const enum spg_status status =
        spg_model_generate(&adapter, &request, &result);
    spg_model_adapter_destroy(&adapter);
    if (status != SPG_E_LIMIT || !result.output_truncated) {
        return 1;
    }
    return strcmp(output, "abc") == 0 ? 0 : 1;
}

static int test_zero_token_limit(void) {
    struct spg_model_adapter adapter = {};
    const struct spg_model_adapter_config config = {
        .kind     = SPG_MODEL_ADAPTER_FAKE,
        .sampling = {.top_p = 1.0f},
    };
    if (spg_model_adapter_init(&adapter, &config) != SPG_OK) {
        return 1;
    }
    const char prompt[] = "p";
    char output[4] = "xxx";
    struct spg_model_generate_result result = {
        .output_capacity = sizeof output,
        .output          = output,
    };
    const struct spg_model_generate_request request = {
        .prompt_n          = sizeof prompt - 1u,
        .prompt            = prompt,
        .max_decode_tokens = 0u,
    };
    if (spg_model_generate(&adapter, &request, &result) != SPG_OK) {
        spg_model_adapter_destroy(&adapter);
        return 1;
    }
    spg_model_adapter_destroy(&adapter);
    return output[0] == '\0' && result.stopped_by_token_limit ? 0 : 1;
}

static int test_invalid_config(void) {
    struct spg_model_adapter adapter = {};
    const char response[] = "x";
    struct spg_model_adapter_config config = {
        .kind            = SPG_MODEL_ADAPTER_FAKE,
        .sampling        = {.temperature = -1.0f, .top_p = 1.0f},
        .fake_response_n = sizeof response - 1u,
        .fake_response   = response,
    };
    if (spg_model_adapter_init(&adapter, &config) != SPG_E_INVALID_ARG) {
        return 1;
    }
    config.sampling.temperature = 0.0f;
    config.fake_response        = nullptr;
    if (spg_model_adapter_init(&adapter, &config) != SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

static int test_invalid_generate_args(void) {
    struct spg_model_adapter adapter = {};
    const struct spg_model_adapter_config config = {
        .kind     = SPG_MODEL_ADAPTER_FAKE,
        .sampling = {.top_p = 1.0f},
    };
    if (spg_model_adapter_init(&adapter, &config) != SPG_OK) {
        return 1;
    }
    char output[8];
    struct spg_model_generate_result result = {
        .output_capacity = sizeof output,
        .output          = output,
    };
    const struct spg_model_generate_request request = {
        .prompt_n          = 1u,
        .prompt            = "not-null-at-one",
        .max_decode_tokens = 1u,
    };
    if (spg_model_generate(nullptr, &request, &result) != SPG_E_INVALID_ARG) {
        spg_model_adapter_destroy(&adapter);
        return 1;
    }
    if (spg_model_generate(&adapter, &request, nullptr) != SPG_E_INVALID_ARG) {
        spg_model_adapter_destroy(&adapter);
        return 1;
    }
    spg_model_adapter_destroy(&adapter);
    return 0;
}

/* In the default build (no libcurl), the REMOTE kind is selectable but
 * unimplemented: init must report SPG_E_UNSUPPORTED and leave the adapter
 * uninitialized. Under SPG_ENABLE_REMOTE the transport is present, so this
 * default-build contract no longer applies and the check is skipped. */
static int test_remote_unsupported(void) {
#ifndef SPG_ENABLE_REMOTE
    struct spg_model_adapter        adapter = {};
    const struct spg_model_adapter_config config = {
        .kind         = SPG_MODEL_ADAPTER_REMOTE,
        .sampling     = {.top_p = 1.0f},
        .endpoint_url = "http://127.0.0.1:1/v1/chat/completions",
        .model_name   = "test-model",
    };
    if (spg_model_adapter_init(&adapter, &config) != SPG_E_UNSUPPORTED) {
        return 1;
    }
    if (adapter.initialized) {
        return 1;
    }
#endif
    return 0;
}

/* When built with the libcurl transport, REMOTE init must create a handle and
 * mark the adapter initialized, destroy must tear it down, and a config missing
 * the endpoint/model is rejected. No network is touched (no generate call).
 * Skipped in the default build, which has no transport. */
static int test_remote_lifecycle(void) {
#ifdef SPG_ENABLE_REMOTE
    struct spg_model_adapter        adapter = {};
    const struct spg_model_adapter_config config = {
        .kind         = SPG_MODEL_ADAPTER_REMOTE,
        .sampling     = {.top_p = 1.0f},
        .endpoint_url = "http://127.0.0.1:9/v1/chat/completions",
        .model_name   = "m",
    };
    if (spg_model_adapter_init(&adapter, &config) != SPG_OK) {
        return 1;
    }
    if (!adapter.initialized || adapter.http == nullptr) {
        spg_model_adapter_destroy(&adapter);
        return 1;
    }
    spg_model_adapter_destroy(&adapter);
    if (adapter.initialized || adapter.http != nullptr) {
        return 1;
    }
    struct spg_model_adapter_config bad = config;
    bad.endpoint_url                    = nullptr;
    if (spg_model_adapter_init(&adapter, &bad) != SPG_E_INVALID_ARG) {
        return 1;
    }
#endif
    return 0;
}

/* Through geistd (GEISTSHELL_TEST_GEISTD = socket path): the same adapter
 * code path an agent run takes, against a resident daemon. Free decode
 * yields text and stops by EOS or budget; the same request again is served
 * from the resident prefix; a constrained choice through force_prefix +
 * capabilities picks from the mask. Skipped without the env. */
static int test_geistd_path(void) {
    const char *sock = getenv("GEISTSHELL_TEST_GEISTD");
    if (sock == nullptr || sock[0] == '\0') {
        return 0;
    }
    struct spg_model_adapter              adapter = {};
    const struct spg_model_adapter_config config  = {
        .kind     = SPG_MODEL_ADAPTER_GEIST,
        .geistd   = sock,
        .sampling = {.max_seq_len = 4096u, .top_p = 1.0f},
    };
    if (spg_model_adapter_init(&adapter, &config) != SPG_OK) {
        fprintf(stderr, "geistd: init failed\n");
        return 1;
    }
    char                             out[512] = "";
    struct spg_model_generate_result result   = {.output_capacity = sizeof out,
                                                 .output          = out};
    const struct spg_model_generate_request request = {
        .prompt_n          = strlen("The capital of France is"),
        .prompt            = "The capital of France is",
        .reset_session     = true,
        .max_decode_tokens = 8u,
    };
    if (spg_model_generate(&adapter, &request, &result) != SPG_OK) {
        fprintf(stderr, "geistd: generate failed\n");
        spg_model_adapter_destroy(&adapter);
        return 1;
    }
    if (result.output_used == 0u || result.tokens_decoded == 0u ||
        !(result.stopped_by_eos || result.stopped_by_token_limit)) {
        fprintf(stderr, "geistd: empty or unterminated output\n");
        spg_model_adapter_destroy(&adapter);
        return 1;
    }
    fprintf(stderr, "geistd free decode: %.*s\n", (int)result.output_used, out);
    struct spg_model_generate_result again = {.output_capacity = sizeof out,
                                              .output          = out};
    if (spg_model_generate(&adapter, &request, &again) != SPG_OK ||
        again.output_used == 0u) {
        fprintf(stderr, "geistd: second generate failed\n");
        spg_model_adapter_destroy(&adapter);
        return 1;
    }
    spg_model_adapter_destroy(&adapter);
    if (adapter.gd != nullptr) {
        return 1;
    }
    static const struct spg_model_capability caps[] = {
        {.name = "shell", .kind = SPG_ACTION_LOCAL_SHELL},
        {.name = "memory", .kind = SPG_ACTION_MEMORY_READ},
    };
    struct spg_model_adapter              c_adapter = {};
    const struct spg_model_adapter_config c_config  = {
        .kind             = SPG_MODEL_ADAPTER_GEIST,
        .geistd           = sock,
        .sampling         = {.max_seq_len = 4096u, .top_p = 1.0f},
        .force_prefix     = "(recommend (kind ",
        .capabilities     = caps,
        .capability_count = 2u,
    };
    if (spg_model_adapter_init(&c_adapter, &c_config) != SPG_OK) {
        fprintf(stderr, "geistd: constrained init failed\n");
        return 1;
    }
    char                             cout[512] = "";
    struct spg_model_generate_result cres      = {.output_capacity = sizeof cout,
                                                  .output          = cout};
    const struct spg_model_generate_request creq = {
        .prompt_n = strlen("Goal: list the files in the current directory.\n"),
        .prompt   = "Goal: list the files in the current directory.\n",
        .reset_session     = true,
        .max_decode_tokens = 48u,
    };
    const enum spg_status cs = spg_model_generate(&c_adapter, &creq, &cres);
    spg_model_adapter_destroy(&c_adapter);
    /* any kind from the mask (finish is always allowed), and a closed form */
    if (cs != SPG_OK || strncmp(cout, "(recommend (kind ", 17) != 0 ||
        strchr(cout, ')') == nullptr) {
        fprintf(stderr, "geistd constrained: status %d output %.*s\n", (int)cs,
                (int)cres.output_used, cout);
        return 1;
    }
    fprintf(stderr, "geistd constrained: %.*s\n", (int)cres.output_used, cout);
    return 0;
}

int main(void) {
    if (test_geistd_path() != 0) {
        fprintf(stderr, "test_geistd_path failed\n");
        return 1;
    }
    if (test_remote_unsupported() != 0) {
        fprintf(stderr, "test_remote_unsupported failed\n");
        return 1;
    }
    if (test_remote_lifecycle() != 0) {
        fprintf(stderr, "test_remote_lifecycle failed\n");
        return 1;
    }
    if (test_fake_generate() != 0) {
        fprintf(stderr, "test_fake_generate failed\n");
        return 1;
    }
    if (test_fake_output_limit() != 0) {
        fprintf(stderr, "test_fake_output_limit failed\n");
        return 1;
    }
    if (test_zero_token_limit() != 0) {
        fprintf(stderr, "test_zero_token_limit failed\n");
        return 1;
    }
    if (test_invalid_config() != 0) {
        fprintf(stderr, "test_invalid_config failed\n");
        return 1;
    }
    if (test_invalid_generate_args() != 0) {
        fprintf(stderr, "test_invalid_generate_args failed\n");
        return 1;
    }
    return 0;
}
