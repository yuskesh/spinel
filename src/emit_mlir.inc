#include "analysis_root.h"
/* M1: generic MLIR transport, included after existing metadata helpers. */
#include <fcntl.h>
#include <unistd.h>
#include "typing_probe.h"
extern int sp_emit_mlir_enabled;
extern const char *sp_emit_mlir_overflow;
extern const char *sp_emit_mlir_path;
extern NodeTable *sp_emit_mlir_pristine;
extern int sp_target_int_bits;
typedef struct { char *bytes; size_t len; } MlirSource;
static int mlir_emitted;
static char *mlir_temp_path;
static int mlir_owns_temp;
static int mlir_emit(Compiler *c);
static void mlir_on_exit(void) {
  if (!mlir_emitted && g_ndiags>0) (void)mlir_emit(NULL);
  if (mlir_owns_temp) { unlink(mlir_temp_path); mlir_owns_temp=0; }
}

static int mlir_write_module(const char *text) {
  size_t size=strlen(sp_emit_mlir_path)+64;
  mlir_temp_path=malloc(size);
  if (!mlir_temp_path) return 0;
  snprintf(mlir_temp_path,size,"%s.tmp.%ld",sp_emit_mlir_path,(long)getpid());
  int fd=open(mlir_temp_path,O_WRONLY|O_CREAT|O_EXCL,0600);
  if (fd<0) return 0;
  mlir_owns_temp=1;
  FILE *f=fdopen(fd,"wb");
  if (!f) { close(fd); return 0; }
  size_t n=strlen(text);
  int ok=fwrite(text,1,n,f)==n;
  if (fflush(f)) ok=0;
  if (ok && fsync(fd)) ok=0;
  if (fclose(f)) ok=0;
  if (ok && rename(mlir_temp_path,sp_emit_mlir_path)) ok=0;
  if (ok) mlir_owns_temp=0;
  return ok;
}

static int mlir_source_references(const NodeTable *nt,int fid) {
  int count=0;
  for (int id=0;id<nt->count;++id)
    if (nt_type(nt,id) && nt_int(nt,id,"node_line",0)>0 && nt_int(nt,id,"node_file",-1)==fid) ++count;
  return count;
}

/* MLIR StringAttr byte escapes, including embedded NUL. */
static void mlir_string(Buf *b, const char *s, size_t n) {
  buf_puts(b, "\"");
  for (size_t i=0; i<n; ++i) {
    unsigned char ch=(unsigned char)s[i];
    if (ch<32 || ch>=127 || ch=='"' || ch=='\\') buf_printf(b,"\\%02X",(unsigned)ch);
    else buf_printf(b,"%c",ch);
  }
  buf_puts(b,"\"");
}
static void mlir_cstr(Buf *b,const char *s) { mlir_string(b,s?s:"",s?strlen(s):0); }
static void mlir_i64(Buf *b,long long n) { buf_printf(b,"%lld : i64",n); }
static void mlir_key(Buf *b,const char *s) { mlir_cstr(b,s); buf_puts(b," = "); }
static int mlir_read_source(const char *path,MlirSource *out) {
  FILE *f=fopen(path,"rb"); if (!f) return 0;
  Buf b={0}; unsigned char block[4096]; size_t n,total=0;
  while ((n=fread(block,1,sizeof block,f))) {
    for (size_t i=0;i<n;++i) buf_printf(&b,"%c",block[i]);
    total+=n;
  }
  int bad=ferror(f); if (fclose(f)) bad=1;
  if (bad) { free(b.p); return 0; }
  out->bytes=b.p?b.p:strdup(""); out->len=total; return 1;
}
static int mlir_offset(const MlirSource *s,int line,int col,size_t *at) {
  if (line<1 || col<0) return 0;
  size_t p=0;
  for (int l=1;l<line;++l) {
    while (p<s->len && s->bytes[p]!='\n') ++p;
    if (p==s->len) return 0; ++p;
  }
  for (int c=0;c<col;++c) {
    if (p>=s->len || s->bytes[p]=='\n') return 0; ++p;
  }
  *at=p; return 1;
}
static int mlir_table(Buf *b,const NodeTable *nt,const char *phase,MlirSource *sources,int source_count) {
  buf_puts(b,"  \"spinel_facts.table\"() ({\n");
  for (int id=0;id<nt->count;++id) {
    const SpNode *n=&nt->nodes[id]; if (!n->type) continue;
    int line=nt_int(nt,id,"node_line",0), col=nt_int(nt,id,"node_col",0);
    int el=nt_int(nt,id,"node_end_line",0), ec=nt_int(nt,id,"node_end_col",0);
    int fid=nt_int(nt,id,"node_file",-1);
    int located=line>0 && el>0 && fid>=0 && fid<source_count;
    buf_puts(b,"    \"spinel_facts.node\"() {id = "); mlir_i64(b,id);
    buf_puts(b,", kind = "); mlir_cstr(b,n->type);
    buf_puts(b,", strings = {");
    for (int j=0;j<n->ns;++j) {
      if (j) buf_puts(b,", "); mlir_key(b,n->s[j].key); mlir_string(b,n->s[j].val,n->s[j].val_len);
    }
    buf_puts(b,"}, integers = {");
    for (int j=0;j<n->ni;++j) {
      if (j) buf_puts(b,", "); mlir_key(b,n->i[j].key);
      char dec[64]; snprintf(dec,sizeof dec,"%lld",n->i[j].val); mlir_cstr(b,dec);
    }
    buf_puts(b,"}, refs = {");
    for (int j=0;j<n->nr;++j) {
      if (j) buf_puts(b,", "); mlir_key(b,n->r[j].key); mlir_i64(b,n->r[j].ref);
    }
    buf_puts(b,"}, arrays = {");
    for (int j=0;j<n->na;++j) {
      if (j) buf_puts(b,", "); mlir_key(b,n->a[j].key); buf_puts(b,"[");
      for (int k=0;k<n->a[j].n;++k) { if (k) buf_puts(b,", "); mlir_i64(b,n->a[j].ids[k]); }
      buf_puts(b,"]");
    }
    buf_puts(b,"}, file_id = "); mlir_i64(b,located?fid:-1); buf_puts(b,", span = [");
    if (located) {
      mlir_i64(b,line); buf_puts(b,", "); mlir_i64(b,col);
      buf_puts(b,", "); mlir_i64(b,el); buf_puts(b,", "); mlir_i64(b,ec);
    }
    buf_puts(b,"]");
    if (!strcmp(n->type,"IntegerNode")) {
      char backend[64]; snprintf(backend,sizeof backend,"%lld",nt_int(nt,id,"value",0));
      const char *bigval=nt_str(nt,id,"bigval");
      buf_puts(b,", literal_decimal = "); mlir_cstr(b,bigval?bigval:backend);
      buf_puts(b,", backend_decimal = "); mlir_cstr(b,backend);
      size_t start=0,end=0;
      if (located && mlir_offset(&sources[fid],line,col,&start) && mlir_offset(&sources[fid],el,ec,&end) && end>=start) {
        buf_puts(b,", literal_text = "); mlir_string(b,sources[fid].bytes+start,end-start);
      } else if (!strcmp(phase,"pristine")) {
        fprintf(stderr,"spinel: MLIR missing original Integer span node=%d file=%d\n",id,fid); return 0;
      }
    }
    if (n->content) { buf_puts(b,", floats = {content = "); mlir_cstr(b,n->content); buf_puts(b,"}"); }
    buf_puts(b,"} : () -> () loc(");
    if (located) { mlir_cstr(b,nt_file_path(nt,fid)); buf_printf(b,":%d:%d",line,col+1); }
    else buf_puts(b,"unknown");
    buf_puts(b,")\n");
  }
  buf_puts(b,"  }) {phase = "); mlir_cstr(b,phase);
  buf_puts(b,", root_id = "); mlir_i64(b,nt->root_id); buf_puts(b,"} : () -> ()\n"); return 1;
}
static void mlir_types(Buf *b,Compiler *c) {
  for (int id=0;id<c->nt->count && id<c->node_cap;++id) {
    const char *pk=nt_type(c->nt,id); if (!pk) continue;
    TyKind t=c->ntype[id];
    const char *pn=strstr(pk,"ParameterNode")?nt_str(c->nt,id,"name"):NULL;
    if (pn && c->nscope && c->nscope[id]>=0 && c->nscope[id]<c->nscopes) {
      LocalVar *pl=scope_local(&c->scopes[c->nscope[id]],pn); if (pl) t=pl->type;
    }
    Buf tag={0}; ty_tag_into(c,t,&tag);
    buf_puts(b,"  \"spinel_facts.type\"() {node_id = "); mlir_i64(b,id);
    buf_puts(b,", phase = \"analyzed\", kind = "); mlir_cstr(b,tag.p); free(tag.p);
    const char *vt=t==TY_UNKNOWN?"unknown":t==TY_VOID?"void":t==TY_NIL?"nil":
      t==TY_BOOL?"bool":(t==TY_INT || t==TY_BIGINT)?"integer":NULL;
    if (vt) buf_printf(b,", value_type = !spinel_facts.%s",vt);
    Buf rbs={0}; ty_to_rbs_into(c,t,&rbs); buf_puts(b,", rbs = "); mlir_cstr(b,rbs.p); free(rbs.p);
    if (!strcmp(pk,"DefNode")) for (int si=1;si<c->nscopes;++si) {
      Scope *s=&c->scopes[si]; if (s->def_node!=id) continue;
      Buf sig={0}; int degraded=rbs_method_type_into(c,&sig,s);
      buf_puts(b,", owner = "); mlir_cstr(b,s->class_id>=0?c->classes[s->class_id].name:"Object");
      buf_puts(b,", signature = "); mlir_cstr(b,sig.p); free(sig.p);
      buf_printf(b,", singleton = %s, widened = %s",s->is_cmethod?"true":"false",degraded?"true":"false"); break;
    }
    buf_puts(b,"} : () -> ()\n");
  }
}
static void mlir_evidence(Buf *b,Compiler *c) {
  for (int id=0;id<c->nt->count && id<c->node_cap;++id) {
    const char *kind=nt_type(c->nt,id); if (!kind) continue;
    int call=!strcmp(kind,"CallNode"),block=!strcmp(kind,"BlockNode");
    int d=id<g_ndecide_cap?g_ndecide[id]:ND_NONE;
    if ((!call && !block) || (call && d==ND_NONE)) continue;
    buf_puts(b,"  \"spinel_facts.evidence\"() {node_id = "); mlir_i64(b,id);
    buf_puts(b,", phase = \"analyzed\", kind = "); mlir_cstr(b,kind);
    if (call) {
      const char *name=nt_str(c->nt,id,"name"); if (name) { buf_puts(b,", name = "); mlir_cstr(b,name); }
      buf_puts(b,", dispatch = "); mlir_cstr(b,d==ND_SWITCH?"switch":d==ND_BOXED?"boxed":"direct");
      const char *tg=id<g_ndtarget_cap?g_ndtarget[id]:NULL;
      if (tg && *tg && d==ND_SWITCH) {
        buf_puts(b,", candidates = [");
        for (const char *p=tg;p;) {
          const char *q=strchr(p,','); if (p!=tg) buf_puts(b,", ");
          mlir_string(b,p,q?(size_t)(q-p):strlen(p)); p=q?q+1:NULL;
        }
        buf_puts(b,"]");
      } else if (tg && *tg && d==ND_DIRECT && !strchr(tg,',')) { buf_puts(b,", callee = "); mlir_cstr(b,tg); }
    } else buf_printf(b,", inlined = %s",d==ND_BLOCK_PROC?"false":"true");
    buf_puts(b,"} : () -> ()\n");
  }
}
#include "analysis_root_emit.inc"
#include "source_map_pin_emit.inc"
#include "typing_probe_emit.inc"
static int mlir_emit(Compiler *c) {
  if(sp_typing_probe_child)return probe_send_json(c);
  mlir_emitted=1; /* Includes writer failure: never publish twice from atexit. */
  Buf b={0}; int ok=1;
if (sp_analysis_root_spec) { mlir_root_header(&b,c); goto mlir_root_header_done; }
  buf_puts(&b,"module attributes {spinel_facts.version = 1 : i64, spinel_facts.producer = \"spinel\", "
    "spinel_facts.producer_revision = \"ad4bb8c4238ddeaa801dc434b84f0b5cfd865af1+M1\", "
    "spinel_facts.profile = \"spinel-codegen-v1\", spinel_facts.status = ");
  mlir_cstr(&b,g_ndiags?"rejected":"success");
  buf_printf(&b,", spinel_facts.integer_bits = %d : i64, spinel_facts.overflow = ",sp_target_int_bits);
  mlir_cstr(&b,sp_emit_mlir_overflow);
  buf_printf(&b,", spinel_facts.refusal_checked = true, spinel_facts.refusal_count = %d : i64} {\n",g_ndiags);
mlir_root_header_done: ;
if (!mlir_m3_check()) ok=0;
  if (g_ndiags) for (int i=0;i<g_ndiags;++i) {
    SpDiag *d=&g_diags[i];
    buf_puts(&b,"  \"spinel_facts.diagnostic\"() {severity = \"error\", file = "); mlir_cstr(&b,d->file);
    buf_printf(&b,", line = %d : i64, col = 0 : i64, message = ",d->line); mlir_cstr(&b,d->msg);
    buf_puts(&b,"} : () -> ()\n");
  } else {
    const NodeTable *pristine=sp_emit_mlir_pristine;
    int count=pristine->nfiles; MlirSource *sources=calloc((size_t)count,sizeof *sources);
    if (!sources && count) ok=0;
    for (int fid=0;fid<count;++fid)
      fprintf(stderr,"MLIR source-inventory file_id=%d pristine=%d analyzed=%d\n",fid,
        mlir_source_references(pristine,fid),mlir_source_references(c->nt,fid));
    for (int fid=0;ok && fid<count;++fid) {
      if (!mlir_source_references(pristine,fid) && !mlir_source_references(c->nt,fid)) continue;
      const char *path=nt_file_path(pristine,fid);
      if (!path || !mlir_read_source(path,&sources[fid])) {
        fprintf(stderr,"spinel: MLIR cannot read source file=%d path=%s\n",fid,path?path:"<missing>"); ok=0; break;
      }
      buf_puts(&b,"  \"spinel_facts.source\"() {file_id = "); mlir_i64(&b,fid);
      buf_puts(&b,", path = "); mlir_cstr(&b,path); buf_puts(&b,", text = ");
      mlir_string(&b,sources[fid].bytes,sources[fid].len); buf_puts(&b,"} : () -> ()\n");
    }
    if (ok) ok=mlir_table(&b,pristine,"pristine",sources,count);
    if (ok) ok=mlir_table(&b,c->nt,"analyzed",sources,count);
    if (ok) { mlir_types(&b,c); mlir_evidence(&b,c); }
    for (int fid=0;fid<count && sources;++fid) free(sources[fid].bytes);
    free(sources);
  }
  buf_puts(&b,"}\n");
  if (ok) ok=mlir_write_module(b.p);
  if (!ok && mlir_owns_temp) { unlink(mlir_temp_path); mlir_owns_temp=0; }
  free(b.p); if (!ok) fprintf(stderr,"spinel: MLIR export failed; no success artifact\n");
  return ok?0:1;
}
