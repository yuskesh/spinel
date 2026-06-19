/* Internal shared state and declarations for the split code generator.
 * The generator was one 19k-line file; it is now split by emission stage
 * (util / fold / call / expr / stmt / decl+driver). Everything here was
 * file-static in the single file and is shared between the parts. */
#ifndef SPINEL_CODEGEN_INTERNAL_H
#define SPINEL_CODEGEN_INTERNAL_H
/* M2 code generator: the M1 scalar/control-flow subset plus user-defined
 * methods (required params, inferred param/return types, recursion, tail-
 * position implicit returns). Emits the same runtime ABI as the legacy
 * generator. Unsupported constructs abort loudly.
 */
#include "codegen.h"
#include "compiler.h"
#include "analyze.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>

/* ---- output buffer ---- */

typedef struct { char *p; size_t len, cap; } Buf;

/* Buffer ops (defined in codegen_util.c). Declared here, before the
   inline emit_indent below uses buf_puts -- otherwise the inline body
   references an undeclared function (clang errors, gcc warns). */
void buf_putn(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);

static inline void emit_indent(Buf *b, int n) { for (int i = 0; i < n; i++) buf_puts(b, "  "); }

/* Statement prelude: some expressions (array/hash literals) lower to
   temp-variable construction that must run before the statement that
   uses them. While a statement line is being built, g_pre collects those
   setup lines at g_indent; the statement wrapper flushes g_pre before the
   line. g_tmp hands out unique temp ids. */
extern Buf *g_pre;
extern int  g_indent;
extern int  g_tmp;

/* Inlining a yielding method: method-local names are renamed (to avoid
   clashing with the call site's locals), and yield emits the active
   block's body. g_block_id is the current BlockNode for yield (-1 if
   none). The rename map holds only the inlined method's locals. */
#define MAX_RENAME 128
extern char g_ren_from[MAX_RENAME][96];
extern char g_ren_to[MAX_RENAME][112];
extern int  g_nren;
extern int  g_block_id;
/* Argument-hoist overrides (see emit_args_filled): node id -> rooted temp
   name substituted by emit_expr. */
#define MAX_ARG_OVERRIDE 64
extern int  g_argov_node[MAX_ARG_OVERRIDE];
extern char g_argov_text[MAX_ARG_OVERRIDE][16];
extern int  g_n_argov;
int subtree_may_allocate(const NodeTable *nt, int id);
/* When a yielding method is inlined, g_yield_block_fallback holds the block
   that was active in the CALLER's context so nested `yield`s inside the
   passed block can chain back to the outermost caller's block. */
extern int  g_yield_block_fallback;
/* Name of the `&block` parameter of the method currently being inlined, so
   `<blk>.call(args)` inside it expands the active block like `yield args`. */
extern const char *g_block_param_name;
/* Result temp for a do{}while(0)-wrapped instance_exec splice; a top-level
   `next <v>` captures into it before continuing out. NULL otherwise. */
extern const char *g_ie_next_var;
/* The C expression for `self` (a pointer). Overridden while inlining an
   instance method at a call site (where there is no real `self` param). */
extern const char *g_self;
extern const char *g_self_deref;
/* When emitting class/module body statements, the class index (-1 outside). */
extern int g_class_body_id;
/* Class id of the scope currently being emitted (-1 if none). Used to resolve
   implicit self calls in included-module methods to the including class. */
extern int g_emitting_class_id;
/* While emitting a compile-time-unrolled define_method body: the loop-var
   name to substitute and the literal node to emit in its place (-1 = none). */
extern const char *g_dm_subst_name;
extern int g_dm_subst_node;
/* When inside an instance_eval block, the class id of the receiver (-1 outside).
   Used so InstanceVariableReadNode/WriteNode use g_self->iv_X instead of civ_Toplevel_X. */
extern int g_ie_class_id;
/* Set while emitting an instance_eval/exec splice in statement position: the
   block's value is discarded, so the last statement emits as a statement
   (not coerced to an expression, which would fail for e.g. a trailing puts). */
extern int g_ie_discard_value;
/* While emitting a rescue handler: the C var names holding the caught
   exception's class/message, so a bare `raise` can re-raise. */
extern const char *g_rescue_cls, *g_rescue_msg;
/* When inside a rescue handler that can `retry`, holds the goto label for the
   retry target (just before `sp_exc_top++`). NULL otherwise. */
extern const char *g_retry_label;
/* Redo label stack: each enclosing loop that contains a `redo` pushes a fresh
   C label id; a RedoNode emits `goto _redo_<top>` to re-run the current
   iteration without re-testing the guard or advancing the iterator. */
extern int g_redo_stack[64];
extern int g_redo_depth;

/* When set inside a loop-as-expression, BreakNode assigns its value here. */
extern const char *g_loop_break_var;
extern const char *g_hoist_len_var;
extern const char *g_hoist_len_recv;
/* When set, tail positions assign to this var instead of `return`ing
   (used to give a begin/rescue a value). */
extern const char *g_result_var;
/* When g_result_var is set, whether that result slot is poly (so a scalar
   tail value must be boxed into it). */
extern int g_result_poly;
/* Return type of the method currently being emitted, so a tail/return value
   can be boxed when the method returns poly but the value is concrete. */
extern TyKind g_ret_type;
/* Set while emitting a self-recursive yield method (is_lowered_yield=1).
   Persists into inner proc literal bodies so { yield } forwards __yblk__. */
extern int g_current_scope_is_lowered;

/* When set (SPINEL_LINE_MAP / SPINEL_DEBUG), emit `#line N "file"` directives
   at statement boundaries so a C compile error is reported against the
   original Ruby source line. Set once by codegen_program. */
extern int g_line_map;
extern int g_debug;
/* Emit a `#line` directive for node `id` into `b`, deduped against the last
   one emitted. No-op when g_line_map is off or the node has no line stamp. */
void emit_line_directive(Compiler *c, int id, Buf *b);

/* Ensure context stack for deferred `return` inside begin..ensure.
   When `return` appears in the body of a begin..ensure block, the return
   is deferred until after the ensure clause runs.  Each ensure clause
   pushes a context on this stack; emit_return uses the top to emit a
   deferred goto instead of a bare C `return`. */
#define MAX_ENSURE_DEPTH 32
typedef struct { int lid; int has_retval; } EnsureCtx;
extern EnsureCtx g_ensure_stack[MAX_ENSURE_DEPTH];
extern int       g_ensure_depth;

/* First-class Proc support: each `proc {}` / `lambda {}` / `->{}` literal
   lowers to a standalone `static mrb_int _proc_N(void *cap, mrb_int *args)`
   function (the ABI sp_proc_call expects). Definitions accumulate in g_procs
   and prototypes in g_proc_protos during the main emission pass, then are
   flushed ahead of the method/main bodies that reference them. */
extern Buf g_procs;
extern Buf g_proc_protos;
extern int g_proc_counter;
extern int g_needs_proc_poly_retslot; /* any proc returns TY_POLY via _sp_proc_poly_ret */
extern int g_needs_proc_poly_argslot; /* any proc takes a TY_POLY arg via _sp_proc_poly_args */
/* Fiber body functions accumulate here (similar to g_procs but void(*)(sp_Fiber*)). */
extern int g_fiber_counter;

/* Static regex-literal table: each distinct (source, flags) pair compiles once
   to an sp_re_pat_<i> global initialized in sp_re_init(). */
extern char **g_re_src;
extern int *g_re_flg;
extern int g_re_count, g_re_cap;
/* Map Prism regex flag bits (IGNORE_CASE=4, EXTENDED=8, MULTI_LINE=16) to the
   engine's RE_FLAG_* (IGNORECASE=1, MULTILINE=2, DOTALL=4, EXTENDED=8); Ruby's
   /m means dot-matches-newline -> MULTILINE|DOTALL = 6. */
/* True if a regex source contains a capturing group: an unescaped '(' that
   isn't the start of a non-capturing/extension group '(?...'. scan returns
   nested arrays for capturing patterns, which the str_array path can't model. */

/* Find or add a RegularExpressionNode literal; returns its table index, or
   -1 if the node isn't a static regex literal. */
/* The unescaped source of a regex literal or a constant bound to one (for
   capture detection). Returns NULL when nid is not a resolvable regex. */


/* A set of local names (borrowed pointers into the node table). */
typedef struct { const char **v; int n, cap; } NameSet;
/* While emitting a capturing proc's body: the cap struct's C type name and the
   set of captured names, so a read/write of a captured var routes to the cell
   held in `_cap` instead of a (non-existent) local. NULL outside such a body. */
extern const char *g_cap_struct;
extern NameSet *g_cap_names;
/* set when the program registers an at_exit hook; main()'s tail then runs them
   in reverse registration order. */
extern int g_needs_at_exit;
/* set when the program may use class-introspection machinery (user classes, or
   .class / is_a? / kind_of? / instance_of? / ancestors / superclass / === on
   builtins, or a builtin class constant used as a value). When clear, the
   sp_class_* / sp_poly_is_a / sp_user_exc_parent helper bank is not emitted --
   a minimal program like `p 42` carries none of it. */
extern int g_needs_class_machinery;

const char *rename_local(const char *nm);

/* Emit the C lvalue for local `name` in the current emission context: a
   captured var inside a proc body -> the cell in _cap; a cell local in its
   enclosing scope -> `(*_cell_x)`; otherwise the plain `lv_x`. Reads and
   writes share this (a cell deref is a valid lvalue). */

/* Emit `sp_Proc *` reference to the synthetic __yblk__ param of a lowered
   self-recursive yield method.  If we are inside an inner proc literal that
   captures __yblk__ via a cell, cast back from the mrb_int cell slot. */

/* Emit the lead of a tail value: `return ` or `<result> = `. */


/* ---- diagnostics ---- */


/* ---- builtin class IDs (negative, distinct from user cls_ids >= 0) ---- */
/* Returns a negative cls_id for well-known builtin class/module names,
   or 0 if the name is not a recognized builtin class. */

/* ---- type -> C ---- */

/* Map an FFI type spec string to the C type used in extern prototypes.
   Uses standard C types to avoid conflicting with system headers. */

/* Append the C type name for `t` to `b` (objects need the class name). */

/* Emit the boxing prefix/suffix to convert a typed value to sp_RbVal.
   Call as: emit_box_open(t, b); emit_expr(c, node, b); emit_box_close(t, b). */

/* "Int" / "Str" / "Float" for the sp_<K>Array_* runtime family. */

/* ---- C string literals ---- */



/* Emit a Ruby string literal. len is the true byte count (may exceed strlen
   when the string contains embedded NUL bytes). */


/* Emit a catch/throw tag (a Symbol or String literal) as a `const char *`.
   The same literal text is produced for both catch and throw sites so the
   runtime's strcmp tag match succeeds. Falls back to a runtime string expr. */
void emit_expr(Compiler *c, int id, Buf *b);

/* ---- forward decls ---- */

int is_builtin_reopen(const char *name);
int is_exc_name(const char *n);
int class_is_exc_subclass(Compiler *c, int ci);
const char *class_ruby_name(Compiler *c, int ci);
const char *exc_builtin_parent(Compiler *c, int ci);
void emit_method_cname(Compiler *c, Scope *s, Buf *b);
void emit_expr(Compiler *c, int id, Buf *b);
void emit_stmt(Compiler *c, int id, Buf *b, int indent);
void emit_stmts(Compiler *c, int id, Buf *b, int indent);
void emit_stmts_tail(Compiler *c, int id, Buf *b, int indent);
void emit_op_assign(Compiler *c, int id, Buf *b, int indent);
void emit_begin(Compiler *c, int id, Buf *b, int indent, const char *resultvar);
int  emit_array_mutate_stmt(Compiler *c, int id, Buf *b, int indent);
int  emit_output_call(Compiler *c, int id, Buf *b, int indent);
int  emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent);
void emit_loop_body(Compiler *c, int body, Buf *b, int indent);
int  subtree_has_own_redo(const NodeTable *nt, int id);
int  emit_inline_call(Compiler *c, int id, Buf *b, int indent);
int  emit_inline_expr(Compiler *c, int id, Buf *b);
void emit_cond(Compiler *c, int id, Buf *b);
void emit_fiber_new(Compiler *c, int id, Buf *b);
int  needs_root(TyKind t);
int  method_is_void(Scope *s);
void emit_index_op_write(Compiler *c, int id, Buf *b, int indent);
void emit_index_and_or_write(Compiler *c, int id, Buf *b, int indent, int is_or);
void emit_boxed(Compiler *c, int node, Buf *b);
void emit_super(Compiler *c, int id, Buf *b);
int  emit_super_inline(Compiler *c, int id, Buf *b, int indent, int as_expr);
void emit_args_filled(Compiler *c, int callee_idx, int argsNode, const char *lead, Buf *out);
void emit_boxed(Compiler *c, int node, Buf *b);
/* Emit a hash key, unboxing a poly value to the typed-hash's key type. */
void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b);
void emit_boxed_text(Compiler *c, TyKind t, const char *expr, Buf *b);
void emit_unbox_text(Compiler *c, TyKind t, const char *expr, Buf *b);
void emit_proc_literal(Compiler *c, int create, Buf *b);
int proc_slot_is_direct(TyKind t);
int proc_slot_is_ptr(TyKind t);
void emit_case_expr(Compiler *c, int id, Buf *b);

/* Strip ParenthesesNode wrappers to reach the inner expression. */

/* ---- calls ---- */


/* Mangle a Ruby method name into a C identifier: `?`->_p, `!`->_bang,
   `=`->_set, anything else non-identifier -> `_`. Returns a static buffer
   (one live result at a time -- fine since each use is consumed inline). */

/* A class method scope is shadowed (and must not be emitted) when a later
   scope redefines the same (class, name, is_cmethod) -- a reopened class
   where the last definition wins, matching comp_method_in_class. */

/* Value node for keyword `name` inside a KeywordHashNode, or -1. */

/* Value-equality family: operands in the same nonzero family compare by value;
   different nonzero families are never == (Ruby does no cross-type coercion,
   except int/float which share family 1). 0 = not a simple comparable type. */

/* Compile-time `is_a?` for a concrete builtin receiver type: 1 yes, 0 no,
   -1 not determinable here. `exact` is instance_of? (no ancestor match). */


/* ---- cross-part function declarations (generated by the split) ---- */
/* buf_putn / buf_puts / buf_printf are declared earlier (next to Buf),
   so the inline emit_indent can use buf_puts. */
int re_engine_flags(int pf);
int re_has_captures(const char *src);
int re_lit_index(Compiler *c, int nid);
const char *re_lit_src(Compiler *c, int nid);
void emit_interp(Compiler *c, int id, Buf *b);
int emit_regex_pat_to_buf(Compiler *c, int nid, Buf *b);
int nameset_has(NameSet *s, const char *nm);
void nameset_add(NameSet *s, const char *nm);
void emit_local_ref(Compiler *c, int scope_node, const char *name, Buf *b);
void emit_yblk_ref(Buf *b);
void emit_tail_lead(Buf *b);
const char *rename_local(const char *nm);
/* `unsupported` never returns: normal mode exits; SP_COLLECT_ERRORS mode
   longjmps to the codegen driver's per-unit recovery (see g_unsup_recover)
   when one is armed, else exits. Marked noreturn so every caller's
   "this construct is unsupported" guard correctly treats the code after it as
   unreachable. */
int collect_mode(void);            /* 1 in SP_COLLECT_ERRORS mode (cached) */
extern jmp_buf g_unsup_recover;    /* per-unit recovery point, armed by the driver */
extern int g_unsup_armed;          /* nonzero while a recovery point is live */
__attribute__((noreturn)) void unsupported(Compiler *c, int id, const char *what);
int builtin_class_id(const char *name);
int is_builtin_class_name(const char *n);
const char *c_type_name(TyKind t);
int is_scalar_ret(TyKind t);
const char *ffi_c_type(const char *spec);
const char *default_value(TyKind t);
void emit_ctype(Compiler *c, TyKind t, Buf *b);
void emit_box_open(Compiler *c, TyKind t, Buf *b);
void emit_box_close(Compiler *c, TyKind t, Buf *b);
const char *array_kind(TyKind t);
void emit_c_escaped_n(Buf *b, const char *s, size_t len);
void emit_c_escaped(Buf *b, const char *s);
void emit_str_literal_n(Buf *b, const char *content, size_t len);
void emit_str_literal(Buf *b, const char *content);
void emit_catch_tag(Compiler *c, int id, Buf *b);
void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b);
int unwrap_parens(Compiler *c, int id);
const char *int_arith_fn(const char *op);
const char *bigint_arith_fn(const char *op);
const char *mc(const char *name);
int scope_is_shadowed(Compiler *c, int s);
int struct_kwarg_value(Compiler *c, int kwh, const char *name);
int eq_family(TyKind t);
int ty_matches_class(TyKind t, const char *cn, int exact);
void emit_method_call(Compiler *c, int id, Buf *b);
int emit_hash_collect_expr(Compiler *c, int id, Buf *b);
int patch_lv_reads(Compiler *c, int id, const char *nm, TyKind ty, int *ids_out, TyKind *ty_out, int cap);
int patch_lv_read_ntype(Compiler *c, int scope_idx, const char *name, TyKind new_ty, int min_id, int **saved_ids, TyKind **saved_tys);
void restore_lv_read_ntype(Compiler *c, int *saved_ids, TyKind *saved_tys, int n);
int emit_transform_hash_expr(Compiler *c, int id, Buf *b);
int emit_bsearch_expr(Compiler *c, int id, Buf *b);
int emit_minmax_by_expr(Compiler *c, int id, Buf *b);
int emit_poly_uniq_block(Compiler *c, int id, Buf *b);
int emit_flat_map_expr(Compiler *c, int id, Buf *b);
int emit_filter_map_expr(Compiler *c, int id, Buf *b);
int emit_gsub_block_expr(Compiler *c, int id, Buf *b);
int emit_sum_block_expr(Compiler *c, int id, Buf *b);
int emit_slice_when_chunk_inspect_expr(Compiler *c, int id, Buf *b);
int emit_product_inspect_expr(Compiler *c, int id, Buf *b);
int emit_step_array_expr(Compiler *c, int id, Buf *b);
int emit_inject_expr(Compiler *c, int id, Buf *b);
int emit_reduce_block_expr(Compiler *c, int id, Buf *b);
int emit_sortby_expr(Compiler *c, int id, Buf *b);
int emit_sort_cmp_expr(Compiler *c, int id, Buf *b);
void emit_block_param_assign(Compiler *c, int scope_id, const char *nm, int tidx, TyKind et, Buf *b);
int emit_minmax_cmp_expr(Compiler *c, int id, Buf *b);
int emit_partition_expr(Compiler *c, int id, Buf *b);
int emit_collect_expr(Compiler *c, int id, Buf *b);
int emit_with_index_expr(Compiler *c, int id, Buf *b);
int emit_each_with_index_chain(Compiler *c, int id, Buf *b);
int emit_each_with_index_terminal(Compiler *c, int id, Buf *b);
int emit_chunk_while_expr(Compiler *c, int id, Buf *b);
int emit_predicate_expr(Compiler *c, int id, Buf *b);
int emit_grep_pred(Compiler *c, int pat, const char *ev, TyKind et, Buf *b);
void emit_obj_alloc_expr(Compiler *c, int cid, Buf *b);
int emit_grep_expr(Compiler *c, int id, Buf *b);
void emit_arg_or_default(Compiler *c, Scope *m, int idx, int provided, Buf *out);
int kwh_lookup(const NodeTable *nt, int kwh, const char *kname);
void emit_rest_pack(Compiler *c, int from, int pos_argc, const int *argv, Buf *b);
void emit_array_elem_at(TyKind at, int tmp, int elem_idx, Buf *b);
void emit_rest_from_splat_and_argv(int tmp, TyKind at, int from_idx, Compiler *c, int argv_from, int pos_argc, const int *argv, Buf *b);
void emit_args_filled(Compiler *c, int callee_idx, int argsNode, const char *lead, Buf *out);
int is_descendant(Compiler *c, int k, int anc);
int dispatch_impl_count(Compiler *c, int cid, const char *name);
void emit_dispatch(Compiler *c, int cid, const char *name, const char *selfptr, int argsNode, int blk_node, Buf *b);
int emit_group_by_expr(Compiler *c, int id, Buf *b);
int emit_each_with_object_expr(Compiler *c, int id, Buf *b);
int emit_tap_then_expr(Compiler *c, int id, Buf *b);
int recv_is_const(const NodeTable *nt, int recv, const char *name);
int sp_is_fiber_storage_recv(const NodeTable *nt, int recv);
int emit_ctor_yield_inline(Compiler *c, int id, int ci, Buf *b);
void emit_call(Compiler *c, int id, Buf *b);
int emit_array_mutate_stmt(Compiler *c, int id, Buf *b, int indent);
void emit_index_op_write(Compiler *c, int id, Buf *b, int indent);
void emit_index_and_or_write(Compiler *c, int id, Buf *b, int indent, int is_or);
int scope_has_return(Compiler *c, int scope_idx);
int emit_inline_call_x(Compiler *c, int id, Buf *b, int indent, int as_expr);
int emit_inline_call(Compiler *c, int id, Buf *b, int indent);
int is_block_call(Compiler *c, int id);
int is_blockless_block_param_call(Compiler *c, int id);
void emit_block_invoke(Compiler *c, int args_node, Buf *b, int indent, int as_expr);
int emit_inline_expr(Compiler *c, int id, Buf *b);
void emit_iter_param_assign(Compiler *c, int block, const char *p0_orig, const char *p0_ren, TyKind src_type, const char *src_expr, Buf *b, int indent);
int subtree_has_own_redo(const NodeTable *nt, int id);
void emit_loop_body(Compiler *c, int body, Buf *b, int indent);
int emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent);
void emit_interp(Compiler *c, int id, Buf *b);
void emit_expr(Compiler *c, int id, Buf *b);
void emit_puts_one(Compiler *c, int arg, Buf *b, int indent);
void emit_print_one(Compiler *c, int arg, Buf *b, int indent);
void emit_p_one(Compiler *c, int arg, Buf *b, int indent);
int emit_output_call(Compiler *c, int id, Buf *b, int indent);
void emit_assign(Compiler *c, int id, Buf *b, int indent);
void emit_op_assign(Compiler *c, int id, Buf *b, int indent);
void emit_cond(Compiler *c, int id, Buf *b);
int static_isa_cond(Compiler *c, int pred);
int static_nil_ivar_cond(Compiler *c, int pred);
void emit_if(Compiler *c, int id, Buf *b, int indent, int is_unless, int tail);
int emit_poly_class_when(Compiler *c, int cond_id, const char *tmp, Buf *b);
void emit_pm_eq(Compiler *c, int t, TyKind pt, int valnode, Buf *b);
int emit_pm_cond(Compiler *c, int pat, int t, TyKind pt, Buf *b);
void emit_case_match(Compiler *c, int id, Buf *b, int indent, int tail);
void emit_case(Compiler *c, int id, Buf *b, int indent);
void emit_case_branch_value(Compiler *c, int stmts, TyKind rt, int cr, Buf *b);
void emit_case_expr(Compiler *c, int id, Buf *b);
void emit_while(Compiler *c, int id, Buf *b, int indent, int is_until);
void emit_for(Compiler *c, int id, Buf *b, int indent);
void emit_return(Compiler *c, int id, Buf *b, int indent);
int rescue_is_catchall_name(const char *n);
int subtree_has_retry(const NodeTable *nt, int id);
void emit_rescue(Compiler *c, int id, Buf *b, int indent, int fr, const char *resultvar);
void emit_begin(Compiler *c, int id, Buf *b, int indent, const char *resultvar);
void emit_with_prelude(Compiler *c, int id, Buf *b, int indent, void (*inner)(Compiler *, int, Buf *, int));
void emit_stmt(Compiler *c, int id, Buf *b, int indent);
void emit_stmt_tail(Compiler *c, int id, Buf *b, int indent);
void emit_stmt_inner(Compiler *c, int id, Buf *b, int indent);
void emit_stmt_tail_inner(Compiler *c, int id, Buf *b, int indent);
void emit_stmts(Compiler *c, int id, Buf *b, int indent);
void emit_stmts_tail(Compiler *c, int id, Buf *b, int indent);
int needs_root(TyKind t);
const char *hash_box_cls(TyKind t);
void emit_boxed_text(Compiler *c, TyKind t, const char *expr, Buf *b);
void emit_unbox_text(Compiler *c, TyKind t, const char *expr, Buf *b);
void emit_boxed(Compiler *c, int node, Buf *b);
void emit_int_expr(Compiler *c, int node, Buf *b);
void emit_int_divisor(Compiler *c, int node, Buf *b);
void emit_float_expr(Compiler *c, int node, Buf *b);
void declare_local(Compiler *c, Buf *b, LocalVar *lv, int vol);
int scope_has_begin(Compiler *c, int si);
void emit_scope_decls(Compiler *c, Scope *s, Buf *b);
int method_is_void(Scope *s);
void emit_method_cname(Compiler *c, Scope *s, Buf *b);
void emit_method_signature(Compiler *c, Scope *s, Buf *b);
void emit_method(Compiler *c, Scope *s, Buf *b);
int is_nested_block(const char *ty);
void proc_collect_locals(Compiler *c, int id, NameSet *locals);
void proc_collect_used(Compiler *c, int id, NameSet *out);
int proc_params_node(Compiler *c, int create);
const char *proc_param_name(Compiler *c, int create, int idx);
int proc_body_node(Compiler *c, int create);
int proc_slot_is_direct(TyKind t);
int proc_slot_is_ptr(TyKind t);
int proc_body_has_yield(Compiler *c, int id);
int fiber_cap_needs_root(TyKind t);
int fiber_body_uses_self(Compiler *c, int id);
void emit_fiber_new(Compiler *c, int id, Buf *b);
void emit_proc_literal(Compiler *c, int create, Buf *b);
int is_builtin_reopen(const char *name);
int is_exc_name(const char *n);
int class_is_exc_subclass(Compiler *c, int ci);
const char *class_ruby_name(Compiler *c, int ci);
const char *exc_builtin_parent(Compiler *c, int ci);
void emit_class_struct(Compiler *c, ClassInfo *ci, Buf *b);
int class_needs_scan(ClassInfo *ci);
void emit_class_scan(Compiler *c, ClassInfo *ci, Buf *b);
void emit_class_new(Compiler *c, ClassInfo *ci, Buf *b);
int emit_super_inline(Compiler *c, int id, Buf *b, int indent, int as_expr);
void emit_super(Compiler *c, int id, Buf *b);
void emit_regex_section(Buf *b);
#endif
