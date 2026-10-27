/* M4: fork before parsing; parent and child never share analyzer mutations. */
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "typing_probe.h"
/* M4': the probe's settings are defined in the library (emit_mlir_globals.inc). */
static char *sp_probe_read(int fd) {
  size_t len=0,cap=4096;char *p=malloc(cap);if(!p)return NULL;
  for(;;){
    if(len+2049>cap){if(cap>=64u*1024u*1024u){free(p);return NULL;}size_t next=cap*2;char *q=realloc(p,next);if(!q){free(p);return NULL;}p=q;cap=next;}
    ssize_t n=read(fd,p+len,cap-len-1);
    if(n<0&&errno==EINTR)continue;
    if(n<0){free(p);return NULL;}if(!n)break;len+=(size_t)n;
  }
  p[len]=0;return p;
}
int sp_typing_probe_prepare(void) {
  if(!sp_typing_probe_enabled)return 1;
  if(!sp_emit_mlir_enabled||!sp_analysis_root_spec||sp_facts_root_count<1){
    fprintf(stderr,"spinel: --emit-typing-probe requires --emit-mlir and analysis roots\n");return 0;
  }
  int pipefd[2];if(pipe(pipefd)){fprintf(stderr,"spinel: typing probe pipe failed: %s\n",strerror(errno));return 0;}
  const char *tmp=getenv("TMPDIR");if(!tmp||!*tmp)tmp="/tmp";
  size_t size=strlen(tmp)+40;char *path=malloc(size);
  if(!path){fprintf(stderr,"spinel: typing probe temp allocation failed\n");close(pipefd[0]);close(pipefd[1]);return 0;}
  snprintf(path,size,"%s/spinel-probe-stderr-XXXXXX",tmp);int errfd=mkstemp(path);
  if(errfd<0){fprintf(stderr,"spinel: typing probe stderr temp failed: %s\n",strerror(errno));close(pipefd[0]);close(pipefd[1]);free(path);return 0;}
  /* Unlink immediately: only our descriptors own the temporary object. */
  unlink(path);free(path);
  fflush(NULL);pid_t pid=fork();
  if(pid<0){fprintf(stderr,"spinel: typing probe fork failed: %s\n",strerror(errno));close(pipefd[0]);close(pipefd[1]);close(errfd);return 0;}
  if(!pid){
    close(pipefd[0]);if(dup2(errfd,STDERR_FILENO)<0)_exit(121);close(errfd);
    sp_typing_probe_child=1;sp_typing_probe_fd=pipefd[1];
    sp_analysis_root_spec=NULL;sp_facts_root_count=0;
    return 1;
  }
  close(pipefd[1]);char *data=sp_probe_read(pipefd[0]);close(pipefd[0]);int status=0;pid_t waited;
  do{waited=waitpid(pid,&status,0);}while(waited<0&&errno==EINTR);
  if(lseek(errfd,0,SEEK_SET)<0){free(data);data=NULL;}
  sp_typing_probe_stderr=sp_probe_read(errfd);close(errfd);
  if(waited!=pid||!WIFEXITED(status)||WEXITSTATUS(status)!=0||!data||strlen(data)<66||data[64]!='\n'||data[65]!='{'||!sp_typing_probe_stderr){
    fprintf(stderr,"spinel: typing probe before analysis failed (wait=%ld status=%d)\n",(long)waited,status);
    if(sp_typing_probe_stderr&&*sp_typing_probe_stderr)fputs(sp_typing_probe_stderr,stderr);
    free(data);return 0;
  }
  memcpy(sp_typing_probe_before_hash,data,64);sp_typing_probe_before_hash[64]=0;
  memmove(data,data+65,strlen(data+65)+1);sp_typing_probe_before=data;return 1;
}
