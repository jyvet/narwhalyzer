/*
 * narwhalyzer.c
 * 
 * Runtime support implementation for the Narwhalyzer GCC instrumentation plugin.
 * Provides section tracking, timing, and profiling report generation.
 * 
 * Copyright (c) 2026
 * Licensed under GPL-3.0 License
 */

#define _GNU_SOURCE
#include "narwhalyzer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>

/* ============================================================================
 * Global State
 * ============================================================================ */

/* Array of all registered sections */
static narwhalyzer_section_stats_t g_sections[NARWHALYZER_MAX_SECTIONS];
static atomic_int g_section_count = 0;

/* Thread-local context stack for tracking nested sections */
static __thread narwhalyzer_context_t g_context_stack[NARWHALYZER_MAX_NESTING_DEPTH];
static __thread int g_context_stack_top = -1;

/* Initialization flag */
static atomic_int g_initialized = 0;

/* Total program time tracking */
static uint64_t g_program_start_time_ns = 0;
static uint64_t g_program_end_time_ns = 0;

/* Mutex for section registration (only used when atomics aren't sufficient) */
static pthread_mutex_t g_registration_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Report already printed flag */
static atomic_int g_report_printed = 0;

/* ============================================================================
 * Internal Utilities
 * ============================================================================ */

/*
 * Get high-resolution monotonic timestamp in nanoseconds.
 */
uint64_t __narwhalyzer_get_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * Compare function for sorting sections by cumulative time (descending).
 */
static int compare_sections_by_time(const void *a, const void *b)
{
    const int *ia = (const int *)a;
    const int *ib = (const int *)b;
    
    uint64_t time_a = g_sections[*ia].cumulative_time_ns;
    uint64_t time_b = g_sections[*ib].cumulative_time_ns;
    
    if (time_b > time_a) return 1;
    if (time_b < time_a) return -1;
    return 0;
}

/*
 * Format time duration for display.
 */
static void format_time(uint64_t ns, char *buf, size_t buf_size)
{
    if (ns >= 1000000000ULL) {
        snprintf(buf, buf_size, "%.3f s", (double)ns / 1e9);
    } else if (ns >= 1000000ULL) {
        snprintf(buf, buf_size, "%.3f ms", (double)ns / 1e6);
    } else if (ns >= 1000ULL) {
        snprintf(buf, buf_size, "%.3f us", (double)ns / 1e3);
    } else {
        snprintf(buf, buf_size, "%lu ns", (unsigned long)ns);
    }
}

/*
 * Print a horizontal line for the table.
 */
static void print_table_separator(int name_width)
{
    printf("+");
    for (int i = 0; i < name_width + 2; i++) printf("-");
    printf("+");
    for (int i = 0; i < 12; i++) printf("-"); /* Entry Count */
    printf("+");
    for (int i = 0; i < 14; i++) printf("-"); /* Cumulative */
    printf("+");
    for (int i = 0; i < 14; i++) printf("-"); /* Mean */
    printf("+");
    for (int i = 0; i < 14; i++) printf("-"); /* Min */
    printf("+");
    for (int i = 0; i < 14; i++) printf("-"); /* Max */
    printf("+");
    for (int i = 0; i < 10; i++) printf("-"); /* Percent */
    printf("+\n");
}

/*
 * Build hierarchy information from parent indices.
 */
typedef struct hierarchy_node {
    int section_index;
    int *children;
    int child_count;
    int child_capacity;
} hierarchy_node_t;

static void add_child(hierarchy_node_t *node, int child_idx)
{
    if (node->child_count >= node->child_capacity) {
        node->child_capacity = node->child_capacity ? node->child_capacity * 2 : 4;
        node->children = realloc(node->children, node->child_capacity * sizeof(int));
    }
    node->children[node->child_count++] = child_idx;
}

/*
 * Print hierarchy tree recursively.
 */
static void print_hierarchy_recursive(hierarchy_node_t *nodes, int idx, 
                                       const char *prefix, int is_last)
{
    narwhalyzer_section_stats_t *s = &g_sections[idx];
    char time_buf[32];
    format_time(s->cumulative_time_ns, time_buf, sizeof(time_buf));
    
    printf("%s%s%s (%s)\n",
           prefix,
           is_last ? "└── " : "├── ",
           s->name,
           time_buf);
    
    /* Build new prefix for children */
    size_t prefix_len = strlen(prefix);
    char *new_prefix = malloc(prefix_len + 8);
    strcpy(new_prefix, prefix);
    strcat(new_prefix, is_last ? "    " : "│   ");
    
    hierarchy_node_t *node = &nodes[idx];
    for (int i = 0; i < node->child_count; i++) {
        print_hierarchy_recursive(nodes, node->children[i], 
                                   new_prefix, i == node->child_count - 1);
    }
    
    free(new_prefix);
}

/*
 * Print the flat summary table.
 */
static void print_flat_summary(int section_count, uint64_t total_time_ns)
{
    if (section_count == 0) {
        printf("No instrumented sections were executed.\n");
        return;
    }
    
    /* Create sorted index array */
    int *sorted_indices = malloc(section_count * sizeof(int));
    for (int i = 0; i < section_count; i++) {
        sorted_indices[i] = i;
    }
    qsort(sorted_indices, section_count, sizeof(int), compare_sections_by_time);
    
    /* Calculate maximum name width */
    int max_name_width = 12; /* Minimum width for "Section Name" */
    for (int i = 0; i < section_count; i++) {
        int len = strlen(g_sections[i].name);
        if (len > max_name_width) max_name_width = len;
    }
    if (max_name_width > 40) max_name_width = 40; /* Cap at 40 chars */
    
    /* Print header */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                              NARWHALYZER PROFILING REPORT                                            ║\n");
    printf("╚══════════════════════════════════════════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    char total_time_buf[32];
    format_time(total_time_ns, total_time_buf, sizeof(total_time_buf));
    printf("Total Program Time: %s\n", total_time_buf);
    printf("Sections Instrumented: %d\n\n", section_count);
    
    printf("═══ FLAT SUMMARY (sorted by cumulative time) ═══\n\n");
    
    print_table_separator(max_name_width);
    printf("| %-*s | %10s | %12s | %12s | %12s | %12s | %8s |\n",
           max_name_width, "Section Name", "Entries", "Cumulative", 
           "Mean", "Min", "Max", "%%Total");
    print_table_separator(max_name_width);
    
    for (int i = 0; i < section_count; i++) {
        int idx = sorted_indices[i];
        narwhalyzer_section_stats_t *s = &g_sections[idx];
        
        if (s->entry_count == 0) continue;
        
        char cumul_buf[32], mean_buf[32], min_buf[32], max_buf[32];
        uint64_t mean_time = s->cumulative_time_ns / s->entry_count;
        
        format_time(s->cumulative_time_ns, cumul_buf, sizeof(cumul_buf));
        format_time(mean_time, mean_buf, sizeof(mean_buf));
        format_time(s->min_time_ns, min_buf, sizeof(min_buf));
        format_time(s->max_time_ns, max_buf, sizeof(max_buf));
        
        double percent = (total_time_ns > 0) 
            ? 100.0 * (double)s->cumulative_time_ns / (double)total_time_ns 
            : 0.0;
        
        /* Truncate name if necessary */
        char name_buf[64];
        if (strlen(s->name) > (size_t)max_name_width) {
            snprintf(name_buf, sizeof(name_buf), "%.*s...", max_name_width - 3, s->name);
        } else {
            snprintf(name_buf, sizeof(name_buf), "%s", s->name);
        }
        
        printf("| %-*s | %10lu | %12s | %12s | %12s | %12s | %7.2f%% |\n",
               max_name_width, name_buf,
               (unsigned long)s->entry_count,
               cumul_buf, mean_buf, min_buf, max_buf,
               percent);
    }
    
    print_table_separator(max_name_width);
    
    free(sorted_indices);
}

/*
 * Print the hierarchical view.
 */
static void print_hierarchy_view(int section_count)
{
    if (section_count == 0) return;
    
    printf("\n═══ HIERARCHICAL VIEW ═══\n\n");
    
    /* Build hierarchy nodes */
    hierarchy_node_t *nodes = calloc(section_count, sizeof(hierarchy_node_t));
    int *root_sections = malloc(section_count * sizeof(int));
    int root_count = 0;
    
    for (int i = 0; i < section_count; i++) {
        nodes[i].section_index = i;
        nodes[i].children = NULL;
        nodes[i].child_count = 0;
        nodes[i].child_capacity = 0;
        
        int parent = g_sections[i].parent_index;
        if (parent < 0) {
            root_sections[root_count++] = i;
        } else if (parent < section_count) {
            add_child(&nodes[parent], i);
        }
    }
    
    /* Print from each root */
    for (int i = 0; i < root_count; i++) {
        int idx = root_sections[i];
        narwhalyzer_section_stats_t *s = &g_sections[idx];
        
        if (s->entry_count == 0) continue;
        
        char time_buf[32];
        format_time(s->cumulative_time_ns, time_buf, sizeof(time_buf));
        printf("%s (%s)\n", s->name, time_buf);
        
        hierarchy_node_t *node = &nodes[idx];
        for (int j = 0; j < node->child_count; j++) {
            print_hierarchy_recursive(nodes, node->children[j], 
                                       "", j == node->child_count - 1);
        }
        printf("\n");
    }
    
    /* Cleanup */
    for (int i = 0; i < section_count; i++) {
        free(nodes[i].children);
    }
    free(nodes);
    free(root_sections);
}

/*
 * Print location details for all sections.
 */
static void print_section_details(int section_count)
{
    printf("═══ SECTION DETAILS ═══\n\n");
    
    for (int i = 0; i < section_count; i++) {
        narwhalyzer_section_stats_t *s = &g_sections[i];
        if (s->entry_count == 0) continue;
        
        printf("  %s\n", s->name);
        printf("    Location: %s:%d\n", s->file ? s->file : "<unknown>", s->line);
        printf("    Entries:  %lu\n", (unsigned long)s->entry_count);
        printf("\n");
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

/*
 * Initialize the runtime.
 */
__attribute__((constructor(101)))
void __narwhalyzer_init(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_initialized, &expected, 1)) {
        return; /* Already initialized */
    }
    
    g_program_start_time_ns = __narwhalyzer_get_timestamp_ns();
    
    /* Initialize section array */
    memset(g_sections, 0, sizeof(g_sections));
    
    /* Register atexit handler as backup */
    atexit(__narwhalyzer_fini);
}

/*
 * Check if initialized.
 */
int __narwhalyzer_is_initialized(void)
{
    return atomic_load(&g_initialized);
}

/*
 * Finalize and print report.
 */
__attribute__((destructor(101)))
void __narwhalyzer_fini(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_report_printed, &expected, 1)) {
        return; /* Report already printed */
    }
    
    g_program_end_time_ns = __narwhalyzer_get_timestamp_ns();
    uint64_t total_time_ns = g_program_end_time_ns - g_program_start_time_ns;
    
    int section_count = atomic_load(&g_section_count);
    
    if (section_count == 0) {
        return; /* No sections instrumented */
    }
    
    print_flat_summary(section_count, total_time_ns);
    print_hierarchy_view(section_count);
    print_section_details(section_count);
    
    printf("═══ END OF NARWHALYZER REPORT ═══\n\n");
}

/*
 * Register a new section.
 */
int __narwhalyzer_register_section(const char *name, const char *file, int line)
{
    /* Ensure initialization */
    if (!atomic_load(&g_initialized)) {
        __narwhalyzer_init();
    }
    
    pthread_mutex_lock(&g_registration_mutex);
    
    /* Check if section already registered (same name, file, line) */
    int count = atomic_load(&g_section_count);
    for (int i = 0; i < count; i++) {
        if (g_sections[i].line == line && 
            g_sections[i].file && file &&
            strcmp(g_sections[i].file, file) == 0 &&
            strcmp(g_sections[i].name, name) == 0) {
            pthread_mutex_unlock(&g_registration_mutex);
            return i;
        }
    }
    
    /* Allocate new section */
    int idx = atomic_fetch_add(&g_section_count, 1);
    if (idx >= NARWHALYZER_MAX_SECTIONS) {
        atomic_fetch_sub(&g_section_count, 1);
        pthread_mutex_unlock(&g_registration_mutex);
        fprintf(stderr, "narwhalyzer: warning: maximum section count exceeded\n");
        return -1;
    }
    
    /* Initialize section */
    narwhalyzer_section_stats_t *s = &g_sections[idx];
    s->name = name;
    s->file = file;
    s->line = line;
    s->entry_count = 0;
    s->cumulative_time_ns = 0;
    s->min_time_ns = UINT64_MAX;
    s->max_time_ns = 0;
    s->parent_index = -1;
    s->depth = 0;
    
    pthread_mutex_unlock(&g_registration_mutex);
    
    return idx;
}

/*
 * Record section entry.
 */
int __narwhalyzer_section_enter(int section_index)
{
    if (section_index < 0 || section_index >= atomic_load(&g_section_count)) {
        return -1;
    }
    
    /* Push context onto stack */
    int ctx_idx = ++g_context_stack_top;
    if (ctx_idx >= NARWHALYZER_MAX_NESTING_DEPTH) {
        g_context_stack_top--;
        fprintf(stderr, "narwhalyzer: warning: maximum nesting depth exceeded\n");
        return -1;
    }
    
    narwhalyzer_context_t *ctx = &g_context_stack[ctx_idx];
    ctx->section_index = section_index;
    ctx->start_time_ns = __narwhalyzer_get_timestamp_ns();
    ctx->parent_context_index = ctx_idx > 0 ? ctx_idx - 1 : -1;
    
    /* Update section stats */
    narwhalyzer_section_stats_t *s = &g_sections[section_index];
    __atomic_fetch_add(&s->entry_count, 1, __ATOMIC_RELAXED);
    
    /* Record parent relationship (first time only) */
    if (s->parent_index == -1 && ctx_idx > 0) {
        int parent_section = g_context_stack[ctx_idx - 1].section_index;
        s->parent_index = parent_section;
    }
    s->depth = ctx_idx;
    
    return ctx_idx;
}

/*
 * Record section exit.
 */
void __narwhalyzer_section_exit(int context_index)
{
    if (context_index < 0 || context_index > g_context_stack_top) {
        return;
    }
    
    uint64_t end_time_ns = __narwhalyzer_get_timestamp_ns();
    
    narwhalyzer_context_t *ctx = &g_context_stack[context_index];
    uint64_t elapsed_ns = end_time_ns - ctx->start_time_ns;
    
    /* Update section statistics */
    narwhalyzer_section_stats_t *s = &g_sections[ctx->section_index];
    __atomic_fetch_add(&s->cumulative_time_ns, elapsed_ns, __ATOMIC_RELAXED);
    
    /* Update min (using compare-and-swap) */
    uint64_t old_min = s->min_time_ns;
    while (elapsed_ns < old_min) {
        if (__atomic_compare_exchange_n(&s->min_time_ns, &old_min, elapsed_ns, 
                                          0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            break;
        }
    }
    
    /* Update max (using compare-and-swap) */
    uint64_t old_max = s->max_time_ns;
    while (elapsed_ns > old_max) {
        if (__atomic_compare_exchange_n(&s->max_time_ns, &old_max, elapsed_ns,
                                          0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            break;
        }
    }
    
    /* Pop context from stack */
    if (context_index == g_context_stack_top) {
        g_context_stack_top--;
    }
}

/*
 * Scope guard cleanup function.
 */
void __narwhalyzer_scope_guard_cleanup(narwhalyzer_scope_guard_t *guard)
{
    if (guard && guard->valid) {
        __narwhalyzer_section_exit(guard->context_index);
        guard->valid = 0;
    }
}
