/* M1 on this pin: the transport's settings, defined in the library.
 *
 * spinel-mlir's M1 defined them in main.c. Tools that link Spinel's
 * objects without main.o (a reader that only parses and analyzes) then had
 * no definition for the references in codegen.c and codegen_util.c.
 * Defined here, in codegen_util.c, they hold their off values for every
 * such tool; main.c sets them from its flags. */
int sp_emit_mlir_enabled = 0;
const char *sp_emit_mlir_overflow = "raise";
const char *sp_emit_mlir_path = 0;
NodeTable *sp_emit_mlir_pristine = NULL;
