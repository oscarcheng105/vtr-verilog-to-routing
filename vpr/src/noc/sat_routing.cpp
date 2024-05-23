
#include "sat_routing.h"
#include "turn_model_routing.h"
#include "move_utils.h"

#include "globals.h"
#include "vtr_time.h"

#include <unordered_map>

#include "ortools/sat/cp_model.h"
#include "ortools/sat/cp_model.pb.h"
#include "ortools/sat/cp_model_solver.h"

namespace orsat = operations_research::sat;

/**
 * For each traffic flow and NoC link pair, we create a boolean variable.
 * When a variable associated with traffic flow t and NoC link l is set,
 * it means that t is routed through l.
 */
typedef std::unordered_map<std::pair<NocTrafficFlowId, NocLinkId>, orsat::BoolVar> t_flow_link_var_map;


/**
 * @brief  Creates a boolean variable for each (traffic flow, link) pair.
 * It also create integer variables for latency-constrained traffic flows.
 *
 * @param cp_model The CP model builder object. Created variables are added
 * to this model builder object.
 * @param flow_link_vars The created boolean variables for (traffic flow, link) pairs
 * are stored in this container to be used for adding constraints and defining objective
 * functions.
 * @param latency_overrun_vars The created integer variables for latency-constrained
 * traffic flows are added to this container for future use, e.g. adding constraints
 * and defining objective functions.
 */
static void create_flow_link_vars(orsat::CpModelBuilder& cp_model,
                                  t_flow_link_var_map& flow_link_vars,
                                  std::map<NocTrafficFlowId , orsat::IntVar>& latency_overrun_vars);

/**
 * @brief Translates a latency constraint for a traffic flow to the maximum number
 * of links that the traffic flow can traverse without violating the latency constraint.
 *
 * @param traffic_flow_id The unique ID of the latency-constrained traffic flow.
 * @return The maximum number links that can travelled by the given traffic flow without
 * violating the latency constraint.
 */
static int comp_max_number_of_traversed_links(NocTrafficFlowId traffic_flow_id);

/**
 * @brief Performs an outer product between traffic_flow_ids and noc_link_ids
 * and returns all boolean variables for the resulting (traffic flow, link) pairs.
 *
 * @param map The container that stores all boolean variables for all
 * (traffic flow, link) pairs.
 * @param traffic_flow_ids Traffic flows whose boolean variables are requested.
 * @param noc_link_ids NoC links whose boolean variables are requested.
 * @return A vector of boolean variables for the requested (traffic flow, link) pairs.
 */
static std::vector<orsat::BoolVar> get_flow_link_vars(const t_flow_link_var_map& map,
                                                      const std::vector<NocTrafficFlowId>& traffic_flow_ids,
                                                      const std::vector<NocLinkId>& noc_link_ids);

/**
 * @brief Adds constraints for latency_overrun_vars variables to make sure
 * that they count the number of extra links a traffic flow is traversing
 * beyond what its latency constraint allows. For example, if a traffic flow
 * latency constraint allows the traffic flow to traverse at most 3 links,
 * but 4 (traffic flow, link) boolean variables are activated for this traffic flow,
 * the corresponding integer variable in latency_overrun_vars should take a value
 * of 1 because an extra link beyond the maximum 3 links has been activated.
 *
 * @param cp_model The CP model builder object. Created variables are added
 * to this model builder object.
 * @param flow_link_vars Boolean variable container for (traffic flow, link) pairs.
 * @param latency_overrun_vars The integer variables that are to be constrained.
 */
static void constrain_latency_overrun_vars(orsat::CpModelBuilder& cp_model,
                                           t_flow_link_var_map& flow_link_vars,
                                           std::map<NocTrafficFlowId , orsat::IntVar>& latency_overrun_vars);

/**
 * @brief Forbids specific turns that traffic flows can take.
 * Turn model routing algorithms forbid specific turns in a mesh topology
 * to make sure that deadlock does not happen. A turn can be determined by
 * specifying two consecutive links. A turn can be forbidden in the SAT
 * formulation by making sure that at most one of two consecutive links
 * that specify a turn is activated.
 *
 * @param flow_link_vars Boolean variable container for (traffic flow, link) pairs.
 * @param cp_model The CP model builder object. Created variables are added
 * to this model builder object.
 */
static void forbid_illegal_turns(t_flow_link_var_map& flow_link_vars,
                                 orsat::CpModelBuilder& cp_model);


static std::vector<orsat::BoolVar> get_flow_link_vars(const t_flow_link_var_map& map,
                                                      const std::vector<NocTrafficFlowId>& traffic_flow_ids,
                                                      const std::vector<NocLinkId>& noc_link_ids) {
    std::vector<orsat::BoolVar> results;
    for (auto traffic_flow_id : traffic_flow_ids) {
        for (auto noc_link_id : noc_link_ids) {
            auto it = map.find({traffic_flow_id, noc_link_id});
            if (it != map.end()) {
                results.push_back(it->second);
            }
        }
    }

    return results;
}

static void forbid_illegal_turns(t_flow_link_var_map& flow_link_vars,
                                 orsat::CpModelBuilder& cp_model) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;

    auto noc_routing_alg = dynamic_cast<const TurnModelRouting*> (noc_ctx.noc_flows_router.get());
    // ensure that the routing algorithm is a turn model algorithm
    VTR_ASSERT(noc_routing_alg != nullptr);

    // forbid illegal turns based on the routing algorithm
    // this includes 180 degree turns
    for (const auto& [link1, link2] : noc_routing_alg->get_all_illegal_turns(noc_ctx.noc_model)) {
        for (auto traffic_flow_id : traffic_flow_storage.get_all_traffic_flow_id()) {
            auto& first_var = flow_link_vars[{traffic_flow_id, link1}];
            auto& second_var = flow_link_vars[{traffic_flow_id, link2}];
            // at most one of two consecutive links that form a turn can be activated
            cp_model.AddBoolOr({first_var.Not(), second_var.Not()});
        }
    }
}

static vtr::vector<NocTrafficFlowId, int> rescale_traffic_flow_bandwidths(int bandwidth_resolution) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;

    const double link_bandwidth = noc_ctx.noc_model.get_noc_link_bandwidth();

    vtr::vector<NocTrafficFlowId, int> rescaled_traffic_flow_bandwidths;
    rescaled_traffic_flow_bandwidths.resize(traffic_flow_storage.get_number_of_traffic_flows());

    // rescale traffic flow bandwidths
    for (auto traffic_flow_id : traffic_flow_storage.get_all_traffic_flow_id()) {
        const auto& traffic_flow = traffic_flow_storage.get_single_noc_traffic_flow(traffic_flow_id);
        double bandwidth = traffic_flow.traffic_flow_bandwidth;
        int rescaled_bandwidth = (int)std::floor((bandwidth / link_bandwidth) * bandwidth_resolution);
        rescaled_traffic_flow_bandwidths[traffic_flow_id] = rescaled_bandwidth;
    }

    return  rescaled_traffic_flow_bandwidths;
}

static void add_congestion_constraints(t_flow_link_var_map& flow_link_vars,
                                       orsat::CpModelBuilder& cp_model,
                                       int bandwidth_resolution) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;

    vtr::vector<NocTrafficFlowId, int> rescaled_traffic_flow_bandwidths = rescale_traffic_flow_bandwidths(bandwidth_resolution);

    // add NoC link congestion constraints
    for (const auto& noc_link : noc_ctx.noc_model.get_noc_links()) {
        const NocLinkId noc_link_id = noc_link.get_link_id();
        orsat::LinearExpr lhs;

        for (auto traffic_flow_id : traffic_flow_storage.get_all_traffic_flow_id()) {
            orsat::BoolVar binary_var = flow_link_vars[{traffic_flow_id, noc_link_id}];
            lhs += orsat::LinearExpr::Term(binary_var, rescaled_traffic_flow_bandwidths[traffic_flow_id]);
        }

        cp_model.AddLessOrEqual(lhs, bandwidth_resolution);
    }
}

static void create_congested_link_vars(vtr::vector<NocLinkId , orsat::BoolVar>& congested_link_vars,
                                       t_flow_link_var_map& flow_link_vars,
                                       orsat::CpModelBuilder& cp_model,
                                       int bandwidth_resolution) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;

    vtr::vector<NocTrafficFlowId, int> rescaled_traffic_flow_bandwidths = rescale_traffic_flow_bandwidths(bandwidth_resolution);

    // add NoC link congestion constraints
    for (const auto& noc_link : noc_ctx.noc_model.get_noc_links()) {
        const NocLinkId noc_link_id = noc_link.get_link_id();
        orsat::LinearExpr lhs;

        for (auto traffic_flow_id : traffic_flow_storage.get_all_traffic_flow_id()) {
            orsat::BoolVar binary_var = flow_link_vars[{traffic_flow_id, noc_link_id}];
            lhs += orsat::LinearExpr::Term(binary_var, rescaled_traffic_flow_bandwidths[traffic_flow_id]);
        }

        orsat::BoolVar congested = cp_model.NewBoolVar();
        cp_model.AddLessOrEqual(lhs, bandwidth_resolution).OnlyEnforceIf(congested.Not());
        cp_model.AddGreaterThan(lhs, bandwidth_resolution).OnlyEnforceIf(congested);
        congested_link_vars.push_back(congested);
    }
}

static void add_continuity_constraints(t_flow_link_var_map& flow_link_vars,
                                       orsat::CpModelBuilder& cp_model) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;
    const auto& place_ctx = g_vpr_ctx.placement();

    for (auto traffic_flow_id : traffic_flow_storage.get_all_traffic_flow_id()) {
        const auto& traffic_flow = traffic_flow_storage.get_single_noc_traffic_flow(traffic_flow_id);

        // get the source and destination logical router blocks in the current traffic flow
        ClusterBlockId logical_source_router_block_id = traffic_flow.source_router_cluster_id;
        ClusterBlockId logical_sink_router_block_id = traffic_flow.sink_router_cluster_id;

        // get the ids of the hard router blocks where the logical router cluster blocks have been placed
        NocRouterId source_router_id = noc_ctx.noc_model.get_router_at_grid_location(place_ctx.block_locs[logical_source_router_block_id].loc);
        NocRouterId sink_router_id = noc_ctx.noc_model.get_router_at_grid_location(place_ctx.block_locs[logical_sink_router_block_id].loc);

        std::vector<orsat::BoolVar> vars;

        // exactly one outgoing link of the source must be selected
        const auto& src_outgoing_link_ids = noc_ctx.noc_model.get_noc_router_outgoing_links(source_router_id);
        vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, src_outgoing_link_ids);
        cp_model.AddExactlyOne(vars);

        // exactly one incoming link of the sink must be selected
        const auto& dst_incoming_link_ids = noc_ctx.noc_model.get_noc_router_incoming_links(sink_router_id);
        vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, dst_incoming_link_ids);
        cp_model.AddExactlyOne(vars);

        // each NoC router has at most one incoming and one outgoing link activated
        for (const auto& noc_router : noc_ctx.noc_model.get_noc_routers()) {
            const int noc_router_user_id = noc_router.get_router_user_id();
            const NocRouterId noc_router_id = noc_ctx.noc_model.convert_router_id(noc_router_user_id);

            if (noc_router_id == source_router_id || noc_router_id == sink_router_id) {
                continue;
            }

            const auto& incoming_links = noc_ctx.noc_model.get_noc_router_incoming_links(noc_router_id);
            vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, incoming_links);
            cp_model.AddAtMostOne(vars);

            orsat::LinearExpr lhs;
            lhs = orsat::LinearExpr::Sum(vars);

            const auto& outgoing_links = noc_ctx.noc_model.get_noc_router_outgoing_links(noc_router_id);
            vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, outgoing_links);
            cp_model.AddAtMostOne(vars);

            orsat::LinearExpr rhs;
            rhs = orsat::LinearExpr::Sum(vars);

            cp_model.AddEquality(lhs, rhs);
        }
    }
}

static void add_distance_constraints(t_flow_link_var_map& flow_link_vars,
                                     orsat::CpModelBuilder& cp_model,
                                     const std::vector<NocLinkId>& up,
                                     const std::vector<NocLinkId>& down,
                                     const std::vector<NocLinkId>& right,
                                     const std::vector<NocLinkId>& left) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;
    const auto& place_ctx = g_vpr_ctx.placement();
    const auto& cluster_ctx = g_vpr_ctx.clustering();

    const int num_layers = g_vpr_ctx.device().grid.get_num_layers();

    // Get the logical block type for router
    const auto router_block_type = cluster_ctx.clb_nlist.block_type(traffic_flow_storage.get_router_clusters_in_netlist()[0]);

    // Get the compressed grid for NoC
    const auto& compressed_noc_grid = place_ctx.compressed_block_grids[router_block_type->index];

    for (auto traffic_flow_id : traffic_flow_storage.get_all_traffic_flow_id()) {
        const auto& traffic_flow = traffic_flow_storage.get_single_noc_traffic_flow(traffic_flow_id);

        // get the source and destination logical router blocks in the current traffic flow
        ClusterBlockId logical_src_router_block_id = traffic_flow.source_router_cluster_id;
        ClusterBlockId logical_dst_router_block_id = traffic_flow.sink_router_cluster_id;

        // get the ids of the hard router blocks where the logical router cluster blocks have been placed
        NocRouterId src_router_id = noc_ctx.noc_model.get_router_at_grid_location(place_ctx.block_locs[logical_src_router_block_id].loc);
        NocRouterId dst_router_id = noc_ctx.noc_model.get_router_at_grid_location(place_ctx.block_locs[logical_dst_router_block_id].loc);

        // get source, current, and destination NoC routers
        const auto& src_router = noc_ctx.noc_model.get_single_noc_router(src_router_id);
        const auto& dst_router = noc_ctx.noc_model.get_single_noc_router(dst_router_id);

        // get the position of source, current, and destination NoC routers
        const auto src_router_pos = src_router.get_router_physical_location();
        const auto dst_router_pos = dst_router.get_router_physical_location();

        // get the compressed location for source, current, and destination NoC routers
        auto compressed_src_loc = get_compressed_loc(compressed_noc_grid, t_pl_loc{src_router_pos, 0}, num_layers)[src_router_pos.layer_num];
        auto compressed_dst_loc = get_compressed_loc(compressed_noc_grid, t_pl_loc{dst_router_pos, 0}, num_layers)[dst_router_pos.layer_num];

        // calculate the distance between the current router and the destination
        const int delta_x = compressed_dst_loc.x - compressed_src_loc.x;
        const int delta_y = compressed_dst_loc.y - compressed_src_loc.y;

        auto right_vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, right);
        auto left_vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, left);
        auto up_vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, up);
        auto down_vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, down);

        orsat::LinearExpr horizontal_expr;
        horizontal_expr += (orsat::LinearExpr::Sum(right_vars) - orsat::LinearExpr::Sum(left_vars));
        cp_model.AddEquality(horizontal_expr, delta_x);

        orsat::LinearExpr vertical_expr;
        vertical_expr += (orsat::LinearExpr::Sum(up_vars) - orsat::LinearExpr::Sum(down_vars));
        cp_model.AddEquality(vertical_expr, delta_y);
    }
}

static void group_noc_links_based_on_direction(std::vector<NocLinkId>& up,
                                               std::vector<NocLinkId>& down,
                                               std::vector<NocLinkId>& right,
                                               std::vector<NocLinkId>& left) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& noc_model = noc_ctx.noc_model;

    for (const auto& noc_link : noc_model.get_noc_links()) {
        const NocLinkId noc_link_id = noc_link.get_link_id();
        const NocRouterId src_noc_router_id = noc_link.get_source_router();
        const NocRouterId dst_noc_router_id = noc_link.get_sink_router();
        const NocRouter& src_noc_router = noc_model.get_single_noc_router(src_noc_router_id);
        const NocRouter& dst_noc_router = noc_model.get_single_noc_router(dst_noc_router_id);
        auto src_loc = src_noc_router.get_router_physical_location();
        auto dst_loc = dst_noc_router.get_router_physical_location();

        VTR_ASSERT(src_loc.x == dst_loc.x || src_loc.y == dst_loc.y);

        if (src_loc.x == dst_loc.x) { // vertical link
            if (dst_loc.y > src_loc.y) {
                up.push_back(noc_link_id);
            } else {
                down.push_back(noc_link_id);
            }
        } else { // horizontal link
            if (dst_loc.x > src_loc.x) {
                right.push_back(noc_link_id);
            } else {
                left.push_back(noc_link_id);
            }
        }
    }
}

static std::vector<NocLinkId> sort_noc_links_in_chain_order(const std::vector<NocLinkId>& links) {
    std::vector<NocLinkId> route;
    if (links.empty()) {
        return route;
    }

    const auto& noc_model = g_vpr_ctx.noc().noc_model;

    // Create a map to find pairs by their first element
    std::unordered_map<NocRouterId , NocLinkId> src_map;
    std::unordered_map<NocRouterId , bool> is_dst;
    for (const auto& l : links) {
        NocRouterId src_router_id = noc_model.get_single_noc_link(l).get_source_router();
        NocRouterId dst_router_id = noc_model.get_single_noc_link(l).get_sink_router();
        src_map[src_router_id] = l;
        is_dst[dst_router_id] = true;
    }

    // Find the starting pair (whose first element is not a second element of any pair)
    auto it = links.begin();
    for (; it != links.end(); ++it) {
        NocRouterId src_router_id = noc_model.get_single_noc_link(*it).get_source_router();
        if (is_dst.find(src_router_id) == is_dst.end()) {
            break;
        }
    }

    // Reconstruct the chain starting from the found starting pair
    auto current = *it;
    while (true) {
        route.push_back(current);
        NocRouterId dst_router_id = noc_model.get_single_noc_link(current).get_sink_router();
        auto nextIt = src_map.find(dst_router_id);
        if (nextIt == src_map.end()) {
            break; // End of chain
        }
        current = nextIt->second;
    }

    VTR_ASSERT(route.size() == links.size());

    return route;
}

static vtr::vector<NocTrafficFlowId, std::vector<NocLinkId>> convert_vars_to_routes(t_flow_link_var_map& flow_link_vars,
                                                                                    const orsat::CpSolverResponse& response) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;

    VTR_ASSERT(response.status() == orsat::CpSolverStatus::FEASIBLE ||
               response.status() == orsat::CpSolverStatus::OPTIMAL);

    vtr::vector<NocTrafficFlowId, std::vector<NocLinkId>> routes;
    routes.resize(traffic_flow_storage.get_number_of_traffic_flows());

    for (auto& [key, var] : flow_link_vars) {
        auto [traffic_flow_id, noc_link_id] = key;
        bool value = orsat::SolutionBooleanValue(response, var);
        if (value) {
            routes[traffic_flow_id].push_back(noc_link_id);
        }
    }

    for (auto& route : routes) {
        route = sort_noc_links_in_chain_order(route);
    }

    return routes;
}

static void create_flow_link_vars(orsat::CpModelBuilder& cp_model,
                                  t_flow_link_var_map& flow_link_vars,
                                  std::map<NocTrafficFlowId , orsat::IntVar>& latency_overrun_vars) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& noc_model = noc_ctx.noc_model;
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;

    // TODO: specify the domain based on NoC topology
    operations_research::Domain latency_overrun_domain(0, 20);

    // create boolean variables for each traffic flow and link pair
    // create integer variables for traffic flows with constrained latency
    for (auto traffic_flow_id : traffic_flow_storage.get_all_traffic_flow_id()) {
        const auto& traffic_flow = traffic_flow_storage.get_single_noc_traffic_flow(traffic_flow_id);

        // create an integer variable for each latency-constrained traffic flow
        if (traffic_flow.max_traffic_flow_latency < 0.1) {
            latency_overrun_vars[traffic_flow_id] = cp_model.NewIntVar(latency_overrun_domain);
        }

        // create (traffic flow, NoC link) pair boolean variables
        for (const auto& noc_link : noc_model.get_noc_links()) {
            const NocLinkId noc_link_id = noc_link.get_link_id();
            flow_link_vars[{traffic_flow_id, noc_link_id}] = cp_model.NewBoolVar();
        }
    }
}

static int comp_max_number_of_traversed_links(NocTrafficFlowId traffic_flow_id) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& noc_model = noc_ctx.noc_model;
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;

    const auto& traffic_flow = traffic_flow_storage.get_single_noc_traffic_flow(traffic_flow_id);

    const double noc_link_latency = noc_model.get_noc_link_latency();
    const double noc_router_latency = noc_model.get_noc_router_latency();
    const double traffic_flow_latency_constraint = traffic_flow.max_traffic_flow_latency;

    VTR_ASSERT(traffic_flow_latency_constraint < 0.1);

    int n_max_links = std::floor((traffic_flow_latency_constraint - noc_router_latency) / (noc_link_latency + noc_router_latency));

    return n_max_links;
}

static void constrain_latency_overrun_vars(orsat::CpModelBuilder& cp_model,
                                           t_flow_link_var_map& flow_link_vars,
                                           std::map<NocTrafficFlowId , orsat::IntVar>& latency_overrun_vars) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& noc_model = noc_ctx.noc_model;

    for (auto& [traffic_flow_id, latency_overrun_var] : latency_overrun_vars) {
        int n_max_links = comp_max_number_of_traversed_links(traffic_flow_id);
        // get all boolean variables for this traffic flow
        auto link_vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id},
                                            {noc_model.get_noc_links().keys().begin(), noc_model.get_noc_links().keys().end()});

        orsat::LinearExpr latency_overrun_expr;
        // count the number of activated links for this traffic flow
        latency_overrun_expr += orsat::LinearExpr::Sum(link_vars);
        // subtract the maximum number of permissible links from the number of activated links
        latency_overrun_expr -= n_max_links;

        // if latency_overrun_expr is non-positive, the latency constraint is met and
        // latency_overrun_var should be zero. Otherwise, should be equal to latency_overrun_expr
        // This is like pushing latency_overrun_expr through a ReLU function to get latency_overrun_var.
        cp_model.AddMaxEquality(latency_overrun_var, {latency_overrun_expr, 0});
    }
}

static void add_movable_continuity_constraints(t_flow_link_var_map& flow_link_vars,
                                               std::map<ClusterBlockId, orsat::IntVar>& x_loc_vars,
                                               std::map<ClusterBlockId, orsat::IntVar>& y_loc_vars,
                                               orsat::CpModelBuilder& cp_model) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& noc_model = noc_ctx.noc_model;
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;
    const auto& place_ctx = g_vpr_ctx.placement();
    const auto& cluster_ctx = g_vpr_ctx.clustering();

    // get the logical block type for router
    const auto router_block_type = cluster_ctx.clb_nlist.block_type(traffic_flow_storage.get_router_clusters_in_netlist()[0]);

    // get the compressed grid for NoC
    const auto& compressed_noc_grid = place_ctx.compressed_block_grids[router_block_type->index];

    std::unordered_map<std::pair<ClusterBlockId, NocRouterId>, orsat::BoolVar> logical_physical_mapped;

    // Create a boolean variable for each physical and logical NoC router pair
    // When set, this variable indicates that the logical NoC router is mapped
    // to its corresponding logical router
    for (auto& [router_blk_id, x_loc_var] : x_loc_vars) {
        auto& y_loc_var = y_loc_vars[router_blk_id];

        for (auto noc_router_id : noc_model.get_noc_routers().keys()) {
            const auto& noc_router = noc_model.get_single_noc_router(noc_router_id);
            const auto noc_router_pos = noc_router.get_router_physical_location();
            const auto compressed_loc = get_compressed_loc_approx(compressed_noc_grid, t_pl_loc{noc_router_pos, 0}, 1)[noc_router_pos.layer_num];

            orsat::BoolVar x_condition = cp_model.NewBoolVar();
            cp_model.AddEquality(x_loc_var, compressed_loc.x).OnlyEnforceIf(x_condition);
            cp_model.AddNotEqual(x_loc_var, compressed_loc.x).OnlyEnforceIf(Not(x_condition));

            orsat::BoolVar y_condition = cp_model.NewBoolVar();
            cp_model.AddEquality(y_loc_var, compressed_loc.y).OnlyEnforceIf(y_condition);
            cp_model.AddNotEqual(y_loc_var, compressed_loc.y).OnlyEnforceIf(Not(y_condition));

            // Combine conditions using AddBoolAnd
            orsat::BoolVar both_conds_met = cp_model.NewBoolVar();
            cp_model.AddBoolAnd({x_condition, y_condition}).OnlyEnforceIf(both_conds_met);
            cp_model.AddBoolOr({Not(x_condition), Not(y_condition)}).OnlyEnforceIf(Not(both_conds_met));

            logical_physical_mapped[{router_blk_id, noc_router_id}] = both_conds_met;
        }
    }

    /*
     * Iterate over all traffic flows and physical NoC routers
     * apply a constraint on the number of activated incoming and outgoing
     * links based on whether the NoC router is the source or destination
     * (or neither) of the traffic flow.
     */
    for (auto traffic_flow_id : traffic_flow_storage.get_all_traffic_flow_id()) {
        const auto& traffic_flow = traffic_flow_storage.get_single_noc_traffic_flow(traffic_flow_id);

        // get the source and destination logical router blocks in the current traffic flow
        ClusterBlockId logical_source_router_block_id = traffic_flow.source_router_cluster_id;
        ClusterBlockId logical_sink_router_block_id = traffic_flow.sink_router_cluster_id;

        for (auto noc_router_id : noc_ctx.noc_model.get_noc_routers().keys()) {

            auto& src_is_mapped = logical_physical_mapped[{logical_source_router_block_id, noc_router_id}];
            auto& dst_is_mapped = logical_physical_mapped[{logical_sink_router_block_id, noc_router_id}];

            orsat::BoolVar nor_src_dst_mapped = cp_model.NewBoolVar();
            cp_model.AddExactlyOne({src_is_mapped, dst_is_mapped, nor_src_dst_mapped});

            const auto& incoming_links = noc_ctx.noc_model.get_noc_router_incoming_links(noc_router_id);
            auto vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, incoming_links);
            //            cp_model.AddAtMostOne(vars).OnlyEnforceIf(nor_src_dst_mapped);
            //            cp_model.AddExactlyOne(vars).OnlyEnforceIf(dst_is_mapped);
            orsat::LinearExpr lhs = orsat::LinearExpr::Sum(vars);
            cp_model.AddEquality(lhs, 0).OnlyEnforceIf(src_is_mapped);
            cp_model.AddLessOrEqual(lhs, 1).OnlyEnforceIf(nor_src_dst_mapped);
            cp_model.AddEquality(lhs, 1).OnlyEnforceIf(dst_is_mapped);

            const auto& outgoing_links = noc_ctx.noc_model.get_noc_router_outgoing_links(noc_router_id);
            vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, outgoing_links);
            //            cp_model.AddAtMostOne(vars).OnlyEnforceIf(nor_src_dst_mapped);
            //            cp_model.AddExactlyOne(vars).OnlyEnforceIf(src_is_mapped);
            orsat::LinearExpr rhs = orsat::LinearExpr::Sum(vars);
            cp_model.AddEquality(rhs, 0).OnlyEnforceIf(dst_is_mapped);
            cp_model.AddLessOrEqual(rhs, 1).OnlyEnforceIf(nor_src_dst_mapped);
            cp_model.AddEquality(rhs, 1).OnlyEnforceIf(src_is_mapped);

            cp_model.AddEquality(lhs, rhs).OnlyEnforceIf(nor_src_dst_mapped);
        }
    }
}

static void add_movable_distance_constraints(t_flow_link_var_map& flow_link_vars,
                                             orsat::CpModelBuilder& cp_model,
                                             std::map<ClusterBlockId, orsat::IntVar>& x_loc_vars,
                                             std::map<ClusterBlockId, orsat::IntVar>& y_loc_vars,
                                             const std::vector<NocLinkId>& up,
                                             const std::vector<NocLinkId>& down,
                                             const std::vector<NocLinkId>& right,
                                             const std::vector<NocLinkId>& left) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;

    for (auto traffic_flow_id : traffic_flow_storage.get_all_traffic_flow_id()) {
        const auto& traffic_flow = traffic_flow_storage.get_single_noc_traffic_flow(traffic_flow_id);

        // get the source and destination logical router blocks in the current traffic flow
        ClusterBlockId logical_src_router_block_id = traffic_flow.source_router_cluster_id;
        ClusterBlockId logical_dst_router_block_id = traffic_flow.sink_router_cluster_id;

        auto right_vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, right);
        auto left_vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, left);
        auto up_vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, up);
        auto down_vars = get_flow_link_vars(flow_link_vars, {traffic_flow_id}, down);

        orsat::LinearExpr horizontal_expr;
        horizontal_expr += (orsat::LinearExpr::Sum(right_vars) - orsat::LinearExpr::Sum(left_vars));
        orsat::LinearExpr horizontal_dist_expr;
        horizontal_dist_expr += x_loc_vars[logical_dst_router_block_id];
        horizontal_dist_expr -= x_loc_vars[logical_src_router_block_id];
        cp_model.AddEquality(horizontal_expr, horizontal_dist_expr);

        orsat::LinearExpr vertical_expr;
        vertical_expr += (orsat::LinearExpr::Sum(up_vars) - orsat::LinearExpr::Sum(down_vars));
        orsat::LinearExpr vertical_dist_expr;
        vertical_dist_expr += y_loc_vars[logical_dst_router_block_id];
        vertical_dist_expr -= y_loc_vars[logical_src_router_block_id];
        cp_model.AddEquality(vertical_expr, vertical_dist_expr);
    }
}

static void convert_vars_to_locs(std::map<ClusterBlockId, t_pl_loc>& noc_router_locs,
                                 std::map<ClusterBlockId, orsat::IntVar>& x_loc_vars,
                                 std::map<ClusterBlockId, orsat::IntVar>& y_loc_vars,
                                 const orsat::CpSolverResponse& response) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;
    const auto& cluster_ctx = g_vpr_ctx.clustering();

    //    const int num_layers = g_vpr_ctx.device().grid.get_num_layers();

    // Get the logical block type for router
    const auto router_block_type = cluster_ctx.clb_nlist.block_type(traffic_flow_storage.get_router_clusters_in_netlist()[0]);

    noc_router_locs.clear();

    VTR_ASSERT(response.status() == orsat::CpSolverStatus::FEASIBLE ||
               response.status() == orsat::CpSolverStatus::OPTIMAL);

    for (auto& [router_blk_id, x_loc_var] : x_loc_vars) {
        auto& y_loc_var = y_loc_vars[router_blk_id];
        int x_value = (int)orsat::SolutionIntegerValue(response, x_loc_var);
        int y_value = (int)orsat::SolutionIntegerValue(response, y_loc_var);

        t_pl_loc mapped_loc;
        compressed_grid_to_loc(router_block_type, {x_value, y_value, 0}, mapped_loc);
        noc_router_locs[router_blk_id] = mapped_loc;
    }
}

static void constrain_fixed_noc_routers(std::map<ClusterBlockId, orsat::IntVar>& x_loc_vars,
                                        std::map<ClusterBlockId, orsat::IntVar>& y_loc_vars,
                                        orsat::CpModelBuilder& cp_model) {
    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;
    const auto& place_ctx = g_vpr_ctx.mutable_placement();
    const auto& cluster_ctx = g_vpr_ctx.clustering();

    const int num_layers = g_vpr_ctx.device().grid.get_num_layers();
    const auto router_block_type = cluster_ctx.clb_nlist.block_type(traffic_flow_storage.get_router_clusters_in_netlist()[0]);
    const auto& compressed_noc_grid = place_ctx.compressed_block_grids[router_block_type->index];

    const std::vector<ClusterBlockId>& router_blk_ids = traffic_flow_storage.get_router_clusters_in_netlist();

    for (auto router_blk_id : router_blk_ids) {
        const auto& router_loc = place_ctx.block_locs[router_blk_id];
        if (router_loc.is_fixed)  {
            auto compressed_loc = get_compressed_loc(compressed_noc_grid, router_loc.loc, num_layers)[router_loc.loc.layer];
            cp_model.AddEquality(x_loc_vars[router_blk_id], compressed_loc.x);
            cp_model.AddEquality(y_loc_vars[router_blk_id], compressed_loc.y);
        }
    }
}

//void noc_sat_place_and_route(vtr::vector<NocTrafficFlowId, std::vector<NocLinkId>>& traffic_flow_routes,
//                             std::map<ClusterBlockId, t_pl_loc>& noc_router_locs,
//                             int seed) {
//    vtr::ScopedStartFinishTimer timer("NoC SAT Placement and Routing");
//    const auto& noc_ctx = g_vpr_ctx.noc();
//    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;
//    const auto& placement_ctx = g_vpr_ctx.placement();
//    const auto& cluster_ctx = g_vpr_ctx.clustering();
//
//    traffic_flow_routes.clear();
//    noc_router_locs.clear();
//
//    orsat::CpModelBuilder cp_model;
//
//    t_flow_link_var_map flow_link_vars;
//
//    std::map<NocTrafficFlowId, orsat::IntVar> latency_overrun_vars;
//
//    vtr::vector<NocLinkId, orsat::BoolVar> link_congested_vars;
//
//    const std::vector<ClusterBlockId>& router_blk_ids = traffic_flow_storage.get_router_clusters_in_netlist();
//    operations_research::Domain loc_domain(0, 9);
//    std::map<ClusterBlockId, orsat::IntVar> x_loc_vars;
//    std::map<ClusterBlockId, orsat::IntVar> y_loc_vars;
//    std::vector<std::pair<orsat::IntervalVar, orsat::IntervalVar>> loc_intervals;
//
//
//    for (auto router_blk_id : router_blk_ids) {
//        x_loc_vars[router_blk_id] = cp_model.NewIntVar(loc_domain);
//        y_loc_vars[router_blk_id] = cp_model.NewIntVar(loc_domain);
//
//        loc_intervals.emplace_back(cp_model.NewIntervalVar(x_loc_vars[router_blk_id], 1, x_loc_vars[router_blk_id]+1),
//                                   cp_model.NewIntervalVar(y_loc_vars[router_blk_id], 1, y_loc_vars[router_blk_id]+1));
//    }
//
//    orsat::NoOverlap2DConstraint no_overlap_2d = cp_model.AddNoOverlap2D();
//
//    for (auto& [x_interval, y_interval] : loc_intervals) {
//        no_overlap_2d.AddRectangle(x_interval, y_interval);
//    }
//
//    create_flow_link_vars(cp_model, flow_link_vars, latency_overrun_vars);
//
//    constrain_latency_overrun_vars(cp_model, flow_link_vars, latency_overrun_vars);
//
//    forbid_illegal_turns(flow_link_vars, cp_model);
//
////    add_congestion_constraints(flow_link_vars, cp_model);
//    create_congested_link_vars(link_congested_vars, flow_link_vars, cp_model);
//
//    add_movable_continuity_constraints(flow_link_vars, x_loc_vars, y_loc_vars, cp_model);
//
//    std::vector<NocLinkId> up, down, right, left;
//    group_noc_links_based_on_direction(up, down, right, left);
//
//    add_movable_distance_constraints(flow_link_vars, cp_model, x_loc_vars, y_loc_vars, up, down, right, left);
//
//    constrain_fixed_noc_routers(x_loc_vars, y_loc_vars, cp_model);
//
//    const int num_layers = g_vpr_ctx.device().grid.get_num_layers();
//
//    // Get the logical block type for router
//    const auto router_block_type = cluster_ctx.clb_nlist.block_type(traffic_flow_storage.get_router_clusters_in_netlist()[0]);
//
//    // Get the compressed grid for NoC
//    const auto& compressed_noc_grid = placement_ctx.compressed_block_grids[router_block_type->index];
//    for (auto router_blk_id : router_blk_ids) {
//        auto router_loc = placement_ctx.block_locs[router_blk_id];
//
//        auto compressed_loc = get_compressed_loc(compressed_noc_grid, router_loc.loc, num_layers)[0];
//
//        cp_model.AddHint(x_loc_vars[router_blk_id], compressed_loc.x);
//        cp_model.AddHint(y_loc_vars[router_blk_id], compressed_loc.y);
////        cp_model.AddEquality(x_loc_vars[router_blk_id], compressed_loc.x);
////        cp_model.AddEquality(y_loc_vars[router_blk_id], compressed_loc.y);
//    }
//
//    orsat::LinearExpr latency_overrun_sum;
//    for (auto& [traffic_flow_id, latency_overrun_var] : latency_overrun_vars) {
//        latency_overrun_sum += latency_overrun_var;
//    }
//
//    latency_overrun_sum *= 1024;
//
//    auto rescaled_traffic_flow_bandwidths = rescale_traffic_flow_bandwidths();
//    orsat::LinearExpr agg_bw_expr;
//    for (auto& [key, var] : flow_link_vars) {
//        auto [traffic_flow_id, noc_link_id] = key;
//        agg_bw_expr += orsat::LinearExpr::Term(var, rescaled_traffic_flow_bandwidths[traffic_flow_id]);
//    }
//
//    for (auto traffic_flow_id : traffic_flow_storage.get_all_traffic_flow_id()) {
//        std::vector<NocLinkId> activated_links;
//        for (auto route_link_id : traffic_flow_storage.get_traffic_flow_route(traffic_flow_id)) {
//            cp_model.AddHint(flow_link_vars[{traffic_flow_id, route_link_id}], true);
////            cp_model.AddEquality(flow_link_vars[{traffic_flow_id, route_link_id}], true);
//            activated_links.push_back(route_link_id);
//        }
//
//        for (auto noc_link_id : noc_ctx.noc_model.get_noc_links().keys()) {
//            if (std::find(activated_links.begin(), activated_links.end(), noc_link_id) == activated_links.end()) {
//                cp_model.AddHint(flow_link_vars[{traffic_flow_id, noc_link_id}], false);
////                cp_model.AddEquality(flow_link_vars[{traffic_flow_id, noc_link_id}], false);
//            }
//        }
//    }
//
//    orsat::LinearExpr congested_link_sum = orsat::LinearExpr::Sum(link_congested_vars);
//    congested_link_sum *= (1024 * 16);
//
//    cp_model.Minimize(latency_overrun_sum + agg_bw_expr + congested_link_sum);
//
//    orsat::SatParameters sat_params;
//    sat_params.set_random_seed(seed);
//    sat_params.set_log_search_progress(true);
//    sat_params.set_max_time_in_seconds(10*60);
//
//    orsat::Model model;
//    model.Add(NewSatParameters(sat_params));
//
//    orsat::CpSolverResponse response = orsat::SolveCpModel(cp_model.Build(), &model);
//
//    if (response.status() == orsat::CpSolverStatus::FEASIBLE ||
//        response.status() == orsat::CpSolverStatus::OPTIMAL) {
//
//        traffic_flow_routes = convert_vars_to_routes(flow_link_vars, response);
//        convert_vars_to_locs(noc_router_locs, x_loc_vars, y_loc_vars, response);
//    }
//}




vtr::vector<NocTrafficFlowId, std::vector<NocLinkId>> noc_sat_route(bool minimize_aggregate_bandwidth,
                                                                    int bandwidth_resolution,
                                                                    int seed) {
    vtr::ScopedStartFinishTimer timer("NoC SAT Routing");

    // Used to add variables and constraints to a CP-SAT model
    orsat::CpModelBuilder cp_model;

    /* For each traffic flow and NoC link pair, we create a boolean variable.
     * When a variable associated with traffic flow t and NoC link l is set,
     * it means that t is routed through l.*/
    t_flow_link_var_map flow_link_vars;

    /* A boolean variable is associated with each NoC link to indicate
     * whether it is congested.*/
    vtr::vector<NocLinkId, orsat::BoolVar> link_congested_vars;

    /* Each traffic flow latency constraint is translated to how many NoC links
     * the traffic flow can traverse without violating the constraint.
     * These integer variables specify the number of additional links traversed
     * beyond the maximum allowed number of links.
     */
    std::map<NocTrafficFlowId, orsat::IntVar> latency_overrun_vars;

    create_flow_link_vars(cp_model, flow_link_vars, latency_overrun_vars);

    constrain_latency_overrun_vars(cp_model, flow_link_vars, latency_overrun_vars);

    forbid_illegal_turns(flow_link_vars, cp_model);

    //    add_congestion_constraints(flow_link_vars, cp_model);
    create_congested_link_vars(link_congested_vars, flow_link_vars, cp_model, bandwidth_resolution);

    add_continuity_constraints(flow_link_vars, cp_model);

    std::vector<NocLinkId> up, down, right, left;
    group_noc_links_based_on_direction(up, down, right, left);

    add_distance_constraints(flow_link_vars, cp_model, up, down, right, left);

    const auto& noc_ctx = g_vpr_ctx.noc();
    const auto& traffic_flow_storage = noc_ctx.noc_traffic_flows_storage;

    for (auto traffic_flow_id : traffic_flow_storage.get_all_traffic_flow_id()) {
        for (auto route_link_id : traffic_flow_storage.get_traffic_flow_route(traffic_flow_id)) {
            cp_model.AddHint(flow_link_vars[{traffic_flow_id, route_link_id}], true);
        }
    }

    orsat::LinearExpr latency_overrun_sum;
    for (auto& [traffic_flow_id, latency_overrun_var] : latency_overrun_vars) {
        latency_overrun_sum += latency_overrun_var;
    }

    latency_overrun_sum *= 1024;

    auto rescaled_traffic_flow_bandwidths = rescale_traffic_flow_bandwidths(bandwidth_resolution);
    orsat::LinearExpr agg_bw_expr;
    for (auto& [key, var] : flow_link_vars) {
        auto [traffic_flow_id, noc_link_id] = key;
        agg_bw_expr += orsat::LinearExpr::Term(var, rescaled_traffic_flow_bandwidths[traffic_flow_id]);
    }

    orsat::LinearExpr congested_link_sum = orsat::LinearExpr::Sum(link_congested_vars);
    congested_link_sum *= (1024 * 16);

    cp_model.Minimize(latency_overrun_sum + agg_bw_expr + congested_link_sum);

    orsat::Model model;

    orsat::SatParameters sat_params;
    //    sat_params.set_num_workers(1);
    sat_params.set_random_seed(seed);
    sat_params.set_log_search_progress(true);

    model.Add(NewSatParameters(sat_params));

    orsat::CpSolverResponse response = orsat::SolveCpModel(cp_model.Build(), &model);

    if (response.status() == orsat::CpSolverStatus::FEASIBLE ||
        response.status() == orsat::CpSolverStatus::OPTIMAL) {

        //        if (!minimize_aggregate_bandwidth) {
        auto routes = convert_vars_to_routes(flow_link_vars, response);
        return routes;
        //        } else {
        //            int latency_overrun_value = (int)orsat::SolutionIntegerValue(response, latency_overrun_sum);
        //            cp_model.AddEquality(latency_overrun_sum, latency_overrun_value);

        //            auto rescaled_traffic_flow_bandwidths = rescale_traffic_flow_bandwidths();
        //            orsat::LinearExpr agg_bw_expr;
        //            for (auto& [key, var] : flow_link_vars) {
        //                auto [traffic_flow_id, noc_link_id] = key;
        //                agg_bw_expr += orsat::LinearExpr::Term(var, rescaled_traffic_flow_bandwidths[traffic_flow_id]);
        //            }

        cp_model.Minimize(agg_bw_expr);
        response = orsat::Solve(cp_model.Build());

        //            if (response.status() == orsat::CpSolverStatus::FEASIBLE ||
        //                response.status() == orsat::CpSolverStatus::OPTIMAL) {
        //                auto routes = convert_vars_to_routes(flow_link_vars, response);
        //                return routes;
        //            }
        //        }
    }

    return {};
}