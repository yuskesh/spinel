/* spinelc: the single-binary C implementation of the Spinel compiler.
 *
 * Pipeline (all in one process, no on-disk intermediates):
 *   .rb --[Prism walk: sp_parse_file_to_text]--> text AST
 *       --[nt_load_text]--> in-memory node table
 *       --[codegen_program]--> C source
 *
 * The C source is written to a file or stdout; a thin external driver
 * invokes `cc` to link it against libspinel_rt.a. Usage:
 *   spinelc app.rb            print C to stdout
 *   spinelc app.rb -o app.c   write C to app.c
 *   spinelc app.rb -c -o f    (-c accepted, C-only is the only mode for now)
 *   spinelc app.rb --dump-ast  print the text AST and exit
 */
#include "node_table.h"
#include "codegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defined in spinel_parse.c (compiled with -DSPINEL_PARSE_AS_LIB). */
char *sp_parse_file_to_text(const char *source_file, const char *argv0);

static void usage(void) {
  fprintf(stderr,
    "Usage: spinelc app.rb [-o out.c] [-c] [--dump-ast]\n"
    "  -o FILE      write generated C to FILE (default: stdout)\n"
    "  -c           generate C only (the only mode currently)\n"
    "  --dump-ast   print the text AST and exit (debug)\n");
}

int main(int argc, char **argv) {
  const char *source = NULL;
  const char *output = NULL;
  int dump_ast = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-o") == 0) {
      if (++i >= argc) { usage(); return 2; }
      output = argv[i];
    }
    else if (strcmp(a, "-c") == 0) {
      /* C-only; accepted for forward-compat with the driver. */
    }
    else if (strcmp(a, "--dump-ast") == 0) {
      dump_ast = 1;
    }
    else if (a[0] == '-' && a[1] != '\0') {
      fprintf(stderr, "spinelc: unknown option '%s'\n", a);
      usage();
      return 2;
    }
    else if (!source) {
      source = a;
    }
    else {
      fprintf(stderr, "spinelc: unexpected extra argument '%s'\n", a);
      return 2;
    }
  }

  if (!source) { usage(); return 2; }

  char *text = sp_parse_file_to_text(source, argv[0]);
  if (!text) {
    fprintf(stderr, "spinelc: parse failed for '%s'\n", source);
    return 1;
  }

  if (dump_ast) {
    fputs(text, stdout);
    free(text);
    return 0;
  }

  NodeTable *nt = nt_load_text(text);
  free(text);
  if (!nt) {
    fprintf(stderr, "spinelc: failed to load AST\n");
    return 1;
  }

  char *c = codegen_program(nt);
  nt_free(nt);
  if (!c) {
    fprintf(stderr, "spinelc: codegen failed\n");
    return 1;
  }

  FILE *out = stdout;
  if (output) {
    out = fopen(output, "wb");
    if (!out) {
      fprintf(stderr, "spinelc: cannot write '%s'\n", output);
      free(c);
      return 1;
    }
  }
  fputs(c, out);
  if (out != stdout) fclose(out);
  free(c);
  return 0;
}
