#include "geistshell/memory.h"

#include <math.h>
#include <stdio.h>

static int test_add_fact(void) {
    struct spg_memory  memory = {};
    struct spg_fact_id fact   = {};
    spg_memory_init(&memory);

    if (spg_memory_add_fact(&memory, SPG_MEMORY_FACT_OBSERVATION,
                            (struct spg_text_span){.offset = 0u, .length = 4u},
                            (struct spg_text_span){.offset = 5u, .length = 2u},
                            (struct spg_text_span){.offset = 8u, .length = 3u},
                            1u, false, (struct spg_node_id){}, 0.75f,
                            &fact) != SPG_OK) {
        return 1;
    }
    if (!spg_memory_fact_valid(&memory, fact) || memory.fact_count != 1u) {
        return 1;
    }
    if (memory.facts[fact.index].status != SPG_MEMORY_FACT_ACTIVE ||
        memory.facts[fact.index].source_event_id != 1u ||
        memory.facts[fact.index].has_graph_node) {
        return 1;
    }
    return memory.facts[fact.index].confidence == 0.75f ? 0 : 1;
}

static int test_graph_provenance(void) {
    struct spg_memory  memory = {};
    struct spg_fact_id fact   = {};
    const struct spg_node_id node = {.index = 7u, .generation = 1u};
    spg_memory_init(&memory);
    if (spg_memory_add_fact(&memory, SPG_MEMORY_FACT_ENTITY,
                            (struct spg_text_span){}, (struct spg_text_span){},
                            (struct spg_text_span){}, 42u, true, node, 1.0f,
                            &fact) != SPG_OK) {
        return 1;
    }
    return memory.facts[fact.index].has_graph_node &&
                   memory.facts[fact.index].graph_node.index == 7u
               ? 0
               : 1;
}

static int test_status_and_confidence_update(void) {
    struct spg_memory  memory = {};
    struct spg_fact_id fact   = {};
    spg_memory_init(&memory);
    if (spg_memory_add_fact(&memory, SPG_MEMORY_FACT_RELATION,
                            (struct spg_text_span){}, (struct spg_text_span){},
                            (struct spg_text_span){}, 1u, false,
                            (struct spg_node_id){}, 0.5f, &fact) != SPG_OK) {
        return 1;
    }
    if (spg_memory_set_status(&memory, fact, SPG_MEMORY_FACT_SUPERSEDED) !=
        SPG_OK) {
        return 1;
    }
    if (spg_memory_set_confidence(&memory, fact, 0.25f) != SPG_OK) {
        return 1;
    }
    return memory.facts[fact.index].status == SPG_MEMORY_FACT_SUPERSEDED &&
                   memory.facts[fact.index].confidence == 0.25f
               ? 0
               : 1;
}

static int test_invalid_confidence(void) {
    struct spg_memory  memory = {};
    struct spg_fact_id fact   = {};
    spg_memory_init(&memory);
    if (spg_memory_add_fact(&memory, SPG_MEMORY_FACT_ENTITY,
                            (struct spg_text_span){}, (struct spg_text_span){},
                            (struct spg_text_span){}, 1u, false,
                            (struct spg_node_id){}, NAN, &fact) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_memory_add_fact(&memory, SPG_MEMORY_FACT_ENTITY,
                            (struct spg_text_span){}, (struct spg_text_span){},
                            (struct spg_text_span){}, 1u, false,
                            (struct spg_node_id){}, 0.5f, &fact) != SPG_OK) {
        return 1;
    }
    return spg_memory_set_confidence(&memory, fact, 1.5f) == SPG_E_INVALID_ARG
               ? 0
               : 1;
}

static int test_stale_handle(void) {
    struct spg_memory  memory = {};
    struct spg_fact_id fact   = {};
    spg_memory_init(&memory);
    if (spg_memory_add_fact(&memory, SPG_MEMORY_FACT_ARTIFACT,
                            (struct spg_text_span){}, (struct spg_text_span){},
                            (struct spg_text_span){}, 1u, false,
                            (struct spg_node_id){}, 0.5f, &fact) != SPG_OK) {
        return 1;
    }
    fact.generation += 1u;
    if (spg_memory_fact_valid(&memory, fact)) {
        return 1;
    }
    return spg_memory_set_status(&memory, fact, SPG_MEMORY_FACT_REJECTED) ==
                   SPG_E_INVALID_ARG
               ? 0
               : 1;
}

static int test_capacity(void) {
    struct spg_memory  memory = {};
    struct spg_fact_id fact   = {};
    spg_memory_init(&memory);
    for (size_t i = 0u; i < SPG_MEMORY_MAX_FACTS; i += 1u) {
        if (spg_memory_add_fact(&memory, SPG_MEMORY_FACT_CONSTRAINT,
                                (struct spg_text_span){},
                                (struct spg_text_span){},
                                (struct spg_text_span){}, 1u, false,
                                (struct spg_node_id){}, 0.0f, &fact) !=
            SPG_OK) {
            return 1;
        }
    }
    return spg_memory_add_fact(&memory, SPG_MEMORY_FACT_CONSTRAINT,
                               (struct spg_text_span){},
                               (struct spg_text_span){},
                               (struct spg_text_span){}, 1u, false,
                               (struct spg_node_id){}, 0.0f, &fact) ==
                   SPG_E_LIMIT
               ? 0
               : 1;
}

static int test_invalid_args(void) {
    struct spg_memory  memory = {};
    struct spg_fact_id fact   = {};
    spg_memory_init(&memory);
    if (spg_memory_add_fact(nullptr, SPG_MEMORY_FACT_ENTITY,
                            (struct spg_text_span){}, (struct spg_text_span){},
                            (struct spg_text_span){}, 1u, false,
                            (struct spg_node_id){}, 0.0f, &fact) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_memory_add_fact(&memory, (enum spg_memory_fact_kind)999,
                            (struct spg_text_span){}, (struct spg_text_span){},
                            (struct spg_text_span){}, 1u, false,
                            (struct spg_node_id){}, 0.0f, &fact) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    if (spg_memory_add_fact(&memory, SPG_MEMORY_FACT_ENTITY,
                            (struct spg_text_span){}, (struct spg_text_span){},
                            (struct spg_text_span){}, 0u, false,
                            (struct spg_node_id){}, 0.0f, &fact) !=
        SPG_E_INVALID_ARG) {
        return 1;
    }
    return 0;
}

int main(void) {
    if (test_add_fact() != 0) {
        fprintf(stderr, "test_add_fact failed\n");
        return 1;
    }
    if (test_graph_provenance() != 0) {
        fprintf(stderr, "test_graph_provenance failed\n");
        return 1;
    }
    if (test_status_and_confidence_update() != 0) {
        fprintf(stderr, "test_status_and_confidence_update failed\n");
        return 1;
    }
    if (test_invalid_confidence() != 0) {
        fprintf(stderr, "test_invalid_confidence failed\n");
        return 1;
    }
    if (test_stale_handle() != 0) {
        fprintf(stderr, "test_stale_handle failed\n");
        return 1;
    }
    if (test_capacity() != 0) {
        fprintf(stderr, "test_capacity failed\n");
        return 1;
    }
    if (test_invalid_args() != 0) {
        fprintf(stderr, "test_invalid_args failed\n");
        return 1;
    }
    return 0;
}
