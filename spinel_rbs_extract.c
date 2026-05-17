/* spinel_rbs_extract -- walk a directory for .rbs files and emit a
 * seed file for spinel_analyze's --rbs path.
 *
 * Reads RBS source through the vendored rbs C parser (vendor/rbs/) and
 * emits a tiny line-oriented seed format consumed by load_rbs_seeds /
 * apply_rbs_seeds in spinel_analyze.rb. Per the design conversation,
 * this is *advisory* seeding: only a subset of RBS maps to spinel's
 * type vocabulary, and anything outside the subset is silently
 * skipped. The analyzer's existing inference still runs on top.
 *
 * Subset supported (everything else dropped without warning):
 *   - Primitives: Integer, Float, String, Symbol, TrueClass,
 *     FalseClass, NilClass, bool, nil, void (→ nil for returns)
 *   - Nominal class instances → obj_<QualifiedName>
 *   - Array[T] where T is in subset → str_array / int_array /
 *     float_array / sym_array / obj_X_ptr_array / poly_array
 *   - Hash[K, V] → str_int_hash / sym_str_hash / str_poly_hash etc.
 *   - Optional T (T?) → <subset>?    (recursive)
 *   - Union T | nil → T?    (any other union → skip)
 *
 * Dropped: generics with variables, overloads beyond #1, blocks,
 * proc types, intersections, records, tuples, interfaces, literal
 * types, type aliases, self / instance / class / top / bottom /
 * any types.
 *
 * Usage:
 *   spinel_rbs_extract DIR [DIR ...]
 *
 * Walks each DIR recursively for *.rbs files, writes seed lines to
 * stdout. The `spinel` wrapper captures stdout to a tmpfile and
 * passes it as ARGV[2] to spinel_analyze. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "rbs/parser.h"
#include "rbs/ast.h"
#include "rbs/util/rbs_encoding.h"
#include "rbs/util/rbs_constant_pool.h"

/* ---- small string buffer ----------------------------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} sbuf_t;

static void sbuf_init(sbuf_t *s) { s->buf = NULL; s->len = 0; s->cap = 0; }
static void sbuf_free(sbuf_t *s) { free(s->buf); s->buf = NULL; s->len = 0; s->cap = 0; }

static void sbuf_grow(sbuf_t *s, size_t need) {
    if (s->cap >= need) return;
    size_t ncap = s->cap == 0 ? 64 : s->cap;
    while (ncap < need) ncap *= 2;
    char *nbuf = (char *) realloc(s->buf, ncap);
    if (nbuf == NULL) {
        fprintf(stderr, "spinel_rbs_extract: out of memory\n");
        exit(1);
    }
    s->buf = nbuf;
    s->cap = ncap;
}

static void sbuf_append(sbuf_t *s, const char *src, size_t n) {
    sbuf_grow(s, s->len + n + 1);
    memcpy(s->buf + s->len, src, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void sbuf_append_cstr(sbuf_t *s, const char *src) {
    sbuf_append(s, src, strlen(src));
}

static void sbuf_set(sbuf_t *s, const char *src, size_t n) {
    s->len = 0;
    sbuf_append(s, src, n);
}

/* ---- name lookup via rbs constant pool --------------------- */

/* Read the bytes for a constant_id out of the parser's pool into a sbuf.
 * Resets the buffer first. Result is NUL-terminated. */
static void name_of_symbol(rbs_parser_t *p, rbs_ast_symbol_t *sym, sbuf_t *out) {
    out->len = 0;
    if (sym == NULL) { sbuf_append(out, "", 0); return; }
    rbs_constant_t *c = rbs_constant_pool_id_to_constant(&p->constant_pool, sym->constant_id);
    if (c == NULL) { sbuf_append(out, "", 0); return; }
    sbuf_set(out, (const char *) c->start, c->length);
}

/* Qualified name from a type_name: namespace path joined with "_"
 * plus the leaf symbol. Spinel registers nested classes with "_"
 * separators (e.g. `module Tep; class Json` → `Tep_Json` in
 * @cls_names; `Tep::Security::Cors` → `Tep_Security_Cors`). RBS
 * spells them with `::`, so we translate here. */
static void name_of_type_name(rbs_parser_t *p, rbs_type_name_t *tn, sbuf_t *out) {
    out->len = 0;
    if (tn == NULL) return;
    if (tn->rbs_namespace != NULL && tn->rbs_namespace->path != NULL) {
        rbs_node_list_node_t *cur = tn->rbs_namespace->path->head;
        while (cur != NULL) {
            if (cur->node->type == RBS_AST_SYMBOL) {
                rbs_ast_symbol_t *seg = (rbs_ast_symbol_t *) cur->node;
                rbs_constant_t *c = rbs_constant_pool_id_to_constant(&p->constant_pool, seg->constant_id);
                if (c != NULL) {
                    sbuf_append(out, (const char *) c->start, c->length);
                    sbuf_append_cstr(out, "_");
                }
            }
            cur = cur->next;
        }
    }
    rbs_constant_t *c = rbs_constant_pool_id_to_constant(&p->constant_pool, tn->name->constant_id);
    if (c != NULL) {
        sbuf_append(out, (const char *) c->start, c->length);
    }
}

/* ---- subset type mapping ----------------------------------- */

/* Categorize a primitive name to a spinel scalar tag. NULL on
 * non-primitive (caller treats as nominal `obj_<Name>`). */
static const char *primitive_tag(const char *name, size_t len) {
    if (len == 7 && memcmp(name, "Integer", 7) == 0) return "int";
    if (len == 5 && memcmp(name, "Float", 5) == 0)   return "float";
    if (len == 6 && memcmp(name, "String", 6) == 0)  return "string";
    if (len == 6 && memcmp(name, "Symbol", 6) == 0)  return "symbol";
    if (len == 9 && memcmp(name, "TrueClass", 9) == 0)  return "bool";
    if (len == 10 && memcmp(name, "FalseClass", 10) == 0) return "bool";
    if (len == 8 && memcmp(name, "NilClass", 8) == 0)   return "nil";
    return NULL;
}

/* Map a typed element (already mapped to a spinel scalar/obj tag) into
 * the array variant name spinel uses. NULL if unsupported. */
static const char *array_tag_for_elem(const char *elem) {
    if (strcmp(elem, "int") == 0)    return "int_array";
    if (strcmp(elem, "float") == 0)  return "float_array";
    if (strcmp(elem, "string") == 0) return "str_array";
    if (strcmp(elem, "symbol") == 0) return "sym_array";
    if (strncmp(elem, "obj_", 4) == 0) {
        /* obj_Foo → obj_Foo_ptr_array (heuristic, mirrors spinel's
         * array-of-objects shape). Caller must append _ptr_array. */
        return NULL;
    }
    return "poly_array";
}

/* Map (K, V) of a Hash[K, V] to spinel's hash variant name. NULL on
 * unsupported combinations. */
static const char *hash_tag_for_kv(const char *k, const char *v) {
    if (strcmp(k, "string") == 0) {
        if (strcmp(v, "int") == 0)    return "str_int_hash";
        if (strcmp(v, "string") == 0) return "str_str_hash";
        return "str_poly_hash";
    }
    if (strcmp(k, "symbol") == 0) {
        if (strcmp(v, "int") == 0)    return "sym_int_hash";
        if (strcmp(v, "string") == 0) return "sym_str_hash";
        return "sym_poly_hash";
    }
    return NULL;
}

/* Forward decl. Write a spinel type tag into out; return true on
 * success, false to signal "out of subset, caller should skip".
 *
 * `enclosing_scope` (may be NULL/"") is the qualified name of the
 * declaration we're currently inside, e.g. `ActiveRecord` while
 * traversing members of `module ActiveRecord; class RecordInvalid`.
 * Used to resolve unqualified nominal type references: `Base` inside
 * `ActiveRecord` becomes `obj_ActiveRecord_Base`. Without this, the
 * extractor emits `obj_Base` and the seed misses any class actually
 * stored at the qualified name in spinel's tables. */
static bool map_type(rbs_parser_t *p, rbs_node_t *node,
                     const char *enclosing_scope, sbuf_t *out);

/* True if the type_name was written unqualified in source (no `::`
 * separators, no leading `::`). RBS exposes this as an empty
 * namespace path. */
static bool type_name_is_unqualified(rbs_type_name_t *tn) {
    if (tn == NULL || tn->rbs_namespace == NULL) return true;
    if (tn->rbs_namespace->path == NULL) return true;
    return tn->rbs_namespace->path->length == 0;
}

/* Map a ClassInstance node. Handles Array[T], Hash[K,V], primitives,
 * and nominal classes. */
static bool map_class_instance(rbs_parser_t *p, rbs_types_class_instance_t *ci,
                                const char *enclosing_scope, sbuf_t *out) {
    sbuf_t name;
    sbuf_init(&name);
    name_of_type_name(p, ci->name, &name);

    /* Primitive lookup uses the leaf name only. After name_of_type_name
     * the separator is "_" (translated from RBS's "::"), so the leaf
     * is the substring after the last "_". Conservative: only treat
     * the trailing segment as the primitive name; a class actually
     * named `Foo_Integer` would shadow the primitive, but that's
     * extremely unlikely in practice. */
    const char *primitive = NULL;
    {
        const char *leaf = name.buf;
        size_t leaf_len = name.len;
        for (size_t i = 0; i < name.len; i++) {
            if (name.buf[i] == '_') {
                leaf = name.buf + i + 1;
                leaf_len = name.len - (i + 1);
            }
        }
        primitive = primitive_tag(leaf, leaf_len);
    }

    size_t argc = ci->args != NULL ? ci->args->length : 0;

    if (primitive != NULL && argc == 0) {
        sbuf_set(out, primitive, strlen(primitive));
        sbuf_free(&name);
        return true;
    }

    /* Array[T] */
    if (name.len >= 5 && strcmp(name.buf + name.len - 5, "Array") == 0 && argc == 1) {
        sbuf_t elem;
        sbuf_init(&elem);
        if (!map_type(p, ci->args->head->node, enclosing_scope, &elem)) {
            sbuf_free(&elem);
            sbuf_free(&name);
            return false;
        }
        if (strncmp(elem.buf, "obj_", 4) == 0) {
            sbuf_set(out, elem.buf, elem.len);
            sbuf_append_cstr(out, "_ptr_array");
        } else {
            const char *tag = array_tag_for_elem(elem.buf);
            if (tag == NULL) {
                sbuf_free(&elem);
                sbuf_free(&name);
                return false;
            }
            sbuf_set(out, tag, strlen(tag));
        }
        sbuf_free(&elem);
        sbuf_free(&name);
        return true;
    }

    /* Hash[K, V] */
    if (name.len >= 4 && strcmp(name.buf + name.len - 4, "Hash") == 0 && argc == 2) {
        sbuf_t k, v;
        sbuf_init(&k); sbuf_init(&v);
        if (!map_type(p, ci->args->head->node, enclosing_scope, &k)
            || !map_type(p, ci->args->head->next->node, enclosing_scope, &v)) {
            sbuf_free(&k); sbuf_free(&v); sbuf_free(&name); return false;
        }
        const char *tag = hash_tag_for_kv(k.buf, v.buf);
        sbuf_free(&k); sbuf_free(&v);
        if (tag == NULL) { sbuf_free(&name); return false; }
        sbuf_set(out, tag, strlen(tag));
        sbuf_free(&name);
        return true;
    }

    /* Generic with arity > 0 of an unrecognized container → skip. */
    if (argc > 0) {
        sbuf_free(&name);
        return false;
    }

    /* Nominal class instance: emit obj_<QualifiedName>. If the source
     * wrote the name unqualified AND we're inside a class/module scope,
     * resolve relative to that scope so e.g. `Base` inside
     * `module ActiveRecord; class RecordInvalid` becomes
     * `obj_ActiveRecord_Base`. Heuristic: spinel will silently drop
     * seeds for types it doesn't recognize, so a wrong guess (e.g.
     * `StandardError` inside `ActiveRecord`) is a no-op rather than a
     * miscompile. Full lexical-scope walk would require a symbol
     * table; this single-level prefix covers the common case. */
    sbuf_set(out, "obj_", 4);
    if (enclosing_scope != NULL && enclosing_scope[0] != '\0'
        && type_name_is_unqualified(ci->name)) {
        sbuf_append_cstr(out, enclosing_scope);
        sbuf_append_cstr(out, "_");
    }
    sbuf_append(out, name.buf, name.len);
    sbuf_free(&name);
    return true;
}

static bool map_type(rbs_parser_t *p, rbs_node_t *node,
                     const char *enclosing_scope, sbuf_t *out) {
    if (node == NULL) return false;
    switch (node->type) {
        case RBS_TYPES_BASES_BOOL:
            sbuf_set(out, "bool", 4);
            return true;
        case RBS_TYPES_BASES_NIL:
            sbuf_set(out, "nil", 3);
            return true;
        case RBS_TYPES_BASES_VOID:
            /* void only appears as a return type; spinel uses "nil"
             * (which void-returning methods discard). */
            sbuf_set(out, "nil", 3);
            return true;
        case RBS_TYPES_CLASS_INSTANCE:
            return map_class_instance(p, (rbs_types_class_instance_t *) node, enclosing_scope, out);
        case RBS_TYPES_OPTIONAL: {
            rbs_types_optional_t *opt = (rbs_types_optional_t *) node;
            sbuf_t inner;
            sbuf_init(&inner);
            if (!map_type(p, opt->type, enclosing_scope, &inner)) { sbuf_free(&inner); return false; }
            /* Avoid double-?: nil? → just nil; obj_Foo?? → obj_Foo? */
            if (inner.len > 0 && inner.buf[inner.len - 1] == '?') {
                sbuf_set(out, inner.buf, inner.len);
            } else if (inner.len == 3 && memcmp(inner.buf, "nil", 3) == 0) {
                sbuf_set(out, "nil", 3);
            } else {
                sbuf_set(out, inner.buf, inner.len);
                sbuf_append_cstr(out, "?");
            }
            sbuf_free(&inner);
            return true;
        }
        case RBS_TYPES_UNION: {
            /* Only support `T | nil` shape (equivalent to T?). Any
             * other union → out-of-subset, skip. */
            rbs_types_union_t *u = (rbs_types_union_t *) node;
            if (u->types == NULL || u->types->length != 2) return false;
            rbs_node_t *a = u->types->head->node;
            rbs_node_t *b = u->types->head->next->node;
            rbs_node_t *t = NULL;
            if (a->type == RBS_TYPES_BASES_NIL) t = b;
            else if (b->type == RBS_TYPES_BASES_NIL) t = a;
            if (t == NULL) return false;
            sbuf_t inner;
            sbuf_init(&inner);
            if (!map_type(p, t, enclosing_scope, &inner)) { sbuf_free(&inner); return false; }
            if (inner.len > 0 && inner.buf[inner.len - 1] == '?') {
                sbuf_set(out, inner.buf, inner.len);
            } else {
                sbuf_set(out, inner.buf, inner.len);
                sbuf_append_cstr(out, "?");
            }
            sbuf_free(&inner);
            return true;
        }
        default:
            /* Out of subset: Self / Top / Bottom / Any / Instance /
             * Class / Block / Function / Interface / Intersection /
             * Literal / Proc / Record / RecordFieldType / Tuple /
             * UntypedFunction / Variable / Alias. */
            return false;
    }
}

/* ---- emit one method signature ----------------------------- */

/* Pick the seed-line keyword from rbs's method-definition kind.
 * `meth` lands in the instance table (@cls_meth_*), `cmeth` in the
 * class table (@cls_cmeth_*). SINGLETON_INSTANCE (def self?.foo)
 * emits both so seed_class_method on the analyzer side hits whichever
 * the source actually defined. */
static void emit_method(rbs_parser_t *p, rbs_ast_members_method_definition_t *m,
                        const char *enclosing_scope, FILE *out) {
    if (m->overloads == NULL || m->overloads->head == NULL) return;
    /* Only the first overload; multiple overloads are out of subset
     * (spinel can't pick between them deterministically). */
    rbs_node_t *first = m->overloads->head->node;
    if (first->type != RBS_AST_MEMBERS_METHOD_DEFINITION_OVERLOAD) return;
    rbs_ast_members_method_definition_overload_t *ov =
        (rbs_ast_members_method_definition_overload_t *) first;

    if (ov->method_type == NULL || ov->method_type->type != RBS_METHOD_TYPE) return;
    rbs_method_type_t *mt = (rbs_method_type_t *) ov->method_type;
    /* type_params on the method (generics) → can't be represented;
     * skip the signature rather than emit a misleading one. */
    if (mt->type_params != NULL && mt->type_params->length > 0) return;
    if (mt->type == NULL || mt->type->type != RBS_TYPES_FUNCTION) {
        /* untyped_function or proc → out of subset. */
        return;
    }
    rbs_types_function_t *fn = (rbs_types_function_t *) mt->type;

    /* Out-of-subset shapes: optional / rest / keyword params. Skip
     * the whole signature rather than emit a partial one. */
    if ((fn->optional_positionals != NULL && fn->optional_positionals->length > 0)
        || fn->rest_positionals != NULL
        || (fn->trailing_positionals != NULL && fn->trailing_positionals->length > 0)
        || (fn->required_keywords != NULL && fn->required_keywords->length > 0)
        || (fn->optional_keywords != NULL && fn->optional_keywords->length > 0)
        || fn->rest_keywords != NULL) {
        return;
    }

    /* Map every required positional. If any maps as out-of-subset,
     * skip the whole signature -- the analyzer can't act on a
     * partially-typed method (would need a sentinel for "leave
     * alone" per-param, which the seed format doesn't have today). */
    sbuf_t ptypes;
    sbuf_init(&ptypes);
    bool first_p = true;
    if (fn->required_positionals != NULL) {
        rbs_node_list_node_t *cur = fn->required_positionals->head;
        while (cur != NULL) {
            if (cur->node->type != RBS_TYPES_FUNCTION_PARAM) { sbuf_free(&ptypes); return; }
            rbs_types_function_param_t *pp = (rbs_types_function_param_t *) cur->node;
            sbuf_t t;
            sbuf_init(&t);
            if (!map_type(p, pp->type, enclosing_scope, &t)) { sbuf_free(&t); sbuf_free(&ptypes); return; }
            if (!first_p) sbuf_append_cstr(&ptypes, ",");
            sbuf_append(&ptypes, t.buf, t.len);
            first_p = false;
            sbuf_free(&t);
            cur = cur->next;
        }
    }
    if (ptypes.len == 0) sbuf_append_cstr(&ptypes, "-");

    sbuf_t ret;
    sbuf_init(&ret);
    if (!map_type(p, fn->return_type, enclosing_scope, &ret)) { sbuf_free(&ret); sbuf_free(&ptypes); return; }

    sbuf_t mname;
    sbuf_init(&mname);
    name_of_symbol(p, m->name, &mname);

    bool emit_inst = (m->kind == RBS_METHOD_DEFINITION_KIND_INSTANCE)
                  || (m->kind == RBS_METHOD_DEFINITION_KIND_SINGLETON_INSTANCE);
    bool emit_cls  = (m->kind == RBS_METHOD_DEFINITION_KIND_SINGLETON)
                  || (m->kind == RBS_METHOD_DEFINITION_KIND_SINGLETON_INSTANCE);

    if (emit_inst) fprintf(out, "meth %s %s %s\n",  mname.buf, ret.buf, ptypes.buf);
    if (emit_cls)  fprintf(out, "cmeth %s %s %s\n", mname.buf, ret.buf, ptypes.buf);

    sbuf_free(&mname);
    sbuf_free(&ret);
    sbuf_free(&ptypes);
}

/* ---- emit attr_accessor / reader / writer ------------------ */

static void emit_attr(rbs_parser_t *p, rbs_ast_symbol_t *name, rbs_node_t *type,
                      const char *enclosing_scope, FILE *out) {
    sbuf_t t;
    sbuf_init(&t);
    if (!map_type(p, type, enclosing_scope, &t)) { sbuf_free(&t); return; }
    sbuf_t n;
    sbuf_init(&n);
    name_of_symbol(p, name, &n);
    fprintf(out, "ivar %s %s\n", n.buf, t.buf);
    sbuf_free(&n);
    sbuf_free(&t);
}

/* ---- traverse class/module body ---------------------------- */

/* `qualified_scope` is this declaration's full path (used as the
 * `class X` line). `lookup_scope` is the *parent* path (used to
 * resolve unqualified type references inside the members). For
 * `module ActiveRecord; class RecordInvalid; def record: () -> Base`:
 * qualified_scope = "ActiveRecord_RecordInvalid",
 * lookup_scope    = "ActiveRecord".
 * That makes `Base` resolve to `obj_ActiveRecord_Base` — the sibling-
 * in-module pattern, which dominates real RBS in practice. Ruby's
 * actual constant lookup walks every enclosing scope outward; this
 * single-level fallback covers the common case without a symbol
 * table. */
static void traverse_members(rbs_parser_t *p, rbs_node_list_t *members,
                             const char *qualified_scope,
                             const char *lookup_scope, FILE *out) {
    if (members == NULL || members->length == 0) return;
    fprintf(out, "class %s\n", qualified_scope);
    rbs_node_list_node_t *cur = members->head;
    while (cur != NULL) {
        rbs_node_t *n = cur->node;
        switch (n->type) {
            case RBS_AST_MEMBERS_METHOD_DEFINITION:
                emit_method(p, (rbs_ast_members_method_definition_t *) n, lookup_scope, out);
                break;
            case RBS_AST_MEMBERS_ATTR_ACCESSOR: {
                rbs_ast_members_attr_accessor_t *a = (rbs_ast_members_attr_accessor_t *) n;
                emit_attr(p, a->name, a->type, lookup_scope, out);
                break;
            }
            case RBS_AST_MEMBERS_ATTR_READER: {
                rbs_ast_members_attr_reader_t *a = (rbs_ast_members_attr_reader_t *) n;
                emit_attr(p, a->name, a->type, lookup_scope, out);
                break;
            }
            case RBS_AST_MEMBERS_ATTR_WRITER: {
                rbs_ast_members_attr_writer_t *a = (rbs_ast_members_attr_writer_t *) n;
                emit_attr(p, a->name, a->type, lookup_scope, out);
                break;
            }
            default:
                /* Skip: include / extend / prepend / public / private
                 * / alias / instance_variable / class_variable. */
                break;
        }
        cur = cur->next;
    }
}

/* Recurse into a declaration. For Class/Module: extend the scope path
 * with the declaration's leaf name, emit members, then recurse into
 * any nested Class/Module members so e.g. `module Foo; class Bar; end;
 * end` produces a `class Foo::Bar` block. */
static void traverse_decl(rbs_parser_t *p, rbs_node_t *node,
                          const char *parent_scope, FILE *out) {
    sbuf_t leaf;
    sbuf_init(&leaf);
    rbs_node_list_t *members = NULL;

    if (node->type == RBS_AST_DECLARATIONS_CLASS) {
        rbs_ast_declarations_class_t *c = (rbs_ast_declarations_class_t *) node;
        name_of_type_name(p, c->name, &leaf);
        members = c->members;
    } else if (node->type == RBS_AST_DECLARATIONS_MODULE) {
        rbs_ast_declarations_module_t *m = (rbs_ast_declarations_module_t *) node;
        name_of_type_name(p, m->name, &leaf);
        members = m->members;
    } else {
        /* Skip Interface, Constant, Global, TypeAlias, ClassAlias,
         * ModuleAlias, Directives. */
        sbuf_free(&leaf);
        return;
    }

    /* qualified = parent_scope (if any) "_" + leaf -- spinel's
     * nested-class storage uses underscore as separator. */
    sbuf_t qualified;
    sbuf_init(&qualified);
    if (parent_scope != NULL && parent_scope[0] != '\0') {
        sbuf_append_cstr(&qualified, parent_scope);
        sbuf_append_cstr(&qualified, "_");
    }
    sbuf_append(&qualified, leaf.buf, leaf.len);

    traverse_members(p, members, qualified.buf, parent_scope, out);

    /* Recurse into nested declarations. */
    if (members != NULL) {
        rbs_node_list_node_t *cur = members->head;
        while (cur != NULL) {
            if (cur->node->type == RBS_AST_DECLARATIONS_CLASS
                || cur->node->type == RBS_AST_DECLARATIONS_MODULE) {
                traverse_decl(p, cur->node, qualified.buf, out);
            }
            cur = cur->next;
        }
    }

    sbuf_free(&qualified);
    sbuf_free(&leaf);
}

/* ---- per-file processing ----------------------------------- */

/* Read a file fully into a malloc'd buffer. Returns NULL on failure;
 * sets *len_out to the byte count on success. */
static char *slurp(const char *path, size_t *len_out) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = (char *) malloc((size_t) sz + 1);
    if (buf == NULL) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t) sz, fp);
    fclose(fp);
    buf[got] = '\0';
    *len_out = got;
    return buf;
}

static void process_file(const char *path, FILE *out) {
    size_t len = 0;
    char *src = slurp(path, &len);
    if (src == NULL) {
        fprintf(stderr, "spinel_rbs_extract: cannot read %s\n", path);
        return;
    }
    rbs_string_t str = rbs_string_new(src, src + len);
    rbs_parser_t *p = rbs_parser_new(str, RBS_ENCODING_UTF_8_ENTRY, 0, (int) len);
    if (p == NULL) { free(src); return; }
    rbs_signature_t *sig = NULL;
    bool ok = rbs_parse_signature(p, &sig);
    if (!ok || sig == NULL) {
        fprintf(stderr, "spinel_rbs_extract: parse failed in %s\n", path);
        if (p->error != NULL && p->error->message != NULL) {
            fprintf(stderr, "  %s\n", p->error->message);
        }
        rbs_parser_free(p);
        free(src);
        return;
    }
    rbs_node_list_node_t *cur = sig->declarations->head;
    while (cur != NULL) {
        traverse_decl(p, cur->node, "", out);
        cur = cur->next;
    }
    rbs_parser_free(p);
    free(src);
}

/* ---- directory walk ---------------------------------------- */

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), su = strlen(suffix);
    if (su > sl) return false;
    return strcmp(s + sl - su, suffix) == 0;
}

static void walk(const char *path, FILE *out) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "spinel_rbs_extract: %s: not found\n", path);
        return;
    }
    if (S_ISREG(st.st_mode)) {
        if (ends_with(path, ".rbs")) process_file(path, out);
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;
    DIR *d = opendir(path);
    if (d == NULL) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t) n >= sizeof(full)) {
            fprintf(stderr, "spinel_rbs_extract: path too long, skipping: %s/%s\n",
                    path, ent->d_name);
            continue;
        }
        walk(full, out);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: spinel_rbs_extract DIR [DIR ...]\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        walk(argv[i], stdout);
    }
    return 0;
}
