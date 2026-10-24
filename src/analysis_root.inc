/* M2: no AST calls are inserted; an external request seeds a normal scope. */
FactsAnalysisRoot sp_facts_roots[FACTS_MAX_ROOTS];
int sp_facts_root_count=0;
static const char *facts_root_specs[FACTS_MAX_ROOTS];
static int facts_root_spec_count=0;
int facts_queue_root_request(const char *spec) {
  if (facts_root_spec_count==FACTS_MAX_ROOTS) {
    fprintf(stderr,"spinel: analysis root limit is 16 requests\n"); return 0;
  }
  facts_root_specs[facts_root_spec_count++]=spec;
  return 1;
}

static int facts_parse_one_root(const char *spec,FactsAnalysisRoot *r) {
  if (!spec || strlen(spec)>4096) return 0;
  const char *colon=strchr(spec,':');
  if (!colon || colon==spec || (size_t)(colon-spec)>127) return 0;
  for (const char *p=spec;p<colon;++p)
    if (!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || *p=='_' ||
          (p>spec && *p>='0' && *p<='9'))) return 0;
  r->scope=-1; r->def_node=-1;
  r->method=strndup(spec,(size_t)(colon-spec));
  r->requested=calloc(16,sizeof(char*));
  r->types=calloc(16,sizeof(TyKind));
  if (!r->method || !r->requested || !r->types) return 0;
  const char *p=colon+1;
  while (*p) {
    const char *end=strchr(p,','); if (!end) end=p+strlen(p);
    if (end==p || (size_t)(end-p)>255 || r->argc==16) return 0;
    for (const char *q=p;q<end;++q)
      if (!((*q>='a' && *q<='z') || (*q>='A' && *q<='Z') ||
            (*q>='0' && *q<='9') || *q=='_' || *q==':')) return 0;
    r->requested[r->argc]=strndup(p,(size_t)(end-p));
    if (!r->requested[r->argc++]) return 0;
    if (!*end) break;
    p=end+1; if (!*p) return 0;
  }
  return 1;
}
int facts_parse_root_request(const char *spec) {
  (void)spec;
  for (int i=0;i<facts_root_spec_count;++i) {
    FactsAnalysisRoot *r=&sp_facts_roots[i];
    if (!facts_parse_one_root(facts_root_specs[i],r)) return 0;
    for (int j=0;j<i;++j) if (!strcmp(r->method,sp_facts_roots[j].method)) {
      fprintf(stderr,"spinel: duplicate analysis root method: %s\n",r->method); return 0;
    }
    ++sp_facts_root_count;
  }
  return sp_facts_root_count>0;
}
static void facts_root_fail(Compiler *c,int id,const char *message) {
  char error[512]; snprintf(error,sizeof error,"analysis root: %s",message);
  unsupported_feature(c,id,error);
}

extern NodeTable *sp_emit_mlir_pristine;
/* Read original declarations, not the RBS matcher's leaf/_ aliases. */
static int facts_path_append(char *out,size_t cap,const char *s) {
  size_t a=strlen(out),b=strlen(s);
  if (a+b+1>cap) return 0;
  memcpy(out+a,s,b+1); return 1;
}
static int facts_constant_path(const NodeTable *nt,int node,char *out,size_t cap,int depth) {
  if (node<0 || depth>64) return 0;
  const char *kind=nt_type(nt,node),*name=nt_str(nt,node,"name");
  if (!kind || !name) return 0;
  if (!strcmp(kind,"ConstantPathNode")) {
    int parent=nt_ref(nt,node,"parent");
    if (parent>=0) {
      if (!facts_constant_path(nt,parent,out,cap,depth+1) || !facts_path_append(out,cap,"::")) return 0;
    } else out[0]=0;
  } else if (strcmp(kind,"ConstantReadNode")) return 0;
  return facts_path_append(out,cap,name);
}
static int facts_same_declaration(const NodeTable *a,int ai,const NodeTable *b,int bi) {
  if (ai<0 || bi<0 || !nt_type(a,ai) || !nt_type(b,bi) ||
      strcmp(nt_type(a,ai),nt_type(b,bi))) return 0;
  const char *af=nt_file_path(a,nt_int(a,ai,"node_file",-1));
  const char *bf=nt_file_path(b,nt_int(b,bi,"node_file",-1));
  if (!af || !bf || strcmp(af,bf)) return 0;
  const char *keys[]={"node_line","node_col","node_end_line","node_end_col"};
  for (int k=0;k<4;++k) {
    long long x=nt_int(a,ai,keys[k],-1),y=nt_int(b,bi,keys[k],-1);
    if (x<0 || x!=y || ((k==0 || k==2) && x==0)) return 0;
  }
  return 1;
}
static int facts_find_class_path(Compiler *c,int target,const NodeTable *nt,int id,
                                 const char *prefix,char *out,size_t cap,
                                 unsigned char *visited,int depth) {
  if (id<0 || id>=nt->count || visited[id]) return 0;
  if (depth>128) return -1;
  visited[id]=1;
  const SpNode *n=&nt->nodes[id];
  if (!n->type || !strcmp(n->type,"DefNode") || !strcmp(n->type,"SingletonClassNode")) return 0;
  char path[512];
  if (strlen(prefix)>=sizeof path) return -1;
  strcpy(path,prefix);
  int declaration=!strcmp(n->type,"ClassNode") || !strcmp(n->type,"ModuleNode");
  int matches=0;
  if (declaration) {
    if (*path && !facts_path_append(path,sizeof path,"::")) return -1;
    if (!facts_constant_path(nt,nt_ref(nt,id,"constant_path"),path,sizeof path,0)) return -1;
    if (facts_same_declaration(nt,id,c->nt,target)) {
      if (strlen(path)>=cap) return -1;
      strcpy(out,path); ++matches;
    }
  }
  for (int i=0;i<n->nr;++i) {
    int count=facts_find_class_path(c,target,nt,n->r[i].ref,path,out,cap,visited,depth+1);
    if (count<0) return -1;
    matches+=count;
  }
  for (int i=0;i<n->na;++i) for (int j=0;j<n->a[i].n;++j) {
    int count=facts_find_class_path(c,target,nt,n->a[i].ids[j],path,out,cap,visited,depth+1);
    if (count<0) return -1;
    matches+=count;
  }
  return matches;
}
static int facts_class_path(Compiler *c,int cid,char *out,size_t cap) {
  const NodeTable *nt=sp_emit_mlir_pristine;
  if (!nt || cid<0 || cid>=c->nclasses || c->classes[cid].def_node<0) return 0;
  unsigned char *visited=calloc((size_t)nt->count,1);
  if (!visited) facts_root_fail(c,-1,"class declaration inventory allocation failed");
  int count=facts_find_class_path(c,c->classes[cid].def_node,nt,nt->root_id,"",out,cap,visited,0);
  free(visited);
  if (count<0) facts_root_fail(c,-1,"class declaration inventory exceeds supported bounds");
  if (count>1) facts_root_fail(c,-1,"ambiguous class declaration identity");
  return count==1;
}
static TyKind facts_requested_type(Compiler *c,const char *token,int node) {
  if (!strcmp(token,"int")) return TY_INT;
  if (!strcmp(token,"float")) return TY_FLOAT;
  if (!strcmp(token,"string")) return TY_STRING;
  if (!strcmp(token,"bool")) return TY_BOOL;
  if (!strcmp(token,"symbol")) return TY_SYMBOL;
  if (strncmp(token,"obj_",4)) { facts_root_fail(c,node,"unknown argument type"); return TY_UNKNOWN; }
  const char *name=token+4;
  int found=-1,matches=0,leaf_matches=0;
  for (int i=0;i<c->nclasses;++i) {
    char path[512]={0};
    if (!facts_class_path(c,i,path,sizeof path)) continue;
    if (!strcmp(path,name)) { found=i; ++matches; }
    const char *leaf=strrchr(path,':'); leaf=leaf?leaf+1:path;
    if (!strcmp(leaf,name)) ++leaf_matches;
  }
  if (matches>1 || (!matches && leaf_matches>1)) facts_root_fail(c,node,"ambiguous argument class; use a fully qualified name");
  if (matches!=1) facts_root_fail(c,node,"unknown argument class");
  return ty_object(found);
}
static int facts_count_pristine_defs(const NodeTable *nt,int id,const char *name,
                                    unsigned char *visited) {
  if (id<0 || id>=nt->count || visited[id]) return 0;
  visited[id]=1;
  const SpNode *node=&nt->nodes[id];
  const char *kind=node->type;
  if (!kind) return 0;
  if (!strcmp(kind,"DefNode")) {
    const char *actual=nt_str(nt,id,"name");
    return nt_ref(nt,id,"receiver")<0 && actual && !strcmp(actual,name);
  }
  if (!strcmp(kind,"ClassNode") || !strcmp(kind,"ModuleNode") ||
      !strcmp(kind,"SingletonClassNode")) return 0;
  int count=0;
  for (int i=0;i<node->nr;++i)
    count+=facts_count_pristine_defs(nt,node->r[i].ref,name,visited);
  for (int i=0;i<node->na;++i)
    for (int j=0;j<node->a[i].n;++j)
      count+=facts_count_pristine_defs(nt,node->a[i].ids[j],name,visited);
  return count;
}
static void facts_seed_one_root(Compiler *c,FactsAnalysisRoot *r) {
  const NodeTable *pristine=sp_emit_mlir_pristine;
  if (!pristine) facts_root_fail(c,-1,"missing pristine snapshot");
  unsigned char *visited=calloc((size_t)pristine->count,1);
  if (!visited) facts_root_fail(c,-1,"candidate inventory allocation failed");
  int candidates=facts_count_pristine_defs(pristine,pristine->root_id,r->method,visited);
  free(visited);
  if (!candidates) facts_root_fail(c,-1,"unknown top-level method");
  if (candidates!=1) facts_root_fail(c,-1,"ambiguous top-level method");
  int found=-1,matches=0;
  /* Count actual top-level definitions, including shadowed declarations. */
  for (int id=0;id<c->nt->count;++id) {
    const char *kind=nt_type(c->nt,id),*name=nt_str(c->nt,id,"name");
    if (!kind || strcmp(kind,"DefNode") || !name || strcmp(name,r->method)) continue;
    for (int si=1;si<c->nscopes;++si) {
      Scope *s=&c->scopes[si];
      if (s->def_node==id && s->class_id<0 && !s->is_cmethod && nt_ref(c->nt,id,"receiver")<0) {
        found=si; ++matches; break;
      }
    }
  }
  if (!matches) facts_root_fail(c,-1,"unknown top-level method");
  if (matches!=1) facts_root_fail(c,-1,"ambiguous top-level method");
  for (int i=0;i<sp_facts_root_count;++i)
    if (&sp_facts_roots[i]!=r && sp_facts_roots[i].seeded && sp_facts_roots[i].scope==found)
      facts_root_fail(c,-1,"duplicate resolved analysis root definition");
  Scope *s=&c->scopes[found];
  int pn=nt_ref(c->nt,s->def_node,"parameters");
  int count=0;
  nt_arr(c->nt,pn,"optionals",&count);
  int keywords=0; nt_arr(c->nt,pn,"keywords",&keywords);
  if (s->rest_idx>=0 || s->kwrest_idx>=0 || s->blk_param || count || keywords ||
      nt_ref(c->nt,pn,"rest")>=0 || nt_ref(c->nt,pn,"keyword_rest")>=0 || nt_ref(c->nt,pn,"block")>=0)
    facts_root_fail(c,s->def_node,"requires fixed positional signature");
  if (s->nparams!=r->argc) facts_root_fail(c,s->def_node,"argument arity mismatch");
  for (int i=0;i<s->nparams;++i) {
    if (s->pdefault && s->pdefault[i]>=0) facts_root_fail(c,s->def_node,"requires fixed positional signature");
    r->types[i]=facts_requested_type(c,r->requested[i],s->def_node);
    LocalVar *lv=scope_local(s,s->pnames[i]);
    if (!lv) facts_root_fail(c,s->def_node,"missing parameter slot");
    lv->type=r->types[i]; lv->rbs_seeded=1; lv->rbs_type=r->types[i];
  }
  r->scope=found; r->def_node=s->def_node; r->seeded=1;
}
void facts_seed_root(Compiler *c) {
  if (!sp_analysis_root_spec) return;
  for (int i=0;i<sp_facts_root_count;++i) facts_seed_one_root(c,&sp_facts_roots[i]);
}
int facts_scope_is_root(int scope) {
  if (!sp_analysis_root_spec) return 0;
  for (int i=0;i<sp_facts_root_count;++i)
    if (sp_facts_roots[i].seeded && sp_facts_roots[i].scope==scope) return 1;
  return 0;
}
void facts_root_census(Compiler *c) {
  if (!sp_analysis_root_spec) return;
  for (int r=0;r<sp_facts_root_count;++r) {
    if (!sp_facts_roots[r].seeded) continue;
    for (int i=0;i<sp_facts_roots[r].argc;++i) {
      TyKind t=sp_facts_roots[r].types[i];
      if (ty_is_object(t)) c->classes[ty_object_class(t)].instantiated=1;
    }
  }
}
