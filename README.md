<div align="center">

<img src="https://github.com/jyvet/narwhalyzer/blob/main/img/narwhalyzer.png?raw=true)" width="400"/>

</div>

The narwhal, often called the "unicorn of the sea," possesses a remarkable tusk that serves as a sophisticated sensory organ. This elongated tooth can detect changes in temperature, pressure, and chemical composition of its Arctic environment, allowing the narwhal to navigate and hunt with extraordinary precision.

Like the narwhal's tusk sensing the ocean's subtle variations, Narwhalyzer senses the temporal landscape of your code. It detects the flow of execution through annotated sections, measuring the duration and frequency of each code passage. This GCC plugin automatically instruments marked code regions and generates detailed profiling reports at program termination.

## Features

- **Pragma-driven instrumentation**: Use `#pragma narwhalyzer <section_name>` to mark code sections
- **Automatic profiling**: Entry counts, cumulative/min/max/mean execution times
- **Nested section support**: Correctly tracks hierarchical section relationships
- **Multiple exit handling**: Handles early returns, breaks, and gotos automatically
- **Low overhead**: Uses high-resolution monotonic clock with minimal runtime impact
- **Detailed reports**: Flat summary tables and hierarchical tree views

## Requirements

- GCC 10.0 or later (with plugin support)
- GCC plugin development headers
- CMake 3.16 or later
- Linux operating system
- pthreads library

### Installing GCC Plugin Development Files

**Ubuntu/Debian:**

```bash
sudo apt install gcc-12-plugin-dev  # Replace 12 with your GCC version
```

**Fedora/RHEL:**

```bash
sudo dnf install gcc-plugin-devel
```

**From source:** If you built GCC from source, plugin headers should already be available.

## Building

### Standard Build

```bash
mkdir build && cd build
cmake ..
make
```

### Specifying a Custom GCC Installation

```bash
cmake -DGCC_ROOT=/path/to/gcc/installation ..
make
```

### Build Options

| Option                       | Default       | Description                |
| ---------------------------- | ------------- | -------------------------- |
| `GCC_ROOT`                   | (auto-detect) | Path to GCC installation   |
| `NARWHALYZER_BUILD_EXAMPLES` | ON            | Build example programs     |
| `CMAKE_BUILD_TYPE`           | Release       | Build type (Debug/Release) |

### Build Outputs

After building, you'll have:

- `narwhalyzer.so` - The GCC plugin
- `libnarwhalyzer.so` - Runtime support library (shared)
- `libnarwhalyzer.a` - Runtime support library (static)

## Usage

### 1. Annotating Code with Pragmas

narwhalyzer supports two pragma forms:

#### Structured Sections (Function-Level)

Use `#pragma narwhalyzer <section_name>` immediately before a function definition:

```c
#include "narwhalyzer.h"

#pragma narwhalyzer compute_phase
void compute_heavy_work(int iterations) {
    for (int i = 0; i < iterations; i++) {
        // ... computation ...
    }
}

#pragma narwhalyzer io_phase
void write_results(const char *filename) {
    // ... I/O operations ...
}

int main() {
    compute_heavy_work(1000000);
    write_results("output.txt");
    return 0;
}
```

#### Unstructured Regions (Arbitrary Code Spans)

Use `#pragma narwhalyzer start <name>` and `#pragma narwhalyzer stop <name>` to instrument arbitrary code regions:

```c
#include "narwhalyzer.h"

void process_data(int *data, int n) {
    // Setup code (not profiled)
    int *buffer = malloc(n * sizeof(int));

    // Profile only the computation
    #pragma narwhalyzer start data_transform

    for (int i = 0; i < n; i++) {
        buffer[i] = data[i] * 2;
    }

    for (int i = 0; i < n; i++) {
        data[i] = buffer[i] + i;
    }

    #pragma narwhalyzer stop data_transform

    // Cleanup (not profiled)
    free(buffer);
}
```

**Key differences:**
| Feature | Structured (`#pragma narwhalyzer name`) | Unstructured (`start`/`stop`) |
|---------|----------------------------------------|-------------------------------|
| Scope | Entire function | Arbitrary code region |
| Instrumentation | Automatic by plugin | Automatic by plugin |
| Exit handling | Automatic (all return paths) | Manual (must reach stop) |
| Use case | Function-level profiling | Fine-grained regions |

### 2. Compiling with the Plugin

```bash
gcc -fplugin=/path/to/narwhalyzer.so \
    -I/path/to/include \
    -include narwhalyzer.h \
    your_program.c \
    -L/path/to/lib \
    -lnarwhalyzer \
    -lpthread \
    -o your_program
```

**Using the build helper script (after CMake build):**

```bash
./narwhalyzer-gcc examples/simple_example.c my_program
```

### 3. Running and Getting Results

Simply run your instrumented program. The profiling report is automatically printed to stdout when the program exits:

```bash
./your_program
```

### Plugin Options

Enable verbose output during compilation:

```bash
gcc -fplugin=/path/to/narwhalyzer.so \
    -fplugin-arg-narwhalyzer-verbose \
    ...
```

## Output Format

### Flat Summary Table

The report begins with a flat table sorted by cumulative execution time:

```
╔══════════════════════════════════════════════════════════════════════════════════════════════════════╗
║                              NARWHALYZER PROFILING REPORT                                            ║
╚══════════════════════════════════════════════════════════════════════════════════════════════════════╝

Total Program Time: 1.234 s
Sections Instrumented: 3

═══ FLAT SUMMARY (sorted by cumulative time) ═══

+----------------+------------+--------------+--------------+--------------+--------------+----------+
| Section Name   |    Entries |   Cumulative |         Mean |          Min |          Max |   %Total |
+----------------+------------+--------------+--------------+--------------+--------------+----------+
| compute_phase  |       1000 |    987.654 ms|    987.654 us|    950.123 us|      1.234 ms|   80.05% |
| io_phase       |        100 |    234.567 ms|      2.346 ms|      2.100 ms|      3.456 ms|   19.01% |
| init_phase     |          1 |     11.234 ms|     11.234 ms|     11.234 ms|     11.234 ms|    0.91% |
+----------------+------------+--------------+--------------+--------------+--------------+----------+
```

### Column Descriptions

| Column           | Description                                            |
| ---------------- | ------------------------------------------------------ |
| **Section Name** | Name from `#pragma narwhalyzer` directive              |
| **Entries**      | Number of times the section was entered                |
| **Cumulative**   | Total time spent in the section across all invocations |
| **Mean**         | Average time per invocation (Cumulative / Entries)     |
| **Min**          | Shortest single execution time                         |
| **Max**          | Longest single execution time                          |
| **%Total**       | Percentage of total program runtime                    |

### Hierarchical View

Shows the nesting relationship between sections:

```
═══ HIERARCHICAL VIEW ═══

main (1.234 s)
├── compute_phase (987.654 ms)
│   ├── kernel_A (654.321 ms)
│   └── kernel_B (333.333 ms)
└── io_phase (234.567 ms)
```

### Section Details

Lists source file locations for each section:

```
═══ SECTION DETAILS ═══

  compute_phase
    Location: src/compute.c:42
    Entries:  1000

  io_phase
    Location: src/io.c:15
    Entries:  100
```

## How It Works

### Pragma Detection

1. The plugin registers a custom pragma handler with GCC's preprocessor
2. When `#pragma narwhalyzer <name>` is encountered, the section name and source location are recorded
3. For structured pragmas, the pragma is associated with the immediately following function definition
4. For `start`/`stop` pragmas, the plugin records the pragma location for later instrumentation

### Sensory Instrumentation

**Structured sections (function-level):**

1. A GIMPLE optimization pass runs after SSA construction
2. For each function with an associated pragma:
   - A static variable is created to cache the section index
   - Entry instrumentation is inserted at the function entry point
   - Exit instrumentation is inserted before all `return` statements
3. The instrumentation calls runtime library functions to begin sensing

**Unstructured regions (start/stop):**

1. The plugin tracks `start` and `stop` pragmas within each function
2. During the GIMPLE pass, for each `start` pragma:
   - Section registration code is inserted
   - A `section_enter` call is inserted to begin sensing
   - A context variable is created to track the region
3. For each `stop` pragma:
   - A `section_exit` call is inserted using the matching context variable
4. The user is responsible for ensuring `stop` is always reached (the plugin cannot automatically handle early returns within unstructured regions)

### Temporal Navigation

1. The runtime maintains a thread-local stack of active section contexts
2. When entering a section, the current stack depth establishes parent relationships
3. When exiting, the context is popped and timing is recorded
4. Hierarchical relationships are reconstructed during report generation

### Environmental Reporting

1. The runtime library uses `__attribute__((constructor))` for initialization
2. An `atexit()` handler is registered to generate the environmental report
3. `__attribute__((destructor))` provides backup report generation
4. The temporal landscape is printed to stdout before program termination

## Limitations and Known Issues

1. **Unstructured regions require reaching stop**: Unlike structured sections, unstructured regions with `start`/`stop` do not automatically handle early returns or exceptions. Ensure the code path always reaches the corresponding `stop` pragma.

2. **C/C++ only**: The plugin is designed for C and C++ code compiled with GCC.

3. **Thread safety**: While the runtime uses atomic operations and thread-local storage, very high contention scenarios may show some overhead.

4. **Inline functions**: Heavily inlined functions may not show expected results; consider using `__attribute__((noinline))`.

5. **Optimizations**: High optimization levels (-O3) may reorder or eliminate some instrumented code paths.

6. **Maximum sections**: Limited to 1024 distinct sections and 64 nesting depth by default (configurable in header).

## Examples

See the [examples/](examples/) directory for complete example programs:

- `simple_example.c` - Basic usage demonstration
- `nested_example.c` - Nested section tracking
- `unstructured_example.c` - Unstructured region profiling with start/stop
- `multifile_example/` - Multi-file project instrumentation

## API Reference

### Pragma Syntax

#### Structured Sections (Function-Level)

```c
#pragma narwhalyzer <section_name>
```

- `section_name`: An identifier or quoted string naming the section
- Must appear immediately before a function definition
- Multiple pragmas can name the same section (results are aggregated)
- Instrumentation is automatic; no macros needed

#### Unstructured Regions (Arbitrary Code)

```c
#pragma narwhalyzer start <section_name>
// ... code to profile ...
#pragma narwhalyzer stop <section_name>
```

- `section_name`: An identifier or quoted string naming the region
- Instrumentation is automatic; no macros needed (the plugin generates the instrumentation code)
- Can span arbitrary code within a single function
- User must ensure `stop` is always reached (the plugin cannot automatically handle early returns within regions)
- Start and stop pragmas must be in the same function

### Optional Macros for Manual Instrumentation

If you need manual control or want to use the macros without the plugin, you can still use the macros directly:

```c
/* Basic usage - section name must be a valid C identifier */
NARWHALYZER_START(section_name);
// ... code ...
NARWHALYZER_STOP(section_name);

/* String-based variant for dynamic names */
int ctx;
NARWHALYZER_START_STR("section_name", ctx);
// ... code ...
NARWHALYZER_STOP_CTX(ctx);
```

### Runtime Configuration

Define these before including the header to customize:

```c
#define NARWHALYZER_MAX_SECTIONS 2048      // Max distinct sections
#define NARWHALYZER_MAX_NESTING_DEPTH 128  // Max nesting depth
#include "narwhalyzer.h"
```

## License

MIT License - see [LICENSE](LICENSE) for details.

## Contributing

Contributions are welcome! Please submit issues and pull requests on GitHub.
