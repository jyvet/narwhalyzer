/*
 * narwhalyzer_pragma_plugin.cc
 * 
 * Alternative GCC Plugin implementation that uses a simpler pragma-to-attribute
 * approach for more reliable instrumentation.
 * 
 * This plugin:
 * 1. Registers the #pragma narwhalyzer directive
 * 2. Converts pragmas to function attributes
 * 3. Uses a late GIMPLE pass to insert instrumentation
 * 
 * Copyright (c) 2026
 * Licensed under MIT License
 */

#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <tree-pass.h>
#include <context.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <tree-iterator.h>
#include <stringpool.h>
#include <attribs.h>
#include <c-family/c-pragma.h>
#include <c-family/c-common.h>
#include <diagnostic.h>
#include <langhooks.h>
#include <cgraph.h>
#include <function.h>
#include <basic-block.h>
#include <gimple-expr.h>
#include <gimple-walk.h>
#include <tree-cfg.h>
#include <cfghooks.h>
#include <stor-layout.h>
#include <varasm.h>
#include <output.h>

#include <cstdio>
#include <cstring>
#include <climits>
#include <algorithm>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <queue>

/* Required for GCC plugin licensing */
int plugin_is_GPL_compatible;

/* Plugin information */
static struct plugin_info narwhalyzer_info = {
    .version = "1.0.0",
    .help = "Narwhalyzer: Source-level profiling instrumentation\n"
            "\n"
            "Usage:\n"
            "  gcc -fplugin=narwhalyzer.so [options] source.c\n"
            "\n"
            "Options:\n"
            "  -fplugin-arg-narwhalyzer-verbose    Enable verbose output\n"
            "\n"
            "Pragma forms:\n"
            "  #pragma narwhalyzer section_name         - Structured (function)\n"
            "  #pragma narwhalyzer start section_name   - Start unstructured region\n"
            "  #pragma narwhalyzer stop section_name    - Stop unstructured region\n"
            "\n"
            "The structured pragma must appear immediately before a function definition.\n"
            "Start/stop pragmas can wrap arbitrary code regions.\n"
            "Link with -lnarwhalyzer_runtime to get the profiling report.\n"
};

/* ============================================================================
 * Configuration and Global State
 * ============================================================================ */

static bool g_verbose = false;

/* Pragma type enumeration */
enum class pragma_type {
    STRUCTURED,     /* Function-level instrumentation */
    START_REGION,   /* Start of unstructured region */
    STOP_REGION     /* End of unstructured region */
};

/* Structure to track pragma information */
struct pragma_info {
    std::string section_name;
    std::string filename;
    int line;
    location_t loc;
    pragma_type type;
};

/* Pending pragmas waiting to be applied */
static std::vector<pragma_info> g_pending_pragmas;

/* Map from function DECL to section info (for STRUCTURED pragmas) */
static std::map<tree, pragma_info> g_function_pragmas;

/* Map from function DECL to list of start/stop pragmas within that function */
static std::map<tree, std::vector<pragma_info>> g_function_regions;

/* Track which functions have been instrumented */
static std::set<tree> g_instrumented_functions;

/* Map to track active region context variables (section_name -> context_var) */
/* Used during GIMPLE instrumentation to connect start and stop */
static std::map<std::string, tree> g_active_region_contexts;

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/*
 * Build a string literal tree.
 */
static tree narwhalyzer_build_string_literal(const char *str)
{
    size_t len = strlen(str) + 1;
    tree str_type = build_array_type(char_type_node, 
                                      build_index_type(size_int(len - 1)));
    tree str_cst = build_string(len, str);
    TREE_TYPE(str_cst) = str_type;
    TREE_CONSTANT(str_cst) = 1;
    TREE_READONLY(str_cst) = 1;
    return str_cst;
}

/*
 * Build address of string literal.
 */
static tree build_string_addr(const char *str)
{
    tree str_cst = narwhalyzer_build_string_literal(str);
    tree ptr_type = build_pointer_type(char_type_node);
    return build1(ADDR_EXPR, ptr_type, str_cst);
}

/*
 * Declare an external function.
 */
static tree declare_runtime_function(const char *name, tree ret_type, 
                                       int nargs, ...)
{
    va_list ap;
    tree *arg_types = new tree[nargs];
    
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) {
        arg_types[i] = va_arg(ap, tree);
    }
    va_end(ap);
    
    /* Build parameter type list */
    tree param_types = void_list_node;
    for (int i = nargs - 1; i >= 0; i--) {
        param_types = tree_cons(NULL_TREE, arg_types[i], param_types);
    }
    
    delete[] arg_types;
    
    /* Build function type */
    tree fn_type = build_function_type(ret_type, param_types);
    
    /* Create declaration */
    tree fn_decl = build_fn_decl(name, fn_type);
    TREE_PUBLIC(fn_decl) = 1;
    DECL_EXTERNAL(fn_decl) = 1;
    DECL_ARTIFICIAL(fn_decl) = 1;
    
    return fn_decl;
}

/* ============================================================================
 * Pragma Handler
 * ============================================================================ */

/*
 * Handle #pragma narwhalyzer <section_name>
 * Handle #pragma narwhalyzer start <section_name>
 * Handle #pragma narwhalyzer stop <section_name>
 */
static void handle_pragma_narwhalyzer(cpp_reader *pfile ATTRIBUTE_UNUSED)
{
    tree token;
    enum cpp_ttype type;
    
    /* Get first token (section name or start/stop keyword) */
    type = pragma_lex(&token);
    
    if (type != CPP_NAME && type != CPP_STRING) {
        error_at(input_location, 
                 "%<#pragma narwhalyzer%> requires a section name or start/stop keyword");
        return;
    }
    
    pragma_info pinfo;
    pinfo.type = pragma_type::STRUCTURED;  /* Default to structured */
    
    if (type == CPP_NAME) {
        const char *first_token = IDENTIFIER_POINTER(token);
        
        /* Check for start/stop keywords */
        if (strcmp(first_token, "start") == 0) {
            pinfo.type = pragma_type::START_REGION;
            
            /* Get the section name after 'start' */
            type = pragma_lex(&token);
            if (type != CPP_NAME && type != CPP_STRING) {
                error_at(input_location,
                         "%<#pragma narwhalyzer start%> requires a section name");
                return;
            }
            
            if (type == CPP_NAME) {
                pinfo.section_name = IDENTIFIER_POINTER(token);
            } else {
                pinfo.section_name = TREE_STRING_POINTER(token);
            }
        } else if (strcmp(first_token, "stop") == 0) {
            pinfo.type = pragma_type::STOP_REGION;
            
            /* Get the section name after 'stop' */
            type = pragma_lex(&token);
            if (type != CPP_NAME && type != CPP_STRING) {
                error_at(input_location,
                         "%<#pragma narwhalyzer stop%> requires a section name");
                return;
            }
            
            if (type == CPP_NAME) {
                pinfo.section_name = IDENTIFIER_POINTER(token);
            } else {
                pinfo.section_name = TREE_STRING_POINTER(token);
            }
        } else {
            /* Regular section name */
            pinfo.section_name = first_token;
        }
    } else {
        /* CPP_STRING - this is a structured section name */
        const char *str = TREE_STRING_POINTER(token);
        pinfo.section_name = str;
    }
    
    /* Get location info */
    pinfo.loc = input_location;
    expanded_location xloc = expand_location(input_location);
    pinfo.filename = xloc.file ? xloc.file : "<unknown>";
    pinfo.line = xloc.line;
    
    /* Check for end of pragma */
    type = pragma_lex(&token);
    if (type != CPP_EOF) {
        warning_at(input_location, 0,
                   "extra tokens at end of %<#pragma narwhalyzer%>");
    }
    
    g_pending_pragmas.push_back(pinfo);
    
    if (g_verbose) {
        const char *type_str = (pinfo.type == pragma_type::STRUCTURED) ? "structured" :
                               (pinfo.type == pragma_type::START_REGION) ? "start" : "stop";
        inform(input_location, "narwhalyzer: recorded %s pragma for section %qs",
               type_str, pinfo.section_name.c_str());
    }
}

static void register_pragmas(void *event_data ATTRIBUTE_UNUSED,
                              void *data ATTRIBUTE_UNUSED)
{
    c_register_pragma(NULL, "narwhalyzer", handle_pragma_narwhalyzer);
    
    if (g_verbose) {
        inform(UNKNOWN_LOCATION, "narwhalyzer: pragma handler registered");
    }
}

/* ============================================================================
 * Function Pre-processing (Associate Pragmas with Functions)
 * ============================================================================ */

/*
 * Get the end location of a function body.
 */
static int get_function_end_line(tree fndecl)
{
    tree body = DECL_SAVED_TREE(fndecl);
    if (!body)
        return 0;
    
    location_t end_loc = EXPR_LOCATION(body);
    if (end_loc == UNKNOWN_LOCATION) {
        /* Try to get from function declaration */
        end_loc = DECL_SOURCE_LOCATION(fndecl);
    }
    
    /* For now, use a heuristic: assume function body spans a reasonable range */
    expanded_location xloc = expand_location(DECL_SOURCE_LOCATION(fndecl));
    return xloc.line + 10000; /* Large value to encompass function body */
}

/*
 * Called before parsing each function.
 * Associates pending pragmas with function declarations.
 * STRUCTURED pragmas are associated with functions for function-level instrumentation.
 * START_REGION and STOP_REGION pragmas within function bodies are also tracked.
 */
static void pre_genericize_callback(void *event_data, void *user_data ATTRIBUTE_UNUSED)
{
    tree fndecl = (tree)event_data;
    
    if (!fndecl || TREE_CODE(fndecl) != FUNCTION_DECL)
        return;
    
    /* Get function location */
    location_t fn_loc = DECL_SOURCE_LOCATION(fndecl);
    expanded_location fn_xloc = expand_location(fn_loc);
    
    if (!fn_xloc.file)
        return;
    
    int fn_end_line = get_function_end_line(fndecl);
    
    /* Process all pending pragmas */
    for (auto it = g_pending_pragmas.begin(); it != g_pending_pragmas.end(); ) {
        bool consumed = false;
        
        if (it->filename != fn_xloc.file) {
            ++it;
            continue;
        }
        
        if (it->type == pragma_type::STRUCTURED) {
            /* STRUCTURED pragma: must appear just before function definition */
            if (it->line < fn_xloc.line && fn_xloc.line - it->line <= 10) {
                g_function_pragmas[fndecl] = *it;
                
                if (g_verbose) {
                    inform(it->loc, 
                           "narwhalyzer: associating section %qs with function %qE",
                           it->section_name.c_str(), DECL_NAME(fndecl));
                }
                consumed = true;
            }
        } else {
            /* START_REGION or STOP_REGION: must be within function body */
            if (it->line >= fn_xloc.line && it->line <= fn_end_line) {
                /* This pragma is inside this function */
                g_function_regions[fndecl].push_back(*it);
                
                if (g_verbose) {
                    const char *type_str = (it->type == pragma_type::START_REGION) 
                                           ? "start" : "stop";
                    inform(it->loc, 
                           "narwhalyzer: associating %s region %qs with function %qE",
                           type_str, it->section_name.c_str(), DECL_NAME(fndecl));
                }
                consumed = true;
            }
        }
        
        if (consumed) {
            it = g_pending_pragmas.erase(it);
        } else {
            ++it;
        }
    }
}

/* ============================================================================
 * GIMPLE Instrumentation Pass
 * ============================================================================ */

namespace {

const pass_data narwhalyzer_gimple_pass_data = {
    GIMPLE_PASS,
    "narwhalyzer_instrument",
    OPTGROUP_NONE,
    TV_NONE,
    PROP_cfg | PROP_ssa,
    0,
    0,
    0,
    TODO_update_ssa | TODO_cleanup_cfg
};

class narwhalyzer_pass : public gimple_opt_pass
{
public:
    narwhalyzer_pass(gcc::context *ctx)
        : gimple_opt_pass(narwhalyzer_gimple_pass_data, ctx)
    {
    }
    
    virtual narwhalyzer_pass *clone() override
    {
        return new narwhalyzer_pass(m_ctxt);
    }
    
    virtual bool gate(function *fn) override
    {
        /* Run on functions that have associated pragmas (structured or regions) */
        tree fndecl = fn->decl;
        bool has_structured = g_function_pragmas.find(fndecl) != g_function_pragmas.end();
        bool has_regions = g_function_regions.find(fndecl) != g_function_regions.end();
        bool already_done = g_instrumented_functions.find(fndecl) != g_instrumented_functions.end();
        
        return (has_structured || has_regions) && !already_done;
    }
    
    virtual unsigned int execute(function *fn) override
    {
        tree fndecl = fn->decl;
        
        /* Handle structured (function-level) instrumentation */
        auto struct_it = g_function_pragmas.find(fndecl);
        if (struct_it != g_function_pragmas.end()) {
            const pragma_info &pinfo = struct_it->second;
            
            if (g_verbose) {
                inform(pinfo.loc,
                       "narwhalyzer: instrumenting %qE for section %qs",
                       DECL_NAME(fndecl), pinfo.section_name.c_str());
            }
            
            instrument_function(fn, pinfo);
        }
        
        /* Handle start/stop region instrumentation */
        auto region_it = g_function_regions.find(fndecl);
        if (region_it != g_function_regions.end()) {
            instrument_regions(fn, region_it->second);
        }
        
        g_instrumented_functions.insert(fndecl);
        
        return 0;
    }
    
private:
    void instrument_function(function *fn, const pragma_info &pinfo);
    void instrument_regions(function *fn, const std::vector<pragma_info> &regions);
    tree get_or_create_section_index_var(const pragma_info &pinfo);
    void insert_entry_instrumentation(function *fn, tree section_var);
    void insert_exit_instrumentation(function *fn, tree ctx_var);
    
    /* Helper to find the first statement at or after a given line */
    gimple_stmt_iterator find_stmt_at_line(function *fn, int line, basic_block *out_bb);
};

/*
 * Create a static variable to cache the section index.
 */
tree narwhalyzer_pass::get_or_create_section_index_var(const pragma_info &pinfo)
{
    /* Create unique variable name */
    char name[512];
    snprintf(name, sizeof(name), 
             "__narwhalyzer_idx_%s_%d", 
             pinfo.section_name.c_str(),
             pinfo.line);
    
    /* Create static integer variable initialized to -1 */
    tree var = build_decl(pinfo.loc, VAR_DECL,
                          get_identifier(name), integer_type_node);
    
    TREE_STATIC(var) = 1;
    TREE_PUBLIC(var) = 0;
    DECL_ARTIFICIAL(var) = 1;
    DECL_IGNORED_P(var) = 1;
    TREE_USED(var) = 1;
    DECL_INITIAL(var) = build_int_cst(integer_type_node, -1);
    DECL_CONTEXT(var) = NULL_TREE;
    
    /* Finalize the variable */
    varpool_node::finalize_decl(var);
    
    return var;
}

/*
 * Insert instrumentation at function entry and all exits.
 */
void narwhalyzer_pass::instrument_function(function *fn, 
                                            const pragma_info &pinfo)
{
    /* Get or create the section index variable */
    tree section_idx_var = get_or_create_section_index_var(pinfo);
    
    /* Create context variable for this invocation */
    tree ctx_var = create_tmp_var(integer_type_node, "nw_ctx");
    /* Force variable to stay in memory (not SSA) so it can be used
       across basic blocks (entry and multiple exit points) */
    DECL_NOT_GIMPLE_REG_P(ctx_var) = 1;
    
    /* Get the entry basic block */
    basic_block entry_bb = single_succ(ENTRY_BLOCK_PTR_FOR_FN(fn));
    gimple_stmt_iterator gsi = gsi_start_bb(entry_bb);
    
    /* Skip labels and debug statements */
    while (!gsi_end_p(gsi)) {
        gimple *stmt = gsi_stmt(gsi);
        if (gimple_code(stmt) != GIMPLE_LABEL &&
            gimple_code(stmt) != GIMPLE_DEBUG)
            break;
        gsi_next(&gsi);
    }
    
    /* Declare runtime functions */
    tree const_char_ptr = build_pointer_type(
        build_qualified_type(char_type_node, TYPE_QUAL_CONST));
    
    tree register_fn = declare_runtime_function(
        "__narwhalyzer_register_section",
        integer_type_node,
        3, const_char_ptr, const_char_ptr, integer_type_node);
    
    tree enter_fn = declare_runtime_function(
        "__narwhalyzer_section_enter",
        integer_type_node,
        1, integer_type_node);
    
    tree exit_fn = declare_runtime_function(
        "__narwhalyzer_section_exit",
        void_type_node,
        1, integer_type_node);
    
    /* Create temporary variables for SSA values */
    tree section_idx_tmp = create_tmp_var(integer_type_node, "narwh_idx");
    tree new_idx_tmp = create_tmp_var(integer_type_node, "narwh_new_idx");
    tree ctx_tmp = create_tmp_var(integer_type_node, "narwh_ctx");
    
    /* Load section index variable */
    gimple *load_stmt = gimple_build_assign(section_idx_tmp, section_idx_var);
    gimple_set_location(load_stmt, pinfo.loc);
    gsi_insert_before(&gsi, load_stmt, GSI_NEW_STMT);
    
    /* Call register_section (runtime handles caching) */
    tree name_arg = build_string_addr(pinfo.section_name.c_str());
    tree file_arg = build_string_addr(pinfo.filename.c_str());
    tree line_arg = build_int_cst(integer_type_node, pinfo.line);
    
    gcall *register_call = gimple_build_call(register_fn, 3,
                                              name_arg, file_arg, line_arg);
    gimple_call_set_lhs(register_call, new_idx_tmp);
    gimple_set_location(register_call, pinfo.loc);
    gsi_insert_after(&gsi, register_call, GSI_NEW_STMT);
    
    /* Store back to static variable */
    gimple *store_stmt = gimple_build_assign(section_idx_var, new_idx_tmp);
    gimple_set_location(store_stmt, pinfo.loc);
    gsi_insert_after(&gsi, store_stmt, GSI_NEW_STMT);
    
    /* Call section_enter */
    gcall *enter_call = gimple_build_call(enter_fn, 1, new_idx_tmp);
    gimple_call_set_lhs(enter_call, ctx_tmp);
    gimple_set_location(enter_call, pinfo.loc);
    gsi_insert_after(&gsi, enter_call, GSI_NEW_STMT);
    
    /* Store context to a regular variable so it's available at exits */
    gimple *store_ctx = gimple_build_assign(ctx_var, ctx_tmp);
    gimple_set_location(store_ctx, pinfo.loc);
    gsi_insert_after(&gsi, store_ctx, GSI_NEW_STMT);
    
    /* Insert exit calls before all return statements */
    basic_block bb;
    FOR_EACH_BB_FN(bb, fn) {
        gimple_stmt_iterator exit_gsi;
        
        for (exit_gsi = gsi_last_bb(bb); !gsi_end_p(exit_gsi); gsi_prev(&exit_gsi)) {
            gimple *stmt = gsi_stmt(exit_gsi);
            
            if (gimple_code(stmt) == GIMPLE_RETURN) {
                /* Load context variable */
                tree exit_ctx_tmp = create_tmp_var(integer_type_node, "narwh_exit_ctx");
                gimple *load_ctx = gimple_build_assign(exit_ctx_tmp, ctx_var);
                gimple_set_location(load_ctx, gimple_location(stmt));
                gsi_insert_before(&exit_gsi, load_ctx, GSI_SAME_STMT);
                
                /* Call section_exit */
                gcall *exit_call = gimple_build_call(exit_fn, 1, exit_ctx_tmp);
                gimple_set_location(exit_call, gimple_location(stmt));
                gsi_insert_before(&exit_gsi, exit_call, GSI_SAME_STMT);
            }
        }
    }
}

/*
 * Find the first GIMPLE statement at or after a given source line.
 * Returns an iterator to the found statement, or end iterator if not found.
 */
gimple_stmt_iterator narwhalyzer_pass::find_stmt_at_line(function *fn, int target_line,
                                                          basic_block *out_bb)
{
    basic_block bb;
    int best_line = INT_MAX;
    gimple_stmt_iterator best_gsi;
    basic_block best_bb = NULL;
    bool found = false;
    
    /* Scan all basic blocks looking for the first statement at or after target_line */
    FOR_EACH_BB_FN(bb, fn) {
        for (gimple_stmt_iterator gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
            gimple *stmt = gsi_stmt(gsi);
            location_t loc = gimple_location(stmt);
            
            if (loc == UNKNOWN_LOCATION)
                continue;
            
            expanded_location xloc = expand_location(loc);
            
            /* Look for statements at or after the target line */
            if (xloc.line >= target_line && xloc.line < best_line) {
                best_line = xloc.line;
                best_gsi = gsi;
                best_bb = bb;
                found = true;
            }
        }
    }
    
    if (out_bb)
        *out_bb = best_bb;
    
    if (!found) {
        /* Return an end iterator from the first basic block */
        bb = single_succ(ENTRY_BLOCK_PTR_FOR_FN(fn));
        gimple_stmt_iterator gsi = gsi_start_bb(bb);
        /* Advance to end */
        while (!gsi_end_p(gsi))
            gsi_next(&gsi);
        return gsi;
    }
    
    return best_gsi;
}

/*
 * Instrument start/stop regions within a function.
 * 
 * For each #pragma narwhalyzer start <name>, insert:
 *   - Section registration call
 *   - section_enter call
 *   - Store context to a variable specific to that region
 * 
 * For each #pragma narwhalyzer stop <name>, insert:
 *   - section_exit call using the context from the matching start
 */
void narwhalyzer_pass::instrument_regions(function *fn, 
                                           const std::vector<pragma_info> &regions)
{
    /* Sort regions by line number */
    std::vector<pragma_info> sorted_regions = regions;
    std::sort(sorted_regions.begin(), sorted_regions.end(),
              [](const pragma_info &a, const pragma_info &b) {
                  return a.line < b.line;
              });
    
    /* Declare runtime functions */
    tree const_char_ptr = build_pointer_type(
        build_qualified_type(char_type_node, TYPE_QUAL_CONST));
    
    tree register_fn = declare_runtime_function(
        "__narwhalyzer_register_section",
        integer_type_node,
        3, const_char_ptr, const_char_ptr, integer_type_node);
    
    tree enter_fn = declare_runtime_function(
        "__narwhalyzer_section_enter",
        integer_type_node,
        1, integer_type_node);
    
    tree exit_fn = declare_runtime_function(
        "__narwhalyzer_section_exit",
        void_type_node,
        1, integer_type_node);
    
    /* Map from section name to context variable (for matching start/stop) */
    std::map<std::string, tree> region_ctx_vars;
    
    /* Map from section name to section index variable */
    std::map<std::string, tree> region_idx_vars;
    
    /* Process each pragma in order */
    for (const pragma_info &pinfo : sorted_regions) {
        basic_block bb;
        gimple_stmt_iterator gsi = find_stmt_at_line(fn, pinfo.line + 1, &bb);
        
        if (gsi_end_p(gsi) || !bb) {
            if (g_verbose) {
                warning_at(pinfo.loc, 0,
                           "narwhalyzer: could not find statement after pragma at line %d",
                           pinfo.line);
            }
            continue;
        }
        
        if (pinfo.type == pragma_type::START_REGION) {
            /* Handle START pragma: register section and enter */
            
            if (g_verbose) {
                inform(pinfo.loc,
                       "narwhalyzer: instrumenting start of region %qs",
                       pinfo.section_name.c_str());
            }
            
            /* Get or create section index variable for this region */
            tree section_idx_var;
            auto idx_it = region_idx_vars.find(pinfo.section_name);
            if (idx_it != region_idx_vars.end()) {
                section_idx_var = idx_it->second;
            } else {
                section_idx_var = get_or_create_section_index_var(pinfo);
                region_idx_vars[pinfo.section_name] = section_idx_var;
            }
            
            /* Create context variable for this region instance */
            char ctx_name[256];
            snprintf(ctx_name, sizeof(ctx_name), "nw_region_ctx_%s_%d",
                     pinfo.section_name.c_str(), pinfo.line);
            tree ctx_var = create_tmp_var(integer_type_node, ctx_name);
            /* Force variable to stay in memory (not SSA) so it can be shared
               between START and STOP instrumentation across basic blocks */
            DECL_NOT_GIMPLE_REG_P(ctx_var) = 1;
            region_ctx_vars[pinfo.section_name] = ctx_var;
            
            /* Create temporary variables */
            tree new_idx_tmp = create_tmp_var(integer_type_node, "narwh_reg_idx");
            tree ctx_tmp = create_tmp_var(integer_type_node, "narwh_reg_ctx");
            
            /* Build arguments for register call */
            tree name_arg = build_string_addr(pinfo.section_name.c_str());
            tree file_arg = build_string_addr(pinfo.filename.c_str());
            tree line_arg = build_int_cst(integer_type_node, pinfo.line);
            
            /* Call register_section */
            gcall *register_call = gimple_build_call(register_fn, 3,
                                                      name_arg, file_arg, line_arg);
            gimple_call_set_lhs(register_call, new_idx_tmp);
            gimple_set_location(register_call, pinfo.loc);
            gsi_insert_before(&gsi, register_call, GSI_SAME_STMT);
            
            /* Store to section index variable */
            gimple *store_idx = gimple_build_assign(section_idx_var, new_idx_tmp);
            gimple_set_location(store_idx, pinfo.loc);
            gsi_insert_before(&gsi, store_idx, GSI_SAME_STMT);
            
            /* Call section_enter */
            gcall *enter_call = gimple_build_call(enter_fn, 1, new_idx_tmp);
            gimple_call_set_lhs(enter_call, ctx_tmp);
            gimple_set_location(enter_call, pinfo.loc);
            gsi_insert_before(&gsi, enter_call, GSI_SAME_STMT);
            
            /* Store context to variable */
            gimple *store_ctx = gimple_build_assign(ctx_var, ctx_tmp);
            gimple_set_location(store_ctx, pinfo.loc);
            gsi_insert_before(&gsi, store_ctx, GSI_SAME_STMT);
            
        } else if (pinfo.type == pragma_type::STOP_REGION) {
            /* Handle STOP pragma: call section_exit */
            
            if (g_verbose) {
                inform(pinfo.loc,
                       "narwhalyzer: instrumenting stop of region %qs",
                       pinfo.section_name.c_str());
            }
            
            /* Find the context variable from the matching start */
            auto ctx_it = region_ctx_vars.find(pinfo.section_name);
            if (ctx_it == region_ctx_vars.end()) {
                error_at(pinfo.loc,
                         "narwhalyzer: stop pragma for region %qs without matching start",
                         pinfo.section_name.c_str());
                continue;
            }
            
            tree ctx_var = ctx_it->second;
            
            /* Load context and call exit */
            tree exit_ctx_tmp = create_tmp_var(integer_type_node, "narwh_exit_ctx");
            gimple *load_ctx = gimple_build_assign(exit_ctx_tmp, ctx_var);
            gimple_set_location(load_ctx, pinfo.loc);
            gsi_insert_before(&gsi, load_ctx, GSI_SAME_STMT);
            
            /* Call section_exit */
            gcall *exit_call = gimple_build_call(exit_fn, 1, exit_ctx_tmp);
            gimple_set_location(exit_call, pinfo.loc);
            gsi_insert_before(&gsi, exit_call, GSI_SAME_STMT);
        }
    }
}

} /* anonymous namespace */

/* ============================================================================
 * Plugin Initialization
 * ============================================================================ */

int plugin_init(struct plugin_name_args *plugin_info,
                struct plugin_gcc_version *version)
{
    /* Version check */
    if (!plugin_default_version_check(version, &gcc_version)) {
        error("narwhalyzer: incompatible GCC version (requires %s.%s)",
              gcc_version.basever, gcc_version.devphase);
        return 1;
    }
    
    /* Parse plugin arguments */
    for (int i = 0; i < plugin_info->argc; i++) {
        if (strcmp(plugin_info->argv[i].key, "verbose") == 0) {
            g_verbose = true;
        }
    }
    
    const char *plugin_name = plugin_info->base_name;
    
    /* Register plugin information */
    register_callback(plugin_name, PLUGIN_INFO, NULL, &narwhalyzer_info);
    
    /* Register pragma handler */
    register_callback(plugin_name, PLUGIN_PRAGMAS, register_pragmas, NULL);
    
    /* Register pre-genericize callback to associate pragmas with functions */
    register_callback(plugin_name, PLUGIN_PRE_GENERICIZE, 
                      pre_genericize_callback, NULL);
    
    /* Register GIMPLE pass */
    struct register_pass_info pass_info;
    pass_info.pass = new narwhalyzer_pass(g);
    pass_info.reference_pass_name = "ssa";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
    
    register_callback(plugin_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
    
    if (g_verbose) {
        inform(UNKNOWN_LOCATION, "narwhalyzer plugin %s loaded",
               narwhalyzer_info.version);
    }
    
    return 0;
}
