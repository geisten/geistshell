#include "geistshell/memory.h"

#include <math.h>

static bool kind_valid(const enum spg_memory_fact_kind kind) {
    return kind == SPG_MEMORY_FACT_ENTITY ||
           kind == SPG_MEMORY_FACT_RELATION ||
           kind == SPG_MEMORY_FACT_OBSERVATION ||
           kind == SPG_MEMORY_FACT_CONSTRAINT ||
           kind == SPG_MEMORY_FACT_ARTIFACT;
}

static bool status_valid(const enum spg_memory_fact_status status) {
    return status == SPG_MEMORY_FACT_ACTIVE ||
           status == SPG_MEMORY_FACT_SUPERSEDED ||
           status == SPG_MEMORY_FACT_REJECTED;
}

static bool confidence_valid(const float confidence) {
    return isfinite(confidence) && confidence >= 0.0f && confidence <= 1.0f;
}

void spg_memory_init(struct spg_memory *memory) {
    if (memory == nullptr) {
        return;
    }
    *memory = (struct spg_memory){};
}

bool spg_memory_fact_valid(const struct spg_memory *memory,
                           const struct spg_fact_id fact) {
    if (memory == nullptr || fact.index >= memory->fact_count) {
        return false;
    }
    const struct spg_memory_fact *slot = &memory->facts[fact.index];
    return slot->live && slot->id.generation == fact.generation;
}

enum spg_status spg_memory_add_fact(
    struct spg_memory *memory, const enum spg_memory_fact_kind kind,
    const struct spg_text_span subject, const struct spg_text_span predicate,
    const struct spg_text_span object, const uint64_t source_event_id,
    const bool has_graph_node, const struct spg_node_id graph_node,
    const float confidence, struct spg_fact_id *out) {
    if (memory == nullptr || out == nullptr || !kind_valid(kind) ||
        !confidence_valid(confidence) || source_event_id == 0u) {
        return SPG_E_INVALID_ARG;
    }
    if (has_graph_node && graph_node.generation == 0u) {
        return SPG_E_INVALID_ARG;
    }
    if (memory->fact_count >= SPG_MEMORY_MAX_FACTS) {
        return SPG_E_LIMIT;
    }

    const uint32_t index = (uint32_t)memory->fact_count;
    const struct spg_fact_id id = {
        .index      = index,
        .generation = 1u,
    };
    memory->facts[index] = (struct spg_memory_fact){
        .id              = id,
        .kind            = kind,
        .status          = SPG_MEMORY_FACT_ACTIVE,
        .subject         = subject,
        .predicate       = predicate,
        .object          = object,
        .source_event_id = source_event_id,
        .graph_node      = graph_node,
        .has_graph_node  = has_graph_node,
        .confidence      = confidence,
        .live            = true,
    };
    memory->fact_count += 1u;
    *out = id;
    return SPG_OK;
}

enum spg_status spg_memory_set_status(
    struct spg_memory *memory, const struct spg_fact_id fact,
    const enum spg_memory_fact_status status) {
    if (!spg_memory_fact_valid(memory, fact) || !status_valid(status)) {
        return SPG_E_INVALID_ARG;
    }
    memory->facts[fact.index].status = status;
    return SPG_OK;
}

enum spg_status spg_memory_set_confidence(struct spg_memory *memory,
                                          const struct spg_fact_id fact,
                                          const float confidence) {
    if (!spg_memory_fact_valid(memory, fact) ||
        !confidence_valid(confidence)) {
        return SPG_E_INVALID_ARG;
    }
    memory->facts[fact.index].confidence = confidence;
    return SPG_OK;
}
