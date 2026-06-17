/* In-memory AST node table for the C Spinel compiler.
 *
 * Mirrors the text node-table schema emitted by spinel_parse.c
 * (N/S/I/F/R/A lines): every node has a type string and a set of named
 * fields. Fields come in four flavors that match the text tags:
 *   S  string field   (e.g. CallNode "name" = "puts")
 *   I  int field      (e.g. IntegerNode "value" = 1)
 *   R  ref field      (a child node id, -1 if absent)
 *   A  array field    (a list of child node ids)
 * plus an F "content" string (floats, raw literals).
 *
 * The single-binary compiler loads this directly from the parser's text
 * output (sp_parse_file_to_text) -- no on-disk intermediate.
 */
#ifndef SPINEL_NODE_TABLE_H
#define SPINEL_NODE_TABLE_H

#include <stddef.h>

typedef struct { char *key; char *val; size_t val_len; } SpStrField;
typedef struct { char *key; long long val; }    SpIntField;
typedef struct { char *key; int ref; }          SpRefField;
typedef struct { char *key; int *ids; int n; }  SpArrField;

typedef struct {
  char *type;          /* node type string ("CallNode"), NULL if unset */
  char *content;       /* F content, NULL if none */
  SpStrField *s; int ns, cs;
  SpIntField *i; int ni, ci;
  SpRefField *r; int nr, cr;
  SpArrField *a; int na, ca;
  int kind;            /* cached NodeKind+1; 0 = not yet computed (see nt_kind) */
} SpNode;

typedef struct {
  SpNode *nodes;
  int count;           /* number of allocated node slots */
  int node_cap;        /* capacity of `nodes`; >= count (for append growth) */
  int root_id;
  char *source_file;   /* SOURCE_FILE path (unescaped), NULL if none */
  char **files;        /* FILE id -> path (unescaped); for multi-file #line maps */
  int nfiles;          /* length of `files` */
} NodeTable;


/* Node-type kinds. The parser emits the type as a string ("CallNode"); to
   avoid an strcmp ladder on every per-node dispatch in the (iterated) analysis
   passes, each node caches an integer kind, computed once by nt_kind. */
#define SP_NODE_KINDS(X) \
  X(AliasGlobalVariableNode) \
  X(AliasMethodNode) \
  X(AlternationPatternNode) \
  X(AndNode) \
  X(ArrayNode) \
  X(ArrayPatternNode) \
  X(AssocNode) \
  X(AssocSplatNode) \
  X(BackReferenceReadNode) \
  X(BeginNode) \
  X(BlockArgumentNode) \
  X(BlockNode) \
  X(BlockParameterNode) \
  X(BlockParametersNode) \
  X(BreakNode) \
  X(CallAndWriteNode) \
  X(CallNode) \
  X(CallOrWriteNode) \
  X(CallTargetNode) \
  X(CapturePatternNode) \
  X(CaseMatchNode) \
  X(CaseNode) \
  X(ClassNode) \
  X(ClassVariableAndWriteNode) \
  X(ClassVariableOperatorWriteNode) \
  X(ClassVariableOrWriteNode) \
  X(ClassVariableReadNode) \
  X(ClassVariableTargetNode) \
  X(ClassVariableWriteNode) \
  X(ConstantAndWriteNode) \
  X(ConstantOperatorWriteNode) \
  X(ConstantOrWriteNode) \
  X(ConstantPathAndWriteNode) \
  X(ConstantPathNode) \
  X(ConstantPathOperatorWriteNode) \
  X(ConstantPathOrWriteNode) \
  X(ConstantPathTargetNode) \
  X(ConstantPathWriteNode) \
  X(ConstantReadNode) \
  X(ConstantTargetNode) \
  X(ConstantWriteNode) \
  X(DefNode) \
  X(DefinedNode) \
  X(ElseNode) \
  X(EmbeddedStatementsNode) \
  X(FalseNode) \
  X(FloatNode) \
  X(ForNode) \
  X(ForwardingSuperNode) \
  X(GlobalVariableAndWriteNode) \
  X(GlobalVariableOperatorWriteNode) \
  X(GlobalVariableOrWriteNode) \
  X(GlobalVariableReadNode) \
  X(GlobalVariableTargetNode) \
  X(GlobalVariableWriteNode) \
  X(HashNode) \
  X(HashPatternNode) \
  X(IfNode) \
  X(ImaginaryNode) \
  X(InNode) \
  X(IndexAndWriteNode) \
  X(IndexOperatorWriteNode) \
  X(IndexOrWriteNode) \
  X(IndexTargetNode) \
  X(InstanceVariableAndWriteNode) \
  X(InstanceVariableOperatorWriteNode) \
  X(InstanceVariableOrWriteNode) \
  X(InstanceVariableReadNode) \
  X(InstanceVariableTargetNode) \
  X(InstanceVariableWriteNode) \
  X(IntegerNode) \
  X(InterpolatedRegularExpressionNode) \
  X(InterpolatedStringNode) \
  X(InterpolatedSymbolNode) \
  X(InterpolatedXStringNode) \
  X(KeywordHashNode) \
  X(KeywordRestParameterNode) \
  X(LambdaNode) \
  X(LocalVariableAndWriteNode) \
  X(LocalVariableOperatorWriteNode) \
  X(LocalVariableOrWriteNode) \
  X(LocalVariableReadNode) \
  X(LocalVariableTargetNode) \
  X(LocalVariableWriteNode) \
  X(MatchRequiredNode) \
  X(ModuleNode) \
  X(MultiTargetNode) \
  X(MultiWriteNode) \
  X(NextNode) \
  X(NilNode) \
  X(NumberedParametersNode) \
  X(NumberedReferenceReadNode) \
  X(OperatorWriteNode) \
  X(OptionalKeywordParameterNode) \
  X(OrNode) \
  X(ParametersNode) \
  X(ParenthesesNode) \
  X(PinnedExpressionNode) \
  X(PinnedVariableNode) \
  X(PostExecutionNode) \
  X(PreExecutionNode) \
  X(RangeNode) \
  X(RationalNode) \
  X(RedoNode) \
  X(RegularExpressionNode) \
  X(RequiredParameterNode) \
  X(RescueModifierNode) \
  X(RescueNode) \
  X(RestParameterNode) \
  X(RetryNode) \
  X(ReturnNode) \
  X(SelfNode) \
  X(SingletonClassNode) \
  X(SourceEncodingNode) \
  X(SourceFileNode) \
  X(SourceLineNode) \
  X(SplatNode) \
  X(StatementsNode) \
  X(StringNode) \
  X(SuperNode) \
  X(SymbolNode) \
  X(TrueNode) \
  X(UndefNode) \
  X(UnlessNode) \
  X(UntilNode) \
  X(WhileNode) \
  X(XStringNode) \
  X(YieldNode)

typedef enum {
  NK_NONE = 0,
#define X(n) NK_##n,
  SP_NODE_KINDS(X)
#undef X
  NK__COUNT
} NodeKind;

/* Integer node-type, computed once per node and cached. NK_NONE if the node
   has no type or an unrecognized one. */
NodeKind nt_kind(const NodeTable *nt, int id);

/* Build a node table from the parser's text AST (NUL-terminated). The
   buffer is consumed read-only; the table owns its own copies. Returns
   NULL on allocation failure. */
NodeTable *nt_load_text(const char *text);

void nt_free(NodeTable *nt);

/* Deep-clone the subtree rooted at `root`; returns the new root id (or -1).
   Appends nodes and grows nt->count -- parallel per-node arrays must be
   resized to match afterward. */
int nt_clone_subtree(NodeTable *nt, int root);

/* ---- synthetic node construction ----
   Append a freshly-typed node and populate its fields, for desugaring AST
   in-place (e.g. a forwarded `&callable` into an equivalent block). Each
   nt_new_node grows nt->count; like nt_clone_subtree, callers must resize the
   parallel per-node arrays (comp_grow_node_arrays) afterward. The set_* helpers
   overwrite a field of the same key if present, else append it. */
int  nt_new_node(NodeTable *nt, const char *type);                 /* new id, -1 on OOM */
void nt_node_set_str(NodeTable *nt, int id, const char *key, const char *val);
void nt_node_set_int(NodeTable *nt, int id, const char *key, long long val);
void nt_node_set_ref(NodeTable *nt, int id, const char *key, int child);
void nt_node_set_arr(NodeTable *nt, int id, const char *key, const int *ids, int n);

/* Accessors. id must be in [0, nt->count). Out-of-range ids return the
   given defaults so callers can walk freely without bounds checks. */
/* Resolve a `node_file` id to its source path, or NULL if out of range.
   Used by codegen to emit `#line N "path"` directives in multi-file builds. */
const char *nt_file_path(const NodeTable *nt, int fid);

const char *nt_type(const NodeTable *nt, int id);          /* NULL if unset */
const char *nt_str(const NodeTable *nt, int id, const char *key);   /* NULL */
/* Overwrite an existing string field's value (no-op if the key is absent).
   Returns 1 if the field was found and updated. */
int         nt_set_str(NodeTable *nt, int id, const char *key, const char *val);
size_t      nt_str_len(const NodeTable *nt, int id, const char *key); /* 0 if absent */
long long   nt_int(const NodeTable *nt, int id, const char *key, long long dflt);
int         nt_ref(const NodeTable *nt, int id, const char *key);   /* -1 */
const int  *nt_arr(const NodeTable *nt, int id, const char *key, int *out_n); /* NULL,0 */
const char *nt_content(const NodeTable *nt, int id);       /* NULL */

/* Generic child iteration (for structural walks that don't know field
   names). Ref fields and array-field elements are the node's children. */
int        nt_num_refs(const NodeTable *nt, int id);
int        nt_ref_at(const NodeTable *nt, int id, int i);   /* ref value, may be -1 */
int        nt_num_arrs(const NodeTable *nt, int id);
const int *nt_arr_at(const NodeTable *nt, int id, int i, int *out_n);

#endif
