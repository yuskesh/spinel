/* M3': refuse when a node's original-file position could not be confirmed
   (derived from spinel-mlir 3accdcd patches/m3/source_map_emit.inc). M3's
   optional spinel_facts.splice op is not emitted on this pin: see
   source_map_pin.inc and the provenance record. */
#include "source_map_pin.h"
static int mlir_m3_check(void) {
  if (!sp_m3_fail_count) return 1;
  fprintf(stderr, "spinel: MLIR M3 unrecoverable span node=%d file=%d buffer_line=%d reason=%s count=%d\n",
          sp_m3_fail_node, sp_m3_fail_file, sp_m3_fail_line,
          sp_m3_fail_reason ? sp_m3_fail_reason : "unknown", sp_m3_fail_count);
  return 0;
}
