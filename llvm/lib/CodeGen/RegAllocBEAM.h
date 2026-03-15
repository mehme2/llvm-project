#ifndef BEAM_H
#define BEAM_H

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>

#define block_t uint64_t
#define weight_t float

#define BLOCK_INDEX(bit_index)              ((bit_index)/(sizeof(block_t)*8))
#define MASK_INDEX(bit_index)               ((bit_index)%(sizeof(block_t)*8))
#define MASK(bit_index)                     ((block_t)1 << MASK_INDEX(bit_index))
#define TOTAL_BLOCK_NUM(vertex_num)         (BLOCK_INDEX(vertex_num-1)+1)

#define CHECK_BIT(array, index)      (array[BLOCK_INDEX(index)] & MASK(index))
#define SET_BIT(array, index)        array[BLOCK_INDEX(index)] |=  MASK(index)
#define RESET_BIT(array, index)      array[BLOCK_INDEX(index)] &= ~MASK(index)

#define for_each_bit(counter, limit, index, array, actions) \
for(counter = 0; counter < limit; counter++) { \
    while (array[counter]) { \
        index = sizeof(block_t)*8*(counter + 1) - __builtin_clzl(array[counter]) - 1; \
        RESET_BIT(array, index); \
        actions \
    } \
}

typedef struct {
    block_t *color_mat;
    block_t *uncolored;
    int color_num;
    int uncolor_num;
    weight_t fitness;
} beam_individual_t;

typedef struct {
    block_t *edge_mat;
    weight_t *weights;
    int size;
} beam_graph_t;

struct crossover_param_s {
    const beam_graph_t *graph;
    int *target_color_count;
    int *child_count;
    pthread_mutex_t* mut;
    int population_size;
    beam_individual_t *population;
    beam_individual_t *child_buffer;
    block_t *used_parents;
};


#ifdef __cplusplus
extern "C"
{
#endif


/**
 * @brief randomly color the graph with max_color being the
 * upper bound of colors used.
 * 
 * @param size Size of the graph.
 * @param edges The edge matrix of the graph.
 * @param colors The result color matrix of the graph.
 * @param max_color The upper bound of colors to be used.
 */
void graph_color_random(
    int graph_size, 
    block_t *colors, 
    int max_color
);

void BEAM_count_conflicts(
    int graph_size, 
    const block_t color[], 
    const block_t *edges, 
    int conflict_count[]
);


/**
 * @brief Color the graph using the beam algorithm.
 * 
 * @param graph A graph instance.
 * @param population_size Size of the population.
 * @param target_color_num The maximum number of colors allowed.
 * @param max_gen_num The maximum number of iterations.
 * @param thread_num The number of parallel threads.
 * @param best_solution_time Output pointer to the time it took to find the last solution.
 * @param best_iteration Output pointer to the number of iterations it took to find the last solution.
 * @param solution Output pointer to the resulting solution instance.
 */
void graph_color_beam (
    const beam_graph_t *graph,
    int population_size,
    int target_color_num, 
    int max_gen_num, 
    int thread_num,
    beam_individual_t *solution
);


/**
 * @brief Get a random color that was not used previously in the 
 * used_color_list. When a color is returned, it is added to the 
 * used_color_list.
 * 
 * @param size Max number of colors.
 * @param colors_used Number of colors used.
 * @param used_color_list List of used colors.
 * @return If an unused color is found, return it. If all colors are 
 * used, return -1.
 */
int BEAM_get_rand_color(int max_color_num, int colors_used, block_t used_color_list[], unsigned int *seed);


/**
 * @brief Do a crossover operation between two parent colors to produce a 
 * new child color. Vertices are checked if they were used previously 
 * through used_vertex_list before being added to the child color.
 * 
 * @param graph A graph instance.
 * @param parent_color Array of pointers to two parents.
 * @param child_color Pointer to the child color.
 * @param pool The pool containing unallocated vertices.
 * @param pool_count Total number of vertices in the pool.
 * @param used_vertex_list List of used vertices.
 * @param used_vertex_count Pointer to number of used vertices.
 */
void BEAM_crossover(
    const beam_graph_t *graph,
    const block_t *parent_color[2],
    block_t child_color[],
    block_t pool[],
    block_t used_vertex_list[],
    int *pool_count,
    int *used_count,
    unsigned int *seed
);


/**
 * @brief Fix the conflicts inside a color.
 * 
 * @param graph A graph instance.
 * @param conflict_count An array of conflict counts for each vertex.
 * @param color Pointer to the color.
 * @param pool The pool containing unallocated vertices.
 * @param pool_count Total number of vertices in the pool.
 */
int BEAM_fix_conflicts(
    const beam_graph_t *graph,
    int conflict_count[],
    block_t color[],
    block_t pool[],
    unsigned int *seed
);


/**
 * @brief local_search previous colors of child until max_color and
 * try to place vertices from the pool to those colors.
 * 
 * @param graph A graph instance.
 * @param child A solution instance.
 * @param max_color A the maximum number of colors to scan starting from 0.
 * @param simple_flag When true, swaps are done one with 1 conflicts.
 * @param pool The pool containing unallocated vertices.
 * @param pool_count Total number of vertices in the pool.
 */
int BEAM_local_search(
    const beam_graph_t *graph,
    beam_individual_t *child,
    int max_color,
    bool simple_flag,
    block_t pool[],
    unsigned int *seed
);


/**
 * @brief Generates a new child solution by combining parent1 and parent2.
 * 
 * @param graph A graph instance.
 * @param parent1 A solution instance as the first parent.
 * @param parent2 A solution instance as the second parent.
 * @param target_color_count Target number of colors.
 * @param child Pointer to the output of the child solution.
 */
void generate_child (
    const beam_graph_t *graph,
    const beam_individual_t *parent1,
    const beam_individual_t *parent2, 
    block_t *used_color_list,
    block_t *used_vertex_list,
    block_t *pool,
    int target_color_count,
    beam_individual_t *child,
    unsigned int *seed
);


bool validate_colors(
    const beam_graph_t *graph,
    beam_individual_t *indiv
);


void* generator_thread(void *param);

#ifdef __cplusplus
}
#endif

#endif
