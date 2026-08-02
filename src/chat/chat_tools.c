#include "geistshell/chat_tools.h"

#include "geistshell/mem_executor.h"
#include "geistshell/sexpr.h"
#include "geistshell/shell_executor.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TOOL_TOKENS 256u
#define TOOL_NODES  256u

#define EXEC_TIMEOUT_MS 5000u
#define EXEC_STDOUT_CAP 4096u
#define EXEC_STDERR_CAP 1024u

/* Find a (argname "value") child under the tool form and return the value's
 * string-payload span (quotes stripped). */
static bool arg_string(size_t input_n, const char *input,
                       const struct spg_sexpr_node *nodes, uint32_t root,
                       const char *argname, struct spg_text_span *out) {
    for (uint32_t c = spg_sexpr_first_child(nodes, root);
         c != SPG_SEXPR_INVALID_INDEX; c = nodes[c].next_sibling) {
        if (nodes[c].kind != SPG_SEXPR_NODE_LIST) {
            continue;
        }
        const uint32_t name = spg_sexpr_first_child(nodes, c);
        const uint32_t val  = spg_sexpr_second_child(nodes, c);
        if (name == SPG_SEXPR_INVALID_INDEX || val == SPG_SEXPR_INVALID_INDEX ||
            nodes[name].kind != SPG_SEXPR_NODE_SYMBOL ||
            nodes[val].kind != SPG_SEXPR_NODE_STRING ||
            nodes[val].span.length < 2u) {
            continue;
        }
        if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span, argname)) {
            return spg_sexpr_string_payload_span(&nodes[val], out);
        }
    }
    return false;
}

static void span_to_buf(const char *input, struct spg_text_span span, char *buf,
                        size_t cap) {
    const size_t take = span.length + 1u > cap ? cap - 1u : span.length;
    memcpy(buf, input + span.offset, take);
    buf[take] = '\0';
}

/* A monotonic logical timestamp for the chat audit trail (chat is interactive,
 * so wall-clock determinism is not required). */
static uint64_t journal_clock(const struct spg_journal_writer *journal) {
    return journal != nullptr ? journal->next_sequence : 1u;
}

/* Run a (tool exec (command "...")) through the governed shell executor: the
 * boundary gates it, the command executor runs it, and the journal records it.
 * The executor's observation (exit code + output, or the denial) is the result. */
static void run_exec(size_t input_n, const char *input,
                     const struct spg_sexpr_node *nodes, bool allow_exec,
                     struct spg_journal_writer *journal, char out[],
                     const size_t out_cap) {
    if (!allow_exec) {
        (void)snprintf(out, out_cap,
                       "error: exec is disabled (start geistshell-chat with "
                       "--allow-exec)");
        return;
    }
    struct spg_text_span cspan;
    if (!arg_string(input_n, input, nodes, 0u, "command", &cspan)) {
        (void)snprintf(out, out_cap, "error: exec needs (command \"...\")");
        return;
    }

    static char payload[1024];
    static char obs[EXEC_STDOUT_CAP];
    static char so[EXEC_STDOUT_CAP];
    static char se[EXEC_STDERR_CAP];
    struct spg_shell_executor_state state = {.journal = journal};
    const struct spg_shell_executor_config config = {
        .actor_id               = 1u,
        .timestamp_ns           = journal_clock(journal),
        .parent_sequence        = 0u,
        .write_journal          = journal != nullptr,
        .execution_enabled      = true,
        .working_dir            = ".",
        .allowed_workdir_prefix = ".",
        .timeout_ms             = EXEC_TIMEOUT_MS,
        .max_stdout_bytes       = sizeof so,
        .max_stderr_bytes       = sizeof se,
    };
    const struct spg_shell_executor_workspace ws = {
        .payload_capacity     = sizeof payload,
        .payload              = payload,
        .observation_capacity = sizeof obs,
        .observation          = obs,
        .stdout_capacity      = sizeof so,
        .stdout_buf           = so,
        .stderr_capacity      = sizeof se,
        .stderr_buf           = se,
    };
    const struct spg_recommendation rec = {
        .state       = SPG_RECOMMENDATION_VALID,
        .action_kind = SPG_ACTION_LOCAL_SHELL,
        .action      = {.uses_network = false},
        .command     = cspan,
        .has_command = true,
    };
    const struct spg_policy_decision decision = {
        .kind = SPG_POLICY_DECISION_ALLOW,
    };
    struct spg_shell_executor_result result = {};
    (void)spg_shell_executor_step(&state, &config, input_n, input, &rec,
                                  &decision, &ws, &result);
    (void)snprintf(out, out_cap, "%s", obs);
}

/* Run a memory_save/delete/read through the governed memory executor so the
 * side effect is journaled. The human-readable result is formatted from the
 * outcome (and the recall buffer for reads). */
static void run_memory(struct spg_mem_store *store,
                       struct spg_journal_writer *journal,
                       const enum spg_action_kind kind, size_t input_n,
                       const char *input, const struct spg_sexpr_node *nodes,
                       char out[], const size_t out_cap) {
    struct spg_text_span slug_span = {};
    struct spg_text_span desc_span = {};
    struct spg_text_span body_span = {};
    const bool has_slug = arg_string(input_n, input, nodes, 0u, "slug", &slug_span);
    if (!has_slug) {
        (void)snprintf(out, out_cap, "error: %s needs a slug",
                       spg_action_kind_to_string(kind));
        return;
    }
    char slug[SPG_MEM_SLUG_MAX + 1u];
    span_to_buf(input, slug_span, slug, sizeof slug);

    struct spg_recommendation rec = {
        .state       = SPG_RECOMMENDATION_VALID,
        .action_kind = kind,
        .mem_slug    = slug_span,
        .has_slug    = true,
    };
    if (kind == SPG_ACTION_MEMORY_SAVE) {
        rec.has_description =
            arg_string(input_n, input, nodes, 0u, "description", &desc_span);
        rec.has_body = arg_string(input_n, input, nodes, 0u, "body", &body_span);
        rec.mem_description = desc_span;
        rec.mem_body        = body_span;
        if (!rec.has_description || !rec.has_body) {
            (void)snprintf(out, out_cap,
                           "error: cannot save (need slug, description, body)");
            return;
        }
    }

    static char payload[1024];
    static char recall[SPG_MEM_BODY_MAX + 1u];
    struct spg_mem_executor_state state = {.store = store, .journal = journal};
    const struct spg_mem_executor_config config = {
        .actor_id        = 1u,
        .timestamp_ns    = journal_clock(journal),
        .parent_sequence = 0u,
        .write_journal   = journal != nullptr,
    };
    const struct spg_mem_executor_workspace ws = {
        .payload_capacity = sizeof payload,
        .payload          = payload,
        .recall_capacity  = sizeof recall,
        .recall           = recall,
    };
    const struct spg_policy_decision decision = {
        .kind = SPG_POLICY_DECISION_ALLOW,
    };
    struct spg_mem_executor_result result = {};
    recall[0] = '\0';
    (void)spg_mem_executor_step(&state, &config, input_n, input, &rec, &decision,
                                &ws, &result);

    if (kind == SPG_ACTION_MEMORY_READ) {
        if (result.save_status == SPG_OK) {
            (void)snprintf(out, out_cap, "%s", recall);
        } else {
            (void)snprintf(out, out_cap, "error: cannot read %s (%s)", slug,
                           spg_status_to_string(result.save_status));
        }
        return;
    }
    if (kind == SPG_ACTION_MEMORY_DELETE) {
        (void)snprintf(out, out_cap,
                       result.save_status == SPG_OK ? "deleted %s"
                                                     : "error: cannot delete %s",
                       slug);
        return;
    }
    (void)snprintf(out, out_cap,
                   result.save_status == SPG_OK
                       ? "saved %s"
                       : "error: cannot save (need slug, description, body)",
                   slug);
}

enum spg_status spg_chat_tool_dispatch(struct spg_mem_store *store,
                                       struct spg_journal_writer *journal,
                                       const bool allow_exec,
                                       const size_t input_n, const char *input,
                                       const size_t out_cap, char out[],
                                       bool *was_tool) {
    if (store == nullptr || input == nullptr || out == nullptr ||
        out_cap == 0u || was_tool == nullptr) {
        return SPG_E_INVALID_ARG;
    }
    *was_tool = false;
    out[0]    = '\0';

    struct spg_sexpr_token tokens[TOOL_TOKENS];
    struct spg_sexpr_node  nodes[TOOL_NODES];
    size_t                 token_count = 0u;
    size_t                 node_count  = 0u;
    struct spg_sexpr_error error       = {};
    if (spg_sexpr_parse_text(input_n, input, TOOL_TOKENS, tokens, TOOL_NODES,
                             nodes, &token_count, &node_count, &error) !=
            SPG_OK ||
        node_count == 0u || nodes[0].kind != SPG_SEXPR_NODE_LIST) {
        return SPG_OK; /* not a tool call */
    }

    const uint32_t head = spg_sexpr_first_child(nodes, 0u);
    const uint32_t name = spg_sexpr_second_child(nodes, 0u);
    if (head == SPG_SEXPR_INVALID_INDEX || name == SPG_SEXPR_INVALID_INDEX ||
        nodes[head].kind != SPG_SEXPR_NODE_SYMBOL ||
        nodes[name].kind != SPG_SEXPR_NODE_SYMBOL ||
        !spg_sexpr_span_eq_cstr(input_n, input, nodes[head].span, "tool")) {
        return SPG_OK; /* not a tool call */
    }
    *was_tool = true;

    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span,
                               "memory_list")) {
        (void)spg_mem_index(store, out_cap, out, nullptr, nullptr);
        if (out[0] == '\0') {
            (void)snprintf(out, out_cap, "(no memories)");
        }
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span,
                               "memory_read")) {
        run_memory(store, journal, SPG_ACTION_MEMORY_READ, input_n, input, nodes,
                   out, out_cap);
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span,
                               "memory_delete")) {
        run_memory(store, journal, SPG_ACTION_MEMORY_DELETE, input_n, input,
                   nodes, out, out_cap);
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span,
                               "memory_save")) {
        run_memory(store, journal, SPG_ACTION_MEMORY_SAVE, input_n, input, nodes,
                   out, out_cap);
        return SPG_OK;
    }
    if (spg_sexpr_span_eq_cstr(input_n, input, nodes[name].span, "exec")) {
        run_exec(input_n, input, nodes, allow_exec, journal, out, out_cap);
        return SPG_OK;
    }

    (void)snprintf(out, out_cap, "error: unknown tool");
    return SPG_OK;
}
