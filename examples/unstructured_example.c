/*
 * unstructured_example.c
 * 
 * Demonstrates unstructured region profiling with Narwhalyzer.
 * Shows how to use start/stop pragmas to instrument arbitrary code regions
 * that don't follow function boundaries.
 * 
 * The plugin automatically generates instrumentation code for start/stop pragmas,
 * no macros are required!
 * 
 * Build with:
 *   gcc -fplugin=narwhalyzer.so -I<include_path> -include narwhalyzer.h \
 *       unstructured_example.c -L<lib_path> -lnarwhalyzer -lpthread -lm \
 *       -o unstructured_example
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "narwhalyzer.h"

/* ============================================================================
 * Example 1: Basic unstructured region spanning multiple statements
 * ============================================================================
 * 
 * This shows the simplest use case: profiling a sequence of operations
 * that are logically related but span multiple statements.
 */

void example_basic_unstructured(int n) {
    double *data = malloc(n * sizeof(double));
    double sum = 0.0;
    
    printf("Example 1: Basic unstructured region\n");
    
    /* Profile only the computation, not the allocation/deallocation */
    #pragma narwhalyzer start computation_phase
    
    /* Initialize data */
    for (int i = 0; i < n; i++) {
        data[i] = sin((double)i * 0.01);
    }
    
    /* Process data */
    for (int i = 0; i < n; i++) {
        sum += data[i] * data[i];
    }
    
    /* Finalize result */
    sum = sqrt(sum);
    
    #pragma narwhalyzer stop computation_phase
    
    printf("  Result: %f\n", sum);
    free(data);
}

/* ============================================================================
 * Example 2: Nested unstructured regions
 * ============================================================================
 * 
 * Unstructured regions can be nested just like structured sections.
 * This allows fine-grained profiling of different phases.
 */

void example_nested_regions(int outer_iters, int inner_iters) {
    double total = 0.0;
    
    printf("Example 2: Nested unstructured regions\n");
    
    #pragma narwhalyzer start outer_loop
    
    for (int i = 0; i < outer_iters; i++) {
        double partial = 0.0;
        
        #pragma narwhalyzer start inner_work
        
        for (int j = 0; j < inner_iters; j++) {
            partial += sin((double)(i * inner_iters + j) * 0.001);
        }
        
        #pragma narwhalyzer stop inner_work
        
        total += partial;
    }
    
    #pragma narwhalyzer stop outer_loop
    
    printf("  Result: %f\n", total);
}

/* ============================================================================
 * Example 3: Mixed structured and unstructured profiling
 * ============================================================================
 * 
 * You can combine function-level (structured) profiling with
 * fine-grained (unstructured) region profiling.
 */

#pragma narwhalyzer mixed_function
void example_mixed_profiling(int n) {
    printf("Example 3: Mixed structured and unstructured profiling\n");
    
    double *buffer = malloc(n * sizeof(double));
    
    /* This region is profiled separately within the function */
    #pragma narwhalyzer start alloc_init
    
    for (int i = 0; i < n; i++) {
        buffer[i] = (double)i;
    }
    
    #pragma narwhalyzer stop alloc_init
    
    /* Another separately profiled region */
    #pragma narwhalyzer start transform
    
    for (int i = 0; i < n; i++) {
        buffer[i] = log(buffer[i] + 1.0) * exp(-buffer[i] * 0.0001);
    }
    
    #pragma narwhalyzer stop transform
    
    /* Sum without separate profiling (included in function total) */
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += buffer[i];
    }
    
    printf("  Result: %f\n", sum);
    free(buffer);
}

/* ============================================================================
 * Example 4: Profiling across control flow
 * ============================================================================
 * 
 * Unstructured regions can span different control flow paths.
 * Be careful to ensure stop is always reached!
 */

double example_control_flow(int mode, int n) {
    double result = 0.0;
    
    printf("Example 4: Profiling across control flow (mode=%d)\n", mode);
    
    #pragma narwhalyzer start processing
    
    if (mode == 0) {
        /* Fast path */
        for (int i = 0; i < n; i++) {
            result += (double)i;
        }
    } else if (mode == 1) {
        /* Medium path */
        for (int i = 0; i < n; i++) {
            result += sqrt((double)i);
        }
    } else {
        /* Slow path */
        for (int i = 0; i < n; i++) {
            result += sin((double)i) * cos((double)i);
        }
    }
    
    #pragma narwhalyzer stop processing
    
    printf("  Result: %f\n", result);
    return result;
}

/* ============================================================================
 * Example 5: Using string-based macros for dynamic names
 * ============================================================================
 * 
 * When you need section names determined at compile time or want to
 * share context variables explicitly.
 */

void example_string_based(int iterations) {
    printf("Example 5: String-based macro variant\n");
    
    int ctx_var;
    
    NARWHALYZER_START_STR("dynamic_section", ctx_var);
    
    volatile double sum = 0.0;
    for (int i = 0; i < iterations; i++) {
        sum += sin((double)i) * cos((double)i);
    }
    
    NARWHALYZER_STOP_CTX(ctx_var);
    
    printf("  Result: %f\n", sum);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    int scale = 10000;
    
    if (argc > 1) {
        scale = atoi(argv[1]);
    }
    
    printf("Narwhalyzer Unstructured Regions Example\n");
    printf("=========================================\n");
    printf("Scale factor: %d\n\n", scale);
    
    /* Run all examples */
    example_basic_unstructured(scale);
    printf("\n");
    
    example_nested_regions(100, scale / 100);
    printf("\n");
    
    example_mixed_profiling(scale);
    printf("\n");
    
    /* Run control flow example with different modes */
    example_control_flow(0, scale);
    example_control_flow(1, scale);
    example_control_flow(2, scale);
    printf("\n");
    
    example_string_based(scale);
    printf("\n");
    
    printf("Done. Profiling report follows:\n");
    printf("=========================================\n\n");
    
    return 0;
}
