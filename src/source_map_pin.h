/* M3' (derived from spinel-mlir's M3 for this pin): the original-file
   positions the facts carry. Shared by the parser (source_map_pin.inc) and
   the MLIR printer (source_map_pin_emit.inc); every recorder runs only under
   SPINEL_MLIR_POSITIONS. */
#ifndef SP_M3_SOURCE_MAP_PIN_H
#define SP_M3_SOURCE_MAP_PIN_H
extern int sp_m3_fail_count, sp_m3_fail_node, sp_m3_fail_file, sp_m3_fail_line;
extern const char *sp_m3_fail_reason;
extern int sp_m3_top_file;   /* M4': the top-level file's id (M3's sp_m3_top_file) */
#endif
