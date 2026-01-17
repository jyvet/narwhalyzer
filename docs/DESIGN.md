# Narwhalyzer Technical Design Document

This document explains the internal architecture and implementation details of the Narwhalyzer GCC plugin and runtime library.

## Overview

Narwhalyzer consists of three main components:

1. **GCC Plugin** (`narwhalyzer.so`) - Parses pragmas and inserts instrumentation
2. **Runtime Library** (`libnarwhalyzer_runtime.so`) - Tracks timing and generates reports
3. **Macro Header** (`narwhalyzer_macros.h`) - Alternative macro-based instrumentation

## How the Pragma is Parsed

### Registration with GCC

The plugin registers a custom pragma handler during initialization:

```cpp
c_register_pragma(NULL, "narwhalyzer", handle_pragma_narwhalyzer);
```

This tells GCC to call our handler whenever it encounters `#pragma narwhalyzer`.

### Pragma Handler

When GCC's preprocessor encounters `#pragma narwhalyzer <name>`, our handler:

1. Extracts the section name (identifier or string literal)
2. Records the source location (file, line number)
3. Stores this information in a pending pragmas list

```cpp
struct pragma_info {
    std::string section_name;
    std::string filename;
    int line;
    location_t loc;
};
```

### Association with Functions

During the `PLUGIN_PRE_GENERICIZE` callback (after parsing, before optimization):

1. We examine each function declaration
2. Check if any pending pragma is within 10 lines before the function
3. Associate matching pragmas with the function for later instrumentation

## How Instrumentation is Inserted

### GIMPLE Pass

We register a custom GIMPLE optimization pass that runs after SSA construction:

```cpp
pass_info.reference_pass_name = "ssa";
pass_info.ref_pass_instance_number = 1;
pass_info.pos_op = PASS_POS_INSERT_AFTER;
```

### Instrumentation Process

For each instrumented function, we:

1. **Create a static index variable** to cache the section's runtime index:
   ```c
   static int __narwhalyzer_idx_<section>_<line> = -1;
   ```

2. **Insert entry code** at the function entry point:
   ```c
   // Register section (runtime handles caching)
   __narwhalyzer_idx = __narwhalyzer_register_section(name, file, line);
   
   // Record entry and get context
   int ctx = __narwhalyzer_section_enter(__narwhalyzer_idx);
   ```

3. **Insert exit code** before all `GIMPLE_RETURN` statements:
   ```c
   __narwhalyzer_section_exit(ctx);
   ```

### Handling Multiple Exits

The GIMPLE pass iterates over all basic blocks and identifies return statements:

```cpp
FOR_EACH_BB_FN(bb, fn) {
    for (gsi = gsi_last_bb(bb); !gsi_end_p(gsi); gsi_prev(&gsi)) {
        if (gimple_code(gsi_stmt(gsi)) == GIMPLE_RETURN) {
            // Insert exit call before return
        }
    }
}
```

For the macro-based interface, we use GCC's `__attribute__((cleanup))` to ensure exit code runs even on early returns:

```c
narwhalyzer_scope_guard_t guard __attribute__((cleanup(__narwhalyzer_scope_guard_cleanup)));
```

## How Nested Sections are Tracked

### Thread-Local Context Stack

The runtime maintains a thread-local stack of active section contexts:

```c
static __thread narwhalyzer_context_t g_context_stack[MAX_NESTING_DEPTH];
static __thread int g_context_stack_top = -1;
```

### Entry Tracking

When entering a section:

1. Push a new context onto the stack
2. Record the current timestamp
3. Note the parent context (if any)
4. Update the section's parent index for hierarchy tracking

```c
int __narwhalyzer_section_enter(int section_index) {
    int ctx_idx = ++g_context_stack_top;
    
    g_context_stack[ctx_idx].section_index = section_index;
    g_context_stack[ctx_idx].start_time_ns = __narwhalyzer_get_timestamp_ns();
    g_context_stack[ctx_idx].parent_context_index = ctx_idx > 0 ? ctx_idx - 1 : -1;
    
    // Record parent relationship for hierarchy
    if (g_sections[section_index].parent_index == -1 && ctx_idx > 0) {
        g_sections[section_index].parent_index = 
            g_context_stack[ctx_idx - 1].section_index;
    }
    
    return ctx_idx;
}
```

### Exit Tracking

When exiting a section:

1. Capture the exit timestamp
2. Compute elapsed time
3. Update statistics (cumulative, min, max) using atomic operations
4. Pop the context from the stack

```c
void __narwhalyzer_section_exit(int context_index) {
    uint64_t end_time = __narwhalyzer_get_timestamp_ns();
    uint64_t elapsed = end_time - g_context_stack[context_index].start_time_ns;
    
    // Atomic updates to statistics
    __atomic_fetch_add(&section->cumulative_time_ns, elapsed, __ATOMIC_RELAXED);
    // CAS loops for min/max updates
    
    g_context_stack_top--;
}
```

## How Runtime Reporting is Triggered

### Initialization

The runtime initializes via a constructor attribute:

```c
__attribute__((constructor(101)))
void __narwhalyzer_init(void) {
    g_program_start_time_ns = __narwhalyzer_get_timestamp_ns();
    atexit(__narwhalyzer_fini);  // Backup handler
}
```

### Report Generation

The report is generated via a destructor attribute:

```c
__attribute__((destructor(101)))
void __narwhalyzer_fini(void) {
    // Compute total program time
    g_program_end_time_ns = __narwhalyzer_get_timestamp_ns();
    
    // Generate flat summary (sorted by cumulative time)
    print_flat_summary();
    
    // Generate hierarchical view (using parent indices)
    print_hierarchy_view();
    
    // Print section details
    print_section_details();
}
```

The priority `101` ensures our destructor runs before most user destructors.

### Hierarchy Reconstruction

The hierarchical view is built by:

1. Creating a node structure for each section
2. Building child lists based on `parent_index` relationships
3. Recursively printing the tree with proper ASCII formatting

```c
typedef struct hierarchy_node {
    int section_index;
    int *children;
    int child_count;
} hierarchy_node_t;

// Build child lists from parent indices
for (int i = 0; i < section_count; i++) {
    int parent = g_sections[i].parent_index;
    if (parent >= 0) {
        add_child(&nodes[parent], i);
    }
}
```

## Thread Safety

### Atomic Operations

Statistics are updated using C11 atomics:

- Entry count: `__atomic_fetch_add`
- Cumulative time: `__atomic_fetch_add`
- Min/Max: Compare-and-swap loops

### Thread-Local Storage

Each thread has its own context stack using `__thread`:

```c
static __thread narwhalyzer_context_t g_context_stack[...];
static __thread int g_context_stack_top = -1;
```

### Registration Mutex

Section registration uses a mutex to prevent duplicate registrations:

```c
pthread_mutex_lock(&g_registration_mutex);
// Check for existing section with same name/file/line
// Allocate new section if needed
pthread_mutex_unlock(&g_registration_mutex);
```

## Performance Considerations

### Low Overhead Design

1. **Lazy registration**: Sections are registered on first entry, with index cached in static variable
2. **Minimal atomic operations**: Only statistics updates use atomics
3. **High-resolution clock**: Uses `CLOCK_MONOTONIC_RAW` for best accuracy
4. **No dynamic allocation**: Fixed-size arrays for sections and context stack

### Expected Overhead

- Per-entry: ~50-100 nanoseconds (timestamp capture + atomic increment)
- Per-exit: ~50-150 nanoseconds (timestamp + atomic updates)
- Total overhead per instrumented call: ~100-250 nanoseconds

## Limitations

1. **Function-level granularity**: The GCC plugin instruments entire functions; block-level requires macros
2. **Fixed limits**: Maximum 1024 sections, 64 nesting depth (configurable)
3. **Single process**: No multi-process aggregation
4. **Linux only**: Uses Linux-specific features (CLOCK_MONOTONIC_RAW)

## Future Enhancements

Potential improvements:

1. Block-level pragma support in the plugin
2. JSON/CSV output formats
3. Runtime API for programmatic access
4. Multi-process support via shared memory
5. Integration with performance analysis tools
