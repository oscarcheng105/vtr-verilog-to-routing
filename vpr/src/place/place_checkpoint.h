#ifndef PLACE_CHECKPOINT_H
#define PLACE_CHECKPOINT_H

#include "vtr_util.h"
#include "vpr_types.h"
#include "vtr_vector_map.h"
#include "place_util.h"
#include "globals.h"
#include "timing_info.h"

//Placement checkpoint
/**
 * @brief Data structure that stores the placement state and saves it as a checkpoint.
 *
 * The placement checkpoints are very useful to solve the problem of critical 
 * delay oscillations, expecially very late in the annealer. 
 *
 *   @param cost The weighted average of the wiring cost and the timing cost.
 *   @param block_locs saves the location of each block
 *   @param cpd Saves the critical path delay of the current checkpoint
 *   @param valid a flag to show whether the current checkpoint is initialized or not
 */
class t_placement_checkpoint {
  private:
    vtr::vector_map<ClusterBlockId, t_block_loc> block_locs;
    float cpd;
    float sTNS;
    float sWNS;
    bool valid = false;
    t_placer_costs costs;

  public:
    void save_placement(const t_placer_costs& costs, const float& cpd);
    t_placer_costs restore_placement();

    float get_cp_cpd();
    double get_cp_bb_cost();
    bool cp_is_valid();
};

void save_placement_checkpoint_if_needed(t_placement_checkpoint& placement_checkpoint, std::shared_ptr<SetupTimingInfo> timing_info, t_placer_costs& costs, float CPD); 
#endif
