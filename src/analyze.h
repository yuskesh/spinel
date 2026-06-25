/* Whole-program type inference (the analyzer pass).
 *
 * Populates the per-node type cache and the local-variable type table in
 * the Compiler. Mirrors the legacy analyzer's role but shares state with
 * codegen in memory instead of via an IR file. infer_type is also the
 * type query codegen uses (through the cache it fills here).
 */
#ifndef SPINEL_ANALYZE_H
#define SPINEL_ANALYZE_H

#include "compiler.h"

/* Set by main.c from --int-overflow=promote. In promote mode the analyzer is
   free to widen accumulating int locals to bigint more aggressively (e.g. block
   iteration loops, not just `while`), since the overflow-raising int macros are
   exactly what promote mode is asking us to avoid. Off (0) for raise/wrap, so
   the default gates and optcarrot (which pins wrap) see no behavior change. */
extern int g_promote_mode;

/* Run inference over the whole program: register locals, reach a fixpoint
   on their types, and fill the node type cache. */
void analyze_program(Compiler *c);

/* Infer (and cache) the type of node `id`. Used during analysis; codegen
   reads the cached results via comp_ntype. */
TyKind infer_type(Compiler *c, int id);

/* Unified type of every value-carrying `break`/`next` in a block body (not
   descending into nested blocks/loops), or TY_UNKNOWN if none. Lets a
   collecting emitter widen its element type past the tail expression so a
   `next <other-type>` is boxed rather than assigned to a mismatched temp. */
TyKind ie_block_break_next_ty(Compiler *c, int node);

/* Name of a block's idx-th required parameter, or NULL. */
const char *block_param_name(Compiler *c, int block, int idx);
/* Name of a block's trailing rest parameter (`|*a|`), or NULL. */
const char *block_rest_name(Compiler *c, int block);

/* True if `id` is a `proc {}` / `lambda {}` / `Proc.new {}` literal (a CallNode
   whose block becomes its own lowered proc fn), or the `Proc` constant. Declared
   here (not analyze_internal.h) so codegen can distinguish an inlined iteration
   block from a nested proc/lambda literal. */
int is_proc_constant(const NodeTable *nt, int n);
int is_proc_literal(Compiler *c, int id);

/* Element type an `each_with_object([])` accumulator is filled with, inferred
   from how the memo param is pushed to (following a forwarded callable's body).
   TY_UNKNOWN when undetermined; callers default an empty `[]` to int_array. */
TyKind ewo_memo_elem_type(Compiler *c, int callid);

/* For a curry-application node, whether it completes the curry (reaches the base
   proc's arity) and the proc's return type. Returns 1 for a recognized chain. */
int curry_apply_info(Compiler *c, int node, int *out_complete, TyKind *out_ret);

/* Class index when a receiverless instance_eval/exec resolves to self, else -1. */
int ie_implicit_self_class(Compiler *c, int id);

/* instance_exec keyword-arg helpers: the call's trailing KeywordHashNode (or
   -1), and the value node bound to a keyword name within it (or -1). */
int ie_call_kwhash(Compiler *c, int id);
int ie_kwhash_value(Compiler *c, int kwhash, const char *name);

/* instance_exec trampoline body-arg resolution (mixed local/ivar/literal args):
   effective arg count, and the node to bind/emit for the p-th block param
   (caller arg substituted for a trampoline param read). -1 to bail. */
int ie_tramp_effective_argc(Compiler *c, int caller_id);
int ie_tramp_effective_arg(Compiler *c, int caller_id, int p);

/* Returns 1 if the idx-th required param is a MultiTargetNode (tuple destructure). */
int block_param_is_multi(Compiler *c, int block, int idx);

/* Returns the number of leaves in the MultiTargetNode at requireds[idx]. */
int block_param_multi_count(Compiler *c, int block, int idx);

/* Returns the name of the leaf_idx-th leaf inside the MultiTargetNode at requireds[idx]. */
const char *block_param_multi_leaf(Compiler *c, int block, int idx, int leaf_idx);

/* Bound-Method (`method(:sym)`) resolution, shared with codegen. */
const char *method_sym_arg(Compiler *c, int node);   /* :sym arg name, or NULL */
int is_method_obj_call(Compiler *c, int node);        /* is node a method(:sym) call? */
int method_obj_target_mi(Compiler *c, int node);      /* target method scope idx, or -1 */
int method_recv_node(Compiler *c, int recv);          /* the method(:sym) node behind a Method expr */

#endif
