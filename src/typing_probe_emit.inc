#include "typing_probe_sha.inc"
#include <errno.h>
static int probe_send_json(Compiler *c){
 if(!c)_exit(124);
 MlirSource source={0};int fid=sp_m3_top_file;
 if(fid<0||fid>=sp_emit_mlir_pristine->nfiles||!mlir_read_source(nt_file_path(sp_emit_mlir_pristine,fid),&source))_exit(125);
 char hash[66];probe_sha256((const unsigned char*)source.bytes,source.len,hash);hash[64]='\n';hash[65]=0;free(source.bytes);
 char *json=build_types_json(c);if(!json)_exit(122);
 size_t header=0;while(header<65){ssize_t k=write(sp_typing_probe_fd,hash+header,65-header);if(k<0&&errno==EINTR)continue;if(k<=0)_exit(123);header+=(size_t)k;}

 size_t n=strlen(json),done=0;while(done<n){ssize_t k=write(sp_typing_probe_fd,json+done,n-done);if(k<0&&errno==EINTR)continue;if(k<=0)_exit(123);done+=(size_t)k;}
 close(sp_typing_probe_fd);free(json);_exit(g_ndiags?1:0);
}
void sp_typing_probe_metadata(Buf *b,Compiler *c){
 MlirSource source={0};int fid=sp_m3_top_file;
 if(!c||!sp_typing_probe_before||fid<0||fid>=sp_emit_mlir_pristine->nfiles||!mlir_read_source(nt_file_path(sp_emit_mlir_pristine,fid),&source)){
  fprintf(stderr,"spinel: typing probe source/before snapshot unavailable\n");exit(1);
 }
 char hash[65];probe_sha256((const unsigned char*)source.bytes,source.len,hash);free(source.bytes);
 if(strcmp(hash,sp_typing_probe_before_hash)){fprintf(stderr,"spinel: typing probe source hash changed between analyses\n");exit(1);}
 char *after=build_types_json(c);if(!after){fprintf(stderr,"spinel: typing probe after snapshot unavailable\n");exit(1);}
 buf_puts(b,", spinel_facts.typing_probe = {version = 1 : i64, complete = true, producer_patch = \"M4\", source_file_id = ");mlir_i64(b,fid);
 buf_puts(b,", source_sha256 = ");mlir_cstr(b,hash);buf_puts(b,", analysis_roots = [");
 for(int r=0;r<sp_facts_root_count;r++){FactsAnalysisRoot *root=&sp_facts_roots[r];if(r)buf_puts(b,", ");buf_puts(b,"{method = ");mlir_cstr(b,root->method);buf_puts(b,", requested_arg_types = [");for(int i=0;i<root->argc;i++){if(i)buf_puts(b,", ");mlir_cstr(b,root->requested[i]);}buf_puts(b,"]}");}
 buf_puts(b,"], before_json = ");mlir_cstr(b,sp_typing_probe_before);buf_puts(b,", after_json = ");mlir_cstr(b,after);
 buf_puts(b,", before_stderr = ");mlir_cstr(b,sp_typing_probe_stderr);buf_puts(b,"}");free(after);
}
