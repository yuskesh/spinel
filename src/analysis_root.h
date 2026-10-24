#ifndef SP_FACTS_ANALYSIS_ROOT_H
#define SP_FACTS_ANALYSIS_ROOT_H
#include "compiler.h"
extern const char *sp_analysis_root_spec;
typedef struct {
  char *method;
  char **requested;
  TyKind *types;
  int argc, scope, def_node, seeded;
} FactsAnalysisRoot;
#define FACTS_MAX_ROOTS 16
extern FactsAnalysisRoot sp_facts_roots[FACTS_MAX_ROOTS];
extern int sp_facts_root_count;
int facts_queue_root_request(const char *spec);
int facts_parse_root_request(const char *spec);
void facts_seed_root(Compiler *c);
int facts_scope_is_root(int scope);
void facts_root_census(Compiler *c);
#endif
