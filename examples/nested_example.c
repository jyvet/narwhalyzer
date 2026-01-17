/*
 * nested_example.c
 * 
 * Demonstrates nested section tracking with Narwhalyzer.
 * Shows how hierarchical profiling data is captured and reported.
 * 
 * Build with:
 *   gcc -fplugin=narwhalyzer.so -I<include_path> -include narwhalyzer_runtime.h \
 *       nested_example.c -L<lib_path> -lnarwhalyzer_runtime -lpthread -lm -o nested_example
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Level 3: Leaf-level operations
 * ============================================================================ */

#pragma narwhalyzer matrix_multiply_kernel
void matrix_multiply_kernel(double *C, const double *A, const double *B, 
                             int M, int N, int K) {
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            double sum = 0.0;
            for (int k = 0; k < K; k++) {
                sum += A[i * K + k] * B[k * N + j];
            }
            C[i * N + j] = sum;
        }
    }
}

#pragma narwhalyzer vector_norm_kernel
double vector_norm_kernel(const double *v, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        sum += v[i] * v[i];
    }
    return sqrt(sum);
}

#pragma narwhalyzer vector_scale_kernel
void vector_scale_kernel(double *v, double scale, int n) {
    for (int i = 0; i < n; i++) {
        v[i] *= scale;
    }
}

/* ============================================================================
 * Level 2: Mid-level operations
 * ============================================================================ */

#pragma narwhalyzer linear_algebra_ops
void perform_linear_algebra(double *matrix, double *vector, int size) {
    /* Temporary storage */
    double *temp = malloc(size * size * sizeof(double));
    double *result = malloc(size * sizeof(double));
    
    /* Matrix operations */
    matrix_multiply_kernel(temp, matrix, matrix, size, size, size);
    
    /* Vector operations */
    double norm = vector_norm_kernel(vector, size);
    if (norm > 1e-10) {
        vector_scale_kernel(vector, 1.0 / norm, size);
    }
    
    /* More matrix-vector work */
    for (int i = 0; i < size; i++) {
        result[i] = 0.0;
        for (int j = 0; j < size; j++) {
            result[i] += temp[i * size + j] * vector[j];
        }
    }
    
    free(temp);
    free(result);
}

#pragma narwhalyzer memory_ops
void perform_memory_operations(double *data, int size) {
    /* Simulate memory-intensive operations */
    double *buffer = malloc(size * sizeof(double));
    
    /* Copy forward */
    memcpy(buffer, data, size * sizeof(double));
    
    /* Process */
    for (int i = 0; i < size; i++) {
        buffer[i] = sin(buffer[i]) + cos(buffer[i]);
    }
    
    /* Copy back */
    memcpy(data, buffer, size * sizeof(double));
    
    free(buffer);
}

/* ============================================================================
 * Level 1: Top-level phases
 * ============================================================================ */

#pragma narwhalyzer initialization_phase
void initialization_phase(double **matrix, double **vector, int size) {
    printf("  Initializing data structures...\n");
    
    *matrix = malloc(size * size * sizeof(double));
    *vector = malloc(size * sizeof(double));
    
    /* Initialize with pseudo-random values */
    for (int i = 0; i < size * size; i++) {
        (*matrix)[i] = (double)(i % 100) / 100.0;
    }
    
    for (int i = 0; i < size; i++) {
        (*vector)[i] = (double)(i % 50) / 50.0;
    }
    
    printf("  Initialization complete.\n");
}

#pragma narwhalyzer computation_phase
void computation_phase(double *matrix, double *vector, int size, int iterations) {
    printf("  Running %d computation iterations...\n", iterations);
    
    for (int iter = 0; iter < iterations; iter++) {
        /* Linear algebra computations */
        perform_linear_algebra(matrix, vector, size);
        
        /* Memory operations */
        perform_memory_operations(matrix, size * size);
        
        /* Some additional kernel calls */
        double norm = vector_norm_kernel(vector, size);
        vector_scale_kernel(vector, 1.0 / (norm + 1e-10), size);
    }
    
    printf("  Computation complete.\n");
}

#pragma narwhalyzer finalization_phase
void finalization_phase(double *matrix, double *vector, int size) {
    printf("  Finalizing and computing results...\n");
    
    /* Compute final statistics */
    double matrix_sum = 0.0;
    for (int i = 0; i < size * size; i++) {
        matrix_sum += matrix[i];
    }
    
    double vector_norm = vector_norm_kernel(vector, size);
    
    printf("  Final matrix sum: %f\n", matrix_sum);
    printf("  Final vector norm: %f\n", vector_norm);
    
    /* Cleanup */
    free(matrix);
    free(vector);
    
    printf("  Finalization complete.\n");
}

/* ============================================================================
 * Main Program
 * ============================================================================ */

#pragma narwhalyzer main_program
void run_program(int size, int iterations) {
    double *matrix = NULL;
    double *vector = NULL;
    
    printf("Running nested example (size=%d, iterations=%d)\n", size, iterations);
    
    /* Phase 1: Initialization */
    initialization_phase(&matrix, &vector, size);
    
    /* Phase 2: Main computation */
    computation_phase(matrix, vector, size, iterations);
    
    /* Phase 3: Finalization */
    finalization_phase(matrix, vector, size);
    
    printf("Program complete.\n\n");
}

int main(int argc, char *argv[]) {
    int size = 100;       /* Matrix/vector size */
    int iterations = 5;   /* Number of computation iterations */
    
    /* Parse command-line arguments */
    if (argc > 1) {
        size = atoi(argv[1]);
        if (size < 10) size = 10;
        if (size > 1000) size = 1000;
    }
    if (argc > 2) {
        iterations = atoi(argv[2]);
        if (iterations < 1) iterations = 1;
        if (iterations > 100) iterations = 100;
    }
    
    printf("=== Narwhalyzer Nested Example ===\n\n");
    printf("This example demonstrates hierarchical section tracking.\n");
    printf("The profiling report will show parent-child relationships.\n\n");
    
    /* Run the program twice to show multiple invocations */
    run_program(size, iterations);
    run_program(size / 2, iterations * 2);
    
    printf("=== Example Complete ===\n");
    printf("\nProfiling report follows:\n\n");
    
    return 0;
}
