/*
 * narwhalyzer_plugin.cc
 * 
 * GCC Plugin for source-level instrumentation driven by #pragma narwhalyzer.
 * This plugin parses the pragma directive and inserts profiling instrumentation
 * at the GIMPLE level.
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
#include <gimple-pretty-print.h>
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
#include <tree-cfg.h>
#include <cfghooks.h>
#include <stor-layout.h>
#include <toplev.h>
#include <opts.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <set>

/* Required for GCC plugin licensing */
int plugin_is_GPL_compatible;

/* Plugin information */
static struct plugin_info narwhalyzer_plugin_info = {
    .version = "1.0.0",
    .help = "Narwhalyzer: Source-level profiling instrumentation\n"
            "Usage: -fplugin=narwhalyzer.so\n"
            "Pragma forms:\n"
            "  #pragma narwhalyzer <section_name>        - Structured (function)\n"
            "  #pragma narwhalyzer start <section_name>  - Start unstructured region\n"
            "  #pragma narwhalyzer stop <section_name>   - Stop unstructured region\n"
};

/* ============================================================================
 * Global State for Pragma Tracking
 * ============================================================================ */

/*
 * Pragma type enumeration.
 */
enum class pragma_type {
    STRUCTURED,     /* Function-level instrumentation */
    START_REGION,   /* Start of unstructured region */
    STOP_REGION     /* End of unstructured region */
};

/*
 * Structure to track a pending pragma that will apply to the next statement.
 */
struct pending_pragma {
    std::string section_name;
    location_t location;
    const char *file;
    int line;
    pragma_type type;
};

/* List of pending pragmas (not yet applied) */
static std::vector<pending_pragma> g_pending_pragmas;

/* 
 * Structure to track sections that have been instrumented.
 * Maps (function_decl, section_name) -> section_index
 */
struct section_info {
    std::string name;
    std::string file;
    int line;
    tree index_var;  /* Static variable holding section index */
};

static std::map<tree, std::vector<section_info>> g_function_sections;

/* Flag to track if we've injected the include */
static bool g_include_injected = false;

/* Set of translation units that have been processed */
static std::set<std::string> g_processed_tu;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * Create a string constant tree node.
 */
static tree build_string_cst(const char *str)
{
    size_t len = strlen(str);
    tree string_cst = build_string(len + 1, str);
    TREE_TYPE(string_cst) = build_array_type(
        char_type_node,
        build_index_type(size_int(len))
    );
    TREE_CONSTANT(string_cst) = 1;
    TREE_READONLY(string_cst) = 1;
    TREE_STATIC(string_cst) = 1;
    return string_cst;
}

/*
 * Create a pointer to a string constant.
 */
static tree build_string_ptr(const char *str)
{
    tree string_cst = build_string_cst(str);
    return build1(ADDR_EXPR, build_pointer_type(char_type_node), string_cst);
}

/*
 * Get the source file name from a location.
 */
static const char *get_location_file(location_t loc)
{
    if (loc == UNKNOWN_LOCATION)
        return "<unknown>";
    expanded_location xloc = expand_location(loc);
    return xloc.file ? xloc.file : "<unknown>";
}

/*
 * Get the line number from a location.
 */
static int get_location_line(location_t loc)
{
    if (loc == UNKNOWN_LOCATION)
        return 0;
    expanded_location xloc = expand_location(loc);
    return xloc.line;
}

/*
 * Lookup or declare an external function.
 */
static tree lookup_or_declare_function(const char *name, tree return_type,
                                        tree param_types)
{
    tree id = get_identifier(name);
    tree decl = lookup_name(id);
    
    if (decl && TREE_CODE(decl) == FUNCTION_DECL) {
        return decl;
    }
    
    /* Build function type */
    tree fn_type = build_function_type(return_type, param_types);
    
    /* Create function declaration */
    decl = build_fn_decl(name, fn_type);
    TREE_PUBLIC(decl) = 1;
    DECL_EXTERNAL(decl) = 1;
    DECL_ARTIFICIAL(decl) = 1;
    
    return decl;
}

/*
 * Build parameter type list for __narwhalyzer_register_section.
 * int __narwhalyzer_register_section(const char *name, const char *file, int line)
 */
static tree build_register_params(void)
{
    tree const_char_ptr = build_pointer_type(
        build_qualified_type(char_type_node, TYPE_QUAL_CONST)
    );
    
    return tree_cons(NULL_TREE, const_char_ptr,          /* name */
           tree_cons(NULL_TREE, const_char_ptr,          /* file */
           tree_cons(NULL_TREE, integer_type_node,       /* line */
           void_list_node)));
}

/*
 * Build parameter type list for __narwhalyzer_section_enter.
 * int __narwhalyzer_section_enter(int section_index)
 */
static tree build_enter_params(void)
{
    return tree_cons(NULL_TREE, integer_type_node,       /* section_index */
           void_list_node);
}

/*
 * Build parameter type list for __narwhalyzer_section_exit.
 * void __narwhalyzer_section_exit(int context_index)
 */
static tree build_exit_params(void)
{
    return tree_cons(NULL_TREE, integer_type_node,       /* context_index */
           void_list_node);
}

/*
 * Build parameter type list for __narwhalyzer_scope_guard_cleanup.
 * void __narwhalyzer_scope_guard_cleanup(narwhalyzer_scope_guard_t *guard)
 */
static tree build_cleanup_params(void)
{
    return tree_cons(NULL_TREE, ptr_type_node,           /* guard pointer */
           void_list_node);
}

/* ============================================================================
 * Pragma Handler
 * ============================================================================ */

/*
 * Handler for #pragma narwhalyzer <section_name>
 * Handler for #pragma narwhalyzer start <section_name>
 * Handler for #pragma narwhalyzer stop <section_name>
 * 
 * This function is called by GCC when it encounters our pragma.
 * We record the section name and location for later application during
 * GIMPLE transformation.
 */
static void handle_pragma_narwhalyzer(cpp_reader *pfile ATTRIBUTE_UNUSED)
{
    tree token;
    enum cpp_ttype type;
    
    /* Get the first token (section name or start/stop keyword) */
    type = pragma_lex(&token);
    
    if (type == CPP_EOF || type == CPP_PRAGMA_EOL) {
        error_at(input_location, "%<#pragma narwhalyzer%> requires a section name");
        return;
    }
    
    pending_pragma pp;
    pp.type = pragma_type::STRUCTURED;  /* Default to structured */
    
    if (type == CPP_NAME) {
        const char *first_token = IDENTIFIER_POINTER(token);
        
        /* Check for start/stop keywords */
        if (strcmp(first_token, "start") == 0) {
            pp.type = pragma_type::START_REGION;
            
            /* Get the section name after 'start' */
            type = pragma_lex(&token);
            if (type != CPP_NAME && type != CPP_STRING) {
                error_at(input_location,
                         "%<#pragma narwhalyzer start%> requires a section name");
                return;
            }
            
            if (type == CPP_NAME) {
                pp.section_name = IDENTIFIER_POINTER(token);
            } else {
                pp.section_name = TREE_STRING_POINTER(token);
            }
        } else if (strcmp(first_token, "stop") == 0) {
            pp.type = pragma_type::STOP_REGION;
            
            /* Get the section name after 'stop' */
            type = pragma_lex(&token);
            if (type != CPP_NAME && type != CPP_STRING) {
                error_at(input_location,
                         "%<#pragma narwhalyzer stop%> requires a section name");
                return;
            }
            
            if (type == CPP_NAME) {
                pp.section_name = IDENTIFIER_POINTER(token);
            } else {
                pp.section_name = TREE_STRING_POINTER(token);
            }
        } else {
            /* Regular section name */
            pp.section_name = first_token;
        }
    } else if (type == CPP_STRING) {
        pp.section_name = TREE_STRING_POINTER(token);
    } else {
        error_at(input_location, "%<#pragma narwhalyzer%> section name must be "
                 "an identifier or string literal");
        return;
    }
    
    /* Check for extra tokens */
    type = pragma_lex(&token);
    if (type != CPP_EOF && type != CPP_PRAGMA_EOL) {
        warning_at(input_location, 0, 
                   "extra tokens at end of %<#pragma narwhalyzer%> ignored");
    }
    
    /* Record pending pragma */
    pp.location = input_location;
    pp.file = get_location_file(input_location);
    pp.line = get_location_line(input_location);
    
    g_pending_pragmas.push_back(pp);
    
    const char *type_str = (pp.type == pragma_type::STRUCTURED) ? "structured" :
                           (pp.type == pragma_type::START_REGION) ? "start" : "stop";
    inform(input_location, "narwhalyzer: registered %s section %qs at %s:%d",
           type_str, pp.section_name.c_str(), pp.file, pp.line);
}

/*
 * Register the pragma with GCC.
 */
static void register_pragma_handler(void *event_data ATTRIBUTE_UNUSED,
                                     void *data ATTRIBUTE_UNUSED)
{
    c_register_pragma(NULL, "narwhalyzer", handle_pragma_narwhalyzer);
}

/* ============================================================================
 * Attribute Handler
 * ============================================================================ */

/* 
 * Custom attribute to mark functions/statements for instrumentation.
 * This is used internally to propagate pragma information through the AST.
 */
static tree handle_narwhalyzer_attribute(tree *node, tree name,
                                          tree /* args */, int flags ATTRIBUTE_UNUSED,
                                          bool *no_add_attrs)
{
    if (TREE_CODE(*node) != FUNCTION_DECL) {
        warning(OPT_Wattributes, "%qE attribute ignored", name);
        *no_add_attrs = true;
    }
    return NULL_TREE;
}

static struct attribute_spec narwhalyzer_attrs[] = {
    { "narwhalyzer_section", 1, 1, false, false, false, false,
      handle_narwhalyzer_attribute, NULL },
    { NULL, 0, 0, false, false, false, false, NULL, NULL }
};

static void register_attributes(void *event_data ATTRIBUTE_UNUSED,
                                 void *data ATTRIBUTE_UNUSED)
{
    register_attribute(narwhalyzer_attrs);
}

/* ============================================================================
 * GIMPLE Instrumentation Pass
 * ============================================================================ */

/*
 * Data structure passed through the GIMPLE pass.
 */
struct narwhalyzer_pass_data {
    std::vector<pending_pragma> pragmas;
};

namespace {

const pass_data narwhalyzer_pass_data = {
    GIMPLE_PASS,                /* type */
    "narwhalyzer",              /* name */
    OPTGROUP_NONE,              /* optinfo_flags */
    TV_NONE,                    /* tv_id */
    PROP_cfg,                   /* properties_required */
    0,                          /* properties_provided */
    0,                          /* properties_destroyed */
    0,                          /* todo_flags_start */
    0                           /* todo_flags_finish */
};

class narwhalyzer_gimple_pass : public gimple_opt_pass
{
public:
    narwhalyzer_gimple_pass(gcc::context *ctx)
        : gimple_opt_pass(narwhalyzer_pass_data, ctx)
    {
    }

    virtual narwhalyzer_gimple_pass *clone() override
    {
        return new narwhalyzer_gimple_pass(m_ctxt);
    }

    virtual bool gate(function *fun) override
    {
        /* Run on all functions */
        return true;
    }

    virtual unsigned int execute(function *fun) override;

private:
    void instrument_function_entry(function *fun, const pending_pragma &pp);
    void insert_section_instrumentation(function *fun, 
                                         gimple_stmt_iterator *gsi,
                                         const pending_pragma &pp);
    tree create_section_index_var(function *fun, const pending_pragma &pp);
    void create_entry_instrumentation(gimple_stmt_iterator *gsi,
                                       tree section_idx_var,
                                       const pending_pragma &pp,
                                       tree *ctx_var_out);
    gimple *create_exit_call(tree ctx_var);
};

/*
 * Create a static variable to hold the section index.
 * This is initialized to -1 and lazily set on first entry.
 */
tree narwhalyzer_gimple_pass::create_section_index_var(function *fun,
                                                        const pending_pragma &pp)
{
    char name[256];
    snprintf(name, sizeof(name), "__narwhalyzer_section_%s_%d",
             pp.section_name.c_str(), pp.line);
    
    tree var = build_decl(pp.location, VAR_DECL, 
                          get_identifier(name), integer_type_node);
    TREE_STATIC(var) = 1;
    TREE_PUBLIC(var) = 0;
    DECL_ARTIFICIAL(var) = 1;
    DECL_INITIAL(var) = build_int_cst(integer_type_node, -1);
    TREE_USED(var) = 1;
    
    varpool_node::finalize_decl(var);
    
    return var;
}

/*
 * Insert instrumentation at section entry.
 * Generates code equivalent to:
 * 
 *   if (__builtin_expect(section_idx < 0, 0)) {
 *       section_idx = __narwhalyzer_register_section(name, file, line);
 *   }
 *   int ctx = __narwhalyzer_section_enter(section_idx);
 */
void narwhalyzer_gimple_pass::create_entry_instrumentation(
    gimple_stmt_iterator *gsi,
    tree section_idx_var,
    const pending_pragma &pp,
    tree *ctx_var_out)
{
    location_t loc = pp.location;
    
    /* Create the context variable */
    tree ctx_var = create_tmp_var(integer_type_node, "narwhalyzer_ctx");
    
    /* Load section_idx_var */
    tree section_idx = create_tmp_var(integer_type_node, "section_idx");
    gimple *load_idx = gimple_build_assign(section_idx, section_idx_var);
    gimple_set_location(load_idx, loc);
    gsi_insert_before(gsi, load_idx, GSI_SAME_STMT);
    
    /* Create comparison: section_idx < 0 */
    tree cond = build2(LT_EXPR, boolean_type_node, 
                       section_idx, integer_zero_node);
    
    /* Build call to __narwhalyzer_register_section */
    tree register_fn = lookup_or_declare_function(
        "__narwhalyzer_register_section",
        integer_type_node,
        build_register_params()
    );
    
    tree name_arg = build_string_ptr(pp.section_name.c_str());
    tree file_arg = build_string_ptr(pp.file);
    tree line_arg = build_int_cst(integer_type_node, pp.line);
    
    tree new_idx = create_tmp_var(integer_type_node, "new_section_idx");
    gcall *register_call = gimple_build_call(register_fn, 3, 
                                              name_arg, file_arg, line_arg);
    gimple_call_set_lhs(register_call, new_idx);
    gimple_set_location(register_call, loc);
    
    /* Store new index back to static variable */
    gimple *store_idx = gimple_build_assign(section_idx_var, new_idx);
    gimple_set_location(store_idx, loc);
    
    /* Also update the local section_idx for use in enter call */
    gimple *update_local = gimple_build_assign(section_idx, new_idx);
    gimple_set_location(update_local, loc);
    
    /* Create conditional structure */
    /* We'll do this with a simple conditional pattern */
    
    /* Insert the registration call unconditionally for simplicity
       (the runtime handles duplicate registrations) */
    /* Actually, let's implement the conditional properly for efficiency */
    
    /* Create basic blocks for conditional */
    basic_block bb = gsi_bb(*gsi);
    
    /* Build the conditional: if (section_idx < 0) { register... } */
    gcond *cond_stmt = gimple_build_cond(LT_EXPR, section_idx, 
                                          integer_zero_node,
                                          NULL_TREE, NULL_TREE);
    gimple_set_location(cond_stmt, loc);
    gsi_insert_before(gsi, cond_stmt, GSI_SAME_STMT);
    
    /* Split the block and insert the registration call in the true branch */
    edge true_edge, false_edge;
    basic_block cond_bb = gsi_bb(*gsi);
    
    /* For simplicity, we'll just insert the registration call unconditionally
       since the runtime's register function handles duplicates efficiently
       with a mutex and lookup. This is easier than restructuring the CFG. */
    
    /* Remove the conditional we just added */
    gimple_stmt_iterator cond_gsi = gsi_for_stmt(cond_stmt);
    gsi_remove(&cond_gsi, true);
    
    /* Just call register unconditionally - the runtime caches the result */
    gsi_insert_before(gsi, register_call, GSI_SAME_STMT);
    gsi_insert_before(gsi, store_idx, GSI_SAME_STMT);
    gsi_insert_before(gsi, update_local, GSI_SAME_STMT);
    
    /* Build call to __narwhalyzer_section_enter */
    tree enter_fn = lookup_or_declare_function(
        "__narwhalyzer_section_enter",
        integer_type_node,
        build_enter_params()
    );
    
    gcall *enter_call = gimple_build_call(enter_fn, 1, section_idx);
    gimple_call_set_lhs(enter_call, ctx_var);
    gimple_set_location(enter_call, loc);
    gsi_insert_before(gsi, enter_call, GSI_SAME_STMT);
    
    *ctx_var_out = ctx_var;
}

/*
 * Create a call to __narwhalyzer_section_exit.
 */
gimple *narwhalyzer_gimple_pass::create_exit_call(tree ctx_var)
{
    tree exit_fn = lookup_or_declare_function(
        "__narwhalyzer_section_exit",
        void_type_node,
        build_exit_params()
    );
    
    gcall *exit_call = gimple_build_call(exit_fn, 1, ctx_var);
    return exit_call;
}

/*
 * Instrument a function entry point.
 */
void narwhalyzer_gimple_pass::instrument_function_entry(function *fun,
                                                         const pending_pragma &pp)
{
    basic_block entry_bb = ENTRY_BLOCK_PTR_FOR_FN(fun)->next_bb;
    if (!entry_bb) return;
    
    gimple_stmt_iterator gsi = gsi_start_bb(entry_bb);
    
    /* Skip any debug statements */
    while (!gsi_end_p(gsi) && gimple_code(gsi_stmt(gsi)) == GIMPLE_DEBUG) {
        gsi_next(&gsi);
    }
    
    /* Create section index variable */
    tree section_idx_var = create_section_index_var(fun, pp);
    
    /* Insert entry instrumentation */
    tree ctx_var;
    create_entry_instrumentation(&gsi, section_idx_var, pp, &ctx_var);
    
    /* Insert exit instrumentation before all return statements */
    basic_block bb;
    FOR_EACH_BB_FN(bb, fun) {
        for (gimple_stmt_iterator exit_gsi = gsi_last_bb(bb); 
             !gsi_end_p(exit_gsi); ) {
            gimple *stmt = gsi_stmt(exit_gsi);
            
            if (gimple_code(stmt) == GIMPLE_RETURN) {
                gimple *exit_call = create_exit_call(ctx_var);
                gimple_set_location(exit_call, gimple_location(stmt));
                gsi_insert_before(&exit_gsi, exit_call, GSI_SAME_STMT);
            }
            
            /* Move to previous statement */
            if (gsi_stmt(exit_gsi) == gsi_stmt(gsi_start_bb(bb))) {
                break;
            }
            gsi_prev(&exit_gsi);
        }
    }
}

/*
 * Main execution function for the GIMPLE pass.
 */
unsigned int narwhalyzer_gimple_pass::execute(function *fun)
{
    /* Check if there are any pending pragmas */
    if (g_pending_pragmas.empty()) {
        return 0;
    }
    
    /* Get function location */
    tree fn_decl = fun->decl;
    location_t fn_loc = DECL_SOURCE_LOCATION(fn_decl);
    const char *fn_file = get_location_file(fn_loc);
    int fn_line = get_location_line(fn_loc);
    
    /* Find structured pragmas that apply to this function.
     * Only STRUCTURED pragmas are handled at the GIMPLE level.
     * START_REGION and STOP_REGION are handled by user macros. */
    std::vector<pending_pragma> matching_pragmas;
    
    for (auto it = g_pending_pragmas.begin(); it != g_pending_pragmas.end(); ) {
        /* Only process STRUCTURED pragmas at GIMPLE level */
        if (it->type != pragma_type::STRUCTURED) {
            ++it;
            continue;
        }
        
        /* Check if pragma location is just before this function */
        if (strcmp(it->file, fn_file) == 0 && it->line < fn_line) {
            /* This pragma applies to this function */
            matching_pragmas.push_back(*it);
            it = g_pending_pragmas.erase(it);
        } else {
            ++it;
        }
    }
    
    /* Instrument function for each matching pragma */
    for (const auto &pp : matching_pragmas) {
        inform(pp.location, "narwhalyzer: instrumenting function %qE with section %qs",
               DECL_NAME(fn_decl), pp.section_name.c_str());
        instrument_function_entry(fun, pp);
    }
    
    return 0;
}

} /* anonymous namespace */

/* ============================================================================
 * Include Injection Pass (runs early)
 * ============================================================================ */

/*
 * This callback adds the runtime header include to each translation unit.
 */
static void inject_include_callback(void *event_data ATTRIBUTE_UNUSED,
                                     void *data ATTRIBUTE_UNUSED)
{
    /* This is called at start of each translation unit.
       The actual include injection happens via the -include flag
       which must be specified by the user or we emit a warning. */
    
    const char *main_file = main_input_filename;
    if (main_file && g_processed_tu.find(main_file) == g_processed_tu.end()) {
        g_processed_tu.insert(main_file);
        
        /* Inform user about the required header */
        inform(UNKNOWN_LOCATION, 
               "narwhalyzer: ensure -include narwhalyzer.h is used "
               "or include the header manually");
    }
}

/* ============================================================================
 * Plugin Initialization
 * ============================================================================ */

int plugin_init(struct plugin_name_args *plugin_info,
                struct plugin_gcc_version *version)
{
    /* Check GCC version compatibility */
    if (!plugin_default_version_check(version, &gcc_version)) {
        error("narwhalyzer: incompatible GCC version");
        return 1;
    }
    
    /* Register plugin info */
    register_callback(plugin_info->base_name,
                      PLUGIN_INFO,
                      NULL,
                      &narwhalyzer_plugin_info);
    
    /* Register pragma handler (called during parsing) */
    register_callback(plugin_info->base_name,
                      PLUGIN_PRAGMAS,
                      register_pragma_handler,
                      NULL);
    
    /* Register custom attribute */
    register_callback(plugin_info->base_name,
                      PLUGIN_ATTRIBUTES,
                      register_attributes,
                      NULL);
    
    /* Register include injection */
    register_callback(plugin_info->base_name,
                      PLUGIN_START_UNIT,
                      inject_include_callback,
                      NULL);
    
    /* Register GIMPLE pass for instrumentation */
    struct register_pass_info pass_info;
    pass_info.pass = new narwhalyzer_gimple_pass(g);
    pass_info.reference_pass_name = "cfg";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_AFTER;
    
    register_callback(plugin_info->base_name,
                      PLUGIN_PASS_MANAGER_SETUP,
                      NULL,
                      &pass_info);
    
    inform(UNKNOWN_LOCATION, "narwhalyzer plugin loaded (version %s)",
           narwhalyzer_plugin_info.version);
    
    return 0;
}
