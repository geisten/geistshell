#include "geistshell/context.h"

#include <string.h>

static uint64_t remaining(const uint64_t configured, const uint64_t consumed) {
    return consumed >= configured ? 0u : configured - consumed;
}

static struct spg_context_budget_item budget_item(const uint64_t configured,
                                                  const uint64_t consumed) {
    return (struct spg_context_budget_item){
        .configured = configured,
        .consumed   = consumed,
        .remaining  = remaining(configured, consumed),
    };
}

static void build_budgets(const struct spg_run_config    *run,
                          const struct spg_policy_usage  *usage,
                          struct spg_context_budget_view *out) {
    const struct spg_run_budgets configured =
        run == nullptr ? (struct spg_run_budgets){} : run->budgets;
    const struct spg_run_budgets consumed =
        usage == nullptr ? (struct spg_run_budgets){} : usage->consumed;
    *out = (struct spg_context_budget_view){
        .inference_steps =
            budget_item(configured.inference_steps, consumed.inference_steps),
        .tokens = budget_item(configured.tokens, consumed.tokens),
        .shell_actions =
            budget_item(configured.shell_actions, consumed.shell_actions),
        .sim_actions =
            budget_item(configured.sim_actions, consumed.sim_actions),
        .memory_actions =
            budget_item(configured.memory_actions, consumed.memory_actions),
        .wall_ms = budget_item(configured.wall_ms, consumed.wall_ms),
        .journal_bytes =
            budget_item(configured.journal_bytes, consumed.journal_bytes),
        .risk_bp = budget_item(configured.risk_bp, consumed.risk_bp),
    };
}

static uint32_t bp_from_score(const float score) {
    if (score <= 0.0f) {
        return 0u;
    }
    if (score >= 1.0f) {
        return 10000u;
    }
    return (uint32_t)(score * 10000.0f + 0.5f);
}

static uint32_t node_kind_weight(const enum spg_graph_node_kind kind) {
    switch (kind) {
    case SPG_GRAPH_NODE_GOAL:
        return 9000u;
    case SPG_GRAPH_NODE_PLAN:
        return 8500u;
    case SPG_GRAPH_NODE_ACTION:
        return 8000u;
    case SPG_GRAPH_NODE_POLICY_DECISION:
        return 7600u;
    case SPG_GRAPH_NODE_RESULT:
        return 7200u;
    case SPG_GRAPH_NODE_MEMORY:
        return 6900u;
    case SPG_GRAPH_NODE_HYPOTHESIS:
        return 6500u;
    case SPG_GRAPH_NODE_OBSERVATION:
        return 6000u;
    case SPG_GRAPH_NODE_EVAL_MARK:
        return 5400u;
    }
    return 0u;
}

static uint32_t graph_rank(const struct spg_graph_node *node) {
    uint32_t rank = node_kind_weight(node->kind);
    if ((node->flags & SPG_GRAPH_NODE_OPEN) != 0u) {
        rank += 5000u;
    }
    if ((node->flags & SPG_GRAPH_NODE_BLOCKED) != 0u) {
        rank += 1200u;
    }
    if ((node->flags & SPG_GRAPH_NODE_CLOSED) != 0u && rank >= 2500u) {
        rank -= 2500u;
    }
    rank += bp_from_score(node->scores.confidence) / 2u;
    rank += bp_from_score(node->scores.utility);
    rank += bp_from_score(node->scores.novelty) / 2u;
    const uint32_t risk = bp_from_score(node->scores.risk);
    rank += risk >= 10000u ? 0u : (10000u - risk) / 3u;
    const uint32_t cost = bp_from_score(node->scores.cost_estimate);
    rank += cost >= 10000u ? 0u : (10000u - cost) / 4u;
    return rank;
}

static bool graph_ref_before(const struct spg_context_graph_ref *left,
                             const struct spg_context_graph_ref *right) {
    if (left->rank != right->rank) {
        return left->rank > right->rank;
    }
    return left->node.index < right->node.index;
}

static void insert_graph_ref(struct spg_context_view           *view,
                             const struct spg_context_graph_ref ref) {
    if (view->graph_ref_capacity == 0u) {
        view->graph_truncated = true;
        return;
    }
    size_t pos = view->graph_ref_count;
    if (pos == view->graph_ref_capacity) {
        const struct spg_context_graph_ref *last = &view->graph_refs[pos - 1u];
        if (!graph_ref_before(&ref, last)) {
            view->graph_truncated = true;
            return;
        }
        pos                   = view->graph_ref_capacity - 1u;
        view->graph_truncated = true;
    } else {
        view->graph_ref_count += 1u;
    }
    while (pos > 0u && graph_ref_before(&ref, &view->graph_refs[pos - 1u])) {
        view->graph_refs[pos] = view->graph_refs[pos - 1u];
        pos -= 1u;
    }
    view->graph_refs[pos] = ref;
}

static uint32_t fact_kind_weight(const enum spg_memory_fact_kind kind) {
    switch (kind) {
    case SPG_MEMORY_FACT_CONSTRAINT:
        return 9000u;
    case SPG_MEMORY_FACT_ARTIFACT:
        return 8200u;
    case SPG_MEMORY_FACT_RELATION:
        return 7800u;
    case SPG_MEMORY_FACT_ENTITY:
        return 7200u;
    case SPG_MEMORY_FACT_OBSERVATION:
        return 6800u;
    }
    return 0u;
}

static uint32_t memory_rank(const struct spg_memory_fact *fact) {
    uint32_t rank = fact_kind_weight(fact->kind);
    if (fact->status == SPG_MEMORY_FACT_ACTIVE) {
        rank += 6000u;
    } else if (fact->status == SPG_MEMORY_FACT_SUPERSEDED) {
        rank += 1200u;
    }
    rank += bp_from_score(fact->confidence);
    if (fact->has_graph_node) {
        rank += 1000u;
    }
    if (fact->source_event_id > UINT64_C(1000000)) {
        rank += 1000u;
    } else {
        rank += (uint32_t)(fact->source_event_id / UINT64_C(1000));
    }
    return rank;
}

static bool memory_ref_before(const struct spg_context_memory_ref *left,
                              const struct spg_context_memory_ref *right) {
    if (left->rank != right->rank) {
        return left->rank > right->rank;
    }
    return left->fact.index < right->fact.index;
}

static void insert_memory_ref(struct spg_context_view            *view,
                              const struct spg_context_memory_ref ref) {
    if (view->memory_ref_capacity == 0u) {
        view->memory_truncated = true;
        return;
    }
    size_t pos = view->memory_ref_count;
    if (pos == view->memory_ref_capacity) {
        const struct spg_context_memory_ref *last =
            &view->memory_refs[pos - 1u];
        if (!memory_ref_before(&ref, last)) {
            view->memory_truncated = true;
            return;
        }
        pos                    = view->memory_ref_capacity - 1u;
        view->memory_truncated = true;
    } else {
        view->memory_ref_count += 1u;
    }
    while (pos > 0u && memory_ref_before(&ref, &view->memory_refs[pos - 1u])) {
        view->memory_refs[pos] = view->memory_refs[pos - 1u];
        pos -= 1u;
    }
    view->memory_refs[pos] = ref;
}

static bool journal_ref_before(const struct spg_context_journal_ref *left,
                               const struct spg_context_journal_ref *right) {
    if (left->sequence != right->sequence) {
        return left->sequence > right->sequence;
    }
    return left->source_index < right->source_index;
}

static void
insert_recent_journal_ref(struct spg_context_view             *view,
                          const struct spg_context_journal_ref ref) {
    if (view->journal_ref_capacity == 0u) {
        view->journal_truncated = true;
        return;
    }
    size_t pos = view->journal_ref_count;
    if (pos == view->journal_ref_capacity) {
        const struct spg_context_journal_ref *oldest_recent =
            &view->journal_refs[pos - 1u];
        if (!journal_ref_before(&ref, oldest_recent)) {
            view->journal_truncated = true;
            return;
        }
        pos                     = view->journal_ref_capacity - 1u;
        view->journal_truncated = true;
    } else {
        view->journal_ref_count += 1u;
    }
    while (pos > 0u &&
           journal_ref_before(&ref, &view->journal_refs[pos - 1u])) {
        view->journal_refs[pos] = view->journal_refs[pos - 1u];
        pos -= 1u;
    }
    view->journal_refs[pos] = ref;
}

static void reverse_journal_refs(struct spg_context_view *view) {
    for (size_t i = 0u, j = view->journal_ref_count; i < j / 2u; i += 1u) {
        const size_t                         other = j - 1u - i;
        const struct spg_context_journal_ref tmp   = view->journal_refs[i];
        view->journal_refs[i]                      = view->journal_refs[other];
        view->journal_refs[other]                  = tmp;
    }
}

void spg_context_view_init(
    struct spg_context_view *view, const size_t graph_ref_capacity,
    struct spg_context_graph_ref   graph_refs[static graph_ref_capacity],
    const size_t                   memory_ref_capacity,
    struct spg_context_memory_ref  memory_refs[static memory_ref_capacity],
    const size_t                   journal_ref_capacity,
    struct spg_context_journal_ref journal_refs[static journal_ref_capacity]) {
    if (view == nullptr) {
        return;
    }
    *view = (struct spg_context_view){
        .graph_ref_capacity   = graph_ref_capacity,
        .graph_refs           = graph_refs,
        .memory_ref_capacity  = memory_ref_capacity,
        .memory_refs          = memory_refs,
        .journal_ref_capacity = journal_ref_capacity,
        .journal_refs         = journal_refs,
    };
}

static bool view_arrays_valid(const struct spg_context_view *view) {
    return (view->graph_ref_capacity == 0u || view->graph_refs != nullptr) &&
           (view->memory_ref_capacity == 0u || view->memory_refs != nullptr) &&
           (view->journal_ref_capacity == 0u || view->journal_refs != nullptr);
}

enum spg_status spg_context_build(const struct spg_context_sources *sources,
                                  const struct spg_context_limits  *limits,
                                  struct spg_context_view          *view) {
    if (sources == nullptr || limits == nullptr || view == nullptr ||
        !view_arrays_valid(view)) {
        return SPG_E_INVALID_ARG;
    }
    if (sources->journal_header_count > 0u &&
        sources->journal_headers == nullptr) {
        return SPG_E_INVALID_ARG;
    }

    view->graph_ref_count   = 0u;
    view->memory_ref_count  = 0u;
    view->journal_ref_count = 0u;
    view->graph_truncated   = false;
    view->memory_truncated  = false;
    view->journal_truncated = false;
    build_budgets(sources->run, sources->usage, &view->budgets);

    if (sources->graph != nullptr) {
        const size_t target = limits->graph_nodes < view->graph_ref_capacity
                                  ? limits->graph_nodes
                                  : view->graph_ref_capacity;
        const size_t old_capacity = view->graph_ref_capacity;
        view->graph_ref_capacity  = target;
        for (size_t i = 0u; i < sources->graph->node_count; i += 1u) {
            const struct spg_graph_node *node = &sources->graph->nodes[i];
            if (!node->live) {
                continue;
            }
            insert_graph_ref(view, (struct spg_context_graph_ref){
                                       .node = node->id,
                                       .rank = graph_rank(node),
                                   });
        }
        view->graph_ref_capacity = old_capacity;
    }

    if (sources->memory != nullptr) {
        const size_t target = limits->memory_facts < view->memory_ref_capacity
                                  ? limits->memory_facts
                                  : view->memory_ref_capacity;
        const size_t old_capacity = view->memory_ref_capacity;
        view->memory_ref_capacity = target;
        for (size_t i = 0u; i < sources->memory->fact_count; i += 1u) {
            const struct spg_memory_fact *fact = &sources->memory->facts[i];
            if (!fact->live || fact->status == SPG_MEMORY_FACT_REJECTED) {
                continue;
            }
            insert_memory_ref(view, (struct spg_context_memory_ref){
                                        .fact = fact->id,
                                        .rank = memory_rank(fact),
                                    });
        }
        view->memory_ref_capacity = old_capacity;
    }

    {
        const size_t target =
            limits->journal_events < view->journal_ref_capacity
                ? limits->journal_events
                : view->journal_ref_capacity;
        const size_t old_capacity  = view->journal_ref_capacity;
        view->journal_ref_capacity = target;
        for (size_t i = 0u; i < sources->journal_header_count; i += 1u) {
            const struct spg_journal_record_header *header =
                &sources->journal_headers[i];
            insert_recent_journal_ref(
                view, (struct spg_context_journal_ref){
                          .sequence        = header->sequence,
                          .parent_sequence = header->parent_sequence,
                          .payload_bytes   = header->payload_bytes,
                          .event_kind      = header->event_kind,
                          .status          = header->status,
                          .source_index    = i,
                      });
        }
        reverse_journal_refs(view);
        view->journal_ref_capacity = old_capacity;
    }

    return SPG_OK;
}

struct render_state {
    size_t capacity;
    char  *dst;
    size_t used;
    size_t required;
};

static void append_char(struct render_state *state, const char ch) {
    if (state->used + 1u < state->capacity) {
        state->dst[state->used] = ch;
        state->used += 1u;
    }
    state->required += 1u;
}

/* Block append with the same one-byte-reserved invariant as append_char: never
 * fills the last slot (it is kept for the terminating NUL) and always counts
 * the full length into required so the caller can size dst exactly. */
static void append_span(struct render_state *state, const size_t n,
                        const char bytes[static n]) {
    if (state->used + 1u < state->capacity) {
        const size_t room = state->capacity - 1u - state->used;
        const size_t take = n < room ? n : room;
        memcpy(state->dst + state->used, bytes, take);
        state->used += take;
    }
    state->required += n;
}

static void append_cstr(struct render_state *state, const char *text) {
    if (text == nullptr) {
        return;
    }
    append_span(state, strlen(text), text);
}

/* Decimal-format an unsigned without snprintf's format parsing: fill a stack
 * buffer back-to-front, then block-append the digit run. */
static void append_u64(struct render_state *state, uint64_t value) {
    char   buffer[sizeof(uint64_t) * 3u + 1u];
    size_t i = sizeof buffer;
    do {
        buffer[--i] = (char)('0' + (unsigned)(value % 10u));
        value /= 10u;
    } while (value != 0u);
    append_span(state, sizeof buffer - i, buffer + i);
}

static void append_u32(struct render_state *state, const uint32_t value) {
    append_u64(state, value);
}

static const char *graph_kind_name(const enum spg_graph_node_kind kind) {
    switch (kind) {
    case SPG_GRAPH_NODE_OBSERVATION:
        return "observation";
    case SPG_GRAPH_NODE_HYPOTHESIS:
        return "hypothesis";
    case SPG_GRAPH_NODE_GOAL:
        return "goal";
    case SPG_GRAPH_NODE_PLAN:
        return "plan";
    case SPG_GRAPH_NODE_ACTION:
        return "action";
    case SPG_GRAPH_NODE_RESULT:
        return "result";
    case SPG_GRAPH_NODE_MEMORY:
        return "memory";
    case SPG_GRAPH_NODE_POLICY_DECISION:
        return "policy_decision";
    case SPG_GRAPH_NODE_EVAL_MARK:
        return "eval_mark";
    }
    return "unknown";
}

static const char *fact_kind_name(const enum spg_memory_fact_kind kind) {
    switch (kind) {
    case SPG_MEMORY_FACT_ENTITY:
        return "entity";
    case SPG_MEMORY_FACT_RELATION:
        return "relation";
    case SPG_MEMORY_FACT_OBSERVATION:
        return "observation";
    case SPG_MEMORY_FACT_CONSTRAINT:
        return "constraint";
    case SPG_MEMORY_FACT_ARTIFACT:
        return "artifact";
    }
    return "unknown";
}

static const char *fact_status_name(const enum spg_memory_fact_status status) {
    switch (status) {
    case SPG_MEMORY_FACT_ACTIVE:
        return "active";
    case SPG_MEMORY_FACT_SUPERSEDED:
        return "superseded";
    case SPG_MEMORY_FACT_REJECTED:
        return "rejected";
    }
    return "unknown";
}

static void append_quoted_span(struct render_state *state, const size_t text_n,
                               const char                *text,
                               const struct spg_text_span span) {
    append_char(state, '"');
    if (text != nullptr && span.offset <= text_n &&
        span.length <= text_n - span.offset) {
        const size_t end = span.offset + span.length;
        for (size_t i = span.offset; i < end; i += 1u) {
            const char ch = text[i];
            if (ch == '"' || ch == '\\') {
                append_char(state, '\\');
                append_char(state, ch);
            } else if (ch == '\n') {
                append_cstr(state, "\\n");
            } else if (ch == '\r') {
                append_cstr(state, "\\r");
            } else if (ch == '\t') {
                append_cstr(state, "\\t");
            } else {
                append_char(state, ch);
            }
        }
    } else {
        append_cstr(state, "@");
        append_u64(state, (uint64_t)span.offset);
        append_cstr(state, ":");
        append_u64(state, (uint64_t)span.length);
    }
    append_char(state, '"');
}

static void append_quoted_cstr(struct render_state *state, const char *text) {
    append_char(state, '"');
    for (size_t i = 0u; text[i] != '\0'; i += 1u) {
        const char ch = text[i];
        if (ch == '"' || ch == '\\') {
            append_char(state, '\\');
            append_char(state, ch);
        } else if (ch == '\n') {
            append_cstr(state, "\\n");
        } else if (ch == '\r') {
            append_cstr(state, "\\r");
        } else if (ch == '\t') {
            append_cstr(state, "\\t");
        } else {
            append_char(state, ch);
        }
    }
    append_char(state, '"');
}

static void append_budget_line(struct render_state *state, const char *name,
                               const struct spg_context_budget_item item) {
    append_cstr(state, "  (");
    append_cstr(state, name);
    append_cstr(state, " (configured ");
    append_u64(state, item.configured);
    append_cstr(state, ") (consumed ");
    append_u64(state, item.consumed);
    append_cstr(state, ") (remaining ");
    append_u64(state, item.remaining);
    append_cstr(state, "))\n");
}

static void render_contract(struct render_state *state) {
    append_cstr(state, "(contract\n");
    append_cstr(state,
                "  (role \"Return exactly one recommendation form.\")\n");
    append_cstr(
        state, "  (schema \"(recommend (kind "
               "<simulator|local_shell|ssh_auth_probe|memory_save|memory_"
               "delete|memory_read>) (capability \\\"<policy capability>\\\") "
               "(cost <positive integer>) (uses_network <true|false>) "
               "(confidence_bp <0..10000>) (reason \\\"<short reason>\\\") "
               "[(command \\\"...\\\")|(target \\\"...\\\")|(slug \\\"...\\\") "
               "(description \\\"...\\\") (body \\\"...\\\")])\")\n");
    append_cstr(state, "  (rules\n");
    append_cstr(state, "    (simulator \"uses_network must be false and no "
                       "command or target\")\n");
    append_cstr(state, "    (local_shell \"uses_network must be false and "
                       "command is only a recommendation\")\n");
    append_cstr(state, "    (ssh_auth_probe \"uses_network must be true and "
                       "target is required; no attack is executed here\")\n");
    append_cstr(state,
                "    (memory_save \"uses_network false; provide slug, "
                "description and body to remember something durably\")\n");
    append_cstr(state, "    (memory_delete \"uses_network false; provide slug "
                       "to forget a memory\")\n");
    append_cstr(state,
                "    (memory_read \"uses_network false; provide slug to recall "
                "a memory; its content arrives next turn as observation\"))\n");
    append_cstr(state, "  (memory \"(memory_index ...) lists recallable "
                       "memories by slug; (observation ...) holds the last "
                       "tool result -- recalled memory or command output\")\n");
    append_cstr(state, ")\n");
}

static void render_budgets(struct render_state                  *state,
                           const struct spg_context_budget_view *budgets) {
    append_cstr(state, "(budgets\n");
    append_budget_line(state, "inference_steps", budgets->inference_steps);
    append_budget_line(state, "tokens", budgets->tokens);
    append_budget_line(state, "shell_actions", budgets->shell_actions);
    append_budget_line(state, "sim_actions", budgets->sim_actions);
    append_budget_line(state, "memory_actions", budgets->memory_actions);
    append_budget_line(state, "wall_ms", budgets->wall_ms);
    append_budget_line(state, "journal_bytes", budgets->journal_bytes);
    append_budget_line(state, "risk_bp", budgets->risk_bp);
    append_cstr(state, ")\n");
}

static void render_graph(const struct spg_context_sources *sources,
                         const struct spg_context_view    *view,
                         struct render_state              *state) {
    append_cstr(state, "(graph");
    if (view->graph_truncated) {
        append_cstr(state, " (truncated true)");
    }
    append_char(state, '\n');
    if (sources->graph != nullptr) {
        for (size_t i = 0u; i < view->graph_ref_count; i += 1u) {
            const struct spg_context_graph_ref *ref = &view->graph_refs[i];
            if (!spg_graph_node_valid(sources->graph, ref->node)) {
                continue;
            }
            const struct spg_graph_node *node =
                &sources->graph->nodes[ref->node.index];
            append_cstr(state, "  (node (index ");
            append_u32(state, node->id.index);
            append_cstr(state, ") (kind ");
            append_cstr(state, graph_kind_name(node->kind));
            append_cstr(state, ") (rank ");
            append_u32(state, ref->rank);
            append_cstr(state, ") (flags ");
            append_u32(state, node->flags);
            append_cstr(state, ") (actor ");
            append_u32(state, node->actor_id);
            append_cstr(state, ") (text ");
            append_quoted_span(state, sources->graph_text_n,
                               sources->graph_text, node->payload);
            append_cstr(state, "))\n");
        }
    }
    append_cstr(state, ")\n");
}

static void render_memory(const struct spg_context_sources *sources,
                          const struct spg_context_view    *view,
                          struct render_state              *state) {
    append_cstr(state, "(memory");
    if (view->memory_truncated) {
        append_cstr(state, " (truncated true)");
    }
    append_char(state, '\n');
    if (sources->memory != nullptr) {
        for (size_t i = 0u; i < view->memory_ref_count; i += 1u) {
            const struct spg_context_memory_ref *ref = &view->memory_refs[i];
            if (!spg_memory_fact_valid(sources->memory, ref->fact)) {
                continue;
            }
            const struct spg_memory_fact *fact =
                &sources->memory->facts[ref->fact.index];
            append_cstr(state, "  (fact (index ");
            append_u32(state, fact->id.index);
            append_cstr(state, ") (kind ");
            append_cstr(state, fact_kind_name(fact->kind));
            append_cstr(state, ") (status ");
            append_cstr(state, fact_status_name(fact->status));
            append_cstr(state, ") (rank ");
            append_u32(state, ref->rank);
            append_cstr(state, ") (source_event ");
            append_u64(state, fact->source_event_id);
            append_cstr(state, ") (subject ");
            append_quoted_span(state, sources->memory_text_n,
                               sources->memory_text, fact->subject);
            append_cstr(state, ") (predicate ");
            append_quoted_span(state, sources->memory_text_n,
                               sources->memory_text, fact->predicate);
            append_cstr(state, ") (object ");
            append_quoted_span(state, sources->memory_text_n,
                               sources->memory_text, fact->object);
            append_cstr(state, "))\n");
        }
    }
    append_cstr(state, ")\n");
}

static void render_journal(const struct spg_context_view *view,
                           struct render_state           *state) {
    append_cstr(state, "(recent_events");
    if (view->journal_truncated) {
        append_cstr(state, " (truncated true)");
    }
    append_char(state, '\n');
    for (size_t i = 0u; i < view->journal_ref_count; i += 1u) {
        const struct spg_context_journal_ref *ref = &view->journal_refs[i];
        append_cstr(state, "  (event (sequence ");
        append_u64(state, ref->sequence);
        append_cstr(state, ") (parent ");
        append_u64(state, ref->parent_sequence);
        append_cstr(state, ") (kind ");
        append_u32(state, ref->event_kind);
        append_cstr(state, ") (status ");
        append_u32(state, ref->status);
        append_cstr(state, ") (payload_bytes ");
        append_u64(state, ref->payload_bytes);
        append_cstr(state, "))\n");
    }
    append_cstr(state, ")\n");
}

/* Long-term memory index: a readable block of one hook per line so the model
 * knows what it can recall. The raw lines are emitted as-is (this is prompt
 * text, not re-parsed). */
static void render_memory_index(const struct spg_context_sources *sources,
                                struct render_state              *state) {
    if (sources->memory_index == nullptr || sources->memory_index[0] == '\0') {
        return;
    }
    append_cstr(state, "(memory_index\n");
    append_cstr(state, sources->memory_index);
    append_cstr(state, ")\n");
}

/* Content of the most recently recalled memory, as a quoted string. */
static void render_observation(const struct spg_context_sources *sources,
                               struct render_state              *state) {
    if (sources->observation == nullptr || sources->observation[0] == '\0') {
        return;
    }
    append_cstr(state, "(observation ");
    append_quoted_cstr(state, sources->observation);
    append_cstr(state, ")\n");
}

/* The machine block is produced by the single renderer that owns its format
 * (spg_machine_state_render), then appended — one definition of the schema,
 * not a second copy here. The bound is a compile-time constant, so the stack
 * buffer can never truncate. */
static void render_machine(const struct spg_context_sources *sources,
                           struct render_state              *state) {
    /* Goal first: what the run is for, then what it is looking at. A model
     * that reads the constraints after the numbers has to re-read the
     * numbers. */
    if (sources->machine_goal != nullptr && sources->machine_goal->present) {
        char   goal_block[512];
        size_t goal_n = 0u;
        if (spg_machine_goal_render(sources->machine_goal, sizeof goal_block,
                                    goal_block, &goal_n) == SPG_OK) {
            append_cstr(state, goal_block);
            append_cstr(state, "\n");
        }
    }
    /* Not an early return: a plant run needs no host telemetry at all, and
     * skipping the block below with it would leave that agent blind. */
    if (sources->machine != nullptr) {
        char   block[SPG_MACHINE_RENDER_CAP];
        size_t required = 0u;
        if (spg_machine_state_render_masked(sources->machine,
                                            sources->machine_ablate,
                                            sizeof block, block,
                                            &required) == SPG_OK) {
            /* A failure cannot happen at this capacity; emitting nothing beats
             * emitting a truncated form the model would misread. */
            append_cstr(state, block);
            append_cstr(state, "\n");
        }
    }

    /* The plant after the host: the machine geistshell runs ON, then the
     * machine it is driving. Rendered by the module that owns the format, for
     * the same reason as the block above — one definition, not a second copy
     * here. */
    if (sources->device_state == nullptr || sources->device_state->n == 0u) {
        return;
    }
    char   plant[SPG_DEVICE_RENDER_CAP];
    size_t plant_required = 0u;
    if (spg_device_state_render(sources->device_state, sizeof plant, plant,
                                &plant_required) != SPG_OK) {
        return; /* cannot happen at this capacity; nothing beats a truncated
                 * form the model would misread as a complete plant */
    }
    append_cstr(state, plant);
    append_cstr(state, "\n");
}

enum spg_status spg_context_render(const struct spg_context_sources *sources,
                                   const struct spg_context_view    *view,
                                   const size_t dst_capacity,
                                   char         dst[static dst_capacity],
                                   size_t      *out_required) {
    if (sources == nullptr || view == nullptr || dst == nullptr ||
        out_required == nullptr || dst_capacity == 0u ||
        view->graph_ref_count > view->graph_ref_capacity ||
        view->memory_ref_count > view->memory_ref_capacity ||
        view->journal_ref_count > view->journal_ref_capacity ||
        (view->graph_ref_count > 0u && view->graph_refs == nullptr) ||
        (view->memory_ref_count > 0u && view->memory_refs == nullptr) ||
        (view->journal_ref_count > 0u && view->journal_refs == nullptr)) {
        return SPG_E_INVALID_ARG;
    }
    struct render_state state = {
        .capacity = dst_capacity,
        .dst      = dst,
    };
    render_contract(&state);
    if (sources->directive != nullptr && sources->directive[0] != '\0') {
        append_cstr(&state, "(directive ");
        append_quoted_cstr(&state, sources->directive);
        append_cstr(&state, ")\n");
    }
    if (sources->goal != nullptr && sources->goal[0] != '\0') {
        append_cstr(&state, "(goal ");
        append_quoted_cstr(&state, sources->goal);
        append_cstr(&state, ")\n");
    }
    if (sources->exemplars != nullptr && sources->exemplars[0] != '\0') {
        append_cstr(&state, "(examples\n");
        append_cstr(&state, sources->exemplars);
        append_cstr(&state, ")\n");
    }
    render_budgets(&state, &view->budgets);
    render_machine(sources, &state);
    render_graph(sources, view, &state);
    render_memory(sources, view, &state);
    render_memory_index(sources, &state);
    render_observation(sources, &state);
    render_journal(view, &state);

    if (state.used < state.capacity) {
        state.dst[state.used] = '\0';
    } else {
        state.dst[state.capacity - 1u] = '\0';
    }
    *out_required = state.required + 1u;
    return *out_required <= dst_capacity ? SPG_OK : SPG_E_LIMIT;
}
