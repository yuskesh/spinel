/* M3': opt-in position check (definitions in source_map_pin.inc, after the map builder). */
#include "source_map_pin.h"
static int sp_m3_active(void) {
  static int on = -1;
  if (on < 0) on = getenv("SPINEL_MLIR_POSITIONS") ? 1 : 0;
  return on;
}
static void sp_m3_build(const char *buffer);
static void sp_m3_fix_start(int id, int bl, int *orig, int *fid);
static void sp_m3_fix_end(int id, int el, int fid, int *eorig);
