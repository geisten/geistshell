#ifndef GEISTSHELL_MEMORY_H
#define GEISTSHELL_MEMORY_H

#include "geistshell/graph.h"
#include "geistshell/sexpr.h"
#include "geistshell/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPG_MEMORY_MAX_FACTS 1024u

struct spg_fact_id {
    uint32_t index;
    uint32_t generation;
};

enum spg_memory_fact_kind {
    SPG_MEMORY_FACT_ENTITY = 0,
    SPG_MEMORY_FACT_RELATION,
    SPG_MEMORY_FACT_OBSERVATION,
    SPG_MEMORY_FACT_CONSTRAINT,
    SPG_MEMORY_FACT_ARTIFACT,
};

enum spg_memory_fact_status {
    SPG_MEMORY_FACT_ACTIVE = 0,
    SPG_MEMORY_FACT_SUPERSEDED,
    SPG_MEMORY_FACT_REJECTED,
};

struct spg_memory_fact {
    struct spg_fact_id            id;
    enum spg_memory_fact_kind     kind;
    enum spg_memory_fact_status   status;
    struct spg_text_span          subject;
    struct spg_text_span          predicate;
    struct spg_text_span          object;
    uint64_t                      source_event_id;
    struct spg_node_id            graph_node;
    bool                          has_graph_node;
    float                         confidence;
    bool                          live;
};

struct spg_memory {
    size_t                 fact_count;
    struct spg_memory_fact facts[SPG_MEMORY_MAX_FACTS];
};

void spg_memory_init(struct spg_memory *memory);

[[nodiscard]] enum spg_status spg_memory_add_fact(
    struct spg_memory *memory, enum spg_memory_fact_kind kind,
    struct spg_text_span subject, struct spg_text_span predicate,
    struct spg_text_span object, uint64_t source_event_id,
    bool has_graph_node, struct spg_node_id graph_node, float confidence,
    struct spg_fact_id *out);

[[nodiscard]] enum spg_status
spg_memory_set_status(struct spg_memory *memory, struct spg_fact_id fact,
                      enum spg_memory_fact_status status);

[[nodiscard]] enum spg_status
spg_memory_set_confidence(struct spg_memory *memory, struct spg_fact_id fact,
                          float confidence);

[[nodiscard]] bool spg_memory_fact_valid(const struct spg_memory *memory,
                                         struct spg_fact_id fact);

#ifdef __cplusplus
}
#endif

#endif
