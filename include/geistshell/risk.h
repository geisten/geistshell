#ifndef GEISTSHELL_RISK_H
#define GEISTSHELL_RISK_H

#include "geistshell/sim_config.h"
#include "geistshell/status.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spg_risk_score {
    uint64_t asset_component;
    uint64_t exposure_component;
    uint64_t vulnerability_component;
    uint64_t credential_component;
    uint64_t reachability_component;
    uint64_t total;
};

[[nodiscard]] enum spg_status
spg_risk_evaluate(const struct spg_sim_config *sim,
                  struct spg_risk_score *out);

#ifdef __cplusplus
}
#endif

#endif
