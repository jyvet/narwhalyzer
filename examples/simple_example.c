/*
 * simple_example.c
 * 
 * Basic demonstration of Narwhalyzer pragma usage.
 * 
 * Build with:
 *   gcc -fplugin=narwhalyzer.so -I<include_path> -include narwhalyzer_runtime.h \
 *       simple_example.c -L<lib_path> -lnarwhalyzer_runtime -lpthread -o simple_example
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Simulates a computationally intensive task */
#pragma narwhalyzer heavy_computation
double compute_primes(int limit) {
    int count = 0;
    
    for (int n = 2; n < limit; n++) {
        int is_prime = 1;
        for (int i = 2; i * i <= n; i++) {
            if (n % i == 0) {
                is_prime = 0;
                break;
            }
        }
        if (is_prime) {
            count++;
        }
    }
    
    return (double)count;
}

/* Simulates I/O-bound work */
#pragma narwhalyzer io_simulation
void simulate_io(int iterations) {
    volatile double sum = 0.0;
    
    for (int i = 0; i < iterations; i++) {
        /* Simulate some I/O by doing light computation */
        sum += sin((double)i) * cos((double)i);
    }
    
    printf("I/O simulation result: %f\n", sum);
}

/* Helper function called multiple times */
#pragma narwhalyzer data_processing
void process_data_chunk(double *data, int size) {
    for (int i = 0; i < size; i++) {
        data[i] = sqrt(data[i] * data[i] + 1.0);
    }
}

/* Main computation driver */
#pragma narwhalyzer main_driver
void run_main_computation(void) {
    printf("Starting main computation...\n");
    
    /* Allocate and initialize data */
    int data_size = 100000;
    double *data = malloc(data_size * sizeof(double));
    
    for (int i = 0; i < data_size; i++) {
        data[i] = (double)i;
    }
    
    /* Process data in chunks */
    int chunk_size = 10000;
    for (int offset = 0; offset < data_size; offset += chunk_size) {
        process_data_chunk(data + offset, chunk_size);
    }
    
    /* Do heavy computation */
    double prime_count = compute_primes(50000);
    printf("Found %.0f primes\n", prime_count);
    
    /* Simulate I/O */
    simulate_io(1000000);
    
    /* Cleanup */
    free(data);
    
    printf("Main computation finished.\n");
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("=== Narwhalyzer Simple Example ===\n\n");
    
    /* Run main computation multiple times */
    for (int i = 0; i < 3; i++) {
        printf("\n--- Iteration %d ---\n", i + 1);
        run_main_computation();
    }
    
    printf("\n=== Example Complete ===\n");
    printf("Profiling report will follow:\n\n");
    
    return 0;
}
