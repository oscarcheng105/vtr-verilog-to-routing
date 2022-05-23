/*
 * This file contains the noc setup function. This function should be used if
 * there is a NoC component in the architecture description file, then the function will create a NoC model based on the noc description. There are a number of internal functions that act as helpers in setting up the NoC. 
 */
#include <climits>
#include <cmath>

#include "setup_noc.h"

#include "vtr_assert.h"
#include "vpr_error.h"
#include "vtr_math.h"
#include "echo_files.h"

static void identify_and_store_noc_router_tile_positions(const DeviceGrid& device_grid, std::vector<t_noc_router_tile_position>& list_of_noc_router_tiles, std::string noc_router_tile_name);

static void generate_noc(const t_arch& arch, NocContext& noc_ctx, std::vector<t_noc_router_tile_position>& list_of_noc_router_tiles);

static void create_noc_routers(const t_noc_inf& noc_info, NocStorage* noc_model, std::vector<t_noc_router_tile_position>& list_of_noc_router_tiles);

static void create_noc_links(const t_noc_inf* noc_info, NocStorage* noc_model);

static void echo_noc(char* file_name);

/**
 * @brief Based on the NoC information provided by the user in the architecture
 *        description file, a NoC model is created. The model defines the
 *        constraints of the NoC as well as its layout on the FPGA device. 
 *        The datastructure used to define the model is  "NocStorage" and that
 *        is created here and stored within the noc_ctx. 
 * 
 * @param arch Contains the parsed information from the architecture
 *             description file.
 */
void setup_noc(const t_arch& arch) {
    // variable to store all the noc router tiles within the FPGA device
    std::vector<t_noc_router_tile_position> list_of_noc_router_tiles;

    // get references to global variables
    auto& device_ctx = g_vpr_ctx.device();
    auto& noc_ctx = g_vpr_ctx.mutable_noc();

    // quick error check that the noc attribute of the arch is not empty
    // basically, no noc topology information was provided by the user in the arch file
    if (arch.noc == nullptr) {
        VPR_FATAL_ERROR(VPR_ERROR_OTHER, "No NoC topology information was provided in the architecture file.");
    }

    // go through the FPGA grid and find the noc router tiles
    // then store the position
    identify_and_store_noc_router_tile_positions(device_ctx.grid, list_of_noc_router_tiles, arch.noc->noc_router_tile_name);

    // check whether the noc topology information provided uses more than the number of available routers in the FPGA
    if (list_of_noc_router_tiles.size() < arch.noc->router_list.size()) {
        VPR_FATAL_ERROR(VPR_ERROR_OTHER, "The Provided NoC topology information in the architecture file has more number of routers than what is available in the FPGA device.");
    } else if (list_of_noc_router_tiles.size() > arch.noc->router_list.size()) // check whether the noc topology information provided is using all the routers in the FPGA
    {
        VPR_FATAL_ERROR(VPR_ERROR_OTHER, "The Provided NoC topology information in the architecture file uses less number of routers than what is available in the FPGA device.");
    } else if (list_of_noc_router_tiles.size() == 0) // case where no physical router tiles were found
    {
        VPR_FATAL_ERROR(VPR_ERROR_OTHER, "No physical NoC routers were found on the FPGA device. Either the provided name for the physical router tile was incorrect or the FPGA device has no routers.");
    }

    // generate noc model
    generate_noc(arch, noc_ctx, list_of_noc_router_tiles);

    // store the general noc properties
    noc_ctx.noc_link_bandwidth = arch.noc->link_bandwidth;
    noc_ctx.noc_link_latency = arch.noc->link_latency;
    noc_ctx.noc_router_latency = arch.noc->router_latency;

    // echo the noc info
    if (getEchoEnabled() && isEchoFileEnabled(E_ECHO_NOC_MODEL)) {
        echo_noc(getEchoFileName(E_ECHO_NOC_MODEL));
    }

    return;
}

/**
 * @brief Goes through the FPGA device and identifies tiles that
 *        are NoC routers based on the name used to describe
 *        the router. Every identified routers grid position is
 *        stored in a list.
 * 
 * @param device_grid The FPGA device description.
 * @param list_of_noc_router_tiles Stores the grid position information
 *                                 for all NoC router tiles in the FPGA.
 * @param noc_router_tile_name The name used when describing the NoC router
 *                             tile in the FPGA architecture description
 *                             file.
 */
static void identify_and_store_noc_router_tile_positions(const DeviceGrid& device_grid, std::vector<t_noc_router_tile_position>& list_of_noc_router_tiles, std::string noc_router_tile_name) {
    int grid_width = device_grid.width();
    int grid_height = device_grid.height();

    int curr_tile_width;
    int curr_tile_height;
    int curr_tile_width_offset;
    int curr_tile_height_offset;
    std::string curr_tile_name;

    double curr_tile_centroid_x;
    double curr_tile_centroid_y;

    // go through the device
    for (int i = 0; i < grid_width; i++) {
        for (int j = 0; j < grid_height; j++) {
            // get some information from the current tile
            curr_tile_name.assign(device_grid[i][j].type->name);
            curr_tile_width_offset = device_grid[i][j].width_offset;
            curr_tile_height_offset = device_grid[i][j].height_offset;

            curr_tile_height = device_grid[i][j].type->height;
            curr_tile_width = device_grid[i][j].type->width;

            /* 
             * Only store the tile position if it is a noc router.
             * Additionally, since a router tile can span multiple grid locations, we only add the tile if the height and width offset are zero (this prevents the router from being added multiple times for each grid location it spans).
             */
            if (!(noc_router_tile_name.compare(curr_tile_name)) && !curr_tile_width_offset && !curr_tile_height_offset) {
                // calculating the centroid position of the current tile
                curr_tile_centroid_x = (curr_tile_width - 1) / (double)2 + i;
                curr_tile_centroid_y = (curr_tile_height - 1) / (double)2 + j;

                list_of_noc_router_tiles.push_back({i, j, curr_tile_centroid_x, curr_tile_centroid_y});
            }
        }
    }

    return;
}

/**
 * @brief Creates NoC routers and adds them to the NoC model based
 *        on the routers provided by the user. Then the NoC links are
 *        created based on the topology. This completes the NoC
 *        model creation.
 * 
 * @param arch Contains the parsed information from the architecture
 *             description file.
 * @param noc_ctx A global variable that contains the NoC Model and other
 *                NoC related information.
 * @param list_of_noc_router_tiles Stores the grid position information
 *                                 for all NoC router tiles in the FPGA.
 */
void generate_noc(const t_arch& arch, NocContext& noc_ctx, std::vector<t_noc_router_tile_position>& list_of_noc_router_tiles) {
    // refrernces to the noc
    NocStorage* noc_model = &noc_ctx.noc_model;
    // reference to the noc description
    const t_noc_inf* noc_info = arch.noc;

    // initialize the noc
    noc_model->clear_noc();

    // create all the routers in the NoC
    create_noc_routers(*arch.noc, noc_model, list_of_noc_router_tiles);

    // create all the links in the NoC
    create_noc_links(noc_info, noc_model);

    // indicate that the NoC has been built
    noc_model->finished_building_noc();

    return;
}

/**
 * @brief Go through the list of logical routers (routers described by the user
 *        in the architecture description file) and assign it a corresponding
 *        physical router tile in the FPGA. Each logical router has a grid
 *        location, so the closest physical router to the grid location is then
 *        assigned to it. Once a physical router is assigned, a NoC router
 *        is created to represent it and this is added to the NoC model.
 * 
 * @param noc_info Contains the parsed NoC topology information from the
 *                 architecture description file.
 * @param noc_model An internal model that describes the NoC. Contains a list of
 *                  routers and links that connect the routers together.
 * @param list_of_noc_router_tiles Stores the grid position information
 *                                 for all NoC router tiles in the FPGA.
 */
static void create_noc_routers(const t_noc_inf& noc_info, NocStorage* noc_model, std::vector<t_noc_router_tile_position>& list_of_noc_router_tiles) {
    // keep track of the shortest distance between a logical router and the curren physical router tile
    // also keep track of the corresponding physical router tile index (within the list)
    double shortest_distance;
    double curr_calculated_distance;
    int closest_physical_router;

    // information regarding physical router position
    double curr_physical_router_pos_x;
    double curr_physical_router_pos_y;

    // information regarding logical router position
    double curr_logical_router_position_x;
    double curr_logical_router_position_y;

    // keep track of the index of each physical router (this helps uniqely identify them)
    int curr_physical_router_index = 0;

    // keep track of the ids of the routers that ceate the case where multiple routers
    // have the same distance to a physical router tile
    int error_case_physical_router_index_1;
    int error_case_physical_router_index_2;

    // keep track of all the logical router and physical router assignments (their pairings)
    std::vector<int> router_assignments;
    router_assignments.resize(list_of_noc_router_tiles.size(), PHYSICAL_ROUTER_NOT_ASSIGNED);

    // Below we create all the routers within the NoC //

    // go through each logical router tile and assign it to a physical router on the FPGA
    for (auto logical_router = noc_info.router_list.begin(); logical_router != noc_info.router_list.end(); logical_router++) {
        // assign the shortest distance to a large value (this is done so that the first distance calculated and we can replace this)
        shortest_distance = LLONG_MAX;

        // get position of the current logical router
        curr_logical_router_position_x = logical_router->device_x_position;
        curr_logical_router_position_y = logical_router->device_y_position;

        closest_physical_router = 0;

        // the starting index of the physical router list
        curr_physical_router_index = 0;

        // initialze the router ids that track the error case where two physical router tiles have the same distance to a logical router
        // we initialize it to a in-valid index, so that it reflects the situation where we never hit this case
        error_case_physical_router_index_1 = INVALID_PHYSICAL_ROUTER_INDEX;
        error_case_physical_router_index_2 = INVALID_PHYSICAL_ROUTER_INDEX;

        // determine the physical router tile that is closest to the current logical router
        for (auto physical_router = list_of_noc_router_tiles.begin(); physical_router != list_of_noc_router_tiles.end(); physical_router++) {
            // get the position of the current physical router tile on the FPGA device
            curr_physical_router_pos_x = physical_router->tile_centroid_x;
            curr_physical_router_pos_y = physical_router->tile_centroid_y;

            // use euclidean distance to calculate the length between the current logical and physical routers
            curr_calculated_distance = sqrt(pow(abs(curr_physical_router_pos_x - curr_logical_router_position_x), 2.0) + pow(abs(curr_physical_router_pos_y - curr_logical_router_position_y), 2.0));

            // if the current distance is the same as the previous shortest distance
            if (vtr::isclose(curr_calculated_distance, shortest_distance)) {
                // store the ids of the two physical routers
                error_case_physical_router_index_1 = closest_physical_router;
                error_case_physical_router_index_2 = curr_physical_router_index;

            } else if (curr_calculated_distance < shortest_distance) // case where the current logical router is closest to the physical router tile
            {
                // update the shortest distance and then the closest router
                shortest_distance = curr_calculated_distance;
                closest_physical_router = curr_physical_router_index;
            }

            // update the index for the next physical router
            curr_physical_router_index++;
        }

        // check the case where two physical router tiles have the same distance to the given logical router
        if (error_case_physical_router_index_1 == closest_physical_router) {
            VPR_FATAL_ERROR(VPR_ERROR_OTHER,
                            "Router with ID:'%d' has the same distance to physical router tiles located at position (%d,%d) and (%d,%d). Therefore, no router assignment could be made.",
                            logical_router->id, list_of_noc_router_tiles[error_case_physical_router_index_1].grid_width_position, list_of_noc_router_tiles[error_case_physical_router_index_1].grid_height_position,
                            list_of_noc_router_tiles[error_case_physical_router_index_2].grid_width_position, list_of_noc_router_tiles[error_case_physical_router_index_2].grid_height_position);
        }

        // check if the current physical router was already assigned previously, if so then throw an error
        if (router_assignments[closest_physical_router] != PHYSICAL_ROUTER_NOT_ASSIGNED) {
            VPR_FATAL_ERROR(VPR_ERROR_OTHER, "Routers with IDs:'%d' and '%d' are both closest to physical router tile located at (%d,%d) and the physical router could not be assigned multiple times.",
                            logical_router->id, router_assignments[closest_physical_router], list_of_noc_router_tiles[closest_physical_router].grid_width_position,
                            list_of_noc_router_tiles[closest_physical_router].grid_height_position);
        }

        // at this point, the closest logical router to the current physical router was found
        // so add the router to the NoC
        noc_model->add_router(logical_router->id, list_of_noc_router_tiles[closest_physical_router].grid_width_position,
                              list_of_noc_router_tiles[closest_physical_router].grid_height_position);

        // add the new assignment to the tracker
        router_assignments[closest_physical_router] = logical_router->id;
    }

    return;
}

/**
 * @brief Goes through the topology information as described in the FPGA
 *        architecture description file and creates NoC links that are stored
 *        into the NoC Model. All the created NoC links describe how the routers
 *        are connected to each other.
 * 
 * @param noc_info Contains the parsed NoC topology information from the
 *                 architecture description file.
 * @param noc_model An internal model that describes the NoC. Contains a list of
 *                  routers and links that connect the routers together.
 */
static void create_noc_links(const t_noc_inf* noc_info, NocStorage* noc_model) {
    // the ids used to represent the routers in the NoC are not the same as the ones provided by the user in the arch desc file.
    // while going through the router connections, the user provided router ids are converted and then stored below before being used
    // in the links.
    NocRouterId source_router;
    NocRouterId sink_router;

    // store the id of each new link we create
    NocLinkId created_link_id;

    // start of by creating enough space for the list of outgoing links for each router in the NoC
    noc_model->make_room_for_noc_router_link_list();

    // go through each router and add its outgoing links to the NoC
    for (auto router = noc_info->router_list.begin(); router != noc_info->router_list.end(); router++) {
        // get the converted id of the current source router
        source_router = noc_model->convert_router_id(router->id);

        // go through all the routers connected to the current one and add links to the noc
        for (auto conn_router_id = router->connection_list.begin(); conn_router_id != router->connection_list.end(); conn_router_id++) {
            // get the converted id of the currently connected sink router
            sink_router = noc_model->convert_router_id(*conn_router_id);

            // add the link to the Noc
            noc_model->add_link(source_router, sink_router);
            ;
        }
    }

    return;
}

/**
 * @brief Writes out the NoC model infromation to a file. This includes
 *        the noc constraints, the list of routers and their connections
 *        to other routers in the NoC.
 * 
 * @param file_name The name of the file that contains the NoC model info.
 */
static void echo_noc(char* file_name) {
    FILE* fp;
    fp = vtr::fopen(file_name, "w");

    fprintf(fp, "--------------------------------------------------------------\n");
    fprintf(fp, "NoC\n");
    fprintf(fp, "--------------------------------------------------------------\n");
    fprintf(fp, "\n");

    auto& noc_ctx = g_vpr_ctx.noc();

    // print the overall constraints of the NoC
    fprintf(fp, "NoC Constraints:\n");
    fprintf(fp, "--------------------------------------------------------------\n");
    fprintf(fp, "\n");
    fprintf(fp, "Maximum NoC Link Bandwidth: %f\n", noc_ctx.noc_link_bandwidth);
    fprintf(fp, "\n");
    fprintf(fp, "NoC Link Latency: %f\n", noc_ctx.noc_link_latency);
    fprintf(fp, "\n");
    fprintf(fp, "NoC Router Latency: %f\n", noc_ctx.noc_router_latency);
    fprintf(fp, "\n");

    // print all the routers and their properties
    fprintf(fp, "NoC Router List:\n");
    fprintf(fp, "--------------------------------------------------------------\n");
    fprintf(fp, "\n");

    auto& noc_routers = noc_ctx.noc_model.get_noc_routers();

    // go through each router and print its information
    for (auto router = noc_routers.begin(); router != noc_routers.end(); router++) {
        fprintf(fp, "Router %d:\n", router->get_router_id());
        // if the router tile is larger than a single grid, the position represents the bottom left corner of the tile
        fprintf(fp, "Equivalent Physical Tile Grid Position -> (%d,%d)\n", router->get_router_grid_position_x(), router->get_router_grid_position_y());
        fprintf(fp, "Router Connections ->");

        auto& router_connections = noc_ctx.noc_model.get_noc_router_connections(noc_ctx.noc_model.convert_router_id(router->get_router_id()));

        // go through the links of the current router and add the connecting router to the list
        for (auto router_link = router_connections.begin(); router_link != router_connections.end(); router_link++) {
            fprintf(fp, " %d", noc_ctx.noc_model.get_noc_router_id(noc_ctx.noc_model.get_noc_link_sink_router(*router_link)));
        }

        fprintf(fp, "\n\n");
    }

    fclose(fp);

    return;
}