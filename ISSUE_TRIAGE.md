# Open issue / PR triage vs the C-version master (2026-06-15)

Triaged every open issue + PR against the current C compiler. Most bug
reports were filed against the OLD Ruby self-host compiler (master until
today), so many no longer reproduce. Reproducers built per item and run
through `./build/spinel` vs CRuby.

## Fixed this pass (committed, unpushed)
- #1393 PR — proc capture struct unrooted -> segfault under GC. FIXED
  (SP_GC_ROOT _capv). commit beebbec.
- #1388 PR — backtick doesn't update $?. FIXED (sp_last_status=pclose).
  commit 87a8def.
- #1391 PR — File.read/write/binread silent on open failure. FIXED
  (raise Errno::ENOENT/EACCES). commit f6a3098.

## Reproduces, documented for deliberate landing (NOT yet fixed)
- #1383 — `require File.expand_path('x',__dir__)` parse corruption.
  Root: src/spinel_parse.c resolve_plain_requires grabs the first quoted
  string on the line regardless of a computed arg. Fix needs a guard +
  loop restructure in a hot require path (risk: all require tests).
- #1373 — circular require_relative double-inlines (now a runtime double
  -output, the compile error is gone). Root: entry file never
  sp_mark_path_included before resolve_requires. Fix: canonicalize +
  mark entry path first.
- #1372 — lambda `->(t){[t]}` with a string arg: param seeded TY_INT
  (analyze_pass.c:1828) unifies INT+STRING->poly -> invalid C. proc form
  already works. Fix = seed TY_UNKNOWN like proc params, BUT this widens
  a hot inference default (memory: don't widen for edge gains) -> land
  deliberately + full optcarrot/bench.
- #1369 — FiberSlot ivar degrades PtrArray<Slot> -> poly_array (dispatch
  + perf). Crash gone (sp_PolyArray_delete_at exists). Element-type
  convergence in analyze.
- #1392 — FIXED, exact one-liner -> 6 (two commits):
  * c894137: sibling iteration-block param collision alpha-renamed
    (blkp_needs_rename accepts call-owned blocks; collision gate filters).
    Unmasked + fixed a latent each_index param-typing bug (index is int).
  * e9d8c86: post-fixpoint re-narrow of iteration-block params that
    transiently locked to poly (read-only params over a now-typed array
    reset to UNKNOWN; re-run re-derives). Reassigned params stay locked
    (read-only guard) so no silent miscompile; instance_exec/rescue/proc
    untouched.
  Gates both commits: 59662 / 847-0-0 / 57-0-0 / clang -Werror clean /
  self-host bootstrap (clang) IR+C fixpoint OK.
  m4 reduce-accumulator promotion ALSO FIXED (separate commit, Refs #1392):
  `[1.5,2.5].reduce(0){|a,x|a+x}` -> 4.0. ty_promote_numeric helper (int
  seed folded over floats -> float, not poly); applied at the reduce return
  type (analyze_infer) + the C accumulator type (codegen_fold), guarded to
  numeric body types so a transient-poly body keeps the seed type (else the
  exact one-liner regressed to a poly acc / invalid initializer). New test
  reduce_numeric_promotion.rb.
- #1389 PR-problem — FFI int_array/float_array boundary punning
  (codegen_call.c FFI switch lacks array cases; needs element-unbox
  bridge in runtime).
- #1384 — sp_net blocking-accept exits 143 on SIGTERM (SA_RESETHAND +
  accept signal race). lib/sp_net.c. Needs ppoll/self-pipe + re-arm.

## Fixed-in-C / already work -> close with evidence
- #1365, #1366, #1375 — old self-host inference mechanisms absent in C;
  repros compile+run correctly.
- #1382, #1386, #1387 — repros compile+run correctly on C.
- #1385, #1371 (PRs) — feature already present on C (uncalled-method
  param back-prop; pointer-array holder specialization).
- #1390 (PR) — self-host-only artifact; C --emit-symbol-map already
  emits populated fields.

## Inconclusive
- #1394 — string ivar collapse to mrb_int (-Wint-conversion). Only the
  full spinel_kit+tep multi-gem bundle triggers it; standalone + minimal
  compile clean. Re-test against the real bundle.
- #1351 — flutie hang not reproducible on C (different codebase). A
  residual unrelated type bug (nil recv into String#split in a
  rails-only method) appears instead.

## instance_exec extension PRs — DONE (6 commits, unpushed)
- #1377 implicit-self — 4942d1b
- #1378 numbered(_1/it) / autosplat / kwargs — 63623c2
- #1374 mixed local/ivar/literal trampoline args — 1b01917
- #1376 break/next in direct splice (return already worked) — 56b3ee7
All porting the PR test files (legacy impl didn't apply); diffed vs ruby
4.0.4, committed with .expected (version-independent; numbered uses `it`).
Gates each: test/bench/optcarrot 59662/clang. The analyze_fail kwargs
rejections (missing-required, kwrest) run under legacy, not the C path; C
degrades (binds default/0) rather than the legacy diagnostic.

## (old) Missing features (instance_exec extension PRs — port deliberately)
Probed all 4 PRs' test files against C master (the legacy PR code doesn't
apply; tests are the spec, diffed vs CRuby). Most sub-cases ALREADY PASS on
C: const-recv, nested, top-level-method, splat, spliced-types, and
nonlocal-RETURN. Remaining real gaps:
- #1377 implicit-self — DONE (4942d1b). Only gap in #1377; rest already
  worked.
- #1374 mixed_args — trampoline `instance_exec(x, @base, 7, &b)`: block
  params type-mismatch (poly slot <- int). Per-kind arg eval + param typing.
- #1378 numbered (`_1`/`it`) — analyze instance_exec block-param arm doesn't
  type NumberedParametersNode from call-site args (codegen arm exists).
- #1378 autosplat — single array arg destructured across N params: not done.
- #1378 kwargs — keyword block params matched to `key:` call-site args:
  returns 0 (unbound). Plus 2 analyze_fail tests (missing-required / kwrest).
- #1376 break/next — non-local control flow through the spliced body
  (return already works). Riskiest; direct-form only.

## RFC / questions / meta (need product decision, not a fix)
- RFCs: #1334 symbol map, #1335 Kernel#caller, #1336 sampling profiler,
  #1338 codegen diagnostics, #1381 doctor subcommands, #925 Gemfile,
  #1333 (PR) pthread Ractor.
- Questions: #1212 release timing, #1261 AOT limits doc, #282 "30x
  slower" (about the OLD Ruby interpreter — obsolete now master is C),
  #1280 RubyVM::MemoryView gap.
- Known/tracked: #1302 analyze OOM (governor done), #1314 GC UAF (needs
  blog repro), #1288 ...-forwarding, #1157 ARGF, #1291 unhandled Prism
  nodes, #1367 require-path inference identity.
- Parked: #773 volatile-pointer, #906 catch/throw (setjmp decision).
- #1380 (PR) README ecosystem list — docs, mergeable as-is.
