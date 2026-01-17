/*
 * macro_example.c
 * 
 * Demonstrates using Narwhalyzer with the macro-based interface.
 * This works without the GCC plugin - useful for testing or as a fallback.
 * 
 * Build with:
 *   gcc -I<include_path> macro_example.c -L<lib_path> -lnarwhalyzer_runtime \
 *       -lpthread -lm -o macro_example
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "narwhalyzer.h"
#include "narwhalyzer_macros.h"

/* ============================================================================
 * Example using NARWHALYZER_SECTION for block instrumentation
 * ============================================================================ */

void compute_with_sections(int n) {
    printf("Computing with sections (n=%d)...\n", n);
    
    /* Instrument the initialization block */
    NARWHALYZER_SECTION("init_arrays") {
        double *a = malloc(n * sizeof(double));
        double *b = malloc(n * sizeof(double));
        
        for (int i = 0; i < n; i++) {
            a[i] = (double)i;
            b[i] = (double)(n - i);
        }
        
        /* Nested section for computation */
        NARWHALYZER_SECTION("dot_product") {
            double dot = 0.0;
            for (int i = 0; i < n; i++) {
                dot += a[i] * b[i];
            }
            printf("  Dot product: %f\n", dot);
        }
        
        /* Another nested section */
        NARWHALYZER_SECTION("norm_computation") {
            double norm_a = 0.0, norm_b = 0.0;
            for (int i = 0; i < n; i++) {
                norm_a += a[i] * a[i];
                norm_b += b[i] * b[i];
            }
            printf("  Norms: %f, %f\n", sqrt(norm_a), sqrt(norm_b));
        }
        
        free(a);
        free(b);
    }
}

/* ============================================================================
 * Example using NARWHALYZER_FUNCTION for whole-function instrumentation
 * ============================================================================ */

double heavy_math(int iterations) {
    NARWHALYZER_FUNCTION("heavy_math");
    
    double result = 0.0;
    
    for (int i = 0; i < iterations; i++) {
        result += sin((double)i / 1000.0) * cos((double)i / 1000.0);
        
        /* Early return is handled correctly */
        if (result > 1e10) {
            printf("  Early exit from heavy_math\n");
            return result;
        }
    }
    
    return result;
}

int recursive_fibonacci(int n) {
    NARWHALYZER_FUNCTION("fibonacci");
    
    if (n <= 1) return n;
    return recursive_fibonacci(n - 1) + recursive_fibonacci(n - 2);
}

/* ============================================================================
 * Example using NARWHALYZER_GUARDED_SECTION for complex control flow
 * ============================================================================ */

int process_with_early_exit(int *data, int n) {
    NARWHALYZER_GUARDED_SECTION("process_data") {
        printf("Processing %d elements...\n", n);
        
        for (int i = 0; i < n; i++) {
            /* Multiple exit paths - all tracked correctly */
            if (data[i] < 0) {
                printf("  Found negative value at %d, aborting\n", i);
                return -1;  /* Early return */
            }
            
            if (data[i] == 0) {
                printf("  Found zero at %d, skipping rest\n", i);
                break;  /* Break out of loop */
            }
            
            data[i] = data[i] * 2;
        }
        
        printf("  Processing complete\n");
    }
    
    return 0;
}

/* ============================================================================
 * Example using manual NARWHALYZER_ENTER/EXIT for precise control
 * ============================================================================ */

NARWHALYZER_DECLARE_SECTION("manual_section", g_manual_section_idx);

void manual_instrumentation_example(void) {
    printf("Manual instrumentation example...\n");
    
    NARWHALYZER_ENTER(g_manual_section_idx, ctx1);
    
    /* Some work */
    volatile double sum = 0.0;
    for (int i = 0; i < 100000; i++) {
        sum += (double)i;
    }
    
    NARWHALYZER_EXIT(ctx1);
    
    printf("  Sum: %f\n", sum);
    
    /* Another invocation */
    NARWHALYZER_ENTER(g_manual_section_idx, ctx2);
    
    for (int i = 0; i < 50000; i++) {
        sum -= (double)i;
    }
    
    NARWHALYZER_EXIT(ctx2);
    
    printf("  Final sum: %f\n", sum);
}

/* ============================================================================
 * Main Program
 * ============================================================================ */

int main(void) {
    printf("=== Narwhalyzer Macro Example ===\n\n");
    
    /* Section-based instrumentation */
    printf("--- Section-based instrumentation ---\n");
    compute_with_sections(10000);
    compute_with_sections(50000);
    printf("\n");
    
    /* Function-based instrumentation */
    printf("--- Function-based instrumentation ---\n");
    double result = heavy_math(1000000);
    printf("  Heavy math result: %f\n", result);
    printf("\n");
    
    printf("--- Recursive function instrumentation ---\n");
    int fib = recursive_fibonacci(20);
    printf("  Fibonacci(20) = %d\n", fib);
    printf("\n");
    
    /* Guarded section with complex control flow */
    printf("--- Guarded section with early exit ---\n");
    int data1[] = {1, 2, 3, 4, 5};
    process_with_early_exit(data1, 5);
    
    int data2[] = {1, 2, -1, 4, 5};
    process_with_early_exit(data2, 5);
    
    int data3[] = {1, 2, 0, 4, 5};
    process_with_early_exit(data3, 5);
    printf("\n");
    
    /* Manual instrumentation */
    printf("--- Manual instrumentation ---\n");
    manual_instrumentation_example();
    printf("\n");
    
    printf("=== Example Complete ===\n");
    printf("\nProfiling report follows:\n\n");
    
    return 0;
}
