/*
 * narwhalyzer_macros.h
 * 
 * Macro-based instrumentation interface for Narwhalyzer.
 * This header provides macros that can be used as a fallback when
 * the GCC plugin is not available, or for manual instrumentation.
 * 
 * Usage:
 *   NARWHALYZER_SECTION("section_name") {
 *       // your code here
 *   }
 * 
 * Or for function-level:
 *   void my_function() {
 *       NARWHALYZER_FUNCTION("my_function");
 *       // function body
 *   }
 * 
 * Copyright (c) 2026
 * Licensed under MIT License
 */

#ifndef NARWHALYZER_MACROS_H
#define NARWHALYZER_MACROS_H

#include "narwhalyzer.h"

/*
 * Generate a unique identifier based on line number.
 */
#define NARWHALYZER_CONCAT_(a, b) a##b
#define NARWHALYZER_CONCAT(a, b) NARWHALYZER_CONCAT_(a, b)
#define NARWHALYZER_UNIQUE(prefix) NARWHALYZER_CONCAT(prefix, __LINE__)

/*
 * Instrument a code block with automatic entry/exit tracking.
 * Uses __attribute__((cleanup)) for automatic exit handling.
 * 
 * Usage:
 *   NARWHALYZER_SECTION("my_section") {
 *       // code to instrument
 *   }
 */
#define NARWHALYZER_SECTION(name) \
    for (int NARWHALYZER_UNIQUE(_nw_once_) = 1; NARWHALYZER_UNIQUE(_nw_once_); ) \
        for (static int NARWHALYZER_UNIQUE(_nw_idx_) = -1; \
             NARWHALYZER_UNIQUE(_nw_once_); ) \
            for (NARWHALYZER_UNIQUE(_nw_idx_) = \
                     (NARWHALYZER_UNIQUE(_nw_idx_) < 0) \
                     ? __narwhalyzer_register_section(name, __FILE__, __LINE__) \
                     : NARWHALYZER_UNIQUE(_nw_idx_); \
                 NARWHALYZER_UNIQUE(_nw_once_); ) \
                for (int NARWHALYZER_UNIQUE(_nw_ctx_) = \
                         __narwhalyzer_section_enter(NARWHALYZER_UNIQUE(_nw_idx_)); \
                     NARWHALYZER_UNIQUE(_nw_once_); \
                     __narwhalyzer_section_exit(NARWHALYZER_UNIQUE(_nw_ctx_)), \
                     NARWHALYZER_UNIQUE(_nw_once_) = 0)

/*
 * Alternative section macro using a scope guard.
 * This version handles early returns, breaks, and other exit paths.
 * 
 * Usage:
 *   NARWHALYZER_GUARDED_SECTION("my_section") {
 *       if (condition) return;  // exit is tracked
 *       // more code
 *   }
 */
#define NARWHALYZER_GUARDED_SECTION(name) \
    static int NARWHALYZER_UNIQUE(_nw_gidx_) = -1; \
    if (NARWHALYZER_UNIQUE(_nw_gidx_) < 0) { \
        NARWHALYZER_UNIQUE(_nw_gidx_) = \
            __narwhalyzer_register_section(name, __FILE__, __LINE__); \
    } \
    int NARWHALYZER_UNIQUE(_nw_gctx_) = \
        __narwhalyzer_section_enter(NARWHALYZER_UNIQUE(_nw_gidx_)); \
    narwhalyzer_scope_guard_t NARWHALYZER_UNIQUE(_nw_guard_) \
        __attribute__((cleanup(__narwhalyzer_scope_guard_cleanup))) = \
        { NARWHALYZER_UNIQUE(_nw_gctx_), 1 }; \
    (void)NARWHALYZER_UNIQUE(_nw_guard_); \
    for (int NARWHALYZER_UNIQUE(_nw_gonce_) = 1; \
         NARWHALYZER_UNIQUE(_nw_gonce_); \
         NARWHALYZER_UNIQUE(_nw_gonce_) = 0)

/*
 * Instrument an entire function.
 * Place at the beginning of the function body.
 * Handles all return paths automatically via cleanup.
 * 
 * Usage:
 *   void my_function(int arg) {
 *       NARWHALYZER_FUNCTION("my_function");
 *       // function body
 *       if (condition) return;  // exit is tracked
 *       // more code
 *   }
 */
#define NARWHALYZER_FUNCTION(name) \
    static int _nw_func_idx_ = -1; \
    if (_nw_func_idx_ < 0) { \
        _nw_func_idx_ = __narwhalyzer_register_section(name, __FILE__, __LINE__); \
    } \
    int _nw_func_ctx_ = __narwhalyzer_section_enter(_nw_func_idx_); \
    narwhalyzer_scope_guard_t _nw_func_guard_ \
        __attribute__((cleanup(__narwhalyzer_scope_guard_cleanup))) = \
        { _nw_func_ctx_, 1 }; \
    (void)_nw_func_guard_

/*
 * Manual entry/exit instrumentation.
 * Use when you need explicit control over timing boundaries.
 * 
 * Usage:
 *   NARWHALYZER_DECLARE_SECTION("my_section", my_section_idx);
 *   
 *   void my_function() {
 *       NARWHALYZER_ENTER(my_section_idx, ctx);
 *       // code
 *       NARWHALYZER_EXIT(ctx);
 *   }
 */
#define NARWHALYZER_DECLARE_SECTION(name, var) \
    static int var = -1; \
    __attribute__((constructor)) static void _nw_init_##var(void) { \
        var = __narwhalyzer_register_section(name, __FILE__, __LINE__); \
    }

#define NARWHALYZER_ENTER(section_idx, ctx_var) \
    int ctx_var = __narwhalyzer_section_enter(section_idx)

#define NARWHALYZER_EXIT(ctx_var) \
    __narwhalyzer_section_exit(ctx_var)

/*
 * Conditional instrumentation - can be disabled at compile time.
 */
#ifdef NARWHALYZER_DISABLE

#define NARWHALYZER_SECTION(name)
#define NARWHALYZER_GUARDED_SECTION(name)
#define NARWHALYZER_FUNCTION(name)
#define NARWHALYZER_DECLARE_SECTION(name, var)
#define NARWHALYZER_ENTER(section_idx, ctx_var) int ctx_var = 0
#define NARWHALYZER_EXIT(ctx_var) (void)ctx_var

#endif /* NARWHALYZER_DISABLE */

#endif /* NARWHALYZER_MACROS_H */
