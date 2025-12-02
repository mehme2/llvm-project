#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
//#include <sys/time.h>

#include "RegAllocBitEA.h"

#define _SORT_R_INLINE inline

#if (defined __APPLE__ || defined __MACH__ || defined __DARWIN__ || \
     (defined __FreeBSD__ && !defined(qsort_r)) || defined __DragonFly__)
#  define _SORT_R_BSD
#elif (defined __GLIBC__ || (defined (__FreeBSD__) && defined(qsort_r)))
#  define _SORT_R_LINUX
#elif (defined _WIN32 || defined _WIN64 || defined __WINDOWS__ || \
       defined __MINGW32__ || defined __MINGW64__)
#  define _SORT_R_WINDOWS
#  undef _SORT_R_INLINE
#  define _SORT_R_INLINE __inline
#else
  /* Using our own recursive quicksort sort_r_simple() */
#endif

#if (defined NESTED_QSORT && NESTED_QSORT == 0)
#  undef NESTED_QSORT
#endif

#define SORT_R_SWAP(a,b,tmp) ((tmp) = (a), (a) = (b), (b) = (tmp))

/* swap a and b */
/* a and b must not be equal! */
static _SORT_R_INLINE void sort_r_swap(char *__restrict a, char *__restrict b,
                                       size_t w)
{
  char tmp, *end = a+w;
  for(; a < end; a++, b++) { SORT_R_SWAP(*a, *b, tmp); }
}

/* swap a, b iff a>b */
/* a and b must not be equal! */
/* __restrict is same as restrict but better support on old machines */
static _SORT_R_INLINE int sort_r_cmpswap(char *__restrict a,
                                         char *__restrict b, size_t w,
                                         int (*compar)(const void *_a,
                                                       const void *_b,
                                                       void *_arg),
                                         void *arg)
{
  if(compar(a, b, arg) > 0) {
    sort_r_swap(a, b, w);
    return 1;
  }
  return 0;
}

/*
Swap consecutive blocks of bytes of size na and nb starting at memory addr ptr,
with the smallest swap so that the blocks are in the opposite order. Blocks may
be internally re-ordered e.g.

  12345ab  ->   ab34512
  123abc   ->   abc123
  12abcde  ->   deabc12
*/
static _SORT_R_INLINE void sort_r_swap_blocks(char *ptr, size_t na, size_t nb)
{
  if(na > 0 && nb > 0) {
    if(na > nb) { sort_r_swap(ptr, ptr+na, nb); }
    else { sort_r_swap(ptr, ptr+nb, na); }
  }
}

/* Implement recursive quicksort ourselves */
/* Note: quicksort is not stable, equivalent values may be swapped */
static _SORT_R_INLINE void sort_r_simple(void *base, size_t nel, size_t w,
                                         int (*compar)(const void *_a,
                                                       const void *_b,
                                                       void *_arg),
                                         void *arg)
{
  char *b = (char *)base, *end = b + nel*w;

  /* for(size_t i=0; i<nel; i++) {printf("%4i", *(int*)(b + i*sizeof(int)));}
  printf("\n"); */

  if(nel < 10) {
    /* Insertion sort for arbitrarily small inputs */
    char *pi, *pj;
    for(pi = b+w; pi < end; pi += w) {
      for(pj = pi; pj > b && sort_r_cmpswap(pj-w,pj,w,compar,arg); pj -= w) {}
    }
  }
  else
  {
    /* nel > 6; Quicksort */

    int cmp;
    char *pl, *ple, *pr, *pre, *pivot;
    char *last = b+w*(nel-1), *tmp;

    /*
    Use median of second, middle and second-last items as pivot.
    First and last may have been swapped with pivot and therefore be extreme
    */
    char *l[3];
    l[0] = b + w;
    l[1] = b+w*(nel/2);
    l[2] = last - w;

    /* printf("pivots: %i, %i, %i\n", *(int*)l[0], *(int*)l[1], *(int*)l[2]); */

    if(compar(l[0],l[1],arg) > 0) { SORT_R_SWAP(l[0], l[1], tmp); }
    if(compar(l[1],l[2],arg) > 0) {
      SORT_R_SWAP(l[1], l[2], tmp);
      if(compar(l[0],l[1],arg) > 0) { SORT_R_SWAP(l[0], l[1], tmp); }
    }

    /* swap mid value (l[1]), and last element to put pivot as last element */
    if(l[1] != last) { sort_r_swap(l[1], last, w); }

    /*
    pl is the next item on the left to be compared to the pivot
    pr is the last item on the right that was compared to the pivot
    ple is the left position to put the next item that equals the pivot
    ple is the last right position where we put an item that equals the pivot

                                           v- end (beyond the array)
      EEEEEELLLLLLLLuuuuuuuuGGGGGGGEEEEEEEE.
      ^- b  ^- ple  ^- pl   ^- pr  ^- pre ^- last (where the pivot is)

    Pivot comparison key:
      E = equal, L = less than, u = unknown, G = greater than, E = equal
    */
    pivot = last;
    ple = pl = b;
    pre = pr = last;

    /*
    Strategy:
    Loop into the list from the left and right at the same time to find:
    - an item on the left that is greater than the pivot
    - an item on the right that is less than the pivot
    Once found, they are swapped and the loop continues.
    Meanwhile items that are equal to the pivot are moved to the edges of the
    array.
    */
    while(pl < pr) {
      /* Move left hand items which are equal to the pivot to the far left.
         break when we find an item that is greater than the pivot */
      for(; pl < pr; pl += w) {
        cmp = compar(pl, pivot, arg);
        if(cmp > 0) { break; }
        else if(cmp == 0) {
          if(ple < pl) { sort_r_swap(ple, pl, w); }
          ple += w;
        }
      }
      /* break if last batch of left hand items were equal to pivot */
      if(pl >= pr) { break; }
      /* Move right hand items which are equal to the pivot to the far right.
         break when we find an item that is less than the pivot */
      for(; pl < pr; ) {
        pr -= w; /* Move right pointer onto an unprocessed item */
        cmp = compar(pr, pivot, arg);
        if(cmp == 0) {
          pre -= w;
          if(pr < pre) { sort_r_swap(pr, pre, w); }
        }
        else if(cmp < 0) {
          if(pl < pr) { sort_r_swap(pl, pr, w); }
          pl += w;
          break;
        }
      }
    }

    pl = pr; /* pr may have gone below pl */

    /*
    Now we need to go from: EEELLLGGGGEEEE
                        to: LLLEEEEEEEGGGG

    Pivot comparison key:
      E = equal, L = less than, u = unknown, G = greater than, E = equal
    */
    sort_r_swap_blocks(b, ple-b, pl-ple);
    sort_r_swap_blocks(pr, pre-pr, end-pre);

    /*for(size_t i=0; i<nel; i++) {printf("%4i", *(int*)(b + i*sizeof(int)));}
    printf("\n");*/

    sort_r_simple(b, (pl-ple)/w, w, compar, arg);
    sort_r_simple(end-(pre-pr), (pre-pr)/w, w, compar, arg);
  }
}


#if defined NESTED_QSORT

  static _SORT_R_INLINE void sort_r(void *base, size_t nel, size_t width,
                                    int (*compar)(const void *_a,
                                                  const void *_b,
                                                  void *aarg),
                                    void *arg)
  {
    int nested_cmp(const void *a, const void *b)
    {
      return compar(a, b, arg);
    }

    qsort(base, nel, width, nested_cmp);
  }

#else /* !NESTED_QSORT */

  /* Declare structs and functions */

  #if defined _SORT_R_BSD

    /* Ensure qsort_r is defined */
    extern void qsort_r(void *base, size_t nel, size_t width, void *thunk,
                        int (*compar)(void *_thunk,
                                      const void *_a, const void *_b));

  #endif

  #if defined _SORT_R_BSD || defined _SORT_R_WINDOWS

    /* BSD (qsort_r), Windows (qsort_s) require argument swap */

    struct sort_r_data
    {
      void *arg;
      int (*compar)(const void *_a, const void *_b, void *_arg);
    };

    static _SORT_R_INLINE int sort_r_arg_swap(void *s,
                                              const void *a, const void *b)
    {
      struct sort_r_data *ss = (struct sort_r_data*)s;
      return (ss->compar)(a, b, ss->arg);
    }

  #endif

  #if defined _SORT_R_LINUX

    typedef int(* __compar_d_fn_t)(const void *, const void *, void *);
    extern void (qsort_r)(void *base, size_t nel, size_t width,
                          __compar_d_fn_t __compar, void *arg)
      __attribute__((nonnull (1, 4)));

  #endif

  /* implementation */

  static _SORT_R_INLINE void sort_r(void *base, size_t nel, size_t width,
                                    int (*compar)(const void *_a,
                                                  const void *_b, void *_arg),
                                    void *arg)
  {
    #if defined _SORT_R_LINUX

      #if defined __GLIBC__ && ((__GLIBC__ < 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 8))

        /* no qsort_r in glibc before 2.8, need to use nested qsort */
        sort_r_simple(base, nel, width, compar, arg);

      #else

        qsort_r(base, nel, width, compar, arg);

      #endif

    #elif defined _SORT_R_BSD

      struct sort_r_data tmp;
      tmp.arg = arg;
      tmp.compar = compar;
      qsort_r(base, nel, width, &tmp, sort_r_arg_swap);

    #elif defined _SORT_R_WINDOWS

      struct sort_r_data tmp;
      tmp.arg = arg;
      tmp.compar = compar;
      qsort_s(base, nel, width, sort_r_arg_swap, &tmp);

    #else

      /* Fall back to our own quicksort implementation */
      sort_r_simple(base, nel, width, compar, arg);

    #endif
  }

#endif /* !NESTED_QSORT */

#undef _SORT_R_INLINE
#undef _SORT_R_WINDOWS
#undef _SORT_R_LINUX
#undef _SORT_R_BSD

#define qsort_r sort_r

char *
strtok_r (char *s, const char *delim, char **save_ptr)
{
  char *end;

  if (s == NULL)
    s = *save_ptr;

  if (*s == '\0')
    {
      *save_ptr = s;
      return NULL;
    }

  /* Scan leading delimiters.  */
  s += strspn (s, delim);
  if (*s == '\0')
    {
      *save_ptr = s;
      return NULL;
    }

  /* Find the end of the token.  */
  end = s + strcspn (s, delim);
  if (*end == '\0')
    {
      *save_ptr = end;
      return s;
    }

  /* Terminate the token and make *SAVE_PTR point past it.  */
  *end = '\0';
  *save_ptr = end + 1;
  return s;
}

int BitEA(
    int graph_size, 
    const block_t *edges, 
    int *weights, 
    int population_size,
    int base_color_count, 
    int max_gen_num, 
    block_t *best_solution, 
    int *best_fitness, 
    float *best_solution_time,
    int *uncolored_num
) {
    // Create the random population.
    block_t *population[population_size];
    int color_count[population_size];
    int uncolored[population_size];
    int fitness[population_size];
    for (int i = 0; i < population_size; i++) {
        population[i] = calloc(base_color_count, TOTAL_BLOCK_NUM(graph_size) * sizeof(block_t));
        uncolored[i] = base_color_count;
        color_count[i] = base_color_count;
        fitness[i] = __INT_MAX__;
    }

    pop_complex_random(
        graph_size,
        edges,
        weights,
        population_size,
        population,
        base_color_count
    );

    //struct timeval t1, t2;
    *best_solution_time = 0;
    //gettimeofday(&t1, NULL);

    block_t *child = malloc(base_color_count * TOTAL_BLOCK_NUM(graph_size) * sizeof(block_t));
    int best_i = 0;
    int target_color = base_color_count;
    int temp_uncolored;
    int parent1, parent2, child_colors, temp_fitness;
    int bad_parent;
    for(int i = 0; i < max_gen_num; i++) {
        if(target_color == 0)
            break;

        // Initialize the child
        memset(child, 0, (TOTAL_BLOCK_NUM(graph_size))*base_color_count*sizeof(block_t));

        // Pick 2 random parents
        parent1 = rand()%population_size;
        do { parent2 = rand()%population_size; } while (parent2 == parent1);

        // Do a crossover
        temp_fitness = crossover (
            graph_size, 
            edges, 
            weights,
            color_count[parent1], 
            color_count[parent2], 
            population[parent1], 
            population[parent2], 
            target_color,
            child, 
            &child_colors,
            &temp_uncolored
        );

        // Choose the bad parent.
        if(fitness[parent1] <= fitness[parent2] && color_count[parent1] <= color_count[parent2])
            bad_parent = parent2;
        else
            bad_parent = parent1;

        // Replace the bad parent if needed.
        if(child_colors <= color_count[bad_parent] && temp_fitness <= fitness[bad_parent]) {
            memmove(population[bad_parent], child, (TOTAL_BLOCK_NUM(graph_size))*base_color_count*sizeof(block_t));
            color_count[bad_parent] = child_colors;
            fitness[bad_parent] = temp_fitness;
            uncolored[bad_parent] = temp_uncolored;

            if (temp_fitness < fitness[best_i] ||
                (temp_fitness == fitness[best_i] && child_colors < color_count[best_i])
            ) {
                best_i = bad_parent;
                //gettimeofday(&t2, NULL);
                //*best_solution_time = (t2.tv_sec - t1.tv_sec) + (t2.tv_usec - t1.tv_usec) / 1000000.0;   // us to ms
            }
        }

        // Make the target harder if it was found.
        if(temp_fitness == 0)
            target_color = child_colors - 1;
    }

    // Return the best solution
    *best_fitness = fitness[best_i];
    *uncolored_num = uncolored[best_i];
    memcpy(best_solution, population[best_i], base_color_count * (TOTAL_BLOCK_NUM(graph_size)) * sizeof(block_t));

    // Free allocated space.
    free(child);
    for(int i = 0; i < population_size; i++)
        free(population[i]);

    return color_count[best_i];
}

int get_rand_color(int max_color_num, int colors_used, block_t used_color_list[]) {
    // There are no available colors.
    if(colors_used >= max_color_num) {
        return -1;

    // There are only 2 colors available, search for them linearly.
    } else if(colors_used > max_color_num - 2) {
        for(int i = 0; i < max_color_num; i++) {
            if(!(used_color_list[BLOCK_INDEX(i)] & MASK(i))) {
                used_color_list[BLOCK_INDEX(i)] |= MASK(i);
                return i;
            }
        }
    }

    // Randomly try to select an available color.
    int temp;
    while(1) {
        temp = rand()%max_color_num;
        if(!(used_color_list[BLOCK_INDEX(temp)] & MASK(temp))) {
            used_color_list[BLOCK_INDEX(temp)] |= MASK(temp);
            return temp;
        }
    }
}

void fix_conflicts(
    int graph_size,
    const block_t *edges, 
    const int *weights,
    int *conflict_count,
    int *total_conflicts,
    block_t *color,
    block_t *pool,
    int *pool_total
) {
    block_t (*edges_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])edges;

    // Keep removing problematic vertices until all conflicts are gone.
    int i, worst_vert = 0, vert_block;
    block_t vert_mask;
    while(*total_conflicts > 0) {
        // Find the vertex with the most conflicts.
        for(i = 0; i < graph_size; i++) {
            if (CHECK_COLOR(color, i) &&
                (conflict_count[worst_vert] < conflict_count[i] ||
                 (conflict_count[worst_vert] == conflict_count[i] && 
                  (weights[worst_vert] > weights[i] || (weights[worst_vert] == weights[i] && rand()%2))))) {
                worst_vert = i;
            }
        }

        // Update other conflict counters.
        vert_mask = MASK(worst_vert);
        vert_block = BLOCK_INDEX(worst_vert);
        for(i = 0; i < graph_size; i++)
            if(CHECK_COLOR(color, i) && ((*edges_p)[i][vert_block] & vert_mask))
                conflict_count[i]--;

        // Remove the vertex.
        color[vert_block] &= ~vert_mask;
        pool[vert_block] |= vert_mask;
        (*pool_total)++;

        // Update the total number of conflicts.
        (*total_conflicts) -= conflict_count[worst_vert];
        conflict_count[worst_vert] = 0;
    }
}

void merge_and_fix(
    int graph_size,
    const block_t *edges, 
    const int *weights,
    const block_t **parent_color,
    block_t *child_color,
    block_t *pool,
    int *pool_count,
    block_t *used_vertex_list,
    int *used_vertex_count
) {
    // Merge the two colors
    int temp_v_count = 0;  
    if(parent_color[0] != NULL && parent_color[1] != NULL)
        for(int i = 0; i < (TOTAL_BLOCK_NUM(graph_size)); i++) {
            child_color[i] = ((parent_color[0][i] | parent_color[1][i]) & ~(used_vertex_list[i]));
            temp_v_count += popcountl(child_color[i]);
        }

    else if(parent_color[0] != NULL)
        for(int i = 0; i < (TOTAL_BLOCK_NUM(graph_size)); i++) {
            child_color[i] = (parent_color[0][i] & ~(used_vertex_list[i]));
            temp_v_count += popcountl(child_color[i]);
        }

    else if(parent_color[1] != NULL)
        for(int i = 0; i < (TOTAL_BLOCK_NUM(graph_size)); i++) {
            child_color[i] = (parent_color[1][i] & ~(used_vertex_list[i]));
            temp_v_count += popcountl(child_color[i]);
        }

    (*used_vertex_count) += temp_v_count;

    // Merge the pool with the new color
    for(int i = 0; i < (TOTAL_BLOCK_NUM(graph_size)); i++) {
        child_color[i] |= pool[i];
        used_vertex_list[i] |= child_color[i];
    }

    memset(pool, 0, (TOTAL_BLOCK_NUM(graph_size))*sizeof(block_t));
    (*pool_count) = 0;


    // List of conflict count per vertex.
    int conflict_count[graph_size];
    memset(conflict_count, 0, graph_size*sizeof(int));

    // Count conflicts.
    int total_conflicts = count_conflicts(
        graph_size,
        child_color,
        edges,
        conflict_count
    );

    // Fix the conflicts.
    fix_conflicts(
        graph_size,
        edges,
        weights,
        conflict_count,
        &total_conflicts,
        child_color,
        pool,
        pool_count
    );
}

void search_back(
    int graph_size,
    const block_t *edges, 
    const int *weights,
    block_t *child, 
    int color_count,
    block_t *pool,
    int *pool_count
) {
    block_t (*edges_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])edges;
    block_t (*child_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])child;

    int conflict_count, last_conflict, last_conflict_block = 0;
    block_t i_mask, temp_mask, last_conflict_mask = 0;
    int i, j, k, i_block;

    // Search back and try placing vertices from the pool in previous colors.
    for(i = 0; i < graph_size && (*pool_count) > 0; i++) {
        i_block = BLOCK_INDEX(i);
        i_mask = MASK(i);

        // Check if the vertex is in the pool.
        if(pool[i_block] & i_mask) {
            // Loop through every previous color.
            for(j = 0; j < color_count; j++) {
                // Count the possible conflicts in this color.
                conflict_count = 0;
                for(k = 0; k < TOTAL_BLOCK_NUM(graph_size); k++) {
                    temp_mask = (*child_p)[j][k] & (*edges_p)[i][k];
                    if(temp_mask) {
                        conflict_count += popcountl(temp_mask);
                        if(conflict_count > 1)
                            break;
                        last_conflict = sizeof(block_t)*8*(k + 1) - 1 - __builtin_clzl(temp_mask);
                        last_conflict_mask = temp_mask;
                        last_conflict_block = k;
                    }
                }

                // Place immediately if there are no conflicts.
                if(conflict_count == 0) {
                    (*child_p)[j][i_block] |= i_mask;
                    pool[i_block] &= ~i_mask;
                    (*pool_count)--;
                    break;

                // If only 1 conflict exists and its weight is smaller
                // than that of the vertex in question, replace it.
                } else if (conflict_count == 1 && weights[last_conflict] < weights[i]) {
                    (*child_p)[j][i_block] |= i_mask;
                    pool[i_block] &= ~i_mask;

                    (*child_p)[j][last_conflict_block] &= ~last_conflict_mask;
                    pool[last_conflict_block] |= last_conflict_mask;
                    break;
                }
            }
        }
    }
}

void local_search(
    int graph_size,
    const block_t *edges, 
    const int *weights,
    block_t *child, 
    int color_count,
    block_t *pool,
    int *pool_count
) {
    block_t (*edges_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])edges;
    block_t (*child_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])child;

    int i, j, k, h, i_block;
    block_t i_mask, temp_mask;
    int competition;
    int conflict_count;
    block_t conflict_array[TOTAL_BLOCK_NUM(graph_size)];

    // Search back and try placing vertices from the pool in the colors.
    for(i = 0; i < graph_size && (*pool_count) > 0; i++) {
        i_block = BLOCK_INDEX(i);
        i_mask = MASK(i);

        // Check if the vertex is in the pool.
        if(pool[i_block] & i_mask) {
            // Loop through every color.
            for(j = 0; j < color_count; j++) {  
                // Count conflicts and calculate competition
                conflict_count = 0;
                competition = 0;
                for(k = 0; k < TOTAL_BLOCK_NUM(graph_size); k++) {
                    conflict_array[k] = (*edges_p)[i][k] & (*child_p)[j][k];
                    if(conflict_array[k]) {
                        temp_mask = conflict_array[k];
                        conflict_count += popcountl(temp_mask);
                        for(h = 0; h < sizeof(block_t)*8; h++)
                            if((temp_mask >> h) & (block_t)1)
                                competition += weights[k*8*sizeof(block_t)+h];
                    }
                }

                // Place immediately if there are no conflicts.
                if(competition == 0) {
                    (*child_p)[j][i_block] |= i_mask;
                    pool[i_block] &= ~i_mask;
                    (*pool_count) += conflict_count - 1;
                    break;

                /**
                 * If the total competition is smaller than the weight
                 * of the vertex in question, move all the conflicts to the 
                 * pool, and place the vertex in the color.
                */
                } else if(competition < weights[i]) {
                    for(k = 0; k < TOTAL_BLOCK_NUM(graph_size); k++) {
                        (*child_p)[j][k] &= ~conflict_array[k];
                        pool[k] |= conflict_array[k];
                    }

                    (*child_p)[j][i_block] |= i_mask;
                    pool[i_block] &= ~i_mask;
                    (*pool_count) += conflict_count - 1;
                    break;
                }
            }
        }
    }
}

int crossover (
    int graph_size, 
    const block_t *edges, 
    const int *weights,
    int color_num1, 
    int color_num2, 
    const block_t *parent1, 
    const block_t *parent2, 
    int target_color_count,
    block_t *child,
    int *child_color_count,
    int *uncolored
) {
    const block_t (*parent1_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])parent1;
    const block_t (*parent2_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])parent2;
    block_t (*child_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])child;

    // max number of colors of the two parents.
    int max_color_num = color_num1 > color_num2 ? color_num1 : color_num2;

    // list of used colors in the parents.
    block_t used_color_list[2][TOTAL_BLOCK_NUM(max_color_num)];
    memset(used_color_list, 0, 2*TOTAL_BLOCK_NUM(max_color_num)*sizeof(block_t));

    // list of used vertices in the parents.
    block_t used_vertex_list[TOTAL_BLOCK_NUM(graph_size)];
    memset(used_vertex_list, 0, (TOTAL_BLOCK_NUM(graph_size))*sizeof(block_t));
    int used_vertex_count = 0;

    // Pool.
    block_t pool[TOTAL_BLOCK_NUM(graph_size)];
    memset(pool, 0, (TOTAL_BLOCK_NUM(graph_size))*sizeof(block_t));
    int pool_count = 0;


    int color1, color2, last_color = 0;
    int i, j;
    const block_t *chosen_parent_colors[2];
    for(i = 0; i < target_color_count; i++) {
        // The child still has vertices that weren't used.
        if(used_vertex_count < graph_size) {
            // Pick 2 random colors.
            color1 = get_rand_color(color_num1, i, used_color_list[0]);
            color2 = get_rand_color(color_num2, i, used_color_list[1]);
            chosen_parent_colors[0] = color1 == -1 ? NULL : (*parent1_p)[color1];
            chosen_parent_colors[1] = color2 == -1 ? NULL : (*parent2_p)[color2];

            merge_and_fix(
                graph_size,
                edges,
                weights,
                chosen_parent_colors,
                (*child_p)[i],
                pool,
                &pool_count,
                used_vertex_list,
                &used_vertex_count
            );

        // If all of the vertices were used and the pool is empty, exit the loop.
        } else if(pool_count == 0) {
            break;
        }

        search_back(
            graph_size,
            edges,
            weights,
            child, 
            i,
            pool,
            &pool_count
        );
    }

    // Record the last color.
    last_color = i;


    // If not all the vertices were visited, drop them in the pool.
    if(used_vertex_count < graph_size) {
        for(j = 0; j < (TOTAL_BLOCK_NUM(graph_size)); j++)
            pool[j] |= ~used_vertex_list[j];
        pool[TOTAL_BLOCK_NUM(graph_size)] &= ((0xFFFFFFFFFFFFFFFF) >> (TOTAL_BLOCK_NUM(graph_size)*sizeof(block_t)*8 - graph_size));

        pool_count += (graph_size - used_vertex_count);
        used_vertex_count = graph_size;
        memset(used_vertex_list, 0xFF, (TOTAL_BLOCK_NUM(graph_size))*sizeof(block_t));
    }

    local_search(
        graph_size,
        edges,
        weights,
        child,
        target_color_count,
        pool,
        &pool_count
    );

    // If the pool is not empty, randomly allocate the remaining vertices in the colors.
    int fitness = 0, temp_block;
    block_t temp_mask;
    if(pool_count > 0) {
        int color_num;
        for(i = 0; i < graph_size; i++) {
            temp_block = BLOCK_INDEX(i);
            temp_mask = MASK(i);
            if(pool[temp_block] & temp_mask) {
                color_num = rand()%target_color_count;
                (*child_p)[color_num][temp_block] |= temp_mask;

                if(color_num + 1 > last_color)
                    last_color = color_num + 1;

                fitness += weights[i];
            }
        }

    // All of the vertices were allocated and no conflicts were detected.
    } else {
        fitness = 0;
    }

    *uncolored = pool_count;
    *child_color_count = last_color;
    return fitness;
}

int comp_crit_1(const void* a, const void* b, void* metrics) {
    int* weights = ((int**)metrics)[0];
    int* degrees = ((int**)metrics)[1];
    return (weights[*(int*)a] * degrees[*(int*)a]) - (weights[*(int*)b] * degrees[*(int*)b]);
}

int comp_crit_2(const void* a, const void* b, void* metrics) {
    int* weights = ((int**)metrics)[0];
    int* degrees = ((int**)metrics)[1];
    return (weights[*(int*)a] * degrees[*(int*)a] * degrees[*(int*)a]) - (weights[*(int*)b] * degrees[*(int*)b] * degrees[*(int*)b]);
}

int comp_crit_3(const void* a, const void* b, void* weights) {
    return (((int*)weights)[*(int*)a]) - (((int*)weights)[*(int*)b]);
}

void pop_complex_random (
    int graph_size, 
    const block_t *edges, 
    const int *weights,
    int pop_size,
    block_t **population, 
    int max_color
) {
    block_t (*edges_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])edges;

    int criteria[3][graph_size];
    for (int i = 0; i < graph_size; i++) {
        criteria[0][i] = i;
        criteria[1][i] = i;
        criteria[2][i] = i;
    }

    int degrees[graph_size];
    count_edges(graph_size, edges, degrees);

    const int* metrics[2] = {weights, degrees};

    qsort_r(criteria[0], graph_size, sizeof(int), comp_crit_1, (void*)metrics);
    qsort_r(criteria[1], graph_size, sizeof(int), comp_crit_2, (void*)metrics);
    qsort_r(criteria[2], graph_size, sizeof(int), comp_crit_3, (void*)weights);

    // Go through the queue and color each vertex.
    block_t adjacent_colors[TOTAL_BLOCK_NUM(graph_size)];
    block_t (*indiv)[][TOTAL_BLOCK_NUM(graph_size)];
    int current_vert;
    int i, j, k;
    for (int indiv_id = 0; indiv_id < pop_size; indiv_id++) {
        indiv = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])population[indiv_id];

        memset(indiv, 0, max_color * TOTAL_BLOCK_NUM(graph_size) * sizeof(block_t));

        for(i = 0; i < graph_size; i++) {
            if (indiv_id < 40)
                current_vert = criteria[0][i];
            else if (indiv_id < 80)
                current_vert = criteria[1][i];
            else
                current_vert = criteria[2][i];

            // Initialize the temporary data.
            memset(adjacent_colors, 0, (TOTAL_BLOCK_NUM(max_color))*sizeof(block_t));
            for(j = 0; j < TOTAL_BLOCK_NUM(graph_size); j++) {
                for(k = 0; k < max_color; k++) {
                    // SET_COLOR(adjacent_colors, edges[current_vert][j] & (*indiv)[k][j] & 1);
                    if ((*edges_p)[current_vert][j] & (*indiv)[k][j]) {
                        SET_COLOR(adjacent_colors, k);
                        break;
                    }
                }
            }

            // Find the first unused color (starting from 0) and assign this vertex to it.
            for(j = 0; j < max_color; j++) {
                if(!CHECK_COLOR(adjacent_colors, j)) {
                    SET_COLOR((*indiv)[j], current_vert);
                    break;
                }
            }

            if (j == max_color)
                SET_COLOR((*indiv)[rand()%max_color], current_vert);
        }
    }
}


bool read_graph (
    const char* filename, 
    int graph_size, 
    block_t *edges, 
    int offset_i
) {
    block_t (*edges_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])edges;
    FILE *fp = fopen(filename, "r");
    
    if(fp == NULL)
        return false;

    memset(edges, 0, graph_size*TOTAL_BLOCK_NUM(graph_size)*sizeof(block_t));

    char buffer[64];
    char *token, *saveptr;
    int row, column;
    while(fgets(buffer, 64, fp) != NULL) {
        buffer[strcspn(buffer, "\n")] = 0;

        token = strtok_r (buffer, " ", &saveptr);
        if(saveptr[0] == 0) 
            break;
        row = atoi(token) + offset_i;
        token = strtok_r (NULL, " ", &saveptr);
        column = atoi(token) + offset_i;

        SET_EDGE(row, column, (*edges_p));
    }
    
    fclose(fp);
    return true;
}


bool read_weights(const char* filename, int graph_size, int weights[]) {
    FILE *fp = fopen(filename, "r");
    
    if(fp == NULL)
        return false;

    memset(weights, 0, graph_size * sizeof(int));

    char buffer[64];
    int vertex = 0;
    while(fgets(buffer, 64, fp) != NULL && vertex < graph_size) {
        buffer[strcspn(buffer, "\n")] = 0;
        weights[vertex] = atoi(buffer);
        vertex++;
    }
    
    fclose(fp);
    return true;
}


bool is_valid(
    int graph_size, 
    const block_t *edges, 
    int color_num, 
    const block_t *colors
) {
    const block_t (*edges_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])edges;
    const block_t (*colors_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])colors;

    // Iterate through vertices.
    int i, j, k, i_block;
    block_t i_mask;
    bool vertex_is_colored, error_flag = false, over_colored;
    for(i = 0; i < graph_size; i++) {
        vertex_is_colored = false;
        over_colored = false;
        i_block = BLOCK_INDEX(i);
        i_mask = MASK(i);

        // Iterate through colors and look for the vertex.
        for(j = 0; j < color_num; j++){
            if(((*colors_p)[j][i_block] & i_mask)) {
                if(!vertex_is_colored) {
                    vertex_is_colored = true;

                } else {
                    over_colored = true;
                }

                for(k = i + 1; k < graph_size; k++) { // Through every vertex after i in color j.
                    if(CHECK_COLOR((*colors_p)[j], k) && ((*edges_p)[k][i_block] & i_mask)) {
                        // The two vertices have the same color.
                        printf("The vertices %d and %d are connected and have the same color %d.\n", i, k, j);
                        error_flag = true;
                    }
                }
            }
        }

        // Check if the vertex had more then one color.
        if(!vertex_is_colored) {
            printf("The vertex %d has no color.\n", i);
            error_flag = true;
        }

        // Check if the vertex had more then one color.
        if(over_colored) {
            printf("The vertex %d has more than one color.\n", i);
            error_flag = true;
        }
    }

    return !error_flag;
}


int count_edges(int graph_size, const block_t *edges, int degrees[]) {
    const block_t (*edges_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])edges;
    memset(degrees, 0, graph_size*sizeof(int));

    int i, j, total = 0;
    for(i = 0; i < graph_size; i++) {
        for(j = 0; j < TOTAL_BLOCK_NUM(graph_size); j++)
            degrees[i] += popcountl((*edges_p)[i][j]);
        total += degrees[i];
    }

    return total;
}


void print_colors(
    const char *filename, 
    const char *header, 
    int color_num, 
    int graph_size, 
    const block_t *colors
) {
    const block_t (*colors_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])colors;

    FILE* fresults;
    fresults = fopen(filename, "w");

    if(!fresults) {
        printf("%s\ncould not print results, aborting ...\n", strerror(errno));
        return;
    }

    fprintf(fresults, "%s\n\n", header);

    for(int i = 0; i < color_num; i++)
        for(int j = 0; j < graph_size; j++)
            if(CHECK_COLOR((*colors_p)[i], j)) 
                fprintf(fresults, "%d %d\n", i, j);

    fclose(fresults);
}


bool exists(int* arr, int len, int target) {
    for(int i = 0; i < len; i++)
        if(target == arr[i]) return true;

    return false;
}


int graph_color_greedy(
    int graph_size, 
    const block_t edges[][TOTAL_BLOCK_NUM(graph_size)], 
    block_t colors[][TOTAL_BLOCK_NUM(graph_size)], 
    int max_color_possible
) {
    // Go through the queue and color each vertex.
    int prob_queue[graph_size];
    block_t adjacent_colors[TOTAL_BLOCK_NUM(max_color_possible)];
    int max_color = 0, current_vert;
    int i, j, k;
    for(i = 0; i < graph_size; i++) {
        // Get a new random vertex.
        do { prob_queue[i] = rand()%graph_size; } while(exists(prob_queue, i, prob_queue[i]));
        current_vert = prob_queue[i];

        // Initialize the temporary data.
        memset(adjacent_colors, 0, (TOTAL_BLOCK_NUM(max_color_possible))*sizeof(block_t));
        for(j = 0; j < TOTAL_BLOCK_NUM(graph_size); j++)
            for(k = 0; k < max_color_possible; k++)
                if((edges[current_vert][j] & colors[k][j]))
                    SET_COLOR(adjacent_colors, k);

        // Find the first unused color (starting from 0) and assign this vertex to it.
        for(j = 0; j < max_color_possible; j++) {
            if(!CHECK_COLOR(adjacent_colors, j)) {
                SET_COLOR(colors[j], current_vert);
                if(max_color < j) 
                    max_color = j;
                break;
            }
        }
    }

    return max_color + 1;
}

int count_conflicts(
    int graph_size, 
    const block_t *color, 
    const block_t *edges, 
    int *conflict_count
) {
    block_t (*edges_p)[][TOTAL_BLOCK_NUM(graph_size)] = (block_t (*)[][TOTAL_BLOCK_NUM(graph_size)])edges;

    int i, j, total_conflicts = 0;
    for(i = 0; i < graph_size; i++) {
        if(CHECK_COLOR(color, i)) {
            conflict_count[i] = 0;
            for(j = 0; j < TOTAL_BLOCK_NUM(graph_size); j++)
                conflict_count[i] += popcountl(color[j] & (*edges_p)[i][j]);
            total_conflicts += conflict_count[i];
        }
    }

    return total_conflicts/2;
}

int popcountl(uint64_t n) {
    int cnt = 0;
    while (n) {
        n &= n - 1; // key point
        ++cnt;
    }
    return cnt;
}
