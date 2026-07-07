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

/* Method visibility (see ClassInfo.vis_names). Default/absent is public. */
enum { SP_VIS_PUBLIC = 0, SP_VIS_PRIVATE = 1, SP_VIS_PROTECTED = 2 };

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
  char *name;          /* class name ("Point"); also drives semantic checks
                          (builtin reopen, name matching) and Ruby-visible output */
  char *c_name;        /* C-identifier stem for `sp_<c_name>` emission: == name,
                          unless name collides with a runtime typedef (sp_RbVal,
                          sp_IntArray, ...) in which case it is disambiguated so a
                          user `class RbVal` does not redefine the runtime type */
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
  /* Method visibility: parallel name/kind arrays (SP_VIS_*). Covers both
     def-defined methods and attr readers/writers (writers stored as "x=").
     A name absent here is public; an explicit entry records private/protected
     (or an explicit re-`public`). Populated by register_method_visibility. */
  char **vis_names;
  int  *vis_kinds;
  int nvis, cvis;
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
  int is_data;         /* defined via Data.define(...): a Struct-like value class
                          that additionally supports the `#with` copy-update. */
  /* Native-bound class (Path B typed object): C-backed, declared by a package
     via native_struct/native_new/native_method. It is a first-class object
     (ty_object(i), a runtime cls_id, GC-managed) whose methods dispatch to
     declared C symbols instead of generated bodies. c_struct is the carried C
     struct name; free_sym its optional finalizer. Method bindings live in the
     compiler's native_methods registry, keyed by this class's index. */
  int is_native_class;
  char *c_struct;      /* e.g. "sp_StringIO", or NULL */
  char *native_free;   /* finalizer C symbol, or NULL */
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
/* ffi_callback :name, [arg_specs], ret_spec -- a C function-pointer type. A
   method(:sym) / non-capturing lambda passed to an arg of this type becomes a
   compile-time trampoline that boxes the C args, calls the compiled method, and
   converts the result back. */
typedef struct { char *mod; char *name; char **arg_specs; int nargs; char *ret_spec; } FfiCallback;
typedef struct { char *name; char *spec; } FfiField;                 /* one ffi_struct member */
typedef struct { char *mod; char *name; FfiField *fields; int nfields; } FfiStruct; /* ffi_struct */
/* ffi_struct method dispatch (see ffi_struct_method, declared below Compiler). */
enum { FFI_SM_NONE = 0, FFI_SM_NEW, FFI_SM_GET, FFI_SM_SET };
typedef struct { char *mod; char *names; } FfiLib;   /* names: ;-separated lib names, or "" */
typedef struct { char *mod; char *val; } FfiCflag;   /* val: ;-separated cflags, or "" */

/* One `native_func` declaration (typed static binding to carried C). Unlike
   FfiFunc, arg/ret specs are the spinel type language ("any"/"string"/"int"/
   "float"/"bool") and csym is the C symbol to call. feat is the require-gate
   feature name from the module's `native_lib`, or "" (always available). */
typedef struct {
  char *mod;       /* module name */
  char *name;      /* Ruby method name */
  char *ret;       /* return type spec */
  char *csym;      /* C symbol to emit */
  char *feat;      /* require-gate feature name, or "" */
  char **args;     /* arg type specs (malloc'd) */
  int nargs;
} NativeFunc;

/* One `native_obj` declaration: a carried C object the package links on demand
   (only when its module's feature is required). path is root-relative, e.g.
   "packages/json/sp_json.o"; feat is the require-gate feature or "". */
typedef struct { char *mod; char *path; char *feat; } NativeObj;

/* One `native_method`/`native_new` binding on a native class. class_id indexes
   the class table; kind 0 = instance method, 1 = constructor (class method
   `new`). Arity-keyed: several entries may share a name with different nargs.
   ret uses the spinel type language plus "string?"/"poly?" nullable specs.
   csym is the C symbol; NULL ret means the emitted call yields no value. */
typedef struct {
  int class_id;
  int kind;        /* 0 = instance, 1 = constructor */
  char *name;      /* Ruby method name */
  char *ret;       /* return type spec, or "" */
  char *csym;      /* C symbol to call */
  char **args;     /* arg type specs */
  int nargs;
} NativeMethod;

typedef struct {
  const NodeTable *nt;
  TyKind *ntype;    /* [node_cap] node id -> inferred type */
  TyKind *nilnarrow; /* [node_cap] param-read narrowed by a `return .. if p.nil?`
                        guard: the read's non-nil type (codegen unboxes the poly
                        slot at the read site); TY_UNKNOWN = not narrowed */
  int *nscope;      /* [node_cap] node id -> owning scope index */
  int *node_cbody;  /* [node_cap] node id -> enclosing class/module-body class id, or -1 */
  char *empty_arr_recv; /* [node_cap] empty `[]` used as a safe-iterator receiver -> TY_POLY_ARRAY */
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

  /* FFI registry: ffi_callback declarations (C function-pointer types) */
  FfiCallback *ffi_callbacks;
  int n_ffi_callbacks, c_ffi_callbacks;

  /* FFI registry: ffi_struct declarations (named C structs + field accessors) */
  FfiStruct *ffi_structs;
  int n_ffi_structs, c_ffi_structs;

  /* FFI library names per module (semicolon-separated) */
  FfiLib *ffi_libs;
  int n_ffi_libs, c_ffi_libs;

  /* FFI cflags per module (semicolon-separated) */
  FfiCflag *ffi_cflags;
  int n_ffi_cflags, c_ffi_cflags;

  /* native-binding registry: native_func declarations (Path B) */
  NativeFunc *native_funcs;
  int n_native_funcs, c_native_funcs;

  /* native-binding registry: native_obj (carried C objects to link on demand) */
  NativeObj *native_objs;
  int n_native_objs, c_native_objs;

  /* native-binding registry: native_method/native_new on native classes */
  NativeMethod *native_methods;
  int n_native_methods, c_native_methods;

  /* a native package declared `native_obj_reflect`: it consumes the generic
     "plain object -> hash of members" reflection, so codegen emits and installs
     sp_obj_to_hash when the program defines Structs. No feature is named in the
     compiler -- the package's require is the declaration. */
  int native_obj_reflect;
  /* body-node id -> enclosing BlockNode id (lazy; emit_stmts block-local
     resets). Sized nt->count; -1 = not a block body. */
  int *blk_body_map;
} Compiler;

Compiler *comp_new(const NodeTable *nt);
void comp_free(Compiler *c);

/* Resize per-node arrays (ntype/nscope) after the node table grew. */
void comp_grow_node_arrays(Compiler *c);

/* If `id` is a ternary `cond ? A : B` -- an IfNode whose then- and else-clauses
   are each a single value expression (the shape the ternary emitter lowers to a
   C `?:`) -- set *then_node and *else_node to A and B and return 1; else return
   0. Shared by the nullable-int recognition in analyze and codegen. */
int comp_ternary_arms(const NodeTable *nt, int id, int *then_node, int *else_node);

/* Is `v` a chain of plain local/ivar writes ending in a literal nil
   (`a = b = nil` seen from a's value)? Returns the terminal NilNode id or -1.
   Shared by analyze (write-type collection) and codegen (chain lowering). */
int comp_nil_chain_bottom(const NodeTable *nt, int v);

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

/* Look up an ffi_callback type by (module, name); returns index or -1. */
int ffi_find_callback(Compiler *c, const char *mod, const char *name);

/* Resolve Module.<method> against ffi_struct declarations: <Name>_new,
   <Name>_get_<field>, <Name>_set_<field>. Returns an FFI_SM_* op kind and,
   via out params, the struct and field indices (field -1 for _new). */
int ffi_struct_method(Compiler *c, const char *mod, const char *method, int *si, int *fi);

/* native-binding registry (Path B): find a native_func by (module, name),
   return its index in c->native_funcs or -1; map a spec to a TyKind. */
int comp_native_find(Compiler *c, const char *mod, const char *name);
int comp_native_method_find(Compiler *c, int class_id, const char *name, int argc, int kind);
int comp_native_method_find_typed(Compiler *c, int class_id, const char *name, int argc, int kind,
                                  const TyKind *argtys);
TyKind native_spec_to_ty(const char *spec);

/* Global variables and top-level constants. *_intern finds or creates. */
LocalVar *comp_gvar(Compiler *c, const char *name);
LocalVar *comp_gvar_intern(Compiler *c, const char *name);
const char *comp_resolve_gvar(Compiler *c, const char *name); /* alias resolution */
void comp_add_gvar_alias(Compiler *c, const char *from, const char *to);
LocalVar *comp_const(Compiler *c, const char *name);
LocalVar *comp_const_intern(Compiler *c, const char *name);
/* 1 when `pred` is a statically-false `defined?(Const)` if-guard (optionally
   the left arm of an `&&` chain) over a constant that resolves to nothing;
   the guarded branch is compile-time dead. */
int comp_defined_guard_false(Compiler *c, int pred);

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
/* Record method `name`'s visibility on a class (overwrite-or-append). */
void       comp_method_vis_set(ClassInfo *ci, const char *name, int kind);
/* Visibility of `name` declared directly on this class (SP_VIS_PUBLIC if none). */
int        comp_method_vis(ClassInfo *ci, const char *name);
/* Visibility of `name` as resolved up class_id's ancestor chain: the first
   class with an explicit entry wins (a subclass may re-`public` an inherited
   private method), defaulting to SP_VIS_PUBLIC when none records it. */
int        comp_method_vis_in_chain(Compiler *c, int class_id, const char *name);
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
