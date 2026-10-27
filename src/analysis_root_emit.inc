static void mlir_root_header(Buf *b,Compiler *c) {
  buf_puts(b,"module attributes {spinel_facts.version = 2 : i64, spinel_facts.producer = \"spinel\", "
    "spinel_facts.producer_revision = \"ad4bb8c4238ddeaa801dc434b84f0b5cfd865af1+M1+M2\", "
    "spinel_facts.profile = \"spinel-codegen-v2\", spinel_facts.status = ");
  mlir_cstr(b,g_ndiags?"rejected":"success");
  buf_printf(b,", spinel_facts.integer_bits = %d : i64, spinel_facts.overflow = ",sp_target_int_bits);
  mlir_cstr(b,sp_emit_mlir_overflow);
  buf_printf(b,", spinel_facts.refusal_checked = true, spinel_facts.refusal_count = %d : i64, "
    "spinel_facts.analysis_roots = [",g_ndiags);
  for (int root=0;root<sp_facts_root_count;++root) {
  FactsAnalysisRoot *r=&sp_facts_roots[root];
  if (root) buf_puts(b,", ");
  buf_puts(b,"{method = ");
  mlir_cstr(b,r->method);
  buf_puts(b,", requested_arg_types = [");
  for (int i=0;i<r->argc;++i) { if (i) buf_puts(b,", "); mlir_cstr(b,r->requested[i]); }
  buf_puts(b,"]");
  if (!g_ndiags) {
    buf_puts(b,", def_node_id = "); mlir_i64(b,r->def_node);
    buf_puts(b,", resolved_arg_types = [");
    Scope *s=&c->scopes[r->scope];
    for (int i=0;i<r->argc;++i) {
      if (i) buf_puts(b,", ");
      LocalVar *lv=scope_local(s,s->pnames[i]); Buf kind={0};
      ty_tag_into(c,lv?lv->type:TY_UNKNOWN,&kind);
      mlir_cstr(b,kind.p); free(kind.p);
    }
    buf_puts(b,"]");
  }
  buf_puts(b,"}");
  }
  if(sp_typing_probe_enabled&&!g_ndiags){
    buf_puts(b,"]");sp_typing_probe_metadata(b,c);buf_puts(b,"} {\n");return;
  }
  buf_puts(b,"]} {\n");
}
