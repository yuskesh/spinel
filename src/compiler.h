/* Shared compiler state for the C Spinel compiler.
 *
 * In the single-binary design the analyzer and code generator share one
 * in-memory state object instead of serializing an IR file. This struct
 * is that object; its field set corresponds to the legacy .ir dump. It
 * grows milestone by milestone -- M2 adds method scopes (one Scope per
 * `def`, plus the top-level scope) on top of M1's node type cache.
 */
#ifndef SPINEL_COMPILER_H
#define SPINEL_COMPILER_H

#include "node_table.h"
#include "types.h"

/* require-gate (defined in spinel_parse.c). sp_feature_enabled(name) is 1 when
   feature `name` may be provided: always when the gate is off (g_require_gate
   == 0), else only if `require "name"` appeared in the program. Used to gate
   require-gated stdlib (stringio, io/console, ...) so they match CRuby's
   uninitialized-constant / NoMethodError when the require is absent. */
extern int g_require_gate;
void sp_feature_mark(const char *name);
int sp_feature_enabled(const char *name);
/* Add a `-I <dir>` feature search root (see resolve_plain_requires). */
void sp_add_feature_root(const char *dir);

typedef struct {
  char *name;       /* Ruby local name (without sigil) */
  TyKind type;      /* inferred type */
  int gc_root;      /* scratch during analysis; rooting is type-derived in codegen */
  int is_param;     /* declared as a method parameter (C function param) */
  int is_block_param; /* bound by a block; typed by block-param inference */
  int proc_ret;     /* when type==TY_PROC: the proc's body return type (TyKind),
                       TY_UNKNOWN if not statically known */
  int is_cell;      /* captured by an escaping proc: lives in a heap cell
                       (mrb_int *_cell_<name>) so the closure and the enclosing
                       scope share mutable storage */
  int init_guarded; /* (consts) initialized via `CONST = Class.new(...)`: reads
                       during the init raise NameError (uninitialized constant) */
  int rbs_seeded;   /* param type pinned from an --rbs advisory seed: the
                       fixpoint must not widen it (see apply_rbs_seeds) */
  int const_def_write; /* (consts) has a definite (non-or/and) assignment; an
                          or/and-write-only const is nil-defaulted (poly) so its
                          `||=` truthiness check fires on first use */
} LocalVar;

typedef struct {
  char *name;       /* method name; NULL for the top-level scope */
  int def_node;     /* DefNode id; -1 for top-level */
  int body;         /* StatementsNode id (-1 if empty) */
  int class_id;     /* owning class index, or -1 for free functions */
  int yields;       /* body contains a YieldNode (inlined at call sites) */
  int reachable;    /* method name is referenced somewhere (else dead code) */
  int is_cmethod;   /* `def self.foo`: a class (singleton) method, no instance self */
  int is_transplanted_source; /* method was copied into another class via include/prepend */
  int is_lowered_yield; /* self-recursive yield method lowered to &block (sp_Proc) form */
  char *blk_param;  /* name of the `&block` parameter, or NULL (anon -> "") */

  /* Compile-time `define_method` unrolling: a method synthesized from
     `[lits].each { |v| define_method("m_#{v}") { body } }`. Within this
     method's body a read of `dm_subst_name` is the literal at node
     `dm_subst_node` (the loop value for this unrolled instance). */
  char *dm_subst_name;
  int dm_subst_node;

  /* Synthesized compiler_state method (no AST body): codegen emits a
     hand-built body looping over the owning class's compiler_state entries.
     0=not synthesized, else CS_SYNTH_* (see codegen). */
  int cs_synth;

  char **pnames;    /* parameter names, in order (requireds then optionals) */
  int *pdefault;    /* per-param default-value node id, or -1 if required */
  int nparams;
  int nrequired;    /* count of leading required params */
  int rest_idx;     /* index in pnames[] of *rest param, -1 if none */
  int npost_rest;   /* number of required params AFTER the rest param (Prism "posts") */
  int kwrest_idx;   /* index in pnames[] of **kwrest param, -1 if none */

  TyKind ret;       /* inferred return type */
  int ret_specialized; /* ret was set by specialization (inherited-cls-new copy);
                          don't overwrite it from the shared body in the fixpoint */
  int ret_rbs_seeded;  /* ret pinned from an --rbs advisory seed: the fixpoint
                          must not recompute it from the body */
  int ret_proc_ret; /* when ret==TY_PROC: the returned proc's body return type
                       (TyKind), so a caller's `m.call` knows the result type */
  int blk_ret;      /* for a method with a &block param: the unified value type
                       its block yields across all call sites, so blocks passed
                       to it are emitted returning that (common) type */

  LocalVar *locals; /* params + body locals */
  int nlocals, clocals;
} Scope;

typedef struct {
  char *name;          /* class name ("Point") */
  int def_node;        /* ClassNode id */
  int parent;          /* superclass index, or -1 */
  char **ivars;        /* instance variable names, incl. leading '@' */
  TyKind *ivar_types;
  int nivars, civars;
  char **rbs_pin_ivars; /* ivar names (incl '@') pinned by an --rbs seed: the
                           fixpoint must not widen their type */
  int n_rbs_pin_ivars, c_rbs_pin_ivars;
  char **cvars;        /* class variable names, incl. leading '@@' */
  TyKind *cvar_types;
  int ncvars, ccvars;
  char **readers;      /* attr reader method names (no '@') */
  int nreaders, creaders;
  char **writers;      /* attr writer base names (no '@', no '=') */
  int nwriters, cwriters;
  char **undefs;       /* method names removed via `undef` */
  int nundefs, cundefs;
  /* class << self attr_accessor/reader/writer: singleton-level accessors
     stored in static globals (cst_<Class>_<field>), not in per-instance ivars */
  char **sg_readers;   /* singleton reader names */
  int nsg_readers, csg_readers;
  char **sg_writers;   /* singleton writer base names (no '=') */
  int nsg_writers, csg_writers;
  char **alias_new;    /* `alias new old`: alias_new[i] redirects to alias_old[i] */
  char **alias_old;
  int naliases, caliases;
  int is_struct;       /* defined via Struct.new(:a, :b): readers[] are the
                          positional members; the constructor takes them in
                          order and there is no user `initialize`. */
  int is_value_type;   /* small immutable scalar-ivar class represented by value
                          (sp_X, not sp_X *): no heap alloc / GC. Set by
                          detect_value_types after analysis. */
  int instantiated;    /* a value with this exact cls_id can come into existence
                          somewhere: `.new`/`.allocate`/`raise Cls`/Struct, or a
                          Marshal.load that can mint any class. When clear, no
                          poly value is ever this class, so the poly-dispatch
                          switch can drop its `case` arm (the referenced method
                          then becomes an unreferenced static the C compiler
                          DCEs). Set by compute_instantiated. */
  /* Prepend shadow chain: when `prepend M` is called on this class,
     M's methods overwrite the active slot; the previous slot is
     renamed to a shadow `__prep_N_<m>`.  The chain maps each name
     (user name or shadow name) to the next shadow in the chain,
     so `emit_super` can follow it rather than the parent class. */
  char **prep_from;      /* source name in the chain link */
  char **prep_to;        /* target shadow name */
  int nprep_chain, cprep_chain;
  int prep_shadow_count; /* next shadow index to assign */
  int enclosing_class;   /* index of enclosing module/class, or -1 for top-level */
  /* compiler_state_* declared fields: parallel arrays of field name (no '@')
     and kind ("int"/"str"/"sa"/"ia"). codegen's synthesized init/dump/set
     methods iterate these. */
  char **cs_names;
  char **cs_kinds;
  int ncs, ccs;
} ClassInfo;

/* One `ffi_func` declaration. ret: "ptr"/"int"/"float"/"double"/"str"/"void"/
   "size_t"/"long"/"bool". args: malloc'd array of arg specs. */
typedef struct {
  char *mod;       /* module name */
  char *name;      /* function name */
  char *ret;       /* return spec */
  char **args;     /* arg specs array (malloc'd) */
  int nargs;
} FfiFunc;

typedef struct { char *mod; char *name; int val; } FfiConst;     /* ffi_const */
typedef struct { char *mod; char *name; int size; } FfiBuf;      /* ffi_buffer */
typedef struct { char *mod; char *name; int offset; char *kind; } FfiReader; /* ffi_read_* ("u32"/"i32"/"ptr") */
typedef struct { char *mod; char *names; } FfiLib;   /* names: ;-separated lib names, or "" */
typedef struct { char *mod; char *val; } FfiCflag;   /* val: ;-separated cflags, or "" */

typedef struct {
  const NodeTable *nt;
  TyKind *ntype;    /* [node_cap] node id -> inferred type */
  int *nscope;      /* [node_cap] node id -> owning scope index */
  int *node_cbody;  /* [node_cap] node id -> enclosing class/module-body class id, or -1 */
  int node_cap;     /* allocated length of ntype/nscope (>= nt->count) */

  Scope *scopes;    /* scope[0] = top level */
  int nscopes, cscopes;

  char **symbols;   /* interned symbol names; index = sp_sym id */
  int nsymbols, csymbols;

  ClassInfo *classes;
  int nclasses, cclasses;

  LocalVar *gvars;    /* global variables ($g), name without '$' */
  int ngvars, cgvars;
  LocalVar *consts;   /* top-level constants (FOO) */
  int nconsts, cconsts;

  /* alias $copy $orig → gvar_alias_from[i]="copy", gvar_alias_to[i]="orig" */
  char **gvar_alias_from;
  char **gvar_alias_to;
  int ngvar_aliases;

  int *toplevel_includes;  /* class indices of modules included at top level */
  int ntoplevel_includes;

  /* FFI registry: ffi_func declarations */
  FfiFunc *ffi_funcs;
  int n_ffi_funcs, c_ffi_funcs;

  /* FFI registry: ffi_const declarations */
  FfiConst *ffi_consts;
  int n_ffi_consts, c_ffi_consts;

  /* FFI registry: ffi_buffer declarations */
  FfiBuf *ffi_bufs;
  int n_ffi_bufs, c_ffi_bufs;

  /* FFI registry: ffi_read_* declarations */
  FfiReader *ffi_readers;
  int n_ffi_readers, c_ffi_readers;

  /* FFI library names per module (semicolon-separated) */
  FfiLib *ffi_libs;
  int n_ffi_libs, c_ffi_libs;

  /* FFI cflags per module (semicolon-separated) */
  FfiCflag *ffi_cflags;
  int n_ffi_cflags, c_ffi_cflags;
} Compiler;

Compiler *comp_new(const NodeTable *nt);
void comp_free(Compiler *c);

/* Resize per-node arrays (ntype/nscope) after the node table grew. */
void comp_grow_node_arrays(Compiler *c);

/* Scopes. */
Scope *comp_scope_new(Compiler *c, const char *name, int def_node);
Scope *comp_scope_of(Compiler *c, int node_id);        /* owning scope */
int    comp_method_index(Compiler *c, const char *name); /* -1 if none */
int    comp_included_method_index(Compiler *c, const char *name);

/* Locals within a scope. */
LocalVar *scope_local(Scope *s, const char *name);
LocalVar *scope_local_intern(Scope *s, const char *name);

/* Symbol intern table. comp_sym_intern returns the symbol's id. */
int comp_sym_intern(Compiler *c, const char *name);

/* Global variables and top-level constants. *_intern finds or creates. */
LocalVar *comp_gvar(Compiler *c, const char *name);
LocalVar *comp_gvar_intern(Compiler *c, const char *name);
const char *comp_resolve_gvar(Compiler *c, const char *name); /* alias resolution */
void comp_add_gvar_alias(Compiler *c, const char *from, const char *to);
LocalVar *comp_const(Compiler *c, const char *name);
LocalVar *comp_const_intern(Compiler *c, const char *name);

/* Classes. */
ClassInfo *comp_class_new(Compiler *c, const char *name, int def_node);
int        comp_class_index(Compiler *c, const char *name);   /* -1 if none */
/* Class index of a `class_eval`/`module_eval { defs }` reopen, else -1.
   enclosing_class resolves bare/`self.` receivers (the class whose body we are
   directly in); ignored for constant receivers. */
int        class_eval_reopen_class(Compiler *c, int id, int enclosing_class);
int        comp_ivar_index(ClassInfo *ci, const char *name);  /* -1 if none */
int        comp_ivar_intern(ClassInfo *ci, const char *name); /* find or add; returns index */
int        comp_cvar_index(ClassInfo *ci, const char *name);  /* class var; -1 if none */
int        comp_cvar_intern(ClassInfo *ci, const char *name); /* find or add; returns index */
/* Find the instance-method scope index for class_id + method name, or -1. */
int        comp_method_in_class(Compiler *c, int class_id, const char *name);
/* Freeze/unfreeze the (class_id,name,is_cmethod)->scope lookup index. Frozen
   only while scope shape is fixed (the inference fixpoint); see compiler.c. */
void       comp_scope_index_set_frozen(int frozen);
/* Find the class (singleton) method scope for class_id + name, or -1 (no chain). */
int        comp_cmethod_in_class(Compiler *c, int class_id, const char *name);
/* Find the class (singleton) method scope, walking the superclass chain. */
int        comp_cmethod_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
/* Like comp_method_in_class but walks the superclass chain. On success,
   *def_class (if non-NULL) is set to the class that defines the method. */
int        comp_method_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
/* Detect an instance_eval/exec trampoline (def m(args,&b); instance_eval/exec(args,&b); end).
   Returns 1 (eval) / 2 (exec) / 0; sets *def_class to the defining class. */
int        comp_trampoline_kind(Compiler *c, int class_id, const char *name, int *def_class);
/* Stage-1 fold for module singleton accessors holding a constant. */
int        comp_sg_const_binding(Compiler *c, int class_id, const char *base);
int        comp_sg_reader_const(Compiler *c, int call_id); /* const class idx for `Class.reader`, or -1 */
int        comp_sg_const_candidates(Compiler *c, int class_id, const char *base, int *out, int max);
int        comp_sg_reader_candidates(Compiler *c, int call_id, int *out, int max); /* Stage-2 distinct consts */
int        comp_is_nested_int_array_literal(Compiler *c, int node); /* `[[ints],...]` literal */
/* Walk the chain for an attr reader/writer; returns 1 and the owning class. */
int        comp_reader_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
int        comp_writer_in_chain(Compiler *c, int class_id, const char *name, int *def_class);
void       comp_add_reader(ClassInfo *ci, const char *name);
void       comp_add_writer(ClassInfo *ci, const char *name);
int        comp_is_reader(ClassInfo *ci, const char *name);
int        comp_is_writer(ClassInfo *ci, const char *name);
void       comp_add_undef(ClassInfo *ci, const char *name);
int        comp_is_undeffed_in_chain(Compiler *c, int class_id, const char *name);
void       comp_add_sg_reader(ClassInfo *ci, const char *name);
void       comp_add_sg_writer(ClassInfo *ci, const char *name);
int        comp_is_sg_reader(ClassInfo *ci, const char *name);
int        comp_is_sg_writer(ClassInfo *ci, const char *name);
void       comp_add_alias(ClassInfo *ci, const char *new_name, const char *old_name);
/* Prepend-chain helpers. */
void        comp_prep_chain_add(ClassInfo *ci, const char *from, const char *to);
const char *comp_prep_chain_target(Compiler *c, int class_id, const char *name);
const char *comp_prep_user_name(const char *name);
/* Resolve `name` through the class's (chain-aware) alias table to the
   underlying method/attr name. Returns `name` unchanged if not aliased. */
const char *comp_resolve_alias(Compiler *c, int class_id, const char *name);

/* Node type cache. */
static inline TyKind comp_ntype(const Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  /* TY_STRBUF is a codegen-only storage refinement (mutable sp_String for a
     `<<`-appended local). All type-directed logic treats it as a string;
     codegen consults the raw scope-local type where the distinction matters. */
  TyKind t = c->ntype[id];
  return t == TY_STRBUF ? TY_STRING : t;
}

/* 1 iff t is a user-object type whose class is represented by value (sp_X,
   not a heap pointer). See detect_value_types / reference_legacy_value_type_logic. */
static inline int comp_ty_value_obj(const Compiler *c, TyKind t) {
  if (!ty_is_object(t)) return 0;
  int cid = ty_object_class(t);
  return cid >= 0 && cid < c->nclasses && c->classes[cid].is_value_type;
}

#endif
