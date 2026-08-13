#include "geistshell/geistshell.h"

#include "geistshell/chat_template.h"
#include "geistshell/chat_tools.h"
#include "geistshell/mem_store.h"
#include "geistshell/model_adapter.h"
#include "geistshell/model_resolve.h"
#include "geistshell/status.h"

#include <geist.h>

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHAT_LINE_BYTES    4096u
#define CHAT_OUTPUT_BYTES  16384u
#define CHAT_HISTORY_BYTES 6144u /* ~ the CHAT_MAX_SEQ_LEN token budget */
#define CHAT_MAX_TOOL_ITERS 5u   /* bound on in-turn tool calls */
#define CHAT_DEF_TOKENS    256u
#define CHAT_MAX_SEQ_LEN   2048u

#ifndef PATH_MAX
#    define PATH_MAX 4096
#endif

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--model <path>] [--max-tokens <n>] [--no-download] "
            "[--fake] [--transcript <path>] [--memory-dir <path>] "
            "[--journal <path>] [--allow-exec]\n"
            "\n"
            "Interactive geistshell chat REPL.\n"
            "Connects to a Gemma 4 GGUF via the geist engine. The model path is\n"
            "taken from --model, then $GEISTSHELL_MODEL, then "
            "./gguf_artifacts/gemma4-e2b-Q4_K_M.gguf;\n"
            "a missing default is auto-downloaded with curl unless --no-download.\n"
            "Use --fake to run offline without a model. Type /quit to exit, "
            "/reset to clear context.\n",
            argv0);
}

static void write_json_string(FILE *file, const char *text) {
    fputc('"', file);
    if (text != nullptr) {
        for (size_t i = 0u; text[i] != '\0'; i += 1u) {
            const unsigned char ch = (unsigned char)text[i];
            if (ch == '"' || ch == '\\') {
                fputc('\\', file);
                fputc((int)ch, file);
            } else if (ch == '\n') {
                fputs("\\n", file);
            } else if (ch == '\r') {
                fputs("\\r", file);
            } else if (ch == '\t') {
                fputs("\\t", file);
            } else if (ch >= 32u && ch <= 126u) {
                fputc((int)ch, file);
            } else {
                fprintf(file, "\\u%04x", (unsigned int)ch);
            }
        }
    }
    fputc('"', file);
}

static int transcript_write(FILE *file, const char *role, const char *text) {
    if (file == nullptr) {
        return 0;
    }
    fputs("{\"role\":", file);
    write_json_string(file, role);
    fputs(",\"content\":", file);
    write_json_string(file, text);
    fputs("}\n", file);
    return fflush(file) == 0 ? 0 : 1;
}

static void trim_newline(char *line) {
    if (line == nullptr) {
        return;
    }
    const size_t n = strlen(line);
    if (n > 0u && line[n - 1u] == '\n') {
        line[n - 1u] = '\0';
    }
}

/* geist renders Gemma's turn/EOS tokens as literal surface text, so cut the
 * reply at the first stop marker the model emits. */
static void trim_at_stop_markers(char *text) {
    static const char *const markers[] = {
        "<end_of_turn>", "<start_of_turn>", "<eos>", "<turn|>",
    };
    char *cut = nullptr;
    for (size_t i = 0u; i < sizeof markers / sizeof markers[0]; i += 1u) {
        char *hit = strstr(text, markers[i]);
        if (hit != nullptr && (cut == nullptr || hit < cut)) {
            cut = hit;
        }
    }
    if (cut != nullptr) {
        *cut = '\0';
    }
}

struct chat_args {
    const char *model_path;
    const char *transcript_path;
    const char *memory_dir;
    const char *journal_path;
    size_t      max_tokens;
    bool        allow_download;
    bool        allow_exec;
    bool        fake;
};

/* Parse argv into args. Returns 0 on success, 1 to exit 0 (--help/--version),
 * 2 on a usage error. */
static int parse_args(int argc, char **argv, struct chat_args *out) {
    *out = (struct chat_args){
        .max_tokens     = CHAT_DEF_TOKENS,
        .allow_download = true,
    };
    for (int i = 1; i < argc; i += 1) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("geistshell-chat %s\n", SPG_VERSION_STRING);
            printf("libgeist %s\n", geist_version_string());
            return 1;
        }
        if (strcmp(argv[i], "--no-download") == 0) {
            out->allow_download = false;
            continue;
        }
        if (strcmp(argv[i], "--allow-exec") == 0) {
            out->allow_exec = true;
            continue;
        }
        if (strcmp(argv[i], "--fake") == 0) {
            out->fake = true;
            continue;
        }
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            out->model_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--transcript") == 0 && i + 1 < argc) {
            out->transcript_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--memory-dir") == 0 && i + 1 < argc) {
            out->memory_dir = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            out->journal_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--max-tokens") == 0 && i + 1 < argc) {
            char           *end = nullptr;
            const unsigned long v = strtoul(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v == 0u) {
                fprintf(stderr, "geistshell-chat: invalid --max-tokens\n");
                return 2;
            }
            out->max_tokens = (size_t)v;
            continue;
        }
        fprintf(stderr, "geistshell-chat: unknown or incomplete argument: %s\n",
                argv[i]);
        return 2;
    }
    return 0;
}

/* Initialize the model adapter (real or fake). Returns SPG_OK on success. */
static enum spg_status init_adapter(const struct chat_args   *args,
                                    struct spg_model_adapter *adapter) {
    if (args->fake) {
        static const char fake_reply[] =
            "(offline fake model — pass a real --model to generate)";
        const struct spg_model_adapter_config cfg = {
            .kind            = SPG_MODEL_ADAPTER_FAKE,
            .sampling        = {.top_p = 1.0f},
            .fake_response   = fake_reply,
            .fake_response_n = sizeof fake_reply - 1u,
        };
        return spg_model_adapter_init(adapter, &cfg);
    }

    char path[PATH_MAX];
    bool downloaded = false;
    const struct spg_model_resolve_opts ropts = {
        .explicit_path  = args->model_path,
        .allow_download = args->allow_download,
    };
    const enum spg_status rstatus =
        spg_model_resolve(&ropts, sizeof path, path, &downloaded);
    if (rstatus != SPG_OK) {
        fprintf(stderr,
                "geistshell-chat: no model (%s). Pass --model <path>, set "
                "$GEISTSHELL_MODEL, run `make fetch-model`, or use --fake.\n",
                spg_status_to_string(rstatus));
        return rstatus;
    }
    if (downloaded) {
        fprintf(stderr, "geistshell-chat: download complete.\n");
    }
    fprintf(stderr, "geistshell-chat: loading model %s ...\n", path);

    const struct spg_model_adapter_config cfg = {
        .kind         = SPG_MODEL_ADAPTER_GEIST,
        .model_path   = path,
        .backend_name = nullptr, /* "auto" */
        .sampling     = {.max_seq_len = CHAT_MAX_SEQ_LEN,
                         .temperature = 0.0f,
                         .top_p       = 1.0f,
                         .top_k       = 0,
                         .random_seed = 0u},
    };
    const enum spg_status istatus = spg_model_adapter_init(adapter, &cfg);
    if (istatus != SPG_OK) {
        fprintf(stderr, "geistshell-chat: model load failed (%s)\n",
                spg_status_to_string(istatus));
    }
    return istatus;
}

static const char *resolve_mem_dir(const struct chat_args *args) {
    return spg_mem_resolve_dir(args->memory_dir);
}

/* Read the first whitespace-delimited token of s into tok; *rest points at the
 * remainder (leading spaces skipped). */
static void split_first(const char *s, char *tok, size_t tok_cap,
                        const char **rest) {
    while (*s == ' ') {
        s += 1;
    }
    size_t i = 0u;
    while (s[i] != '\0' && s[i] != ' ' && i + 1u < tok_cap) {
        tok[i] = s[i];
        i += 1u;
    }
    tok[i]      = '\0';
    const char *r = s + i;
    while (*r == ' ') {
        r += 1;
    }
    *rest = r;
}

/* Handle the /memories, /recall, /remember, /forget slash-commands. Returns
 * true when line was such a command (already handled). /recall stashes the
 * memory text into pending for the next user message. */
static bool handle_memory_command(struct spg_mem_store *mem, bool have_mem,
                                  const char *line, const char *last_reply,
                                  char *pending, size_t pending_cap) {
    if (strcmp(line, "/memories") == 0) {
        static char idx[4096];
        bool        tr = false;
        if (!have_mem) {
            puts("assistant> (memory unavailable)");
            return true;
        }
        (void)spg_mem_index(mem, sizeof idx, idx, nullptr, &tr);
        if (idx[0] == '\0') {
            puts("assistant> (no memories yet)");
        } else {
            printf("assistant> known memories:\n%s", idx);
        }
        return true;
    }
    if (strncmp(line, "/recall", 7) == 0 &&
        (line[7] == ' ' || line[7] == '\0')) {
        char        slug[SPG_MEM_SLUG_MAX + 1u];
        const char *rest;
        split_first(line + 7, slug, sizeof slug, &rest);
        if (!have_mem || slug[0] == '\0') {
            puts("assistant> usage: /recall <slug>");
            return true;
        }
        static char content[SPG_MEM_BODY_MAX + 1024u];
        const enum spg_status s =
            spg_mem_read(mem, slug, sizeof content, content, nullptr);
        if (s != SPG_OK) {
            printf("assistant> (cannot recall %s: %s)\n", slug,
                   spg_status_to_string(s));
            return true;
        }
        (void)snprintf(pending, pending_cap, "[recalled memory %s]\n%s", slug,
                       content);
        printf("assistant> recalled %s (will use it in your next message)\n",
               slug);
        return true;
    }
    if (strncmp(line, "/remember", 9) == 0 &&
        (line[9] == ' ' || line[9] == '\0')) {
        char        slug[SPG_MEM_SLUG_MAX + 1u];
        const char *desc;
        split_first(line + 9, slug, sizeof slug, &desc);
        if (!have_mem || slug[0] == '\0' || desc[0] == '\0') {
            puts("assistant> usage: /remember <slug> <description>");
            return true;
        }
        if (last_reply[0] == '\0') {
            puts("assistant> (nothing to remember yet)");
            return true;
        }
        const enum spg_status s = spg_mem_save(mem, slug, desc, last_reply);
        if (s != SPG_OK) {
            printf("assistant> (cannot remember %s: %s)\n", slug,
                   spg_status_to_string(s));
        } else {
            printf("assistant> remembered %s\n", slug);
        }
        return true;
    }
    if (strncmp(line, "/forget", 7) == 0 &&
        (line[7] == ' ' || line[7] == '\0')) {
        char        slug[SPG_MEM_SLUG_MAX + 1u];
        const char *rest;
        split_first(line + 7, slug, sizeof slug, &rest);
        if (!have_mem || slug[0] == '\0') {
            puts("assistant> usage: /forget <slug>");
            return true;
        }
        const enum spg_status s = spg_mem_delete(mem, slug);
        if (s != SPG_OK) {
            printf("assistant> (cannot forget %s: %s)\n", slug,
                   spg_status_to_string(s));
        } else {
            printf("assistant> forgot %s\n", slug);
        }
        return true;
    }
    return false;
}

/* The one gate on agent_run: printed proposal, y/N from the terminal. The
 * model never sees this exchange — only "declined" or the run's exit. */
static bool agent_confirm(void *userdata, const char *config_path) {
    (void)userdata;
    printf("assistant> (agent_run wants to launch: %s)\nconfirm [y/N]> ",
           config_path);
    fflush(stdout);
    char answer[16];
    if (fgets(answer, sizeof answer, stdin) == nullptr) {
        return false;
    }
    return answer[0] == 'y' || answer[0] == 'Y';
}

/* $GEISTSHELL_BIN wins; otherwise the geistshell binary next to this one
 * (both land in the same bin/ directory), falling back to $PATH lookup being
 * someone else's problem — execl gets a plain name and fails visibly. */
static void resolve_agent_bin(const char *argv0, char *buf, const size_t cap) {
    const char *env = getenv("GEISTSHELL_BIN");
    if (env != nullptr && env[0] != '\0') {
        (void)snprintf(buf, cap, "%s", env);
        return;
    }
    const char *slash = strrchr(argv0, '/');
    if (slash != nullptr) {
        (void)snprintf(buf, cap, "%.*s/geistshell", (int)(slash - argv0),
                       argv0);
    } else {
        (void)snprintf(buf, cap, "geistshell");
    }
}

int main(int argc, char **argv) {
    struct chat_args args = {};
    const int        pa   = parse_args(argc, argv, &args);
    if (pa != 0) {
        if (pa == 2) {
            print_usage(argv[0]);
        }
        return pa == 1 ? 0 : pa;
    }

    struct spg_model_adapter adapter = {};
    if (init_adapter(&args, &adapter) != SPG_OK) {
        return 1;
    }

    struct spg_mem_store mem;
    bool                 have_mem = false;
    if (!args.fake) {
        have_mem = spg_mem_store_open(&mem, resolve_mem_dir(&args)) == SPG_OK;
        if (!have_mem) {
            fprintf(stderr, "geistshell-chat: memory disabled (cannot open %s)\n",
                    resolve_mem_dir(&args));
        }
    }

    FILE *transcript = nullptr;
    if (args.transcript_path != nullptr) {
        transcript = fopen(args.transcript_path, "ab");
        if (transcript == nullptr) {
            fprintf(stderr, "geistshell-chat: transcript open failed\n");
            spg_model_adapter_destroy(&adapter);
            return 1;
        }
    }

    struct spg_journal_writer journal      = {};
    bool                      have_journal = false;
    struct spg_journal_writer *journal_ptr = nullptr;
    if (args.journal_path != nullptr) {
        if (spg_journal_writer_open(&journal, args.journal_path) != SPG_OK) {
            fprintf(stderr, "geistshell-chat: journal open failed\n");
            if (transcript != nullptr) {
                (void)fclose(transcript);
            }
            spg_model_adapter_destroy(&adapter);
            return 1;
        }
        have_journal = true;
        journal_ptr  = &journal;
    }

    char agent_bin[PATH_MAX];
    resolve_agent_bin(argv[0], agent_bin, sizeof agent_bin);
    const struct spg_chat_agent_launcher launcher = {
        .agent_bin = agent_bin,
        .confirm   = agent_confirm,
    };

    puts("geistshell-chat ready. /quit, /reset, /memories, /recall <slug>, "
         "/remember <slug> <desc>, /forget <slug>. The model may propose an "
         "agent run; every launch asks you first.");
    char                    line[CHAT_LINE_BYTES];
    char                    output[CHAT_OUTPUT_BYTES];
    char                    history_buf[CHAT_HISTORY_BYTES];
    char                    last_reply[CHAT_OUTPUT_BYTES] = {0};
    char                    pending[4096]                 = {0};
    bool                    index_injected                = false;
    struct spg_chat_history history;
    spg_chat_history_init(&history, sizeof history_buf, history_buf);
    int rc = 0;
    for (;;) {
        fputs("user> ", stdout);
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == nullptr) {
            break;
        }
        trim_newline(line);
        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
            break;
        }
        if (strcmp(line, "/reset") == 0) {
            spg_chat_history_reset(&history);
            puts("assistant> (context cleared)");
            continue;
        }
        if (line[0] == '\0') {
            continue;
        }
        if (line[0] == '/' &&
            handle_memory_command(&mem, have_mem, line, last_reply, pending,
                                  sizeof pending)) {
            continue;
        }
        if (transcript_write(transcript, "user", line) != 0) {
            fprintf(stderr, "geistshell-chat: transcript write failed\n");
            rc = 1;
            break;
        }

        /* Real path: feed the whole templated transcript each turn (reset +
         * re-prefill), prepending the memory index (first turn) and any
         * /recall'd content. Fake path: just echo the raw line. */
        const char *prompt = line;
        bool        reset  = false;
        if (!args.fake) {
            char composed[CHAT_HISTORY_BYTES];
            composed[0] = '\0';
            if (!index_injected && have_mem) {
                static char idx[2048];
                bool        tr = false;
                (void)spg_mem_index(&mem, sizeof idx, idx, nullptr, &tr);
                (void)snprintf(
                    composed, sizeof composed,
                    "[tools] You have long-term memory. To use it, reply with "
                    "EXACTLY one s-expression and nothing else:\n"
                    "  (tool memory_list)\n"
                    "  (tool memory_read (slug \"<slug>\"))\n"
                    "  (tool memory_save (slug \"<slug>\") (description "
                    "\"<one line>\") (body \"<text>\"))\n"
                    "  (tool memory_delete (slug \"<slug>\"))\n"
                    "When the user asks about anything that might be stored "
                    "(see the index below), do NOT guess: first emit a "
                    "(tool memory_read ...) for the matching slug. The answer "
                    "returns as [tool_result]; then reply to the user using "
                    "it.\n"
                    "Example:\n"
                    "user: when is standup?\n"
                    "model: (tool memory_read (slug \"standup\"))\n"
                    "[tool_result]\nStandup is 9:30 daily.\n"
                    "model: Standup is at 9:30 every day.\n"
                    "[memory index]\n%s[/memory index]\n\n",
                    idx);
                if (args.allow_exec) {
                    const size_t o = strlen(composed);
                    (void)snprintf(
                        composed + o, sizeof composed - o,
                        "[tools+] You may also run a guarded shell command. To "
                        "do so, reply with EXACTLY (tool exec (command "
                        "\"<cmd>\")) and nothing else. The real output returns "
                        "as [tool_result]; never invent it.\n"
                        "Example:\n"
                        "user: what kernel is this?\n"
                        "model: (tool exec (command \"uname -s\"))\n"
                        "[tool_result]\nexit 0\nDarwin\n"
                        "model: This system's kernel is Darwin.\n\n");
                }
                {
                    const size_t o = strlen(composed);
                    (void)snprintf(
                        composed + o, sizeof composed - o,
                        "[tools+] You may propose a governed agent run from an "
                        "existing run config. Reply with EXACTLY (tool "
                        "agent_run (config \"<run.spg>\")) and nothing else. "
                        "The operator confirms every launch at the terminal "
                        "before anything runs; a declined launch returns as "
                        "[tool_result] and is final for this turn.\n\n");
                }
                index_injected = true;
            }
            if (pending[0] != '\0') {
                const size_t o = strlen(composed);
                (void)snprintf(composed + o, sizeof composed - o, "%s\n\n",
                               pending);
                pending[0] = '\0';
            }
            const size_t o = strlen(composed);
            (void)snprintf(composed + o, sizeof composed - o, "%s", line);
            if (spg_chat_history_add_user(&history, composed) != SPG_OK) {
                puts("assistant> (message too long for the context window)");
                continue;
            }
            prompt = spg_chat_history_text(&history);
            reset  = true;
        }

        /* Generate, then run any in-turn (tool ...) calls the model emits,
         * feeding each result back, until it answers normally (bounded). */
        struct spg_model_generate_result result  = {};
        enum spg_status                  gstatus = SPG_E_INTERNAL;
        size_t                           tool_iters = 0u;
        for (;;) {
            result = (struct spg_model_generate_result){
                .output_capacity = sizeof output,
                .output          = output,
            };
            const struct spg_model_generate_request req = {
                .prompt            = prompt,
                .prompt_n          = strlen(prompt),
                .reset_session     = reset,
                .max_decode_tokens = args.max_tokens,
            };
            gstatus = spg_model_generate(&adapter, &req, &result);
            if (gstatus != SPG_OK && gstatus != SPG_E_LIMIT) {
                break;
            }
            if (!args.fake) {
                trim_at_stop_markers(output);
            }
            if (args.fake || !have_mem || tool_iters >= CHAT_MAX_TOOL_ITERS) {
                break;
            }
            static char tool_result[CHAT_OUTPUT_BYTES];
            bool        was_tool = false;
            (void)spg_chat_tool_dispatch(&mem, journal_ptr, args.allow_exec,
                                         &launcher, strlen(output), output,
                                         sizeof tool_result, tool_result,
                                         &was_tool);
            if (!was_tool) {
                break;
            }
            printf("assistant> (tool: %.*s)\n", (int)strcspn(output, "\n"),
                   output);
            (void)spg_chat_history_add_assistant(&history, output);
            char feedback[2048];
            (void)snprintf(feedback, sizeof feedback, "[tool_result]\n%s",
                           tool_result);
            if (spg_chat_history_add_user(&history, feedback) != SPG_OK) {
                break;
            }
            prompt = spg_chat_history_text(&history);
            reset  = true;
            tool_iters += 1u;
        }

        const char *reply =
            gstatus == SPG_OK || gstatus == SPG_E_LIMIT ? output : nullptr;
        if (reply != nullptr) {
            if (!args.fake) {
                (void)spg_chat_history_add_assistant(&history, output);
                (void)snprintf(last_reply, sizeof last_reply, "%s", output);
            }
            printf("assistant> %s%s\n", reply,
                   result.output_truncated ? " …[truncated]" : "");
            if (transcript_write(transcript, "assistant", reply) != 0) {
                fprintf(stderr, "geistshell-chat: transcript write failed\n");
                rc = 1;
                break;
            }
        } else {
            printf("assistant> (generation error: %s)\n",
                   spg_status_to_string(gstatus));
        }
    }

    spg_model_adapter_destroy(&adapter);
    if (have_journal) {
        (void)spg_journal_writer_close(&journal);
    }
    if (transcript != nullptr && fclose(transcript) != 0) {
        fprintf(stderr, "geistshell-chat: transcript close failed\n");
        return 1;
    }
    return rc;
}
