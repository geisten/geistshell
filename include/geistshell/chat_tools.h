#ifndef GEISTSHELL_CHAT_TOOLS_H
#define GEISTSHELL_CHAT_TOOLS_H

#include "geistshell/journal.h"
#include "geistshell/mem_store.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How chat may launch a governed agent run. The model can only PROPOSE a run;
 * confirm() puts the decision with the operator, per launch, at the terminal.
 * A null launcher (or a null field) makes the tool report itself unavailable —
 * there is no fallback path that runs without a human in the loop. */
struct spg_chat_agent_launcher {
    /* Path to the geistshell binary to exec ("agent --config <path>"). */
    const char *agent_bin;
    /* Asked before every launch with the proposed config path; returning
     * false declines the run and the model is told so. */
    bool (*confirm)(void *userdata, const char *config_path);
    void *userdata;
};

/* In-turn agentic tool calls for the chat REPL. When the model's reply is a
 * (tool <name> ...) s-expression, the chat executes it and feeds the result
 * back for another generation, rather than showing it to the user.
 *
 * If input parses as a tool call, run it against the memory store, write a
 * human/agent-readable result into out (NUL-terminated), and set *was_tool.
 * Otherwise *was_tool is false and out is empty (the reply is a normal answer).
 *
 * Supported tools:
 *   (tool memory_list)
 *   (tool memory_read (slug "<slug>"))
 *   (tool memory_save (slug "<slug>") (description "<d>") (body "<b>"))
 *   (tool memory_delete (slug "<slug>"))
 *   (tool exec (command "<shell command>"))   [only when allow_exec]
 *   (tool agent_run (config "<run.spg>"))     [only with a launcher, and only
 *                                              past its confirm()]
 *
 * Side effects run through the shared governed executors (shell_executor /
 * mem_executor) and the executor boundary, so chat's tool calls enforce the
 * same execution contract as the orchestrator. When journal is non-null, each
 * side-effecting call is recorded there (audit trail); memory_list is a
 * read-only query and is not journaled. exec is gated behind allow_exec; when
 * false the tool reports that exec is disabled. agent_run is not journaled
 * here: the launched run writes its own journal, named by the confirmed
 * config, and that is the audit record.
 *
 * Returns SPG_E_INVALID_ARG on null arguments; otherwise SPG_OK (a tool's own
 * failure is reported in the result text, not the return value). */
[[nodiscard]] enum spg_status
spg_chat_tool_dispatch(struct spg_mem_store *store,
                       struct spg_journal_writer *journal, bool allow_exec,
                       const struct spg_chat_agent_launcher *launcher,
                       size_t input_n, const char *input, size_t out_cap,
                       char out[], bool *was_tool);

#ifdef __cplusplus
}
#endif

#endif
