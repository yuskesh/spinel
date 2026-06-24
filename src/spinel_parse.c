/*
 * spinel_parse.c - Prism AST Serializer (C version)
 *
 * Equivalent to spinel_parse.rb but links with libprism directly.
 * Parses Ruby source and outputs line-based text AST for spinel_codegen.
 *
 * Build: cc -O2 -I$(PRISM)/include spinel_parse.c -L$(PRISM)/build -lprism -o spinel_parse
 *
 * Output format:
 *   ROOT <id>
 *   N <id> <type>           - node declaration
 *   S <id> <field> <escaped> - string field
 *   I <id> <field> <integer> - integer field
 *   F <id> <field> <float>   - float field
 *   R <id> <field> <ref_id>  - reference (-1 for nil)
 *   A <id> <field> <ids>     - array of references
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <prism.h>

/* ---- In-memory output buffer ----
   The final text AST is assembled into a growable byte buffer rather than a
   FILE*, so the in-process entry point needs no open_memstream (POSIX.1-2008,
   absent on Solaris/AIX/HP-UX/MinGW and pre-10.13 macOS) nor a temp-file
   round-trip. One code path on every platform. */
typedef struct { char *data; size_t len; size_t cap; } SpStrBuf;

static void sb_ensure(SpStrBuf *sb, size_t extra) {
  if (sb->len + extra + 1 <= sb->cap) return;
  size_t nc = sb->cap ? sb->cap : 4096;
  while (nc < sb->len + extra + 1) {
    if (nc > (((size_t)-1) / 2)) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    nc *= 2;
  }
  char *nd = realloc(sb->data, nc);
  if (!nd) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  sb->data = nd;
  sb->cap = nc;
}

static void sb_puts(SpStrBuf *sb, const char *s) {
  size_t n = strlen(s);
  sb_ensure(sb, n);
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}

static void sb_printf(SpStrBuf *sb, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int needed = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (needed < 0) {
    va_end(ap2);
    fprintf(stderr, "spinel_parse: vsnprintf failed\n");
    exit(1);
  }
  sb_ensure(sb, (size_t)needed);
  vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, ap2);
  va_end(ap2);
  sb->len += (size_t)needed;
}

/* ---- Output buffer ---- */
static char **lines;
static size_t line_count;
static size_t line_cap;
static int node_counter;

static void out_add(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int needed = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (needed < 0) {
    va_end(ap2);
    fprintf(stderr, "spinel_parse: vsnprintf failed\n");
    exit(1);
  }
  char *buf = malloc((size_t)needed + 1);
  if (!buf) {
    va_end(ap2);
    fprintf(stderr, "spinel_parse: out of memory\n");
    exit(1);
  }
  vsnprintf(buf, (size_t)needed + 1, fmt, ap2);
  va_end(ap2);
  if (line_count >= line_cap) {
    if (line_cap > (((size_t)-1) - 256) / 2) {
      fprintf(stderr, "spinel_parse: out of memory\n");
      exit(1);
    }
    size_t new_cap = (line_cap * 2) + 256;
    char **new_lines = realloc(lines, sizeof(char *) * new_cap);
    if (!new_lines) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    lines = new_lines;
    line_cap = new_cap;
  }
  lines[line_count++] = buf;
}

/* ---- Name from constant pool ---- */
static const pm_parser_t *g_parser;
static const char *g_source_file = "";
static char *g_source_file_escaped = NULL;  /* escape_str(g_source_file), set once at init */
/* Debug builds: when SPINEL_DEBUG=1, flatten() emits a per-node
   `node_line` field so codegen can place C `#line` directives. Off by
   default so the AST text format (and golden tests) are unchanged. */
static int g_emit_line = 0;
/* Debug multi-file source map, populated by sp_build_line_map() before
   flatten() runs and read in flatten() to attribute each node to its
   original file/line. Declared here because flatten() precedes the
   require-resolution machinery that defines the builder. */
static int *sp_line_file = NULL;  /* buffer line (1-based) -> file id */
static int *sp_line_orig = NULL;  /* buffer line (1-based) -> original line */
static int sp_line_map_n = 0;

static char *cstr(pm_constant_id_t id) {
  if (id == 0) return strdup("");
  pm_constant_t *c = &g_parser->constant_pool.constants[id - 1];
  char *buf = malloc(c->length + 1);
  memcpy(buf, c->start, c->length);
  buf[c->length] = '\0';
  return buf;
}

/* ---- String escaping ---- */
static char *escape_str(const uint8_t *src, size_t len) {
  /* Worst case: every char becomes %XX = 3x */
  char *out = malloc((len * 3) + 1);
  size_t j = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = src[i];
    if (c == '%')       { out[j++]='%'; out[j++]='2'; out[j++]='5'; }
    else if (c == '\n') { out[j++]='%'; out[j++]='0'; out[j++]='A'; }
    else if (c == '\r') { out[j++]='%'; out[j++]='0'; out[j++]='D'; }
    else if (c == '\t') { out[j++]='%'; out[j++]='0'; out[j++]='9'; }
    else if (c == ' ')  { out[j++]='%'; out[j++]='2'; out[j++]='0'; }
    /* Issue #722: NUL byte inside a string literal would truncate
       the field at the AST text-serialization layer (lines split
       on '\n' and the loader uses strlen on fields). Encode as %00
       so the byte survives the round-trip. */
    else if (c == 0)    { out[j++]='%'; out[j++]='0'; out[j++]='0'; }
    else out[j++] = c;
  }
  out[j] = '\0';
  return out;
}

static char *escape_pm_string(const pm_string_t *s) {
  return escape_str(pm_string_source(s), pm_string_length(s));
}

/* Convert "PM_FOO_BAR_NODE" -> "FooBarNode" into `out`, truncated to
   out_size-1 chars + NUL. Prism's node-type strings are pure ASCII
   upper + underscore so we add 32 to lowercase non-leading letters. */
static size_t prism_kind_to_pascal(const char *raw, char *out, size_t out_size) {
  if (out_size == 0) return 0;
  if (strncmp(raw, "PM_", 3) == 0) raw += 3;
  size_t j = 0;
  int upper = 1;
  for (; *raw && j < out_size - 1; raw++) {
    if (*raw == '_') { upper = 1; continue; }
    out[j++] = upper ? *raw : (char)(*raw + 32);
    upper = 0;
  }
  out[j] = '\0';
  return j;
}

/* ---- Forward ---- */
static int flatten(pm_node_t *node);

/* ---- Emit helpers ---- */
static void emit_str(int id, const char *field, const char *val) {
  out_add("S %d %s %s", id, field, val);
}

static void emit_int(int id, const char *field, long long val) {
  out_add("I %d %s %lld", id, field, val);
}

static void emit_float(int id, const char *field, double val) {
  /* Issue #766: 64-byte buf; "%.17g" produces at most 24 chars, plus
     ".0" trailer = 26. Safe by margin.
     Issue #767: snprintf is locale-sensitive (de_DE produces "3,14"
     for 3.14, which then fails to parse as a C float literal).
     Format into a fresh sprintf via the C locale by using a manual
     dot conversion: produce in current locale then replace ',' -> '.'.
     The lib-side replacement is safe because Ruby float literals never
     contain a comma. */
  char buf[64];
  snprintf(buf, sizeof(buf), "%.17g", val);
  for (char *p = buf; *p; p++) if (*p == ',') *p = '.';
  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
    size_t l = strlen(buf);
    if (l + 3 < sizeof(buf)) strcat(buf, ".0");
  }
  out_add("F %d %s %s", id, field, buf);
}

static void emit_ref(int id, const char *field, pm_node_t *child) {
  int cid = child ? flatten(child) : -1;
  out_add("R %d %s %d", id, field, cid);
}

static void emit_node_array(int id, const char *field, pm_node_list_t *list) {
  if (!list || list->size == 0) {
    out_add("A %d %s ", id, field);
    return;
  }
  int *ids = malloc(sizeof(int) * list->size);
  for (size_t i = 0; i < list->size; i++)
    ids[i] = flatten(list->nodes[i]);
  /* Issue #744: build comma-separated string into a growable buffer.
     Previously a fixed 65536-byte stack buffer overflowed on
     20k+-element arrays -- snprintf returns the bytes it WOULD have
     written, advancing pos past the end and corrupting the stack. */
  size_t cap = 1024;
  char *buf = malloc(cap);
  size_t pos = 0;
  for (size_t i = 0; i < list->size; i++) {
    /* worst case per iter: ", -2147483648\0" -> 14 bytes; reserve 16 to be safe */
    if (pos + 16 >= cap) {
      size_t new_cap = (cap * 2) + 16;
      char *nb = realloc(buf, new_cap);
      if (!nb) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
      buf = nb;
      cap = new_cap;
    }
    if (i > 0) buf[pos++] = ',';
    int n = snprintf(buf + pos, cap - pos, "%d", ids[i]);
    if (n > 0) pos += (size_t)n;
  }
  buf[pos] = '\0';
  out_add("A %d %s %s", id, field, buf);
  free(buf);
  free(ids);
}

/* ---- Integer value extraction ---- */
static long long pm_int_value(pm_integer_t *integer) {
  uint64_t val = 0;
  uint64_t max_positive = (uint64_t)LLONG_MAX;
  uint64_t max_negative = max_positive + 1ULL;
  const size_t limb_bits = 32;
  const size_t value_bits = sizeof(val) * CHAR_BIT;
  int overflow = 0;

  if (integer->values == NULL) {
    val = (uint64_t)integer->value;
  }
else {
    for (size_t i = 0; i < integer->length; i++) {
      if (i >= value_bits / limb_bits) {
        if (integer->values[i] != 0) overflow = 1;
        continue;
      }
      size_t shift = i * limb_bits;
      val |= ((uint64_t)integer->values[i]) << shift;
    }
  }

  if (integer->negative) {
    if (overflow || val >= max_negative) return LLONG_MIN;
    return -(long long)val;
  }
  if (overflow || val > max_positive) return LLONG_MAX;
  return (long long)val;
}

/* Whether an integer literal does not fit in a signed 64-bit `value` (so
   pm_int_value saturated it). Mirrors the limb scan above. */
static int pm_int_overflows(pm_integer_t *integer) {
  if (integer->values == NULL) return 0;
  const size_t limb_bits = 32;
  const size_t value_bits = sizeof(uint64_t) * CHAR_BIT;
  uint64_t val = 0;
  int overflow = 0;
  for (size_t i = 0; i < integer->length; i++) {
    if (i >= value_bits / limb_bits) {
      if (integer->values[i] != 0) overflow = 1;
      continue;
    }
    val |= ((uint64_t)integer->values[i]) << (i * limb_bits);
  }
  uint64_t max_positive = (uint64_t)LLONG_MAX;
  uint64_t max_negative = max_positive + 1ULL;
  if (integer->negative) return overflow || val >= max_negative;
  return overflow || val > max_positive;
}

/* ---- Main flattening ---- */
static int flatten(pm_node_t *node) {
  if (!node) return -1;

  int id = node_counter++;
  pm_node_type_t t = PM_NODE_TYPE(node);

  /* Debug builds only: stamp every node with its source line — and, when a
     multi-file map was built, its original file — so codegen can emit
     `#line N "file"` directives. Written to dedicated `node_line` /
     `node_file` fields (NOT the overloaded `value`/`start_line` slot). The
     raw value is the line in the concatenated buffer; the map translates it
     back to the original file and line. */
  if (g_emit_line) {
    pm_line_column_t lc = pm_newline_list_line_column(&g_parser->newline_list,
                                                      node->location.start,
                                                      g_parser->start_line);
    int32_t bl = lc.line;
    int orig = bl;
    int fid = 0;
    if (sp_line_map_n > 0 && bl >= 1 && bl <= sp_line_map_n) {
      orig = sp_line_orig[bl];
      fid = sp_line_file[bl];
    }
    emit_int(id, "node_line", (long long)orig);
    emit_int(id, "node_file", (long long)fid);
    /* Column is concatenation-stable (require splicing is line-based), so the
       buffer column equals the original-file column. 0-based, as Prism gives. */
    emit_int(id, "node_col", (long long)lc.column);
  }

#define N(type_name) out_add("N %d " type_name, id)
#define S(field, val) do { char *_e = (val); emit_str(id, field, _e); free(_e); } while(0)
#define I(field, val) emit_int(id, field, val)
#define F(field, val) emit_float(id, field, val)
#define R(field, child) emit_ref(id, field, (pm_node_t *)(child))
#define A(field, list) emit_node_array(id, field, list)
#define NAME(field, cid) do { char *_n = cstr(cid); char *_e = escape_str((const uint8_t *)_n, strlen(_n)); emit_str(id, field, _e); free(_e); free(_n); } while(0)

  switch (t) {
  case PM_PROGRAM_NODE: {
    pm_program_node_t *n = (pm_program_node_t *)node;
    N("ProgramNode");
    R("statements", n->statements);
    break;
  }
  case PM_STATEMENTS_NODE: {
    pm_statements_node_t *n = (pm_statements_node_t *)node;
    N("StatementsNode");
    A("body", &n->body);
    break;
  }
  case PM_CLASS_NODE: {
    pm_class_node_t *n = (pm_class_node_t *)node;
    N("ClassNode");
    R("constant_path", n->constant_path);
    R("superclass", n->superclass);
    R("body", n->body);
    break;
  }
  case PM_MODULE_NODE: {
    pm_module_node_t *n = (pm_module_node_t *)node;
    N("ModuleNode");
    R("constant_path", n->constant_path);
    R("body", n->body);
    break;
  }
  case PM_SINGLETON_CLASS_NODE: {
    /* `class << self; ...; end` — the singleton class block. We
       only support `expression == SelfNode` today (i.e. the
       enclosing class/module's singleton). The body is flattened
       up one level so codegen sees `attr_accessor :x` / `def foo`
       inside the parent ClassNode/ModuleNode body, and the
       SingletonClassNode marker survives so dispatch can route
       methods/accessors to the class-method path. */
    pm_singleton_class_node_t *n = (pm_singleton_class_node_t *)node;
    N("SingletonClassNode");
    R("expression", n->expression);
    R("body", n->body);
    break;
  }
  case PM_DEF_NODE: {
    pm_def_node_t *n = (pm_def_node_t *)node;
    N("DefNode");
    NAME("name", n->name);
    R("parameters", n->parameters);
    R("body", n->body);
    R("receiver", n->receiver);
    break;
  }
  case PM_CALL_NODE: {
    pm_call_node_t *n = (pm_call_node_t *)node;
    N("CallNode");
    NAME("name", n->name);
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    R("block", n->block);
    /* Issue #793: emit explicit call_operator for non-safe-nav calls
       too, so codegen reads "." instead of an unset field. */
    if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)) {
      S("call_operator", escape_str((const uint8_t *)"&.", 2));
    }
else {
      S("call_operator", escape_str((const uint8_t *)".", 1));
    }
    break;
  }
  case PM_CONSTANT_WRITE_NODE: {
    pm_constant_write_node_t *n = (pm_constant_write_node_t *)node;
    N("ConstantWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_CONSTANT_PATH_WRITE_NODE: {
    pm_constant_path_write_node_t *n = (pm_constant_path_write_node_t *)node;
    N("ConstantPathWriteNode");
    R("value", n->value);
    R("target", n->target);
    break;
  }
  case PM_CONSTANT_OPERATOR_WRITE_NODE: {
    pm_constant_operator_write_node_t *n = (pm_constant_operator_write_node_t *)node;
    N("ConstantOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_CONSTANT_OR_WRITE_NODE: {
    pm_constant_or_write_node_t *n = (pm_constant_or_write_node_t *)node;
    N("ConstantOrWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_CONSTANT_AND_WRITE_NODE: {
    pm_constant_and_write_node_t *n = (pm_constant_and_write_node_t *)node;
    N("ConstantAndWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_CONSTANT_PATH_OPERATOR_WRITE_NODE: {
    pm_constant_path_operator_write_node_t *n = (pm_constant_path_operator_write_node_t *)node;
    N("ConstantPathOperatorWriteNode");
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    R("target", (pm_node_t *)n->target);
    break;
  }
  case PM_CONSTANT_PATH_OR_WRITE_NODE: {
    pm_constant_path_or_write_node_t *n = (pm_constant_path_or_write_node_t *)node;
    N("ConstantPathOrWriteNode");
    R("value", n->value);
    R("target", (pm_node_t *)n->target);
    break;
  }
  case PM_CONSTANT_PATH_AND_WRITE_NODE: {
    pm_constant_path_and_write_node_t *n = (pm_constant_path_and_write_node_t *)node;
    N("ConstantPathAndWriteNode");
    R("value", n->value);
    R("target", (pm_node_t *)n->target);
    break;
  }
  case PM_CONSTANT_READ_NODE: {
    pm_constant_read_node_t *n = (pm_constant_read_node_t *)node;
    N("ConstantReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_CONSTANT_PATH_NODE: {
    pm_constant_path_node_t *n = (pm_constant_path_node_t *)node;
    N("ConstantPathNode");
    R("parent", n->parent);
    NAME("name", n->name);
    break;
  }
  case PM_LOCAL_VARIABLE_WRITE_NODE: {
    pm_local_variable_write_node_t *n = (pm_local_variable_write_node_t *)node;
    N("LocalVariableWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_READ_NODE: {
    pm_local_variable_read_node_t *n = (pm_local_variable_read_node_t *)node;
    N("LocalVariableReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_LOCAL_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_local_variable_operator_write_node_t *n = (pm_local_variable_operator_write_node_t *)node;
    N("LocalVariableOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_OR_WRITE_NODE: {
    pm_local_variable_or_write_node_t *n = (pm_local_variable_or_write_node_t *)node;
    N("LocalVariableOrWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_AND_WRITE_NODE: {
    pm_local_variable_and_write_node_t *n = (pm_local_variable_and_write_node_t *)node;
    N("LocalVariableAndWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_LOCAL_VARIABLE_TARGET_NODE: {
    pm_local_variable_target_node_t *n = (pm_local_variable_target_node_t *)node;
    N("LocalVariableTargetNode");
    NAME("name", n->name);
    break;
  }
  case PM_INSTANCE_VARIABLE_WRITE_NODE: {
    pm_instance_variable_write_node_t *n = (pm_instance_variable_write_node_t *)node;
    N("InstanceVariableWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_INSTANCE_VARIABLE_READ_NODE: {
    pm_instance_variable_read_node_t *n = (pm_instance_variable_read_node_t *)node;
    N("InstanceVariableReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_INSTANCE_VARIABLE_TARGET_NODE: {
    pm_instance_variable_target_node_t *n = (pm_instance_variable_target_node_t *)node;
    N("InstanceVariableTargetNode");
    NAME("name", n->name);
    break;
  }
  case PM_CALL_TARGET_NODE: {
    pm_call_target_node_t *n = (pm_call_target_node_t *)node;
    N("CallTargetNode");
    NAME("name", n->name);
    R("receiver", n->receiver);
    break;
  }
  case PM_CONSTANT_TARGET_NODE: {
    pm_constant_target_node_t *n = (pm_constant_target_node_t *)node;
    N("ConstantTargetNode");
    NAME("name", n->name);
    break;
  }
  case PM_CLASS_VARIABLE_TARGET_NODE: {
    pm_class_variable_target_node_t *n = (pm_class_variable_target_node_t *)node;
    N("ClassVariableTargetNode");
    NAME("name", n->name);
    break;
  }
  case PM_CONSTANT_PATH_TARGET_NODE: {
    pm_constant_path_target_node_t *n = (pm_constant_path_target_node_t *)node;
    N("ConstantPathTargetNode");
    R("parent", n->parent);
    NAME("name", n->name);
    break;
  }
  case PM_INSTANCE_VARIABLE_AND_WRITE_NODE: {
    pm_instance_variable_and_write_node_t *n = (pm_instance_variable_and_write_node_t *)node;
    N("InstanceVariableAndWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_INSTANCE_VARIABLE_OR_WRITE_NODE: {
    pm_instance_variable_or_write_node_t *n = (pm_instance_variable_or_write_node_t *)node;
    N("InstanceVariableOrWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_INSTANCE_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_instance_variable_operator_write_node_t *n = (pm_instance_variable_operator_write_node_t *)node;
    N("InstanceVariableOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_CLASS_VARIABLE_WRITE_NODE: {
    pm_class_variable_write_node_t *n = (pm_class_variable_write_node_t *)node;
    N("ClassVariableWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_CLASS_VARIABLE_READ_NODE: {
    pm_class_variable_read_node_t *n = (pm_class_variable_read_node_t *)node;
    N("ClassVariableReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_CLASS_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_class_variable_operator_write_node_t *n = (pm_class_variable_operator_write_node_t *)node;
    N("ClassVariableOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_CLASS_VARIABLE_OR_WRITE_NODE: {
    pm_class_variable_or_write_node_t *n = (pm_class_variable_or_write_node_t *)node;
    N("ClassVariableOrWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_CLASS_VARIABLE_AND_WRITE_NODE: {
    pm_class_variable_and_write_node_t *n = (pm_class_variable_and_write_node_t *)node;
    N("ClassVariableAndWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_INDEX_OPERATOR_WRITE_NODE: {
    pm_index_operator_write_node_t *n = (pm_index_operator_write_node_t *)node;
    N("IndexOperatorWriteNode");
    NAME("binary_operator", n->binary_operator);
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    R("value", n->value);
    break;
  }
  case PM_INDEX_AND_WRITE_NODE: {
    pm_index_and_write_node_t *n = (pm_index_and_write_node_t *)node;
    N("IndexAndWriteNode");
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    R("value", n->value);
    break;
  }
  case PM_INDEX_OR_WRITE_NODE: {
    pm_index_or_write_node_t *n = (pm_index_or_write_node_t *)node;
    N("IndexOrWriteNode");
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    R("value", n->value);
    break;
  }
  case PM_INDEX_TARGET_NODE: {
    pm_index_target_node_t *n = (pm_index_target_node_t *)node;
    N("IndexTargetNode");
    R("receiver", n->receiver);
    R("arguments", n->arguments);
    break;
  }
  case PM_CALL_OPERATOR_WRITE_NODE: {
    pm_call_operator_write_node_t *n = (pm_call_operator_write_node_t *)node;
    N("CallOperatorWriteNode");
    R("receiver", n->receiver);
    NAME("name", n->read_name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)) {
      S("call_operator", escape_str((const uint8_t *)"&.", 2));
    }
    break;
  }
  case PM_CALL_AND_WRITE_NODE: {
    pm_call_and_write_node_t *n = (pm_call_and_write_node_t *)node;
    N("CallAndWriteNode");
    R("receiver", n->receiver);
    NAME("name", n->read_name);
    R("value", n->value);
    if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)) {
      S("call_operator", escape_str((const uint8_t *)"&.", 2));
    }
    break;
  }
  case PM_CALL_OR_WRITE_NODE: {
    pm_call_or_write_node_t *n = (pm_call_or_write_node_t *)node;
    N("CallOrWriteNode");
    R("receiver", n->receiver);
    NAME("name", n->read_name);
    R("value", n->value);
    if (PM_NODE_FLAG_P(node, PM_CALL_NODE_FLAGS_SAFE_NAVIGATION)) {
      S("call_operator", escape_str((const uint8_t *)"&.", 2));
    }
    break;
  }
  case PM_GLOBAL_VARIABLE_WRITE_NODE: {
    pm_global_variable_write_node_t *n = (pm_global_variable_write_node_t *)node;
    N("GlobalVariableWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_GLOBAL_VARIABLE_READ_NODE: {
    pm_global_variable_read_node_t *n = (pm_global_variable_read_node_t *)node;
    N("GlobalVariableReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_GLOBAL_VARIABLE_OPERATOR_WRITE_NODE: {
    pm_global_variable_operator_write_node_t *n = (pm_global_variable_operator_write_node_t *)node;
    N("GlobalVariableOperatorWriteNode");
    NAME("name", n->name);
    NAME("binary_operator", n->binary_operator);
    R("value", n->value);
    break;
  }
  case PM_GLOBAL_VARIABLE_OR_WRITE_NODE: {
    pm_global_variable_or_write_node_t *n = (pm_global_variable_or_write_node_t *)node;
    N("GlobalVariableOrWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_GLOBAL_VARIABLE_AND_WRITE_NODE: {
    pm_global_variable_and_write_node_t *n = (pm_global_variable_and_write_node_t *)node;
    N("GlobalVariableAndWriteNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_GLOBAL_VARIABLE_TARGET_NODE: {
    pm_global_variable_target_node_t *n = (pm_global_variable_target_node_t *)node;
    N("GlobalVariableTargetNode");
    NAME("name", n->name);
    break;
  }
  case PM_NO_KEYWORDS_PARAMETER_NODE: {
    /* `def f(**nil)` -- explicit kwarg rejection. Spinel's keyword-arg
       handling is already conservative (only known keys accepted),
       so the explicit "no keywords" marker is effectively a no-op
       at the codegen level. We emit the node so a ParametersNode
       carrying it doesn't leave a NULL keyword slot, but the
       compile-time effect is nothing. */
    N("NoKeywordsParameterNode");
    break;
  }
  case PM_INTEGER_NODE: {
    pm_integer_node_t *n = (pm_integer_node_t *)node;
    N("IntegerNode");
    I("value", pm_int_value(&n->value));
    /* A literal wider than int64 (`100000000000000000000`) saturates `value`;
       keep the exact decimal text so codegen can build an arbitrary-precision
       bigint (`sp_bigint_new_str`). CRuby treats it as a Bignum in every mode. */
    if (pm_int_overflows(&n->value)) {
      pm_buffer_t _bb;
      pm_buffer_init(&_bb);
      pm_integer_string(&_bb, &n->value);
      /* pm_buffer_value is not guaranteed NUL-terminated; bound by length. */
      size_t _bl = pm_buffer_length(&_bb);
      char *_bs = malloc(_bl + 1);
      memcpy(_bs, pm_buffer_value(&_bb), _bl);
      _bs[_bl] = '\0';
      emit_str(id, "bigval", _bs);
      free(_bs);
      pm_buffer_free(&_bb);
    }
    break;
  }
  case PM_FLOAT_NODE: {
    pm_float_node_t *n = (pm_float_node_t *)node;
    N("FloatNode");
    F("value", n->value);
    break;
  }
  case PM_IMAGINARY_NODE: {
    /* `2i` -- imaginary numeric. Issue #840. Wraps an inner
       IntegerNode or FloatNode in `numeric`. Codegen surfaces this
       as a sp_Complex literal {re=0, im=numeric}. */
    pm_imaginary_node_t *n = (pm_imaginary_node_t *)node;
    N("ImaginaryNode");
    R("numeric", n->numeric);
    break;
  }
  case PM_RATIONAL_NODE: {
    /* `1/2r` or `1.5r` -- rational literal. Issue #841. Emits
       numerator + denominator as decimal-text fields (loader pins
       them in @nd_rat_num / @nd_rat_den). Prism stores both as
       reduced pm_integer_t. */
    pm_rational_node_t *n = (pm_rational_node_t *)node;
    N("RationalNode");
    char nbuf[32];
    snprintf(nbuf, sizeof(nbuf), "%lld", (long long)pm_int_value(&n->numerator));
    emit_str(id, "rat_num", nbuf);
    snprintf(nbuf, sizeof(nbuf), "%lld", (long long)pm_int_value(&n->denominator));
    emit_str(id, "rat_den", nbuf);
    break;
  }
  case PM_STRING_NODE: {
    pm_string_node_t *n = (pm_string_node_t *)node;
    N("StringNode");
    S("content", escape_pm_string(&n->unescaped));
    break;
  }
  case PM_INTERPOLATED_STRING_NODE: {
    pm_interpolated_string_node_t *n = (pm_interpolated_string_node_t *)node;
    N("InterpolatedStringNode");
    A("parts", &n->parts);
    break;
  }
  case PM_EMBEDDED_STATEMENTS_NODE: {
    pm_embedded_statements_node_t *n = (pm_embedded_statements_node_t *)node;
    N("EmbeddedStatementsNode");
    R("statements", n->statements);
    break;
  }
  case PM_SYMBOL_NODE: {
    pm_symbol_node_t *n = (pm_symbol_node_t *)node;
    N("SymbolNode");
    S("value", escape_pm_string(&n->unescaped));
    break;
  }
  case PM_TRUE_NODE:
    N("TrueNode");
    break;
  case PM_FALSE_NODE:
    N("FalseNode");
    break;
  case PM_NIL_NODE:
    N("NilNode");
    break;
  case PM_SELF_NODE:
    N("SelfNode");
    break;
  case PM_ARRAY_NODE: {
    pm_array_node_t *n = (pm_array_node_t *)node;
    N("ArrayNode");
    A("elements", &n->elements);
    break;
  }
  case PM_HASH_NODE: {
    pm_hash_node_t *n = (pm_hash_node_t *)node;
    N("HashNode");
    A("elements", &n->elements);
    break;
  }
  case PM_ASSOC_NODE: {
    pm_assoc_node_t *n = (pm_assoc_node_t *)node;
    N("AssocNode");
    R("key", n->key);
    /* Hash shorthand `{ x: }` lowers to an AssocNode whose value is a
       PM_IMPLICIT_NODE. The top-level PM_IMPLICIT_NODE case below
       handles the unwrap by recursing into n->value at the same id
       slot, so the codegen never sees the implicit wrapper here. */
    R("value", n->value);
    break;
  }
  case PM_KEYWORD_HASH_NODE: {
    pm_keyword_hash_node_t *n = (pm_keyword_hash_node_t *)node;
    N("KeywordHashNode");
    A("elements", &n->elements);
    break;
  }
  case PM_RANGE_NODE: {
    pm_range_node_t *n = (pm_range_node_t *)node;
    N("RangeNode");
    R("left", n->left);
    R("right", n->right);
    /* PM_RANGE_FLAGS_EXCLUDE_END = 4. Codegen reads bit 2 to decide
       whether `..` (inclusive) or `...` (exclusive). */
    I("flags", n->base.flags);
    break;
  }
  case PM_IF_NODE: {
    pm_if_node_t *n = (pm_if_node_t *)node;
    N("IfNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    R("subsequent", n->subsequent);
    break;
  }
  case PM_ELSE_NODE: {
    pm_else_node_t *n = (pm_else_node_t *)node;
    N("ElseNode");
    R("statements", n->statements);
    break;
  }
  case PM_UNLESS_NODE: {
    pm_unless_node_t *n = (pm_unless_node_t *)node;
    N("UnlessNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_WHILE_NODE: {
    pm_while_node_t *n = (pm_while_node_t *)node;
    N("WhileNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    /* PM_LOOP_FLAGS_BEGIN_MODIFIER = 4 (bit 2): begin..end while form,
       which is a post-test loop (body runs at least once). The codegen
       reads bit 2 to decide between `while (cond) {}` and `do {} while (cond);`. */
    I("flags", n->base.flags);
    break;
  }
  case PM_UNTIL_NODE: {
    pm_until_node_t *n = (pm_until_node_t *)node;
    N("UntilNode");
    R("predicate", n->predicate);
    R("statements", n->statements);
    I("flags", n->base.flags);
    break;
  }
  case PM_FOR_NODE: {
    pm_for_node_t *n = (pm_for_node_t *)node;
    N("ForNode");
    R("index", n->index);
    R("collection", n->collection);
    R("statements", n->statements);
    break;
  }
  case PM_CASE_NODE: {
    pm_case_node_t *n = (pm_case_node_t *)node;
    N("CaseNode");
    R("predicate", n->predicate);
    A("conditions", &n->conditions);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_CASE_MATCH_NODE: {
    pm_case_match_node_t *n = (pm_case_match_node_t *)node;
    N("CaseMatchNode");
    R("predicate", n->predicate);
    A("conditions", &n->conditions);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_WHEN_NODE: {
    pm_when_node_t *n = (pm_when_node_t *)node;
    N("WhenNode");
    A("conditions", &n->conditions);
    R("statements", n->statements);
    break;
  }
  case PM_IN_NODE: {
    pm_in_node_t *n = (pm_in_node_t *)node;
    N("InNode");
    R("pattern", n->pattern);
    R("statements", n->statements);
    break;
  }
  case PM_BEGIN_NODE: {
    pm_begin_node_t *n = (pm_begin_node_t *)node;
    N("BeginNode");
    R("statements", n->statements);
    R("rescue_clause", n->rescue_clause);
    R("ensure_clause", n->ensure_clause);
    R("else_clause", n->else_clause);
    break;
  }
  case PM_ENSURE_NODE: {
    pm_ensure_node_t *n = (pm_ensure_node_t *)node;
    N("EnsureNode");
    R("statements", n->statements);
    break;
  }
  case PM_RESCUE_NODE: {
    pm_rescue_node_t *n = (pm_rescue_node_t *)node;
    N("RescueNode");
    A("exceptions", &n->exceptions);
    R("reference", n->reference);
    R("statements", n->statements);
    R("subsequent", n->subsequent);
    break;
  }
  case PM_RESCUE_MODIFIER_NODE: {
    pm_rescue_modifier_node_t *n = (pm_rescue_modifier_node_t *)node;
    N("RescueModifierNode");
    R("expression", n->expression);
    R("rescue_expression", n->rescue_expression);
    break;
  }
  case PM_RETURN_NODE: {
    pm_return_node_t *n = (pm_return_node_t *)node;
    N("ReturnNode");
    R("arguments", n->arguments);
    break;
  }
  case PM_BREAK_NODE: {
    pm_break_node_t *n = (pm_break_node_t *)node;
    N("BreakNode");
    R("arguments", n->arguments);
    break;
  }
  case PM_NEXT_NODE: {
    pm_next_node_t *n = (pm_next_node_t *)node;
    N("NextNode");
    R("arguments", n->arguments);
    break;
  }
  case PM_RETRY_NODE:
    N("RetryNode");
    break;
  case PM_YIELD_NODE: {
    pm_yield_node_t *n = (pm_yield_node_t *)node;
    N("YieldNode");
    R("arguments", n->arguments);
    break;
  }
  case PM_BLOCK_NODE: {
    pm_block_node_t *n = (pm_block_node_t *)node;
    N("BlockNode");
    /* Serialize block parameters */
    if (n->parameters) {
      if (PM_NODE_TYPE(n->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)n->parameters;
        int bpid = node_counter++;
        out_add("N %d BlockParametersNode", bpid);
        if (bp->parameters) {
          emit_ref(bpid, "parameters", (pm_node_t *)bp->parameters);
        }
        out_add("R %d %s %d", id, "parameters", bpid);
      }
else if (PM_NODE_TYPE(n->parameters) == PM_NUMBERED_PARAMETERS_NODE) {
        pm_numbered_parameters_node_t *np = (pm_numbered_parameters_node_t *)n->parameters;
        int npid = node_counter++;
        out_add("N %d NumberedParametersNode", npid);
        emit_int(npid, "maximum", np->maximum);
        out_add("R %d %s %d", id, "parameters", npid);
      }
else {
        R("parameters", n->parameters);
      }
    }
    R("body", n->body);
    break;
  }
  case PM_PARAMETERS_NODE: {
    pm_parameters_node_t *n = (pm_parameters_node_t *)node;
    N("ParametersNode");
    A("requireds", &n->requireds);
    A("optionals", &n->optionals);
    A("keywords", &n->keywords);
    if (n->rest) R("rest", n->rest);
    if (n->block) R("block", n->block);
    A("posts", &n->posts);
    /* Surface keyword_rest so the new PM_NO_KEYWORDS_PARAMETER_NODE
       case below is reachable. Existing collect_params_str walker
       only acts on KeywordRestParameterNode (`**kw`), so the
       NoKeywordsParameter (`**nil`) marker passes through as a
       no-op which matches its compile-time semantics. */
    if (n->keyword_rest) R("keyword_rest", n->keyword_rest);
    break;
  }
  case PM_REQUIRED_PARAMETER_NODE: {
    pm_required_parameter_node_t *n = (pm_required_parameter_node_t *)node;
    N("RequiredParameterNode");
    NAME("name", n->name);
    break;
  }
  case PM_OPTIONAL_PARAMETER_NODE: {
    pm_optional_parameter_node_t *n = (pm_optional_parameter_node_t *)node;
    N("OptionalParameterNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_REST_PARAMETER_NODE: {
    pm_rest_parameter_node_t *n = (pm_rest_parameter_node_t *)node;
    N("RestParameterNode");
    if (n->name) { NAME("name", n->name); }
    break;
  }
  case PM_BLOCK_PARAMETER_NODE: {
    pm_block_parameter_node_t *n = (pm_block_parameter_node_t *)node;
    N("BlockParameterNode");
    if (n->name) { NAME("name", n->name); }
    break;
  }
  case PM_BLOCK_ARGUMENT_NODE: {
    /* `&expr` in call argument position. Wraps the expression that
     * provides the proc to forward (typically a LocalVariableReadNode
     * for a `&block`-captured param, or a SymbolNode for `&:to_s`). */
    pm_block_argument_node_t *n = (pm_block_argument_node_t *)node;
    N("BlockArgumentNode");
    R("expression", n->expression);
    break;
  }
  case PM_BLOCK_LOCAL_VARIABLE_NODE: {
    pm_block_local_variable_node_t *n = (pm_block_local_variable_node_t *)node;
    N("BlockLocalVariableNode");
    NAME("name", n->name);
    break;
  }
  case PM_KEYWORD_REST_PARAMETER_NODE: {
    pm_keyword_rest_parameter_node_t *n = (pm_keyword_rest_parameter_node_t *)node;
    N("KeywordRestParameterNode");
    if (n->name) { NAME("name", n->name); }
    break;
  }
  case PM_REQUIRED_KEYWORD_PARAMETER_NODE: {
    pm_required_keyword_parameter_node_t *n = (pm_required_keyword_parameter_node_t *)node;
    N("RequiredKeywordParameterNode");
    NAME("name", n->name);
    break;
  }
  case PM_OPTIONAL_KEYWORD_PARAMETER_NODE: {
    pm_optional_keyword_parameter_node_t *n = (pm_optional_keyword_parameter_node_t *)node;
    N("OptionalKeywordParameterNode");
    NAME("name", n->name);
    R("value", n->value);
    break;
  }
  case PM_PARENTHESES_NODE: {
    pm_parentheses_node_t *n = (pm_parentheses_node_t *)node;
    N("ParenthesesNode");
    R("body", n->body);
    break;
  }
  case PM_AND_NODE: {
    pm_and_node_t *n = (pm_and_node_t *)node;
    N("AndNode");
    R("left", n->left);
    R("right", n->right);
    break;
  }
  case PM_OR_NODE: {
    pm_or_node_t *n = (pm_or_node_t *)node;
    N("OrNode");
    R("left", n->left);
    R("right", n->right);
    break;
  }
  case PM_DEFINED_NODE: {
    pm_defined_node_t *n = (pm_defined_node_t *)node;
    N("DefinedNode");
    R("value", n->value);
    break;
  }
  case PM_SOURCE_LINE_NODE: {
    N("SourceLineNode");
    int32_t line = pm_newline_list_line(&g_parser->newline_list, node->location.start, g_parser->start_line);
    I("start_line", (long long)line);
    break;
  }
  case PM_SOURCE_FILE_NODE: {
    /* `__FILE__`. Spinel inlines `require`/`require_relative` at parse
       time so we cannot recover the per-call-site source file; we
       always return the toplevel script path passed to spinel_parse,
       documented in test/source_file.rb. The escaped form is cached
       in g_source_file_escaped at init since it never changes. */
    N("SourceFileNode");
    emit_str(id, "content", g_source_file_escaped);
    break;
  }
  case PM_SOURCE_ENCODING_NODE: {
    /* `__ENCODING__`. Spinel sources are UTF-8; codegen returns a
       small Encoding value for Ruby-compatible `.class` / `.name`. */
    N("SourceEncodingNode");
    break;
  }
  case PM_IMPLICIT_NODE: {
    /* Wraps an implicit value reference, e.g. the value side of a
       hash-shorthand `{x:}`. Lowers to its inner value at the same
       id slot so the codegen never sees the implicit wrapper. Covers
       PM_IMPLICIT_NODE in any context (AssocNode value, kwarg
       shorthand inside KeywordHashNode, future Prism evolutions). */
    pm_implicit_node_t *n = (pm_implicit_node_t *)node;
    node_counter--;
    return flatten(n->value);
  }
  case PM_MISSING_NODE: {
    /* Prism emits MissingNode as an error-recovery placeholder. main()
       bails out before flatten() runs when parser.error_list is
       non-empty, so reaching this case means Prism produced a
       MissingNode without flagging an error -- a contract violation we
       surface clearly rather than silently miscompile. */
    fprintf(stderr, "spinel_parse: internal error: MissingNode reached flatten() at byte offset %td; parse error not in error_list\n",
            node->location.start - g_parser->start);
    exit(1);
  }
  case PM_SHAREABLE_CONSTANT_NODE: {
    /* `# shareable_constant_value: literal` magic comment that wraps
       a constant write. Spinel has no Ractor support, so the
       shareability state is a no-op. We lower at parse time by
       flattening the inner write directly into THIS node slot and
       discarding the wrapper. Many later codegen scanner passes
       look for ConstantWriteNode at the top level of statements;
       lowering here lets all of them work without modification. */
    pm_shareable_constant_node_t *n = (pm_shareable_constant_node_t *)node;
    /* Re-flatten the inner write at *this* id by rewinding the counter. */
    node_counter--;
    return flatten(n->write);
  }
  case PM_SPLAT_NODE: {
    pm_splat_node_t *n = (pm_splat_node_t *)node;
    N("SplatNode");
    R("expression", n->expression);
    break;
  }
  case PM_ASSOC_SPLAT_NODE: {
    /* `**h` in argument position — splice a hash's entries as
       keyword args. Wraps the inner hash expression; codegen
       handles the expansion at call sites. Issue #917. */
    pm_assoc_splat_node_t *n = (pm_assoc_splat_node_t *)node;
    N("AssocSplatNode");
    R("value", n->value);
    break;
  }
  case PM_SUPER_NODE: {
    pm_super_node_t *n = (pm_super_node_t *)node;
    N("SuperNode");
    R("arguments", n->arguments);
    R("block", n->block);
    break;
  }
  case PM_FORWARDING_SUPER_NODE: {
    pm_forwarding_super_node_t *n = (pm_forwarding_super_node_t *)node;
    N("ForwardingSuperNode");
    R("block", (pm_node_t *)n->block);
    break;
  }
  case PM_MULTI_WRITE_NODE: {
    pm_multi_write_node_t *n = (pm_multi_write_node_t *)node;
    N("MultiWriteNode");
    A("lefts", &n->lefts);
    if (n->rest) R("rest", n->rest);
    A("rights", &n->rights);
    R("value", n->value);
    break;
  }
  case PM_IMPLICIT_REST_NODE:
    N("ImplicitRestNode");
    break;
  /* `def foo(...)` forwarding parameter and `bar(...)` forwarding
     arguments. No payload: `...` has no explicit names. analyze/codegen
     lower these into synthetic `*args, **kw, &block` slots (issue
     #1288). */
  case PM_FORWARDING_PARAMETER_NODE:
    N("ForwardingParameterNode");
    break;
  case PM_FORWARDING_ARGUMENTS_NODE:
    N("ForwardingArgumentsNode");
    break;
  case PM_LAMBDA_NODE: {
    pm_lambda_node_t *n = (pm_lambda_node_t *)node;
    N("LambdaNode");
    if (n->parameters) {
      if (PM_NODE_TYPE(n->parameters) == PM_BLOCK_PARAMETERS_NODE) {
        pm_block_parameters_node_t *bp = (pm_block_parameters_node_t *)n->parameters;
        if (bp->parameters) {
          R("parameters", bp->parameters);
        }
      }
else if (PM_NODE_TYPE(n->parameters) != PM_NUMBERED_PARAMETERS_NODE) {
        R("parameters", n->parameters);
      }
    }
    if (n->body) R("body", n->body);
    break;
  }
  case PM_X_STRING_NODE: {
    pm_x_string_node_t *n = (pm_x_string_node_t *)node;
    N("XStringNode");
    S("content", escape_pm_string(&n->unescaped));
    break;
  }
  case PM_INTERPOLATED_X_STRING_NODE: {
    pm_interpolated_x_string_node_t *n = (pm_interpolated_x_string_node_t *)node;
    N("InterpolatedXStringNode");
    A("parts", &n->parts);
    break;
  }
  case PM_REGULAR_EXPRESSION_NODE: {
    pm_regular_expression_node_t *n = (pm_regular_expression_node_t *)node;
    N("RegularExpressionNode");
    S("unescaped", escape_pm_string(&n->unescaped));
    /* Emit Prism's regex flags so the codegen can pass /i, /x, /m
       through to the engine. PM_REGULAR_EXPRESSION_FLAGS_IGNORE_CASE=4,
       _EXTENDED=8, _MULTI_LINE=16. */
    I("flags", n->base.flags);
    break;
  }
  case PM_INTERPOLATED_REGULAR_EXPRESSION_NODE: {
    /* `/foo_#{x}/`. Same shape as InterpolatedStringNode -- carries
       `parts` -- plus a flags integer matching RegularExpressionNode.
       Codegen builds the pattern string via compile_interpolated and
       feeds it to sp_re_runtime_compile at execution time. */
    pm_interpolated_regular_expression_node_t *n = (pm_interpolated_regular_expression_node_t *)node;
    N("InterpolatedRegularExpressionNode");
    A("parts", &n->parts);
    I("flags", n->base.flags);
    break;
  }
  case PM_NUMBERED_REFERENCE_READ_NODE: {
    pm_numbered_reference_read_node_t *n = (pm_numbered_reference_read_node_t *)node;
    N("NumberedReferenceReadNode");
    I("number", n->number);
    break;
  }
  case PM_MATCH_WRITE_NODE: {
    pm_match_write_node_t *n = (pm_match_write_node_t *)node;
    N("MatchWriteNode");
    R("call", n->call);
    break;
  }
  case PM_MATCH_REQUIRED_NODE: {
    /* Rightward assignment: `expr => var` (Ruby 3.0+). When the
       pattern is a single LocalVariableTargetNode, this is just
       `var = expr` and we lower it to a LocalVariableWriteNode so
       the codegen reuses the regular assignment path. Full pattern
       matching (array / hash patterns, pinned vars) is out of scope
       and falls through to the unknown-node passthrough. */
    pm_match_required_node_t *n = (pm_match_required_node_t *)node;
    if (n->pattern && PM_NODE_TYPE_P(n->pattern, PM_LOCAL_VARIABLE_TARGET_NODE)) {
      pm_local_variable_target_node_t *t = (pm_local_variable_target_node_t *)n->pattern;
      N("LocalVariableWriteNode");
      NAME("name", t->name);
      R("value", n->value);
    }
else {
      N("MatchRequiredNode");
      R("value", n->value);
      R("pattern", n->pattern);
    }
    break;
  }
  case PM_ALTERNATION_PATTERN_NODE: {
    pm_alternation_pattern_node_t *n = (pm_alternation_pattern_node_t *)node;
    N("AlternationPatternNode");
    R("left", n->left);
    R("right", n->right);
    break;
  }
  case PM_ARRAY_PATTERN_NODE: {
    /* `case x in [a, b, c]`. requireds is the list of fixed-position
       sub-patterns. rest / posts cover `[a, *, c]`. constant covers
       `Foo[a, b]` (Foo() destructuring). Minimal initial support:
       emit requireds as a node-id array; rest / posts / constant
       handled later when the codegen path catches up. Issue #669. */
    pm_array_pattern_node_t *n = (pm_array_pattern_node_t *)node;
    N("ArrayPatternNode");
    R("constant", n->constant);
    A("requireds", &n->requireds);
    R("rest", n->rest);
    A("posts", &n->posts);
    break;
  }
  case PM_HASH_PATTERN_NODE: {
    /* `case x in {a:, b: 2}`. elements is a list of AssocNodes
       (key + sub-pattern); a `:b:` shorthand binds the key's symbol
       as a same-named LV. Constant covers `Foo[a:]` destructuring
       (not yet handled by codegen). Rest covers `**rest` (also not
       yet handled). Issue #805. */
    pm_hash_pattern_node_t *n = (pm_hash_pattern_node_t *)node;
    N("HashPatternNode");
    R("constant", n->constant);
    A("elements", &n->elements);
    R("rest", n->rest);
    break;
  }
  case PM_FIND_PATTERN_NODE: {
    /* `case x in [*a, b, *c]` -- find-anywhere pattern with leading
       and trailing wildcards plus required middle elements.
       Currently surfaces all three slots so the codegen can match
       on length + extract requireds; the find-anywhere semantics
       (variable-position match) are not yet implemented but the
       shape no longer drops to UnsupportedNode. Issue #805. */
    pm_find_pattern_node_t *n = (pm_find_pattern_node_t *)node;
    N("FindPatternNode");
    R("constant", n->constant);
    R("left", (pm_node_t *)n->left);
    A("requireds", &n->requireds);
    R("right", n->right);
    break;
  }
  case PM_CAPTURE_PATTERN_NODE: {
    /* `case x in pat => var` -- match `pat`, bind matched value to
       `var` (LocalVariableTargetNode). Issue #884. */
    pm_capture_pattern_node_t *n = (pm_capture_pattern_node_t *)node;
    N("CapturePatternNode");
    R("value", (pm_node_t *)n->value);
    R("target", (pm_node_t *)n->target);
    break;
  }
  case PM_PINNED_EXPRESSION_NODE: {
    /* `case x in ^(expr)`. The pinned expression is evaluated at
       match time and compared by `==` against the scrutinee. */
    pm_pinned_expression_node_t *n = (pm_pinned_expression_node_t *)node;
    N("PinnedExpressionNode");
    R("expression", n->expression);
    break;
  }
  case PM_PINNED_VARIABLE_NODE: {
    /* `case x in ^var`. The wrapped variable can be any read-shape
       node prism emits for `^var` -- LocalVariableReadNode,
       InstanceVariableReadNode, ClassVariableReadNode,
       GlobalVariableReadNode, ConstantReadNode,
       NumberedReferenceReadNode, BackReferenceReadNode. Routed to
       the same `@nd_expression` slot as PinnedExpressionNode so the
       codegen treats them uniformly -- both are "match the scrutinee
       by `==` against this expression". */
    pm_pinned_variable_node_t *n = (pm_pinned_variable_node_t *)node;
    N("PinnedVariableNode");
    R("expression", n->variable);
    break;
  }
  case PM_NUMBERED_PARAMETERS_NODE: {
    pm_numbered_parameters_node_t *n = (pm_numbered_parameters_node_t *)node;
    N("NumberedParametersNode");
    I("maximum", n->maximum);
    break;
  }
  case PM_ARGUMENTS_NODE: {
    pm_arguments_node_t *n = (pm_arguments_node_t *)node;
    N("ArgumentsNode");
    A("arguments", &n->arguments);
    break;
  }
  case PM_BLOCK_PARAMETERS_NODE: {
    pm_block_parameters_node_t *n = (pm_block_parameters_node_t *)node;
    N("BlockParametersNode");
    if (n->parameters) R("parameters", n->parameters);
    break;
  }
  case PM_IT_PARAMETERS_NODE:
    /* Ruby 3.4 implicit `it` is semantically `_1` — lower to a
       NumberedParametersNode so the codegen's existing
       NumberedParametersNode arity path (get_block_param) handles it
       transparently. The block body's `it` references separately
       become PM_IT_LOCAL_VARIABLE_READ_NODE, also lowered below. */
    N("NumberedParametersNode");
    I("maximum", 1);
    break;
  case PM_IT_LOCAL_VARIABLE_READ_NODE:
    /* `it` inside the block body. Lowered to a regular
       LocalVariableReadNode named "_1" so it pairs with the
       lowered NumberedParametersNode { maximum: 1 } above. */
    N("LocalVariableReadNode");
    S("name", escape_str((const uint8_t *)"_1", 2));
    break;
  case PM_INTERPOLATED_SYMBOL_NODE: {
    /* `:"foo_#{x}"`. Carries `parts` like InterpolatedStringNode;
       codegen builds the string the same way and uses it directly --
       Spinel treats dynamic symbols as their assembled string value
       since it doesn't intern non-literal symbols. */
    pm_interpolated_symbol_node_t *n = (pm_interpolated_symbol_node_t *)node;
    N("InterpolatedSymbolNode");
    A("parts", &n->parts);
    break;
  }
  case PM_REDO_NODE:
    /* `redo`. Re-run the current iteration of the enclosing loop
       without re-evaluating the loop guard or advancing the
       iterator. Codegen emits a labeled `goto` to a label installed
       at the top of the iteration body. */
    N("RedoNode");
    break;
  case PM_BACK_REFERENCE_READ_NODE: {
    /* `$&`, `$~`, `$'`, $`. Spinel populates sp_re_match_str / _pre /
       _post during regex matches alongside sp_re_captures (which the
       NumberedReferenceReadNode arm already reads). */
    pm_back_reference_read_node_t *n = (pm_back_reference_read_node_t *)node;
    N("BackReferenceReadNode");
    NAME("name", n->name);
    break;
  }
  case PM_MULTI_TARGET_NODE: {
    /* Nested LHS in destructuring multi-assign:
       `a, (b, c), d = 1, [2, 3], 4`. The inner (b, c) parenthesized
       group becomes a MultiTargetNode that recursively unpacks its
       slot of the RHS into the inner targets. */
    pm_multi_target_node_t *n = (pm_multi_target_node_t *)node;
    N("MultiTargetNode");
    A("lefts", &n->lefts);
    if (n->rest) R("rest", n->rest);
    A("rights", &n->rights);
    break;
  }
  case PM_EMBEDDED_VARIABLE_NODE: {
    /* `"foo #@bar"` shorthand for `"foo #{@bar}"`. The cleanest
       implementation is parser-side lowering: synthesize an
       EmbeddedStatementsNode wrapping a StatementsNode whose single
       body element is the variable read. The existing interpolation
       path then handles it without any codegen change.

       Same lowering trick as PM_IT_LOCAL_VARIABLE_READ_NODE -- emit
       a different node type at the same `id` slot so parent walks
       (parts list of the surrounding InterpolatedString) keep the
       slot unchanged. The wrapped StatementsNode gets a fresh id
       allocated via node_counter++. */
    pm_embedded_variable_node_t *n = (pm_embedded_variable_node_t *)node;
    N("EmbeddedStatementsNode");
    int var_id = flatten(n->variable);
    int stmts_id = node_counter++;
    out_add("N %d StatementsNode", stmts_id);
    out_add("A %d body %d", stmts_id, var_id);
    out_add("R %d statements %d", id, stmts_id);
    break;
  }
  case PM_ALIAS_METHOD_NODE: {
    /* `alias new old` -- compile-time method-name aliasing inside a
       class body. new_name and old_name are nodes representing the
       names (typically SymbolNode literals; InterpolatedSymbolNode is
       tolerated and silently skipped by the codegen helper). Spinel
       registers a duplicate method-table entry under the new name
       pointing to the same body, so dispatch on `.greet` lands on the
       same C function as `.hello`. */
    pm_alias_method_node_t *n = (pm_alias_method_node_t *)node;
    N("AliasMethodNode");
    R("new_name", n->new_name);
    R("old_name", n->old_name);
    break;
  }
  case PM_POST_EXECUTION_NODE: {
    /* `END { ... }`. CRuby runs END blocks in REVERSE registration
       order at program exit. Spinel emits each as a static C
       function and registers them via atexit() at main() startup --
       atexit naturally invokes handlers LIFO, matching CRuby. */
    pm_post_execution_node_t *n = (pm_post_execution_node_t *)node;
    N("PostExecutionNode");
    R("statements", n->statements);
    break;
  }
  case PM_PRE_EXECUTION_NODE: {
    /* `BEGIN { ... }`. CRuby runs all BEGIN blocks in source order
       BEFORE any other top-level statements. Spinel collects the
       bodies during a pre-pass and emits them at the very top of
       main() in source-encounter order. */
    pm_pre_execution_node_t *n = (pm_pre_execution_node_t *)node;
    N("PreExecutionNode");
    R("statements", n->statements);
    break;
  }
  case PM_UNDEF_NODE: {
    /* `undef foo, bar` inside a class body. CRuby raises NoMethodError
       at runtime when an undef'd method is called; Spinel's AOT model
       resolves dispatch at compile time, so we record the undefs but
       leave compile-time enforcement of "calling an undef'd method
       fails" to a future pass. */
    pm_undef_node_t *n = (pm_undef_node_t *)node;
    N("UndefNode");
    A("names", &n->names);
    break;
  }
  case PM_ALIAS_GLOBAL_VARIABLE_NODE: {
    /* `alias $copy $orig` -- compile-time gvar aliasing. The
       new_name and old_name slots are GlobalVariableReadNodes whose
       `name` field carries the literal $-prefixed name. Spinel
       resolves $copy to $orig everywhere the alias is in scope, so
       the C output uses one storage slot for both. */
    pm_alias_global_variable_node_t *n = (pm_alias_global_variable_node_t *)node;
    N("AliasGlobalVariableNode");
    R("new_name", n->new_name);
    R("old_name", n->old_name);
    break;
  }
  default: {
    /* Previously emitted UnknownNode_<n> which silently degraded to
       "0" in codegen. Now emit a hard-error sentinel carrying the
       Prism node kind name (in human-friendly Ruby vocabulary) + the
       source line so codegen refuses to compile and tells the user
       exactly what's wrong. */
    char pretty[128];
    size_t plen = prism_kind_to_pascal(pm_node_type_to_str(t), pretty, sizeof(pretty));
    int32_t line = pm_newline_list_line(&g_parser->newline_list, node->location.start, g_parser->start_line);

    N("UnsupportedNode");
    char *kind_e = escape_str((const uint8_t *)pretty, plen);
    emit_str(id, "kind", kind_e);
    free(kind_e);
    emit_int(id, "source_line", (long long)line);
    break;
  }
  }

#undef N
#undef S
#undef I
#undef F
#undef R
#undef A
#undef NAME

  return id;
}

/* ---- require_relative resolution ---- */
static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long len = ftell(f);
  /* Issue #768: ftell returning -1 (stream error) used to wrap len + 1
     to 0, malloc(0) returns NULL, then fread(NULL, 1, SIZE_MAX, f)
     tried to read 16 exabytes. Bail cleanly. */
  if (len < 0) { fclose(f); return NULL; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  char *buf = malloc((size_t)len + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t nread = fread(buf, 1, (size_t)len, f);
  /* Issue #763: detect short read and bail rather than processing
     a truncated source file silently. */
  if (nread != (size_t)len) { free(buf); fclose(f); return NULL; }
  buf[nread] = '\0';
  fclose(f);
  return buf;
}

/* Track files already inlined so duplicate requires/require_relatives in
   different files don't re-emit (and re-define structs/classes) the same
   content. Dynamic so we don't silently drop entries on large projects. */
static char **sp_included_paths = NULL;
static int sp_included_count = 0;
static int sp_included_cap = 0;

/* Resolve a path to its canonical form for dedup. realpath() returns NULL
   on missing files; in that case fall back to the literal path. */
/* Issue #749: realpath/_fullpath + strdup can both fail under OOM,
   leaving callers with a NULL pointer that goes to strcmp(NULL, ...)
   and segfaults. Fall back to a static empty string so downstream
   `strcmp(canonical, ...) == 0` consistently misses (the caller then
   treats this as "not already included" and proceeds with the file
   read, which fails through the normal LoadError path). */
static char *sp_canonical_path(const char *path) {
  if (!path) { char *e = strdup(""); return e ? e : NULL; }
  char *real = realpath(path, NULL);
  if (real) return real;
  char *d = strdup(path);
  if (d) return d;
  /* Last resort: return a static empty string to avoid NULL. The
     buffer is read-only by callers (passed to strcmp, then freed by
     the caller's free() -- so allocate a fresh empty string instead
     of returning a string literal). */
  return strdup("");
}

static int sp_path_already_included(const char *canonical) {
  if (!canonical) return 0;
  for (int i = 0; i < sp_included_count; i++) {
    if (sp_included_paths[i] && strcmp(sp_included_paths[i], canonical) == 0) return 1;
  }
  return 0;
}

static void sp_mark_path_included(const char *canonical) {
  if (sp_included_count >= sp_included_cap) {
    int new_cap = sp_included_cap == 0 ? 16 : sp_included_cap * 2;
    char **np = (char **)realloc(sp_included_paths, sizeof(char *) * new_cap);
    if (!np) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    sp_included_paths = np;
    sp_included_cap = new_cap;
  }
  sp_included_paths[sp_included_count++] = strdup(canonical);
}

/* Free the included-paths table at end of run. The process is short-lived,
   so this matters mostly for tools (leak checkers, embedders) that
   scrutinise end-of-run state. */
static void sp_includes_free(void) {
  for (int i = 0; i < sp_included_count; i++) {
    free(sp_included_paths[i]);
  }
  free(sp_included_paths);
  sp_included_paths = NULL;
  sp_included_count = 0;
  sp_included_cap = 0;
}

/* ---- Debug multi-file source map (g_emit_line only) -------------------
   require/require_relative are resolved by *textual* splicing into one
   buffer, so a node's Prism line is a line in that concatenated buffer and
   the original file boundaries are lost. For --debug we recover them: each
   spliced file's content is wrapped in PUSH/POP marker *comments* (Prism
   ignores comments, so the AST is unchanged), and a single pass over the
   final buffer reconstructs, per buffer line, which file and original line
   it came from. The stack accounting needs no original-line bookkeeping at
   splice time: a PUSH consumes the parent's (replaced) require line, then
   content lines count within the child's own coordinates until POP. */
#define SP_PUSH_PREFIX "#<SPINEL_PUSH>"
#define SP_POP_PREFIX "#<SPINEL_POP>"

static char **sp_file_table = NULL;  /* id -> path */
static int sp_file_count = 0, sp_file_cap = 0;

static int sp_intern_file(const char *path) {
  for (int i = 0; i < sp_file_count; i++)
    if (strcmp(sp_file_table[i], path) == 0) return i;
  if (sp_file_count >= sp_file_cap) {
    int nc = sp_file_cap == 0 ? 8 : sp_file_cap * 2;
    char **np = (char **)realloc(sp_file_table, sizeof(char *) * nc);
    if (!np) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
    sp_file_table = np; sp_file_cap = nc;
  }
  char *dup_path = strdup(path);
  if (!dup_path) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  sp_file_table[sp_file_count] = dup_path;
  return sp_file_count++;
}

/* In debug mode, wrap an included file's (already-resolved) content with
   PUSH <path> / POP marker lines. Takes ownership of `content`; returns it
   unchanged when not in debug mode. */
static char *sp_wrap_included(char *content, const char *path) {
  if (!g_emit_line) return content;
  size_t clen = strlen(content);
  int need_nl = (clen > 0 && content[clen - 1] != '\n') ? 1 : 0;
  size_t total = strlen(SP_PUSH_PREFIX) + strlen(path) + 1 + clen + need_nl
               + strlen(SP_POP_PREFIX) + 1 + 1;
  char *w = malloc(total + 8);
  if (!w) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  size_t o = (size_t)sprintf(w, SP_PUSH_PREFIX "%s\n", path);
  memcpy(w + o, content, clen); o += clen;
  if (need_nl) w[o++] = '\n';
  o += (size_t)sprintf(w + o, SP_POP_PREFIX "\n");
  w[o] = '\0';
  free(content);
  return w;
}

/* Reconstruct the line map from the final spliced buffer. toplevel is the
   path of the outermost file (interned as the initial frame). */
static void sp_build_line_map(const char *src, const char *toplevel) {
  size_t nlines = 1;
  for (const char *p = src; *p; p++) if (*p == '\n') nlines++;
  sp_line_map_n = (int)nlines;
  sp_line_file = (int *)calloc(nlines + 2, sizeof(int));
  sp_line_orig = (int *)calloc(nlines + 2, sizeof(int));

  int *stk_file = (int *)malloc(sizeof(int) * (nlines + 2));
  int *stk_next = (int *)malloc(sizeof(int) * (nlines + 2));
  if (!sp_line_file || !sp_line_orig || !stk_file || !stk_next) {
    fprintf(stderr, "spinel_parse: out of memory\n"); exit(1);
  }
  int sp = 0;
  stk_file[sp] = sp_intern_file(toplevel);
  stk_next[sp] = 1;

  int bl = 1;
  const char *line = src;
  while (1) {
    const char *eol = strchr(line, '\n');
    size_t len = eol ? (size_t)(eol - line) : strlen(line);
    if (strncmp(line, SP_PUSH_PREFIX, strlen(SP_PUSH_PREFIX)) == 0) {
      /* The replaced require occupied one parent line: consume it. */
      if (sp >= 0) stk_next[sp] += 1;
      char pathbuf[1024];
      size_t plen = len - strlen(SP_PUSH_PREFIX);
      if (plen >= sizeof(pathbuf)) plen = sizeof(pathbuf) - 1;
      memcpy(pathbuf, line + strlen(SP_PUSH_PREFIX), plen);
      pathbuf[plen] = '\0';
      sp++;
      stk_file[sp] = sp_intern_file(pathbuf);
      stk_next[sp] = 1;
      /* marker line maps to nothing meaningful */
    }
else if (strncmp(line, SP_POP_PREFIX, strlen(SP_POP_PREFIX)) == 0) {
      if (sp > 0) sp--;
    }
else {
      sp_line_file[bl] = stk_file[sp];
      sp_line_orig[bl] = stk_next[sp];
      stk_next[sp] += 1;
    }
    bl++;
    if (!eol) break;
    line = eol + 1;
    if (*line == '\0') break;
  }
  free(stk_file);
  free(stk_next);
}

/* Simple require_relative resolver: replace lines matching
   require_relative "path" with the file content. Files that have
   already been included once are silently skipped on subsequent
   requires (matching Ruby's load-once semantics). */
static char *resolve_requires(const char *source, const char *source_path) {
  /* Get base directory */
  char *path_copy = strdup(source_path);
  char *dir = strdup(path_copy);
  /* Find last / */
  char *slash = strrchr(dir, '/');
  if (slash) *slash = '\0';
  else { free(dir); dir = strdup("."); }
  free(path_copy);

  char *result = strdup(source);
  char *pos;
  char *scan_from = result;
  while ((pos = strstr(scan_from, "require_relative")) != NULL) {
    /* Check it's at start of line. If the match is mid-line (e.g.
       the word appears in a comment or string), advance past it and
       keep scanning the rest of the file — don't abort the whole
       loop, since later lines may have legitimate require_relative
       statements. */
    if (pos != result && *(pos - 1) != '\n') {
      scan_from = pos + 1;
      continue;
    }
    char *line_end = strchr(pos, '\n');
    if (!line_end) line_end = pos + strlen(pos);

    /* Extract quoted path */
    char *q1 = strchr(pos, '"');
    char *q2 = strchr(pos, '\'');
    char quote_char;
    char *start;
    if (q1 && q1 < line_end && (!q2 || q1 < q2)) {
      quote_char = '"';
      start = q1 + 1;
    }
else if (q2 && q2 < line_end) {
      quote_char = '\'';
      start = q2 + 1;
    }
else { scan_from = pos + 1; continue; }

    char *end = strchr(start, quote_char);
    if (!end || end > line_end) { scan_from = pos + 1; continue; }

    size_t path_len = end - start;
    /* Issue #765: bail rather than silently truncating overlong paths
       into the fixed-size buffers. */
    if (path_len >= sizeof(((struct {char x[512];}*)0)->x)) { scan_from = pos + 1; continue; }
    char rel_path[512];
    snprintf(rel_path, sizeof(rel_path), "%.*s", (int)path_len, start);

    /* Build full path */
    char full_path[1024];
    int fp_n = snprintf(full_path, sizeof(full_path), "%s/%s", dir, rel_path);
    if (fp_n < 0 || (size_t)fp_n >= sizeof(full_path)) { scan_from = pos + 1; continue; }
    {
      size_t fl = strlen(full_path);
      if (fl < sizeof(full_path) - 4 && (fl < 3 || strcmp(full_path + fl - 3, ".rb") != 0))
        strcat(full_path, ".rb");
    }

    char *canonical = sp_canonical_path(full_path);
    char *content;
    if (sp_path_already_included(canonical)) {
      /* Already inlined once -- replace require with empty content */
      content = strdup("# require_relative skipped (already included)");
      free(canonical);
    }
else {
      sp_mark_path_included(canonical);
      content = read_file(full_path);
      if (!content) {
        fprintf(stderr,
                "warning: require_relative \"%s\" from %s could not be resolved (no such file: %s); the call is ignored\n",
                rel_path, source_path, full_path);
        content = strdup("# require_relative not found");
      }
else {
        /* Recursively resolve */
        char *resolved = resolve_requires(content, full_path);
        free(content);
        content = resolved;
      }
      free(canonical);
    }

    /* Debug: wrap with PUSH/POP markers so the line map can attribute this
       file's nodes to the right source. No-op outside --debug. */
    content = sp_wrap_included(content, full_path);

    /* Replace the line */
    size_t line_len = (line_end - pos) + ((*line_end == '\n') ? 1 : 0);
    size_t content_len = strlen(content);
    size_t result_len = strlen(result);
    size_t before_len = pos - result;

    char *new_result = malloc(result_len - line_len + content_len + 2);
    memcpy(new_result, result, before_len);
    memcpy(new_result + before_len, content, content_len);
    if (content_len > 0 && content[content_len - 1] != '\n')
      new_result[before_len + content_len++] = '\n';
    memcpy(new_result + before_len + content_len, pos + line_len, result_len - before_len - line_len + 1);

    free(result);
    result = new_result;
    /* Buffer reallocated; restart scan from the top of the new buffer. */
    scan_from = result;
    free(content);
  }
  free(dir);
  return result;
}

/* A `require "X"` whose X has no bundled lib/X.rb may still be provided
   natively by the Spinel runtime/codegen (e.g. the JSON module). Such a
   require is a harmless no-op and must not warn. Bundled .rb libs (set,
   stringio, strscan, erb, ...) are resolved by file existence and never
   reach the not-found branch, so they don't belong here -- only the
   C-native modules that map to a stdlib require name do. */
static int sp_lib_is_native(const char *name) {
  static const char *const natives[] = { "json", NULL };
  for (int i = 0; natives[i]; i++) {
    if (strcmp(name, natives[i]) == 0) return 1;
  }
  return 0;
}

/* ---- Plain require resolution ---- */
static char *resolve_plain_requires(char *source, const char *exe_path) {
  /* Find lib/ directory relative to this executable */
  char lib_dir[1024];
  strncpy(lib_dir, exe_path, sizeof(lib_dir) - 1);
  char *slash = strrchr(lib_dir, '/');
  if (slash) *slash = '\0';
  else strcpy(lib_dir, ".");
  strcat(lib_dir, "/lib");

  char *result = source;
  char *pos;
  /* `scan` lets us step past a computed-argument require without aborting
     the loop; it resets to `result` after each rebuild below. */
  char *scan = result;
  /* Match a `require` at the very start of the buffer (offset 0) or
     immediately after a newline. Re-checking offset 0 every iteration --
     not just while `result == source` -- is what lets a first-line
     `require` still be processed when a later `require` exists: once the
     buffer is rebuilt below, `result != source`, and the old condition
     stranded the line-1 require. */
  for (;;) {
    if (scan == result && strncmp(result, "require ", 8) == 0) {
      pos = result;
    }
else {
      pos = strstr(scan, "\nrequire ");
      if (pos == NULL) break;
      pos++; /* skip the matched newline */
    }
    char *line_end = strchr(pos, '\n');
    if (!line_end) line_end = pos + strlen(pos);

    /* Only a bare string-literal argument is inlined. A computed argument
       (`require File.expand_path('x', __dir__)`, `require some_const`) is
       left untouched: grabbing the first quote on the line would mistake an
       inner string literal for the lib name and strand the rest of the
       expression, producing invalid syntax. Skip past it and keep
       scanning later lines. (#1383) */
    {
      const char *arg = pos + 8;
      while (arg < line_end && (*arg == ' ' || *arg == '\t')) arg++;
      if (arg >= line_end || (*arg != '"' && *arg != '\'')) {
        scan = line_end;
        continue;
      }
    }

    /* Must be: require "name" or require 'name' */
    char *q1 = strchr(pos + 7, '"');
    char *q2 = strchr(pos + 7, '\'');
    char *start; char quote_char;
    if (q1 && q1 < line_end && (!q2 || q1 < q2)) { quote_char = '"'; start = q1 + 1; }
    else if (q2 && q2 < line_end) { quote_char = '\''; start = q2 + 1; }
    else break;
    char *end = strchr(start, quote_char);
    if (!end || end > line_end) break;

    char lib_name[256];
    snprintf(lib_name, sizeof(lib_name), "%.*s", (int)(end - start), start);
    char lib_path[1024];
    snprintf(lib_path, sizeof(lib_path), "%s/%s", lib_dir, lib_name);
    {
      size_t fl = strlen(lib_path);
      if (fl < sizeof(lib_path) - 4 && (fl < 3 || strcmp(lib_path + fl - 3, ".rb") != 0))
        strcat(lib_path, ".rb");
    }

    /* Same dedup as resolve_requires: a file pulled in via plain `require`
       must not be re-inlined if a previous `require` or `require_relative`
       already pulled it. Otherwise mixing the two forms for the same lib
       still produces struct-redefinition errors. */
    char *canonical = sp_canonical_path(lib_path);
    char *content;
    if (sp_path_already_included(canonical)) {
      content = strdup("# require skipped (already included)");
      free(canonical);
    }
else {
      sp_mark_path_included(canonical);
      free(canonical);
      content = read_file(lib_path);
      if (!content) {
        /* the compiler binary may live one level below the repo root (e.g.
           build/spinel with the stdlib at ../lib): retry one level up */
        char alt_path[1024];
        int exe_len = (int)strlen(lib_dir) - 4;   /* strip the trailing "/lib" */
        if (exe_len < 0) exe_len = 0;
        snprintf(alt_path, sizeof(alt_path), "%.*s/../lib/%s", exe_len, lib_dir, lib_name);
        size_t al = strlen(alt_path);
        if (al < sizeof(alt_path) - 4 && (al < 3 || strcmp(alt_path + al - 3, ".rb") != 0))
          strcat(alt_path, ".rb");
        content = read_file(alt_path);
        if (content) snprintf(lib_path, sizeof(lib_path), "%s", alt_path);
      }
      if (!content) {
        if (sp_lib_is_native(lib_name)) {
          /* Provided natively by the Spinel runtime; the require is a
             harmless no-op, so don't warn. */
          content = strdup("# require provided by Spinel runtime");
        }
else {
          fprintf(stderr,
                  "warning: '%s' is not available in Spinel; the require is ignored and code using it will fail\n",
                  lib_name);
          content = strdup("# require not resolved");
        }
      }
else {
        char *resolved = resolve_requires(content, lib_path);
        free(content);
        content = resolved;
      }
    }

    /* A trailing same-line modifier (`rescue`, `if`, `unless`, `and`,
       `&&`, ...) puts the require in expression position: `require 'x'
       rescue nil`. Replacing it with inlined statements or a comment
       strands the modifier and Prism reports `unexpected 'rescue'`.
       Substitute a `nil` expression instead so the modifier attaches;
       an optional/unavailable require degrades to nil, which is what the
       `rescue` guards for. `;`, `#`, newline and EOF are statement
       boundaries, not modifiers, and keep the normal inlining path. */
    {
      char *trail = end + 1;
      while (*trail == ' ' || *trail == '\t') trail++;
      /* `\r` is part of a CRLF line ending, not a modifier: treat it as a
         statement boundary so Windows sources keep the normal inlining
         path. */
      if (*trail != '\n' && *trail != '\r' && *trail != ';' &&
          *trail != '\0' && *trail != '#') {
        free(content);
        size_t rl_len = (size_t)((end + 1) - pos);   /* the `require 'x'` span */
        size_t result_len = strlen(result);
        size_t before_len = (size_t)(pos - result);
        char *new_result = malloc(result_len - rl_len + 4);
        memcpy(new_result, result, before_len);
        memcpy(new_result + before_len, "nil", 3);
        memcpy(new_result + before_len + 3, pos + rl_len,
               result_len - before_len - rl_len + 1);
        free(result);
        result = new_result;
        scan = result;
        continue;
      }
    }

    /* Debug: marker-wrap so plain-require'd lib content doesn't corrupt the
       line map's accounting for code after the require. No-op without --debug. */
    content = sp_wrap_included(content, lib_path);

    /* Replace only the `require "name"` statement itself, not the whole
       line, so `require "x"; code` keeps `code`. Consume trailing
       horizontal whitespace and a single terminating newline (so a
       require on its own line leaves no blank line); stop at `;` or any
       other trailing code, which the inserted content's trailing newline
       then pushes onto its own line. */
    char *stmt_end = end + 1;  /* just past the closing quote */
    while (*stmt_end == ' ' || *stmt_end == '\t') stmt_end++;
    if (*stmt_end == '\n') stmt_end++;
    size_t line_len = stmt_end - pos;
    size_t content_len = strlen(content);
    size_t result_len = strlen(result);
    size_t before_len = pos - result;
    char *new_result = malloc(result_len - line_len + content_len + 2);
    memcpy(new_result, result, before_len);
    memcpy(new_result + before_len, content, content_len);
    if (content_len > 0 && content[content_len - 1] != '\n')
      new_result[before_len + content_len++] = '\n';
    memcpy(new_result + before_len + content_len, pos + line_len, result_len - before_len - line_len + 1);
    free(result);
    result = new_result;
    scan = result;
    free(content);
  }
  return result;
}

/* ---- Syntax sugar rewriting ---- */

/* Ruby method-name char class: idents, digits, `?` / `!` / `=` suffixes,
   operator-method chars (`+`, `-`, `*`, etc.), and `[` / `]` for the
   index operators `[]` and `[]=`. Digits are allowed in the body but
   the macro rejects digit-leading names (invalid Ruby method syntax). */
static int sp_is_method_name_char(char c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '?' || c == '!' || c == '+' ||
         c == '-' || c == '*' || c == '/' || c == '<' || c == '>' ||
         c == '=' || c == '&' || c == '|' || c == '^' || c == '~' ||
         c == '%' || c == '[' || c == ']';
}

static char *rewrite_syntax_sugar(char *source) {
  /* Rewrite .send(:foo, args) / .send("foo", args) → .foo(args) */
  /* Rewrite &:symbol → { |_spx| _spx.symbol } */
  size_t len = strlen(source);
  size_t cap = (len * 2) + 256;
  char *out = malloc(cap);
  size_t oi = 0;
  size_t i = 0;

  #define OUT_CHAR(c) do { if (oi >= cap - 1) { size_t _nc = cap * 2; char *_no = realloc(out, _nc); if (!_no) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); } out = _no; cap = _nc; } out[oi++] = (c); } while(0)
  #define OUT_STR(s) do { const char *_s = (s); while (*_s) { OUT_CHAR(*_s); _s++; } } while(0)

  /* Rewrite one .send(:foo / .send("foo dispatch. `string_form` is 1
     for the double-quoted variant (requires a closing `"`) and 0 for
     the colon-symbol variant. On mismatch (empty / digit-leading
     name, unclosed quote), emits the prefix verbatim and advances
     past it so the outer loop resumes scanning. The args copy tracks
     `"..."` and `'...'` state with `\` escape skipping so parens
     inside string literals do not prematurely close the call. */
  #define REWRITE_SEND_CALL(prefix_str, prefix_len, string_form) do {     \
    size_t _save_i = i;                                                   \
    i += (prefix_len);                                                    \
    size_t _ns = i;                                                       \
    while (i < len && sp_is_method_name_char(source[i])) i++;             \
    size_t _name_len = i - _ns;                                           \
    int _ok = (_name_len > 0);                                            \
 /* Reject digit-leading names: `.123foo()` is not valid Ruby. */         \
    if (_ok && source[_ns] >= '0' && source[_ns] <= '9') { _ok = 0; }     \
    if (_ok && (string_form)) {                                           \
      if (i >= len || source[i] != '"') { _ok = 0; }                      \
      else { i++; /* skip closing quote */ }                              \
    }                                                                     \
    if (!_ok) { i = _save_i; OUT_STR(prefix_str); i += (prefix_len); }    \
    else {                                                                \
      OUT_CHAR('.');                                                      \
      { size_t _k; for (_k = 0; _k < _name_len; _k++) OUT_CHAR(source[_ns + _k]); } \
      if (i < len && source[i] == ')') {                                  \
        i++; /* no args */                                                \
      } \
      else if (i < len && source[i] == ',') {                           \
        i++;                                                              \
        while (i < len && source[i] == ' ') i++;                          \
        OUT_CHAR('(');                                                    \
        { int _depth = 1; char _qc = 0;                                   \
          while (i < len && _depth > 0) {                                 \
            char _ac = source[i];                                         \
            if (_qc != 0) {                                               \
 /* Inside string: `\` escapes next char, skip it as a pair. */           \
              if (_ac == '\\' && i + 1 < len) {                           \
                OUT_CHAR(_ac); OUT_CHAR(source[i + 1]); i += 2; continue; \
              }                                                            \
              if (_ac == _qc) _qc = 0;                                    \
              OUT_CHAR(_ac); i++; continue;                                \
            }                                                              \
            if (_ac == '"' || _ac == '\'') { _qc = _ac; OUT_CHAR(_ac); i++; continue; } \
            if (_ac == '(') _depth++;                                     \
            else if (_ac == ')') { _depth--; if (_depth == 0) { i++; break; } } \
            OUT_CHAR(_ac); i++;                                           \
          } }                                                             \
        OUT_CHAR(')');                                                    \
      }                                                                   \
    }                                                                     \
  } while(0)

  while (i < len) {
    /* .send(:foo, args) → .foo(args) */
    if (i + 7 < len && strncmp(source + i, ".send(:", 7) == 0) {
      REWRITE_SEND_CALL(".send(:", 7, 0);
      continue;
    }
    /* .send("foo", args) → .foo(args). Plain string literal only;
       interpolated strings parse as InterpolatedStringNode and are
       left untouched (Spinel can't resolve the name statically). */
    if (i + 7 < len && strncmp(source + i, ".send(\"", 7) == 0) {
      REWRITE_SEND_CALL(".send(\"", 7, 1);
      continue;
    }
    /* .__send__(:foo, args) → .foo(args). CRuby's overrides-resistant
       alias of send; semantically identical for Spinel's static dispatch. */
    if (i + 11 < len && strncmp(source + i, ".__send__(:", 11) == 0) {
      REWRITE_SEND_CALL(".__send__(:", 11, 0);
      continue;
    }
    /* .__send__("foo", args) → .foo(args). String-form variant. */
    if (i + 11 < len && strncmp(source + i, ".__send__(\"", 11) == 0) {
      REWRITE_SEND_CALL(".__send__(\"", 11, 1);
      continue;
    }
    /* .public_send(:foo, args) → .foo(args). Visibility isn't modeled
       in Spinel's static dispatch -- public_send is semantically
       identical to send. Issue #735. */
    if (i + 14 < len && strncmp(source + i, ".public_send(:", 14) == 0) {
      REWRITE_SEND_CALL(".public_send(:", 14, 0);
      continue;
    }
    if (i + 14 < len && strncmp(source + i, ".public_send(\"", 14) == 0) {
      REWRITE_SEND_CALL(".public_send(\"", 14, 1);
      continue;
    }
    /* `&:symbol` symbol-to-proc. Prism parses `&:sym` natively, but
       Spinel only lowers the native BlockArgumentNode+SymbolNode shape
       for a few methods (inject / reduce / sort_by); general block
       takers rely on this textual rewrite to an explicit block. The
       block has to land where Ruby reads it as a block, not a hash:
         - sole parenthesized arg `m(&:s)`   -> `m { |_spx| _spx.s }`
         - after a positional, paren call
           `m(a, &:s)`                       -> `m(a) { |_spx| _spx.s }`
         - after a positional, command call
           `m a, &:s`                        -> `m a do |_spx| _spx.s end`
         - command-position sole arg `m &:s` -> `m  { |_spx| _spx.s }`
       The bare `{ |_spx| ... }` was only correct in block position; a
       `,`-prefixed brace parses as a hash literal, which was the bundler
       (`File.open(f, "r:UTF-8", &:read)`) / minitest
       (`define_method :mu_pp, &:pretty_inspect`) parse failure.

       Operator symbols (`&:+`) have name_len 0 here and are left for
       Prism's native path (the arith reduce/inject lowering handles
       them). Known limitation: a paren-less command carrying a
       symbol-proc that is itself nested as an argument inside a paren
       call -- `p(foo :a, &:s)` -- mis-binds the block to the outer
       call, because textually its args sit at the same paren depth as
       the enclosing call (indistinguishable without re-implementing
       Ruby's command/arg disambiguation). Neither target gem hits
       this; a robust fix is an AST-level lowering, deferred. */
    if (i + 2 < len && source[i] == '&' && source[i + 1] == ':') {
      /* Last emitted non-space char. Skip newlines too so a multi-line
         `m(a,\n  &:s)` still sees the comma. */
      size_t back = oi;
      while (back > 0 && (out[back - 1] == ' ' || out[back - 1] == '\t' ||
                          out[back - 1] == '\n' || out[back - 1] == '\r')) back--;
      char prev = (back > 0) ? out[back - 1] : '\0';
      i += 2;
      size_t ns = i;
      while (i < len && (source[i] == '_' || (source[i] >= 'a' && source[i] <= 'z') ||
             (source[i] >= 'A' && source[i] <= 'Z') || (source[i] >= '0' && source[i] <= '9') ||
             source[i] == '?' || source[i] == '!')) i++;
      size_t name_len = i - ns;
      if (name_len == 0) {
        /* Operator / empty symbol: leave `&:` for Prism's native path. */
        OUT_STR("&:");
        continue;
      }
      if (prev == '(') {
        /* Sole parenthesized arg: drop the `(` (and any trailing ws)
           and the matching `)` so the block binds paren-less. #792:
           tolerate whitespace before the `)`. Restore `i` if no `)`
           follows so trailing content isn't swallowed. */
        oi = back - 1;
        size_t after_sym = i;
        while (i < len && (source[i] == ' ' || source[i] == '\t' ||
                           source[i] == '\n' || source[i] == '\r')) i++;
        if (i < len && source[i] == ')') i++;
        else i = after_sym;
        OUT_STR(" { |_spx| _spx.");
        { size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]); }
        OUT_STR(" }");
        continue;
      }
      if (prev == ',') {
        /* After a positional arg. Drop the separating comma and
           relocate the block: a parenthesized call closes first then
           takes a brace block; a command call takes a trailing do..end
           (a brace would still read as a hash). Only consume up to the
           `)`; otherwise restore `i` so the newline/`end` that follows
           a paren-less command is preserved. */
        oi = back - 1;
        size_t after_sym = i;
        while (i < len && (source[i] == ' ' || source[i] == '\t' ||
                           source[i] == '\n' || source[i] == '\r')) i++;
        if (i < len && source[i] == ')') {
          i++;
          OUT_CHAR(')');
          OUT_STR(" { |_spx| _spx.");
          { size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]); }
          OUT_STR(" }");
        }
else {
          i = after_sym;
          OUT_STR(" do |_spx| _spx.");
          { size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]); }
          OUT_STR(" end");
        }
        continue;
      }
      /* Command-position sole arg (`m &:s`): the brace binds as a
         block (no comma precedes it). */
      OUT_STR(" { |_spx| _spx.");
      { size_t k; for (k = 0; k < name_len; k++) OUT_CHAR(source[ns + k]); }
      OUT_STR(" }");
      continue;
    }
    OUT_CHAR(source[i]);
    i++;
  }
  out[oi] = '\0';
  free(source);
  return out;
  #undef OUT_CHAR
  #undef OUT_STR
}

/* ---- Main ---- */
/* Parse `source_file` and append the text AST to `out`. `argv0` is the
   invoking program path (used to locate the stdlib for plain `require`s).
   Returns 0 on success, 1 on read/parse error. This is the library copy;
   the standalone CLI's FILE*-streaming variant lives in
   legacy/spinel_parse.c. */
static int sp_parse_emit(const char *source_file, const char *argv0, SpStrBuf *out) {
  char *source = read_file(source_file);
  if (!source) {
    fprintf(stderr, "spinel_parse: cannot open '%s'\n", source_file);
    return 1;
  }

  /* Set the debug flag before resolving requires so the resolvers insert the
     PUSH/POP markers used to rebuild the multi-file source map. */
  {
    const char *dbg = getenv("SPINEL_DEBUG");
    const char *lm = getenv("SPINEL_LINE_MAP");
    int on = (dbg != NULL && dbg[0] == '1' && dbg[1] == '\0')
          || (lm  != NULL && lm[0]  == '1' && lm[1]  == '\0');
    g_emit_line = on ? 1 : 0;
  }

  /* Resolve require_relative and plain require */
  /* Register the entry file itself as already-included, so a circular
     require_relative pointing back at it resolves to the dedup stub
     instead of splicing the entry's body a second time (#1373). */
  {
    char *entry_canon = sp_canonical_path(source_file);
    sp_mark_path_included(entry_canon);
    free(entry_canon);
  }
  char *resolved = resolve_requires(source, source_file);
  free(source);
  source = resolve_plain_requires(resolved, argv0);

  /* Debug: build the buffer-line -> (file, original line) map from the
     marker-annotated buffer *before* syntax-sugar rewriting (which could
     touch a marker line's text). Sugar preserves line count, so the map
     stays aligned with the parsed node lines; if that ever fails to hold,
     disable the map rather than emit wrong attributions. */
  char *premap = NULL;
  if (g_emit_line) {
    premap = strdup(source);
    if (!premap) { fprintf(stderr, "spinel_parse: out of memory\n"); exit(1); }
  }
  source = rewrite_syntax_sugar(source);
  if (g_emit_line) {
    size_t la = 1, lb = 1;
    for (const char *p = premap; *p; p++) if (*p == '\n') la++;
    for (const char *p = source; *p; p++) if (*p == '\n') lb++;
    if (la == lb) {
      sp_build_line_map(premap, source_file);
    }
else {
      /* Multi-file line attribution unavailable for this program; #line
         falls back to buffer lines. Only worth a word under an explicit
         --debug build (faithful stepping matters there); stay silent for
         the default line-map so normal builds aren't noisy. */
      const char *dbg = getenv("SPINEL_DEBUG");
      if (dbg != NULL && dbg[0] == '1' && dbg[1] == '\0') {
        fprintf(stderr, "spinel_parse: multi-file line map disabled "
                        "(syntax-sugar changed line count)\n");
      }
    }
    free(premap);
  }

  size_t source_len = strlen(source);

  /* Parse with Prism */
  pm_parser_t parser;
  pm_parser_init(&parser, (const uint8_t *)source, source_len, NULL);
  pm_node_t *root = pm_parse(&parser);

  if (parser.error_list.size > 0) {
    fprintf(stderr, "Parse errors in '%s':\n", source_file);
    pm_diagnostic_t *diag;
    for (diag = (pm_diagnostic_t *)parser.error_list.head; diag; diag = (pm_diagnostic_t *)diag->node.next) {
      fprintf(stderr, "  %s\n", diag->message);
    }
    /* Issue #764: free the registered include paths on the parse-
       error exit path too. */
    pm_node_destroy(&parser, root);
    pm_parser_free(&parser);
    free(source);
    sp_includes_free();
    return 1;
  }

  g_parser = &parser;
  g_source_file = source_file;
  g_source_file_escaped = escape_str((const uint8_t *)g_source_file, strlen(g_source_file));

  /* Flatten AST to text */
  lines = NULL;
  line_count = 0;
  line_cap = 0;
  node_counter = 0;

  int root_id = flatten(root);

  /* Output */
  sb_printf(out, "ROOT %d\n", root_id);
  /* Issue #878: emit the source file path as a top-level fact so
     `__dir__` and similar compile-time helpers can recover it
     even when the source contains no `__FILE__` reference. The
     loader stashes it in @source_file_path. */
  sb_printf(out, "SOURCE_FILE %s\n", g_source_file_escaped);
  /* Debug multi-file map: emit the id -> path table so codegen can resolve
     each node's `node_file` id to a path for its `#line` directive. */
  for (int i = 0; i < sp_file_count; i++) {
    char *esc = escape_str((const uint8_t *)sp_file_table[i], strlen(sp_file_table[i]));
    sb_printf(out, "FILE %d %s\n", i, esc);
    free(esc);
  }
  for (size_t i = 0; i < line_count; i++) {
    sb_puts(out, lines[i]);
    sb_puts(out, "\n");
    free(lines[i]);
  }
  free(lines);

  pm_node_destroy(&parser, root);
  pm_parser_free(&parser);
  free(source);
  free(g_source_file_escaped);  /* paired with the escape_str() in init */
  g_source_file_escaped = NULL;
  sp_includes_free();
  return 0;
}

/* This is the library copy of the Prism front-end, linked into the C
   compiler (build/spinel). The standalone CLI `main` lives in the legacy
   copy (legacy/spinel_parse.c); here only the in-process lib API is kept. */

/* In-process entry for the single-binary C compiler: parse `source_file`
   and return the text AST as a malloc'd NUL-terminated buffer (caller
   frees), or NULL on error. `argv0` locates the stdlib for plain requires.
   Avoids any on-disk intermediate by writing to an in-memory stream. */
char *sp_parse_file_to_text(const char *source_file, const char *argv0) {
  SpStrBuf out = {0};
  int rc = sp_parse_emit(source_file, argv0, &out);
  if (rc != 0) {
    free(out.data);
    return NULL;
  }
  if (!out.data) {
    /* Success with no bytes emitted: hand back a valid empty string. */
    out.data = (char *)malloc(1);
    if (out.data) out.data[0] = '\0';
  }
  return out.data;
}
