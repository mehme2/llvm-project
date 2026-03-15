#include "RegAllocBEAM.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void graph_color_beam(
    const beam_graph_t *graph,
    int population_size,
    int base_color_count, 
    int max_gen_num, 
    int thread_num,
    beam_individual_t *solution
) {
    beam_individual_t *population = malloc(population_size * sizeof(beam_individual_t)); 
    for (int i = 0; i < population_size; i++) {
        population[i].color_mat = calloc(base_color_count * TOTAL_BLOCK_NUM((size_t)graph->size), sizeof(block_t));
        population[i].uncolored = calloc(TOTAL_BLOCK_NUM((size_t)graph->size), sizeof(block_t));
        population[i].uncolor_num = base_color_count;
        population[i].color_num = base_color_count;
        population[i].fitness = __INT_MAX__;

        graph_color_random(graph->size, population[i].color_mat, base_color_count);
    }

    // Create and initialize the list of used parents.
    block_t used_parents[(TOTAL_BLOCK_NUM(population_size))];
    memset(used_parents, 0, (TOTAL_BLOCK_NUM(population_size))*sizeof(block_t));

    // Initialize thread parameters.
    int target_color = base_color_count;
    int gen_num = max_gen_num;

    pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
    struct crossover_param_s temp_param[thread_num]; 
    for (int i = 0; i < thread_num; i++) {
        temp_param[i] = (struct crossover_param_s){
            graph,
            &target_color,
            &gen_num,
            &mut,
            population_size,
            population,
            malloc(sizeof(beam_individual_t)),
            used_parents
        };

        temp_param[i].child_buffer->fitness = -1;
        temp_param[i].child_buffer->color_mat = malloc(target_color * TOTAL_BLOCK_NUM(graph->size) * sizeof(block_t));
        temp_param[i].child_buffer->uncolored = malloc(TOTAL_BLOCK_NUM(graph->size) * sizeof(block_t));
    }

    // Launch the generator threads.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_t thread_id[thread_num];
    for(int i = 0; i < thread_num; i++)
        pthread_create(&thread_id[i], &attr, generator_thread, &temp_param[i]);

    // Wait for all threads to return. 
    for(int i = 0; i < thread_num; i++) {
        pthread_join(thread_id[i], NULL);
        free(temp_param[i].child_buffer->color_mat);
        free(temp_param[i].child_buffer->uncolored);
        free(temp_param[i].child_buffer);
    }

    // Find the best solution in the population.
    int best_i = 0;
    for(int i = 0; i < population_size; i++)
        if (population[i].fitness < population[best_i].fitness || 
            (population[i].fitness == population[best_i].fitness && population[i].color_num <= population[best_i].color_num))
            best_i = i;

    // Return the best solution
    *solution = population[best_i];
    for(int i = 0; i < solution->color_num; i++)
        for(int j = 0; j < TOTAL_BLOCK_NUM(graph->size); j++)
            solution->color_mat[TOTAL_BLOCK_NUM(graph->size)*i + j] &= ~solution->uncolored[j];
    population[best_i].color_mat = malloc(1);
    population[best_i].uncolored = malloc(1);

    // Free allocated space.
    pthread_mutex_destroy(&mut);
    for(int i = 0; i < population_size; i++) {
        free(population[i].color_mat);
        free(population[i].uncolored);
    }
    free(population);
}

int BEAM_get_rand_color(int max_color_num, int colors_used, block_t used_color_list[], unsigned int *seed) {
    // There are no available colors.
    if(colors_used >= max_color_num) {
        return -1;

    // There are only 2 colors available, search for them linearly.
    } else if(colors_used > (max_color_num*0.9 - 1)) {
        for(int i = 0; i < max_color_num; i++) {
            if(!CHECK_BIT(used_color_list, i)) {
                SET_BIT(used_color_list, i);
                return i;
            }
        }
    }

    // Randomly try to select an available color.
    int temp;
    while(1) {
        temp = rand_r(seed)%max_color_num;
        if(!CHECK_BIT(used_color_list, temp)) {
            SET_BIT(used_color_list, temp);
            return temp;
        }
    }
}

int BEAM_fix_conflicts(
    const beam_graph_t *graph,
    int conflict_count[],
    block_t color[],
    block_t pool[],
    unsigned int *seed
) {
    block_t (*edge_matrix)[][TOTAL_BLOCK_NUM(graph->size)] = 
        (block_t (*)[][TOTAL_BLOCK_NUM(graph->size)])graph->edge_mat;

    block_t conflict_array[TOTAL_BLOCK_NUM(graph->size)];
    block_t interf_array[TOTAL_BLOCK_NUM(graph->size)];
    block_t scan_array[TOTAL_BLOCK_NUM(graph->size)];

    memset(interf_array, 0, TOTAL_BLOCK_NUM(graph->size) * sizeof(block_t));
    for (size_t i = 0; i < graph->size; i++)
        if (conflict_count[i] > 0)
            SET_BIT(interf_array, i);

    // Keep removing problematic vertices until all conflicts are gone.
    int i, worst_vert = 0, vert_block, pool_count = 0, id;
    block_t vert_mask;
    while(1) {
        // Find the vertex with the most conflicts.
        memcpy(scan_array, interf_array, TOTAL_BLOCK_NUM(graph->size) * sizeof(block_t));
        for_each_bit(i, TOTAL_BLOCK_NUM(graph->size), id, scan_array,
            if (conflict_count[worst_vert] < conflict_count[id] ||
                (conflict_count[worst_vert] == conflict_count[id] && 
                (graph->weights[worst_vert] > graph->weights[id] || 
                (graph->weights[worst_vert] == graph->weights[id] && rand_r(seed)%2)))
            ) {
                worst_vert = id;
            }
        )

        if(conflict_count[worst_vert] <= 0)
            return pool_count;

        for(i = 0; i < TOTAL_BLOCK_NUM(graph->size); i++)
            conflict_array[i] = (*edge_matrix)[worst_vert][i] & color[i];

        for_each_bit(i, TOTAL_BLOCK_NUM(graph->size), id, conflict_array,
            if (conflict_count[id] == 1)
                RESET_BIT(interf_array, id);
            else
                conflict_count[id]--;
        )

        // Remove the vertex.
        vert_mask = MASK(worst_vert);
        vert_block = BLOCK_INDEX(worst_vert);
        color[vert_block] &= ~vert_mask;
        pool[vert_block] |= vert_mask;

        RESET_BIT(interf_array, worst_vert);
        conflict_count[worst_vert] = 0;
        pool_count++;
    }
}

void BEAM_crossover(
    const beam_graph_t *graph,
    const block_t *parent_color[2],
    block_t child_color[],
    block_t pool[],
    block_t used_vertex_list[],
    int *pool_count,
    int *used_count,
    unsigned int *seed
) {
    // Merge the two colors
    for(int i = 0; i < (TOTAL_BLOCK_NUM(graph->size)); i++) {
        child_color[i] = (parent_color[0][i] | parent_color[1][i]) & ~(used_vertex_list[i]);
        used_vertex_list[i] |= child_color[i];
        *used_count += __builtin_popcountl(child_color[i]);
        child_color[i] |= pool[i];
    }

    memset(pool, 0, (TOTAL_BLOCK_NUM(graph->size))*sizeof(block_t));

    // Count conflicts.
    int conflict_counts[graph->size];
    BEAM_count_conflicts(
        graph->size,
        child_color,
        graph->edge_mat,
        conflict_counts
    );

    // Fix the conflicts.
    *pool_count = BEAM_fix_conflicts(
        graph,
        conflict_counts,
        child_color,
        pool,
        seed
    );
}

int BEAM_local_search(
    const beam_graph_t *graph,
    beam_individual_t *child,
    int max_color,
    bool simple_flag,
    block_t pool[],
    unsigned int *seed
) {
    block_t (*edge_matrix)[][TOTAL_BLOCK_NUM(graph->size)] = 
        (block_t (*)[][TOTAL_BLOCK_NUM(graph->size)])graph->edge_mat;

    block_t (*child_color_mat)[][TOTAL_BLOCK_NUM(graph->size)] = 
        (block_t (*)[][TOTAL_BLOCK_NUM(graph->size)])child->color_mat;

    int i, j, k, h, pool_id;
    block_t temp_pool[TOTAL_BLOCK_NUM(graph->size)];
    block_t scan_array[TOTAL_BLOCK_NUM(graph->size)];
    block_t conflict_array[TOTAL_BLOCK_NUM(graph->size)];
    int add_to_pool = 0;
    int conflict_id, conflict_block, conflict_count;
    bool place_flag;
    weight_t competition;

    // Search back and try placing vertices from the pool in the colors.
    memcpy(temp_pool, pool, TOTAL_BLOCK_NUM(graph->size) * sizeof(block_t));
    for_each_bit(i, TOTAL_BLOCK_NUM(graph->size), pool_id, temp_pool,
        // Loop through every color.
        for(j = 0; j < max_color; j++) {  
            // Count conflicts and calculate competition
            conflict_count = 0;
            for(k = 0; k < TOTAL_BLOCK_NUM(graph->size); k++) {
                conflict_array[k] = (*edge_matrix)[pool_id][k] & (*child_color_mat)[j][k];
                conflict_count += __builtin_popcountl(conflict_array[k]);
            }

            // /**
            //  * If the total competition is smaller than the weight
            //  * of the vertex in question, move all the conflicts to the 
            //  * pool, and place the vertex in the color.
            // */
            place_flag = true;
            if (simple_flag && conflict_count == 1) {
                conflict_id = 0;
                conflict_block = 0;
                for(k = 0; k < TOTAL_BLOCK_NUM(graph->size); k++) {
                    if(conflict_array[k]) {
                        conflict_id = sizeof(block_t)*8*(k + 1) - 1 - __builtin_clzl(conflict_array[k]);
                        conflict_block = k;
                        break;
                    }
                }

                if(graph->weights[pool_id] <= graph->weights[conflict_id]) {
                    place_flag = false;

                } else {
                    (*child_color_mat)[j][conflict_block] &= ~conflict_array[conflict_block];
                    pool[conflict_block] |= conflict_array[conflict_block];
                    temp_pool[conflict_block] |= conflict_array[conflict_block];
                    add_to_pool++;
                }

            } else if (!simple_flag && conflict_count > 0) {
                competition = 0;
                memcpy(scan_array, conflict_array, TOTAL_BLOCK_NUM(graph->size) * sizeof(block_t));
                for_each_bit(h, TOTAL_BLOCK_NUM(graph->size), conflict_id, scan_array,
                    competition += graph->weights[conflict_id];
                )

                if (graph->weights[pool_id] <= competition) {
                    place_flag = false;

                } else {
                    for(k = 0; k < TOTAL_BLOCK_NUM(graph->size); k++) {
                        (*child_color_mat)[j][k] &= ~conflict_array[k];
                        pool[k] |= conflict_array[k];
                        temp_pool[k] |= conflict_array[k];
                    }

                    add_to_pool += conflict_count;
                }

            } else if (conflict_count != 0) {
                place_flag = false;
            }

            if (place_flag) {
                SET_BIT((*child_color_mat)[j], pool_id);
                RESET_BIT(pool, pool_id);
                add_to_pool--;

                break;
            }
        }
    )

    return add_to_pool;
}

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
) {
    block_t (*child_color_mat)[][TOTAL_BLOCK_NUM(graph->size)] = 
        (block_t (*)[][TOTAL_BLOCK_NUM(graph->size)])child->color_mat;

    block_t (*used_color_list_p)[][TOTAL_BLOCK_NUM(target_color_count)] = 
        (block_t (*)[][TOTAL_BLOCK_NUM(target_color_count)])used_color_list;

    int color1, color2;
    const block_t *chosen_parent_colors[2];
    int pool_count = 0, used_count = 0;
    for(
        child->color_num = 0; 
        child->color_num < target_color_count; 
        child->color_num++
    ) {
        color1 = BEAM_get_rand_color(parent1->color_num, child->color_num, (*used_color_list_p)[0], seed);
        color2 = BEAM_get_rand_color(parent2->color_num, child->color_num, (*used_color_list_p)[1], seed);
        chosen_parent_colors[0] = &parent1->color_mat[color1*TOTAL_BLOCK_NUM(graph->size)];
        chosen_parent_colors[1] = &parent2->color_mat[color2*TOTAL_BLOCK_NUM(graph->size)];

        BEAM_crossover(
            graph,
            chosen_parent_colors,
            (*child_color_mat)[child->color_num],
            pool,
            used_vertex_list,
            &pool_count,
            &used_count,
            seed
        );

        pool_count += BEAM_local_search(
            graph,
            child,
            child->color_num,
            true,
            pool,
            seed
        );

        if(pool_count == 0 && used_count >= graph->size){
            child->color_num++;
            break;
        }
    }

    // If there are unseen vertices, drop them in the pool.
    for(int i = 0; i < (TOTAL_BLOCK_NUM(graph->size)); i++)
        pool[i] |= ~used_vertex_list[i];
    pool[TOTAL_BLOCK_NUM(graph->size)-1] &= 
        ((~((block_t)0)) >> (TOTAL_BLOCK_NUM(graph->size)*sizeof(block_t)*8 - graph->size));

    BEAM_local_search(
        graph,
        child,
        target_color_count,
        false,
        pool,
        seed
    );


    // Calculate the fitness while randomly allocating the remaining vertices in the colors.
    child->uncolor_num = 0;
    child->fitness = 0;

    int i, id, color_num;
    block_t scan_array[TOTAL_BLOCK_NUM(graph->size)];
    memcpy(scan_array, pool, TOTAL_BLOCK_NUM(graph->size) * sizeof(block_t));
    for_each_bit(i, TOTAL_BLOCK_NUM(graph->size), id, scan_array, 
        color_num = rand_r(seed)%target_color_count;
        SET_BIT((*child_color_mat)[color_num], id);

        if(color_num >= child->color_num)
            child->color_num = color_num + 1;

        child->fitness += graph->weights[id];
        child->uncolor_num++;
    )
}

void* generator_thread(void *param) {
    const beam_graph_t *graph        = ((struct crossover_param_s*)param)->graph;
    int *target_color_count             = ((struct crossover_param_s*)param)->target_color_count;
    int *gen_num                        = ((struct crossover_param_s*)param)->child_count;
    pthread_mutex_t* mut                = ((struct crossover_param_s*)param)->mut;
    int population_size                 = ((struct crossover_param_s*)param)->population_size;
    beam_individual_t *population    = ((struct crossover_param_s*)param)->population;
    beam_individual_t *child         = ((struct crossover_param_s*)param)->child_buffer;
    block_t *used_parents               = ((struct crossover_param_s*)param)->used_parents;


    unsigned int seed = (unsigned int)time(NULL);

    // list of used colors in the parents.
    block_t *used_color_list = 
        calloc(2, (TOTAL_BLOCK_NUM(*target_color_count))*sizeof(block_t));

    // list of used vertices in the parents.
    block_t *used_vertex_list = 
        calloc(1, (TOTAL_BLOCK_NUM(graph->size))*sizeof(block_t));


    int temp_target_color = *target_color_count;
    int parent1, parent2, bad_parent = -1;
    beam_individual_t temp_indiv;
    while(1) {

        pthread_mutex_lock(mut);

        if ((*gen_num) <= 0) {
            pthread_mutex_unlock(mut);
            break;
        } else {
            (*gen_num)--;
        }

        if (bad_parent != -1) {
            RESET_BIT(used_parents, parent1);
            RESET_BIT(used_parents, parent2);

            // Make the target harder if it was found.
            if(child->fitness == 0)
                (*target_color_count) = child->color_num-1;
            temp_target_color = *target_color_count;
        }

        // Pick 2 random parents
        do { parent1 = rand_r(&seed)%population_size; } while (CHECK_BIT(used_parents, parent1));
        SET_BIT(used_parents, parent1);
        do { parent2 = rand_r(&seed)%population_size; } while (CHECK_BIT(used_parents, parent2));
        SET_BIT(used_parents, parent2);

        pthread_mutex_unlock(mut);


        memset(used_color_list, 0, 2*(TOTAL_BLOCK_NUM(temp_target_color))*sizeof(block_t));
        memset(used_vertex_list,0,   (TOTAL_BLOCK_NUM(graph->size))*sizeof(block_t));
        memset(child->uncolored,0,   (TOTAL_BLOCK_NUM(graph->size))*sizeof(block_t));


        // Generate a child
        generate_child (
            graph,
            &population[parent1], 
            &population[parent2], 
            used_color_list,
            used_vertex_list,
            child->uncolored,
            temp_target_color,
            child,
            &seed
        );


        if(population[parent1].fitness <= population[parent2].fitness && population[parent1].color_num <= population[parent2].color_num)
            bad_parent = parent2;
        else
            bad_parent = parent1;

        // Replace a bad parent.
        if(child->color_num <= population[bad_parent].color_num && child->fitness <= population[bad_parent].fitness) {
            temp_indiv = population[bad_parent];
            population[bad_parent] = *child;
            *child = temp_indiv;
        }
    }

    free(used_color_list);
    free(used_vertex_list);

    return NULL;
}

bool validate_colors(
    const beam_graph_t *graph,
    beam_individual_t *indiv
) {
    block_t (*edge_mat)[][TOTAL_BLOCK_NUM(graph->size)] = 
        (block_t (*)[][TOTAL_BLOCK_NUM(graph->size)])graph->edge_mat;

    block_t (*color_mat)[][TOTAL_BLOCK_NUM(graph->size)] = 
        (block_t (*)[][TOTAL_BLOCK_NUM(graph->size)])indiv->color_mat;


    int vertex_counts[graph->size];
    memset(vertex_counts, 0, graph->size*sizeof(int));

    int i, id;
    weight_t calc_fitness = 0;
    block_t scan_array[TOTAL_BLOCK_NUM(graph->size)];
    memcpy(scan_array, indiv->uncolored, TOTAL_BLOCK_NUM(graph->size) * sizeof(block_t));
    for_each_bit(i, TOTAL_BLOCK_NUM(graph->size), id, scan_array, 
        calc_fitness += graph->weights[id];
        vertex_counts[id]++;
    )

    if(calc_fitness != indiv->fitness) {
        printf("Fitness does not match, given %f vs actual %f.\n", indiv->fitness, calc_fitness);
    }


    // Iterate through vertices.
    block_t conflict_array[TOTAL_BLOCK_NUM(graph->size)];
    memset(conflict_array, 0, TOTAL_BLOCK_NUM(graph->size)*sizeof(block_t));

    int j, k, id2;
    bool error_flag = false;
    for(i = 0; i < indiv->color_num; i++){
        memcpy(scan_array, (*color_mat)[i], TOTAL_BLOCK_NUM(graph->size) * sizeof(block_t));
        for_each_bit(j, TOTAL_BLOCK_NUM(graph->size), id, scan_array, 
            for (k = 0; k < TOTAL_BLOCK_NUM(graph->size); k++)
                conflict_array[k] = (*color_mat)[i][k] & (*edge_mat)[id][k];

            for_each_bit(k, TOTAL_BLOCK_NUM(graph->size), id2, conflict_array, 
                printf("The vertices %d and %d are connected and have the same color %d.\n", id, id2, i);
                error_flag = true;
            )
            
            vertex_counts[id]++;
        )
    }

    for(i = 0; i < graph->size; i++) {
        if(vertex_counts[i] > 1) {
            printf("The vertex %d was repeated %d times.\n", i, vertex_counts[i]);
            error_flag = true;
        } else if (vertex_counts[i] == 0) {
            printf("The vertex %d was not found.\n", i);
            error_flag = true;
        }
    }

    return !error_flag;
}

void graph_color_random (
    int graph_size, 
    block_t *colors, 
    int max_color
) {
    block_t (*color_mat)[][TOTAL_BLOCK_NUM(graph_size)] = 
        (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])colors;

    int index;
    for(int i = 0; i < graph_size; i++) {
        index = rand()%max_color;
        SET_BIT((*color_mat)[index], i);
    }
}

void BEAM_count_conflicts(
    int graph_size, 
    const block_t color[], 
    const block_t *edges, 
    int conflict_count[]
) {
    block_t (*edge_mat)[][TOTAL_BLOCK_NUM(graph_size)] = 
        (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])edges;

    int i, j;
    memset(conflict_count, 0, graph_size * sizeof(int));
    
    int id;
    block_t scan_array[TOTAL_BLOCK_NUM(graph_size)];
    memcpy(scan_array, color, TOTAL_BLOCK_NUM(graph_size) * sizeof(block_t));
    for_each_bit(i, TOTAL_BLOCK_NUM(graph_size), id, scan_array,
        for(j = 0; j < TOTAL_BLOCK_NUM(graph_size); j++)
            conflict_count[id] += __builtin_popcountl(color[j] & (*edge_mat)[id][j]);
    )
}
