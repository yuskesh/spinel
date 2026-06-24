/* sp_array.c -- cold typed-array operations (see sp_array.h).
 *
 * The hot accessors (new / push / pop / shift / get / set / length /
 * empty) are inline in sp_array.h and compiled per generated TU. The
 * out-of-line ops below -- sort / slice / dup / set algebra / join /
 * inspect-free reductions -- are cold (never on optcarrot's inner loop),
 * so they compile once into libspinel_rt.a instead of into every TU.
 *
 * They reach the GC string heap via sp_alloc.h and call back into the
 * inline core through sp_array.h; sp_sprintf / sp_raise_* are resolved
 * at the final link against the generated TU. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sp_array.h"

/* Issue #799: clamp e-s+1 against size_t overflow + an arbitrary sanity
   cap (1 << 30 elements; ~8 GB at 8 bytes/elem). Without the cap,
   `(1..MRB_INT_MAX).to_a` overflows the realloc size_t to a tiny number,
   then writes past the allocation. */
sp_IntArray*sp_IntArray_from_range(mrb_int s,mrb_int e){sp_IntArray*a=sp_IntArray_new();mrb_int n=e-s+1;if(n<0)n=0;if(n>(mrb_int)(1LL<<30))n=(mrb_int)(1LL<<30);if(n>a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=n;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}for(mrb_int i=0;i<n;i++)a->data[i]=s+i;a->len=n;return a;}
/* `(a..b).step(k)` -- emit a, a+k, a+2k, ... up to b. k must be > 0;
   k <= 0 returns empty. Issue #731. */
sp_IntArray*sp_IntArray_from_range_step(mrb_int s,mrb_int e,mrb_int k){sp_IntArray*a=sp_IntArray_new();if(k<=0)return a;mrb_int v=s;while(v<=e){sp_IntArray_push(a,v);v+=k;}return a;}
sp_IntArray*sp_IntArray_dup(sp_IntArray*a){SP_GC_ROOT(a);sp_IntArray*b=sp_IntArray_new();if(a->len>b->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)b-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*b->cap;h->size-=sizeof(mrb_int)*b->cap;b->cap=a->len;void*nd=realloc(b->data,sizeof(mrb_int)*b->cap);if(!nd)sp_oom_die();b->data=(mrb_int*)nd;h->size+=sizeof(mrb_int)*b->cap;sp_gc_bytes+=sizeof(mrb_int)*b->cap;}memcpy(b->data,a->data+a->start,sizeof(mrb_int)*a->len);b->len=a->len;return b;}
/* a[start, len] / a[start..end] for IntArray. Negative start counts from
   the end. start past the array length yields an empty result; len is
   clamped so we never read past the source. CRuby returns nil for
   out-of-bounds start; we return an empty IntArray since this typed
   collection has no nullable form. */
sp_IntArray*sp_IntArray_slice(sp_IntArray*a,mrb_int start,mrb_int len){SP_GC_ROOT(a);if(start<0)start+=a->len;if(start<0)start=0;sp_IntArray*b=sp_IntArray_new();if(start>=a->len||len<=0)return b;if(start+len>a->len)len=a->len-start;if(len>b->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)b-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*b->cap;h->size-=sizeof(mrb_int)*b->cap;b->cap=len;b->data=(mrb_int*)realloc(b->data,sizeof(mrb_int)*b->cap);h->size+=sizeof(mrb_int)*b->cap;sp_gc_bytes+=sizeof(mrb_int)*b->cap;}memcpy(b->data,a->data+a->start+start,sizeof(mrb_int)*len);b->len=len;return b;}
/* a[start..end] / a[start...end] with possibly negative endpoints.
   Normalize end against a->len first; the bare _slice already handles
   negative start. Issue #496. */
sp_IntArray*sp_IntArray_slice_range(sp_IntArray*a,mrb_int start,mrb_int end_,mrb_int excl){if(end_<0)end_+=a->len;if(start<0)start+=a->len;mrb_int n=end_-start+(excl?0:1);if(n<0||start<0)n=0;return sp_IntArray_slice(a,start,n);}
void sp_IntArray_replace(sp_IntArray*dst,sp_IntArray*src){dst->len=0;dst->start=0;if(src->len>dst->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)dst-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*dst->cap;h->size-=sizeof(mrb_int)*dst->cap;void*nd=realloc(dst->data,sizeof(mrb_int)*src->len);if(!nd){perror("realloc");exit(1);}dst->data=(mrb_int*)nd;dst->cap=src->len;h->size+=sizeof(mrb_int)*dst->cap;sp_gc_bytes+=sizeof(mrb_int)*dst->cap;}memcpy(dst->data,src->data+src->start,sizeof(mrb_int)*src->len);dst->len=src->len;}
void sp_IntArray_reverse_bang(sp_IntArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=0,j=a->len-1;i<j;i++,j--){mrb_int t=a->data[a->start+i];a->data[a->start+i]=a->data[a->start+j];a->data[a->start+j]=t;}}
void sp_IntArray_rotate_bang(sp_IntArray*a,mrb_int n){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;mrb_int*d=a->data+a->start;mrb_int lo=0,hi=n-1;while(lo<hi){mrb_int t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=n;hi=a->len-1;while(lo<hi){mrb_int t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=0;hi=a->len-1;while(lo<hi){mrb_int t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}}
static int _sp_int_cmp(const void*a,const void*b){mrb_int va=*(const mrb_int*)a,vb=*(const mrb_int*)b;return(va>vb)-(va<vb);}
sp_IntArray*sp_IntArray_sort(sp_IntArray*a){sp_IntArray*b=sp_IntArray_dup(a);qsort(b->data+b->start,b->len,sizeof(mrb_int),_sp_int_cmp);return b;}
void sp_IntArray_sort_bang(sp_IntArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}qsort(a->data+a->start,a->len,sizeof(mrb_int),_sp_int_cmp);}
void sp_IntArray_uniq_bang(sp_IntArray*a){if(!a||a->frozen){if(a&&a->frozen)sp_raise_frozen_array();return;}for(mrb_int i=0;i<a->len;){int dup=0;for(mrb_int j=0;j<i;j++){if(a->data[a->start+j]==a->data[a->start+i]){dup=1;break;}}if(dup){for(mrb_int k2=i;k2<a->len-1;k2++)a->data[a->start+k2]=a->data[a->start+k2+1];a->len--;}else i++;}}
void sp_IntArray_shuffle_bang(sp_IntArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));mrb_int t=a->data[a->start+i];a->data[a->start+i]=a->data[a->start+j];a->data[a->start+j]=t;}}
sp_IntArray*sp_IntArray_shuffle(sp_IntArray*a){sp_IntArray*b=sp_IntArray_dup(a);sp_IntArray_shuffle_bang(b);return b;}
/* Array#sample. CRuby returns nil for `[].sample`; in spinel's typed-array
   slot nil collapses to 0. Guards rand()%0 (SIGFPE under -O0, UB at -O2+).
   Issue #536. */
mrb_int sp_IntArray_sample(sp_IntArray*a){if(a->len<=0)return 0;return a->data[a->start+(mrb_int)(rand()%a->len)];}
/* Issue #745/#832: empty min/max return SP_INT_NIL (caller treats as
   int?); without the guard, the first read is uninitialized memory. */
mrb_int sp_IntArray_min(sp_IntArray*a){if(!a||a->len<=0)return SP_INT_NIL;mrb_int m=a->data[a->start];for(mrb_int i=1;i<a->len;i++)if(a->data[a->start+i]<m)m=a->data[a->start+i];return m;}
mrb_int sp_IntArray_max(sp_IntArray*a){if(!a||a->len<=0)return SP_INT_NIL;mrb_int m=a->data[a->start];for(mrb_int i=1;i<a->len;i++)if(a->data[a->start+i]>m)m=a->data[a->start+i];return m;}
mrb_int sp_IntArray_sum(sp_IntArray*a,mrb_int init){mrb_int s=init;for(mrb_int i=0;i<a->len;i++)s+=a->data[a->start+i];return s;}
mrb_bool sp_IntArray_include(sp_IntArray*a,mrb_int v){if(!a)return FALSE;for(mrb_int i=0;i<a->len;i++)if(a->data[a->start+i]==v)return TRUE;return FALSE;}
mrb_int sp_IntArray_index(sp_IntArray*a,mrb_int v){for(mrb_int i=0;i<a->len;i++)if(a->data[a->start+i]==v)return i;return -1;}
mrb_int sp_IntArray_rindex(sp_IntArray*a,mrb_int v){for(mrb_int i=a->len-1;i>=0;i--)if(a->data[a->start+i]==v)return i;return -1;}
mrb_int sp_IntArray_delete_at(sp_IntArray*a,mrb_int i){if(a&&a->frozen){sp_raise_frozen_array();return SP_INT_NIL;}if(i<0)i+=a->len;if(i<0||i>=a->len)return SP_INT_NIL;mrb_int v=a->data[a->start+i];for(mrb_int j=i;j<a->len-1;j++)a->data[a->start+j]=a->data[a->start+j+1];a->len--;return v;}
mrb_int sp_IntArray_delete(sp_IntArray*a,mrb_int v){if(a&&a->frozen){sp_raise_frozen_array();return 0;}mrb_int w=0;for(mrb_int i=0;i<a->len;i++){if(a->data[a->start+i]!=v){a->data[a->start+w]=a->data[a->start+i];w++;}}mrb_int d=a->len-w;a->len=w;return d>0?v:0;}
/* Issue #788: clamp i so a very-negative index doesn't underflow past
   a->start and write into the array's GC header. */
void sp_IntArray_insert(sp_IntArray*a,mrb_int i,mrb_int v){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(i<0)i+=a->len+1;if(i<0)i=0;if(i>a->len)i=a->len;sp_IntArray_push(a,0);for(mrb_int j=a->len-1;j>i;j--)a->data[a->start+j]=a->data[a->start+j-1];a->data[a->start+i]=v;}
sp_IntArray*sp_IntArray_uniq(sp_IntArray*a){sp_IntArray*b=sp_IntArray_new();for(mrb_int i=0;i<a->len;i++){int found=0;for(mrb_int j=0;j<b->len;j++){if(b->data[b->start+j]==a->data[a->start+i]){found=1;break;}}if(!found)sp_IntArray_push(b,a->data[a->start+i]);}return b;}
sp_IntArray*sp_IntArray_intersect(sp_IntArray*a,sp_IntArray*b){sp_IntArray*r=sp_IntArray_new();if(!a||!b)return r;for(mrb_int i=0;i<a->len;i++){mrb_int v=a->data[a->start+i];if(sp_IntArray_include(b,v)&&!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}return r;}
sp_IntArray*sp_IntArray_union(sp_IntArray*a,sp_IntArray*b){sp_IntArray*r=sp_IntArray_new();if(a)for(mrb_int i=0;i<a->len;i++){mrb_int v=a->data[a->start+i];if(!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}if(b){for(mrb_int i=0;i<b->len;i++){mrb_int v=b->data[b->start+i];if(!sp_IntArray_include(r,v))sp_IntArray_push(r,v);}}return r;}
/* Array#- / Array#difference: keep every LHS element NOT in RHS,
   preserving the LHS's duplicates. `[1,1,2,3] - [3]` is `[1,1,2]`. */
sp_IntArray*sp_IntArray_difference(sp_IntArray*a,sp_IntArray*b){sp_IntArray*r=sp_IntArray_new();if(!a)return r;for(mrb_int i=0;i<a->len;i++){mrb_int v=a->data[a->start+i];if(!sp_IntArray_include(b,v))sp_IntArray_push(r,v);}return r;}
void sp_IntArray_unshift(sp_IntArray*a,mrb_int v){if(a->frozen){sp_raise_frozen_array();return;}if(a->start>0){a->start--;a->data[a->start]=v;a->len++;}else{mrb_int e=a->len+1;if(e>a->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)a-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_int)*a->cap;h->size-=sizeof(mrb_int)*a->cap;a->cap=a->cap*2+1;a->data=(mrb_int*)realloc(a->data,sizeof(mrb_int)*a->cap);h->size+=sizeof(mrb_int)*a->cap;sp_gc_bytes+=sizeof(mrb_int)*a->cap;}memmove(a->data+1,a->data,sizeof(mrb_int)*a->len);a->data[0]=v;a->len++;}}
const char*sp_IntArray_join(sp_IntArray*a,const char*sep){size_t sl=strlen(sep),cap=256;char*buf=(char*)malloc(cap);size_t len=0;for(mrb_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}char tmp[32];int n=snprintf(tmp,32,"%lld",(long long)a->data[a->start+i]);if(len+n>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,tmp,n);len+=n;}buf[len]=0;char*r=sp_str_alloc(len);memcpy(r,buf,len);free(buf);return r;}
mrb_bool sp_IntArray_eq(sp_IntArray*a,sp_IntArray*b){if(!a||!b)return a==b;if(a->len!=b->len)return FALSE;for(mrb_int i=0;i<a->len;i++)if(a->data[a->start+i]!=b->data[b->start+i])return FALSE;return TRUE;}
/* Array#<=> for IntArray. Lexicographic: per-element compare, shorter
   array sorts before longer if all common elements match
   (`[1,2] <=> [1,2,3] == -1`). NULL recv compares equal to NULL, lower
   than any non-NULL. */
mrb_int sp_IntArray_cmp(sp_IntArray*a,sp_IntArray*b){if(!a||!b)return a==b?0:(a?1:-1);mrb_int n=a->len<b->len?a->len:b->len;for(mrb_int i=0;i<n;i++){mrb_int av=a->data[a->start+i],bv=b->data[b->start+i];if(av<bv)return -1;if(av>bv)return 1;}if(a->len<b->len)return -1;if(a->len>b->len)return 1;return 0;}

/* ============================ sp_FloatArray ============================ */
void sp_FloatArray_unshift(sp_FloatArray*a,mrb_float v){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}sp_FloatArray_push(a,0.0);if(a->len>1)memmove(&a->data[1],&a->data[0],(size_t)(a->len-1)*sizeof(mrb_float));a->data[0]=v;}
/* Float#step materialised as a FloatArray. Direction follows the sign of
   k; k==0 yields an empty array to avoid an infinite loop. */
sp_FloatArray*sp_FloatArray_from_step(mrb_float s,mrb_float e,mrb_float k){sp_FloatArray*a=sp_FloatArray_new();if(k==0.0)return a;mrb_float v=s;if(k>0){while(v<=e){sp_FloatArray_push(a,v);v+=k;}}else{while(v>=e){sp_FloatArray_push(a,v);v+=k;}}return a;}
mrb_float sp_FloatArray_min(sp_FloatArray*a){if(a->len==0)return 0;mrb_float m=a->data[0];for(mrb_int i=1;i<a->len;i++)if(a->data[i]<m)m=a->data[i];return m;}
mrb_float sp_FloatArray_max(sp_FloatArray*a){if(a->len==0)return 0;mrb_float m=a->data[0];for(mrb_int i=1;i<a->len;i++)if(a->data[i]>m)m=a->data[i];return m;}
mrb_float sp_FloatArray_sum(sp_FloatArray*a,mrb_float init){mrb_float s=init;for(mrb_int i=0;i<a->len;i++)s+=a->data[i];return s;}
void sp_FloatArray_replace(sp_FloatArray*dst,sp_FloatArray*src){dst->len=0;if(src->len>dst->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)dst-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*dst->cap;h->size-=sizeof(mrb_float)*dst->cap;void*nd=realloc(dst->data,sizeof(mrb_float)*src->len);if(!nd){perror("realloc");exit(1);}dst->data=(mrb_float*)nd;dst->cap=src->len;h->size+=sizeof(mrb_float)*dst->cap;sp_gc_bytes+=sizeof(mrb_float)*dst->cap;}memcpy(dst->data,src->data,sizeof(mrb_float)*src->len);dst->len=src->len;}
/* a[start, len] / a[start..end] for FloatArray. Same negative-start and
   length-clamping semantics as sp_IntArray_slice. */
sp_FloatArray*sp_FloatArray_slice(sp_FloatArray*a,mrb_int start,mrb_int len){SP_GC_ROOT(a);if(start<0)start+=a->len;if(start<0)start=0;sp_FloatArray*b=sp_FloatArray_new();if(start>=a->len||len<=0)return b;if(start+len>a->len)len=a->len-start;if(len>b->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)b-sizeof(sp_gc_hdr));sp_gc_bytes-=sizeof(mrb_float)*b->cap;h->size-=sizeof(mrb_float)*b->cap;b->cap=len;b->data=(mrb_float*)realloc(b->data,sizeof(mrb_float)*b->cap);h->size+=sizeof(mrb_float)*b->cap;sp_gc_bytes+=sizeof(mrb_float)*b->cap;}memcpy(b->data,a->data+start,sizeof(mrb_float)*len);b->len=len;return b;}
sp_FloatArray*sp_FloatArray_slice_range(sp_FloatArray*a,mrb_int start,mrb_int end_,mrb_int excl){if(end_<0)end_+=a->len;if(start<0)start+=a->len;mrb_int n=end_-start+(excl?0:1);if(n<0||start<0)n=0;return sp_FloatArray_slice(a,start,n);}
void sp_FloatArray_reverse_bang(sp_FloatArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=0,j=a->len-1;i<j;i++,j--){mrb_float t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
void sp_FloatArray_rotate_bang(sp_FloatArray*a,mrb_int n){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;mrb_float*d=a->data;mrb_int lo=0,hi=n-1;while(lo<hi){mrb_float t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=n;hi=a->len-1;while(lo<hi){mrb_float t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=0;hi=a->len-1;while(lo<hi){mrb_float t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}}
static int _sp_float_cmp(const void*a,const void*b){mrb_float va=*(const mrb_float*)a,vb=*(const mrb_float*)b;return(va>vb)-(va<vb);}
void sp_FloatArray_sort_bang(sp_FloatArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}qsort(a->data,a->len,sizeof(mrb_float),_sp_float_cmp);}
void sp_FloatArray_shuffle_bang(sp_FloatArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));mrb_float t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
sp_FloatArray*sp_FloatArray_dup(sp_FloatArray*a){SP_GC_ROOT(a);sp_FloatArray*b=sp_FloatArray_new();sp_FloatArray_replace(b,a);return b;}
sp_FloatArray*sp_FloatArray_sort(sp_FloatArray*a){sp_FloatArray*b=sp_FloatArray_dup(a);sp_FloatArray_sort_bang(b);return b;}
sp_FloatArray*sp_FloatArray_shuffle(sp_FloatArray*a){sp_FloatArray*r=sp_FloatArray_new();sp_FloatArray_replace(r,a);sp_FloatArray_shuffle_bang(r);return r;}
mrb_float sp_FloatArray_sample(sp_FloatArray*a){if(a->len<=0)return 0.0;return a->data[(mrb_int)(rand()%a->len)];}
/* IEEE 754 == on mrb_float: NaN never matches; +0.0 == -0.0 (diverges from Float#eql?). */
mrb_bool sp_FloatArray_include(sp_FloatArray*a,mrb_float v){if(!a)return FALSE;for(mrb_int i=0;i<a->len;i++)if(a->data[i]==v)return TRUE;return FALSE;}
sp_FloatArray*sp_FloatArray_intersect(sp_FloatArray*a,sp_FloatArray*b){sp_FloatArray*r=sp_FloatArray_new();if(!a||!b)return r;for(mrb_int i=0;i<a->len;i++){mrb_float v=a->data[i];if(sp_FloatArray_include(b,v)&&!sp_FloatArray_include(r,v))sp_FloatArray_push(r,v);}return r;}
sp_FloatArray*sp_FloatArray_union(sp_FloatArray*a,sp_FloatArray*b){sp_FloatArray*r=sp_FloatArray_new();if(a)for(mrb_int i=0;i<a->len;i++){mrb_float v=a->data[i];if(!sp_FloatArray_include(r,v))sp_FloatArray_push(r,v);}if(b){for(mrb_int i=0;i<b->len;i++){mrb_float v=b->data[i];if(!sp_FloatArray_include(r,v))sp_FloatArray_push(r,v);}}return r;}
sp_FloatArray*sp_FloatArray_difference(sp_FloatArray*a,sp_FloatArray*b){sp_FloatArray*r=sp_FloatArray_new();for(mrb_int i=0;i<a->len;i++){mrb_float v=a->data[i];if(!sp_FloatArray_include(b,v))sp_FloatArray_push(r,v);}return r;}

/* ============================= sp_PtrArray ============================ */
/* `Array#delete_at(i)` -- remove and return the element at index i.
   Negative indices count from the end; NULL when out of range. */
void*sp_PtrArray_delete_at(sp_PtrArray*a,mrb_int i){if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;void*v=a->data[i];for(mrb_int j=i;j<a->len-1;j++)a->data[j]=a->data[j+1];a->len--;return v;}
void sp_PtrArray_reverse_bang(sp_PtrArray*a){for(mrb_int i=0,j=a->len-1;i<j;i++,j--){void*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
void sp_PtrArray_rotate_bang(sp_PtrArray*a,mrb_int n){if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;void**d=a->data;mrb_int lo=0,hi=n-1;while(lo<hi){void*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=n;hi=a->len-1;while(lo<hi){void*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=0;hi=a->len-1;while(lo<hi){void*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}}
sp_PtrArray*sp_PtrArray_dup(sp_PtrArray*a){sp_PtrArray*b=sp_PtrArray_new_scan(a->scan_elem);for(mrb_int i=0;i<a->len;i++)sp_PtrArray_push(b,a->data[i]);return b;}
sp_PtrArray*sp_PtrArray_slice(sp_PtrArray*a,mrb_int start,mrb_int len){if(start<0)start+=a->len;if(start<0)start=0;sp_PtrArray*b=sp_PtrArray_new_scan(a->scan_elem);if(start>=a->len||len<=0)return b;if(start+len>a->len)len=a->len-start;for(mrb_int i=0;i<len;i++)sp_PtrArray_push(b,a->data[start+i]);return b;}
void sp_PtrArray_shuffle_bang(sp_PtrArray*a){for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));void*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
sp_PtrArray*sp_PtrArray_shuffle(sp_PtrArray*a){sp_PtrArray*b=sp_PtrArray_dup(a);sp_PtrArray_shuffle_bang(b);return b;}
void *sp_PtrArray_sample(sp_PtrArray*a){if(a->len<=0)return NULL;return a->data[(mrb_int)(rand()%a->len)];}

/* ============================= sp_StrArray ============================ */
void sp_StrArray_replace(sp_StrArray*dst,sp_StrArray*src){dst->len=0;if(src->len>dst->cap){sp_gc_hdr*h=(sp_gc_hdr*)((char*)dst-sizeof(sp_gc_hdr));void*nd;if(dst->data==dst->inline_data){nd=malloc(sizeof(const char*)*src->len);if(!nd){perror("malloc");exit(1);}}else{sp_gc_bytes-=sizeof(const char*)*dst->cap;h->size-=sizeof(const char*)*dst->cap;nd=realloc(dst->data,sizeof(const char*)*src->len);if(!nd){perror("realloc");exit(1);}}dst->data=(const char**)nd;dst->cap=src->len;h->size+=sizeof(const char*)*dst->cap;sp_gc_bytes+=sizeof(const char*)*dst->cap;}memcpy(dst->data,src->data,sizeof(const char*)*src->len);dst->len=src->len;}
const char*sp_StrArray_pop(sp_StrArray*a){if(!a||a->len<=0)return NULL;if(a->frozen){sp_raise_frozen_array();return NULL;}return a->data[--a->len];}
const char*sp_StrArray_shift(sp_StrArray*a){if(!a||a->len<=0)return NULL;if(a->frozen){sp_raise_frozen_array();return NULL;}const char*v=a->data[0];memmove(a->data,a->data+1,(size_t)(--a->len)*sizeof(const char*));return v;}
/* a[start, len] / a[start..end] for StrArray. Same negative-start and
   length-clamping semantics as sp_IntArray_slice. */
sp_StrArray*sp_StrArray_slice(sp_StrArray*a,mrb_int start,mrb_int len){SP_GC_ROOT(a);if(start<0)start+=a->len;if(start<0)start=0;sp_StrArray*b=sp_StrArray_new();if(start>=a->len||len<=0)return b;if(start+len>a->len)len=a->len-start;for(mrb_int i=0;i<len;i++)sp_StrArray_push(b,a->data[start+i]);return b;}
sp_StrArray*sp_StrArray_slice_range(sp_StrArray*a,mrb_int start,mrb_int end_,mrb_int excl){if(end_<0)end_+=a->len;if(start<0)start+=a->len;mrb_int n=end_-start+(excl?0:1);if(n<0||start<0)n=0;return sp_StrArray_slice(a,start,n);}
void sp_StrArray_reverse_bang(sp_StrArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=0,j=a->len-1;i<j;i++,j--){const char*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
void sp_StrArray_rotate_bang(sp_StrArray*a,mrb_int n){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(a->len<=0)return;n=((n%a->len)+a->len)%a->len;if(n==0)return;const char**d=a->data;mrb_int lo=0,hi=n-1;while(lo<hi){const char*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=n;hi=a->len-1;while(lo<hi){const char*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}lo=0;hi=a->len-1;while(lo<hi){const char*t=d[lo];d[lo]=d[hi];d[hi]=t;lo++;hi--;}}
static int _sp_str_cmp(const void*a,const void*b){return strcmp(*(const char*const*)a,*(const char*const*)b);}
void sp_StrArray_sort_bang(sp_StrArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}qsort(a->data,a->len,sizeof(const char*),_sp_str_cmp);}
void sp_StrArray_uniq_bang(sp_StrArray*a){if(!a||a->frozen){if(a&&a->frozen)sp_raise_frozen_array();return;}for(mrb_int i=0;i<a->len;){int dup=0;for(mrb_int j=0;j<i;j++){if(a->data[j]==a->data[i]||(a->data[j]&&a->data[i]&&!strcmp(a->data[j],a->data[i]))){dup=1;break;}}if(dup){for(mrb_int k2=i;k2<a->len-1;k2++)a->data[k2]=a->data[k2+1];a->len--;}else i++;}}
const char*sp_StrArray_join(sp_StrArray*a,const char*sep){size_t sl=strlen(sep),cap=256;char*buf=(char*)malloc(cap);size_t len=0;for(mrb_int i=0;i<a->len;i++){if(i>0){if(len+sl>=cap){cap*=2;buf=(char*)realloc(buf,cap);}memcpy(buf+len,sep,sl);len+=sl;}size_t el=strlen(a->data[i]);if(len+el>=cap){cap=(len+el)*2+1;buf=(char*)realloc(buf,cap);}memcpy(buf+len,a->data[i],el);len+=el;}buf[len]=0;char*r=sp_str_alloc(len);memcpy(r,buf,len);free(buf);return r;}
mrb_bool sp_StrArray_include(sp_StrArray*a,const char*v){if(!a)return FALSE;for(mrb_int i=0;i<a->len;i++)if(strcmp(a->data[i],v)==0)return TRUE;return FALSE;}
sp_StrArray*sp_StrArray_intersect(sp_StrArray*a,sp_StrArray*b){sp_StrArray*r=sp_StrArray_new();if(!a||!b)return r;for(mrb_int i=0;i<a->len;i++){const char*v=a->data[i];if(sp_StrArray_include(b,v)&&!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}return r;}
sp_StrArray*sp_StrArray_union(sp_StrArray*a,sp_StrArray*b){sp_StrArray*r=sp_StrArray_new();if(a)for(mrb_int i=0;i<a->len;i++){const char*v=a->data[i];if(!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}if(b){for(mrb_int i=0;i<b->len;i++){const char*v=b->data[i];if(!sp_StrArray_include(r,v))sp_StrArray_push(r,v);}}return r;}
sp_StrArray*sp_StrArray_difference(sp_StrArray*a,sp_StrArray*b){sp_StrArray*r=sp_StrArray_new();for(mrb_int i=0;i<a->len;i++){const char*v=a->data[i];if(!sp_StrArray_include(b,v))sp_StrArray_push(r,v);}return r;}
mrb_int sp_StrArray_index(sp_StrArray*a,const char*v){for(mrb_int i=0;i<a->len;i++)if(strcmp(a->data[i],v)==0)return i;return -1;}
mrb_int sp_StrArray_rindex(sp_StrArray*a,const char*v){for(mrb_int i=a->len-1;i>=0;i--)if(strcmp(a->data[i],v)==0)return i;return -1;}
sp_StrArray*sp_StrArray_compact(sp_StrArray*a){sp_StrArray*r=sp_StrArray_new();for(mrb_int i=0;i<a->len;i++)if(a->data[i]!=NULL)sp_StrArray_push(r,a->data[i]);return r;}
const char*sp_StrArray_delete_at(sp_StrArray*a,mrb_int i){if(!a)return NULL;if(a->frozen){sp_raise_frozen_array();return NULL;}if(i<0)i+=a->len;if(i<0||i>=a->len)return NULL;const char*v=a->data[i];for(mrb_int j=i;j<a->len-1;j++)a->data[j]=a->data[j+1];a->len--;return v;}
const char*sp_StrArray_delete(sp_StrArray*a,const char*v){if(!a)return NULL;if(a->frozen){sp_raise_frozen_array();return NULL;}mrb_int w=0;const char*found=NULL;for(mrb_int i=0;i<a->len;i++){if(strcmp(a->data[i],v)!=0){a->data[w]=a->data[i];w++;}else{found=a->data[i];}}a->len=w;return found;}
void sp_StrArray_insert(sp_StrArray*a,mrb_int i,const char*v){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}if(i<0)i+=a->len+1;sp_StrArray_push(a,sp_str_empty);for(mrb_int j=a->len-1;j>i;j--)a->data[j]=a->data[j-1];a->data[i]=v;}
void sp_StrArray_shuffle_bang(sp_StrArray*a){if(!a)return;if(a->frozen){sp_raise_frozen_array();return;}for(mrb_int i=a->len-1;i>0;i--){mrb_int j=(mrb_int)(rand()%(i+1));const char*t=a->data[i];a->data[i]=a->data[j];a->data[j]=t;}}
sp_StrArray*sp_StrArray_dup(sp_StrArray*a){SP_GC_ROOT(a);sp_StrArray*r=sp_StrArray_new();sp_StrArray_replace(r,a);return r;}
sp_StrArray*sp_StrArray_sort(sp_StrArray*a){sp_StrArray*b=sp_StrArray_dup(a);sp_StrArray_sort_bang(b);return b;}
sp_StrArray*sp_StrArray_shuffle(sp_StrArray*a){sp_StrArray*r=sp_StrArray_new();sp_StrArray_replace(r,a);sp_StrArray_shuffle_bang(r);return r;}
const char *sp_StrArray_sample(sp_StrArray*a){if(a->len<=0)return sp_str_empty;return a->data[(mrb_int)(rand()%a->len)];}
