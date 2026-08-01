# spinel-ebpf patches against matz/spinel

このファイルは [ADR-004](../../docs/adr/ADR-004_spinel_modification_policy.md)
に基づき、spinel-ebpf プロジェクトが matz/spinel に当てている改変を一元管理する。

## 改変方針 (要約)

- **opt-in のみ**: 環境変数や CLI flag で明示的に有効化したときだけ新挙動が走る
- **追加のみ**: 既存挙動の変更は別 ADR で正当化
- **upstream PR を目指す**: 安定 hook は本家に PR

## ベース (upstream) と移行 (ADR-009)

- **upstream master**: `83d1315d` (2026-08-13 同期、E436、**origin/master tip**。直前は `53feb9df`)
  - **一時ビルド修正なし** — pristine `83d1315d` が `spnlbuild` (gcc 14) でクリーンにビルドできることを実測。
    ⇒ **footprint = 恒久 footprint = `src/codegen.c` の Patch 1 単一ファイルのみ** (E350 から 3 回連続)。
- **大移行 (2026-06-17, [ADR-009](../../docs/adr/ADR-009_c_compiler_integration_emit_ir.md))**:
  upstream は `cb23cc6 → 5a9bd8d2` で **Ruby 実装 (`spinel_analyze.rb` /
  `spinel_codegen.rb`) を C へ全面書き換え** (+699 commits)。旧 Ruby パイプラインは
  `legacy/` に凍結 (regression oracle)、本流は単一 C バイナリ `build/spinel` に。
  spinel-ebpf co-process はこの **C コンパイラが吐く `.ast` / `.ir` を読む** ように移行。
  旧 `cb23cc6` 系列の patch のうち **Ruby codegen を触っていたもの (旧 Patch 1 / Patch 4)
  は C codegen へ再実装**、C 化で不要になったものは廃止 (下記)。
- **同期履歴 (旧)**: `409d73f → 0620060` (+783, E098)、`0620060 → cb23cc6` (+168, E162)、
  `cb23cc6 → 5a9bd8d2` (+699, ADR-009)、`5a9bd8d2 → f5e1a22a` (+231, E250)、
  `f5e1a22a → 2d7dbb09` (+346, E256)、`2d7dbb09 → 5a247bb4` (+1199, E278)、
  `5a247bb4 → d746d051` (+538, E350、**一時ビルド修正を drop**)、
  `d746d051 → 53feb9df` (+484, E389、**手動 re-plant** + upstream の FFI 記号改名に追随)、
  `53feb9df → 83d1315d` (+381, **E436**、本同期、rebase クリーン・追随 0 行・fixture drift 0)。
- **E250 同期 (2026-06-21)**: 境界戦略の検証。footprint は **`src/codegen.c` の 1 ファイル (Patch 1) のみ**
  なので、231 commits 先の upstream に Patch 1 (81 行) を **手動 re-plant** (位置だけずれて context が
  合わず `git apply` 不可、ロジックは無改変)。pristine `f5e1a22a` と patched で **`make test` が同一
  (991 pass / 1 fail)** ⇒ Patch 1 は upstream 挙動に byte-identical (env-gate)。stage2 MATCH=90。
- **E256 同期 (2026-06-28)**: `f5e1a22a → 2d7dbb09` (今日時点の upstream tip、+346)。Patch 1 (48 行) は
  anchor (`emit_method_cname` / `emit_method_signature` / `emit_method`) 健在で **`git apply` クリーン**
  ⇒ `git rebase --onto 2d7dbb09 f5e1a22a` で再 plant (upstream codegen.c は +346 で進化したが Patch 1
  追加 48 行は byte-identical)。pristine `2d7dbb09` と patched で **`make test` が同一
  (1113 pass / 1 fail / 1 error)** ⇒ Patch 1 は byte-identical (env-gate)。host unit 558 / golden 90 /
  stage2 MATCH=90。`1 error` (`fiber_kill`) は upstream のテストで **pristine でも同様に error** = 本 patch 無関係。
- **E278 同期 (2026-07-15)**: `2d7dbb09 → 5a247bb4` (**+1199、origin/master tip**)。
  - **Patch 1**: `git apply` は position drift で失敗する (upstream が `emit_method_cname` と
    `emit_method_signature` の**間に**新関数 `emit_poly_iter_obj_normalize()` を挿入したため) が、
    **`patch -p1 --fuzz=3` で機械的に載る** (手動移植不要)。追加 **48 行は前同期と byte-identical**
    (SHA `006f006e…`、**削除 0 = 純粋 additive**)。旧 `c-emit-ir` は `backup-c-emit-ir-3da6f09f` に退避。
  - **一時ビルド修正 (1 行)**: tip は gcc 14 で `src/codegen_call.c:2881` が permerror。
    `return;` → `return 1;` を carry (詳細と drop 条件は下記「一時ビルド修正」節)。
  - **byte-identity 検証**: pristine (tip + 1 行修正のみ) と patched (+ Patch 1) で
    **テストが完全一致 (1793 pass / 1 fail / 5 error、1799 stamps、非 pass の 6 件も同一)**。
    加えて **emit C を env 未設定で 60 テスト直接 diff → 60/60 完全一致**、
    positive control (env set で `extern` 化 / pristine では env 不活性) も実施。
    host unit 558 runs / 2977 assertions / 0 fail、golden PASS=90 / DIFF=0、stage2 MATCH=90 / MISMATCH=0。
  - **bisect の教訓 (記録)**: 当初 tip がビルド不可だったため `git bisect run` で
    **first bad = `c2838254`** (`lib/sp_bigint.c` の `sp_sprintf` 暗黙宣言 → `-Wint-conversion`) と
    特定したが、**これは tip の原因ではなかった**。upstream の壊れ方が**非単調**で、
    12:31 に破壊1 混入 → 12:47 に破壊2 (`codegen_call.c`) 混入 → **14:38 に破壊1 のみ修正**
    (`3e8d8fda` "runtime: forward-declare sp_sprintf in sp_bigint.c"、**issue #2063 CLOSED**) された結果、
    tip に残っていたのは**破壊2 だけ**だった。`3e8d8fda` は `dd634028` の祖先であり、
    pristine tip の実ビルドログにも `sp_sprintf` の言及は 0 件。**first bad ≠ 現在の原因**であり、
    bisect 結果を現状の因果として読んではいけない (「今なお何が壊れているか」は tip で直接確認する)。
  - **upstream CI が緑な理由**: `common.mk` の `CFLAGS` に **`-Wno-all`** があり、`-Wreturn-mismatch` が
    **clang 19 でも gcc 13 でも warning 止まり**。落ちるのは **gcc 14+ だけ** — gcc 14 が同診断を **permerror**
    に昇格させ `-Wno-all` では抑止できないため (L-method-001)。「gcc/clang の非対称」ではなく **gcc 14+ 固有**の
    permerror 挙動が原因で、upstream CI (clang or gcc≤13) では顕在化していない。
- **E350 同期 (2026-07-21)**: `5a247bb4 → d746d051` (**+538、origin/master tip**、"Merge PR #3134: fix analyze
  fixpoint convergence")。
  - **Patch 1**: E278 の diff (`5a247bb4 → f75e59fa` の `src/codegen.c`) が **`git apply` クリーン**で載る
    (anchor 健在)。追加 **48 行は E278 と byte-identical** (backup `backup-c-emit-ir-97a5e450` の added-lines と
    `diff` 空、**削除 0 = 純粋 additive**)。旧 `c-emit-ir` は `backup-c-emit-ir-97a5e450` に退避。
  - **一時ビルド修正 (1 行) を drop**: tip の `src/codegen_call.c` の `emit_class_new_call()` は既に
    `return 1;` を持ち、gcc 14 (`spnlbuild` debian:trixie) で `bin/spinel` がクリーンにビルドできる。
    E278 で carry していた 1 行は不要になったので削除。**恒久 footprint (1 ファイル) と現 footprint が一致**。
  - **byte-identity 検証**: env 未設定で golden PASS=**115** / DIFF=0、stage2 MATCH=**115** / MISMATCH=0
    (in-process codegen が新 spinel object を link しても .bpf.c は不変)。positive/negative control
    (env unset → body、`SPINEL_EXTERN_METHODS=helper` → `extern mrb_int sp_helper(...)` + body 0 個) も実施。
    host unit 全 pass、E2E probe build (`audit_http.rb --build`) 成功。
  - **fixture の進化**: 123 fixture 中 **33 の `.ast`/`.ir` が変化** (上流 analyze の詳細化、例 BlockNode に
    `S <id> locals <name>` 追加)。**codegen 出力 (.bpf.c) には影響しない** (golden 不変) = 良性の上流進化。
  - **`make test` の唯一の失敗** (`rbs-seed-test`: "pinned container: C did not compile") は Patch 1 を外した
    pristine でも同一に再現 = **upstream / container-env 起因**、本 patch 無関係 (E278 の rbs-seed-test 事象を継承)。
  - **build fix drop の意義**: [clean-upstream-boundary-milestone] で目指した「境界は `src/codegen.c` の Patch 1
    のみ」に **一時 carry を挟まず** 戻った初の同期。
- **E389 同期 (2026-08-01)**: `d746d051 → 53feb9df` (**+484、origin/master tip**、"Merge #3492: render a
  fixed-offset Time's civil fields in strftime and iso8601")。
  - **Patch 1**: `git apply` / `patch --fuzz=3` とも **失敗** → **手動 re-plant** (E250 と同じ形)。原因は
    anchor 周辺の upstream 変化 2 つ: (a) `emit_poly_iter_obj_normalize` と `emit_method_signature` の**間に**
    新関数 `pf_yield_ty()` が挿入され、間の**空行も消えた** (hunk 1 の context が全滅)、(b) `emit_method` の
    冒頭が `if (s->cs_synth)` から proc-form の `yield` 状態退避 (`g_yield_proc_ref` / `g_yield_slot_ty`、#3399)
    に変わった (hunk 2 の context が全滅)。**ロジックは無改変**で、挿入位置は同じ意味の場所
    (`emit_method_signature` の直前 / `emit_method_signature` 本体の先頭 / `emit_method` 本体の先頭)。
    追加 **48 行は E350/E278 と byte-identical** (added-lines SHA-256 `bc9d05f1…`、**削除 0 = 純粋 additive**)。
    旧 `c-emit-ir` は `c-emit-ir-e350-backup` に退避。
  - **依存 API はすべて健在**: `mc()` (`src/codegen_util.c`)、`buf_printf`/`buf_puts`、`Scope` の
    `class_id`/`name`/`ret`/`nparams`/`pnames` (`src/compiler.h`)、`TY_INT`。
  - **一時ビルド修正は不要** (pristine が gcc 14 でクリーンビルド)。
  - **byte-identity 検証**: pristine (`53feb9df` 素) と patched で **`make -k test-run` が完全一致**
    (**PASS=2587 / FAIL=0 / ERR=7 / 2594 stamps**、非 pass の 7 件も同一集合)。加えて **env 未設定で
    `spinel -S --no-line-map` の出力を `test/*.rb` 200 本で直接 diff → 200/200 完全一致**。
    positive/negative control (env unset → `static mrb_int sp_helper(...)` + body、
    `SPINEL_EXTERN_METHODS=helper` → `extern mrb_int sp_helper(mrb_int lv_x);` + body 0 個、
    **pristine + env set は不活性**) も実施。
  - **`make test` の非 pass 7 件は pristine と同一** = upstream / container-env 起因、本 patch 無関係:
    `rbs-seed-test` (FAIL、upstream issue **#3203** "override-family return seed split decl/call-site repr"。
    E350 の "pinned container: C did not compile" とは別事象に入れ替わっている) +
    `block_tail_prelude_inside` / `ffi_struct` / `fiber_kill` / `monitor_reentrant` / `thread_condvar` /
    `thread_kill_raise` / `thread_mutex` (ERR)。
  - **spinel-ebpf 側に 1 件の追随が要った (upstream の API 改名)**: upstream が binary-safe FFI 返却
    (`:binstr`) の byte 数公開変数を **`sp_net_bin_len` → `sp_ffi_bin_len`** に改名し、宣言を
    `lib/sp_net.h` から **`lib/sp_alloc.h`** に移した (net 専用から任意 FFI へ一般化)。spinel-ebpf 所有の
    `src/runtime/sp_net_ext.c` (ADR-010 の externalize 先) が旧名を参照していたため **compile error** に。
    upstream `lib/sp_net.c` と同じ形 (`#include "sp_alloc.h"` + 新名) に追随。**upstream ソースは無改変**
    (改名に合わせただけ)。実機で NUL 入りペイロードの `:binstr` echo が `len=8` を返すことを確認。
  - **in-process codegen (ADR-011 Stage 2) の適応は 0 行**: `src/codegen_c/spinel_upstream_contract.h` が
    束ねている `Compiler` / `Scope` / `ClassInfo` / `LocalVar` の読み取り面は +484 でも無傷で、
    新 upstream object への link がそのまま通った (= 1 ファイルに集約した設計が効いた)。
  - **fixture の進化**: 123 fixture 中 **33 の `.ast` が変化** (合計 **+122 行、削除 0**)。中身はすべて
    `StringNode` への `I <id> fzl 1` (frozen literal フラグ) の追加。**`.ir` は 1 本も変化せず**、
    codegen 出力 (.bpf.c) も不変 (golden 115/DIFF 0) = 良性の上流進化。

- **E436 同期 (2026-08-13)**: `53feb9df → 83d1315d` (**+381、origin/master tip**、"Keep a minor cycle off
  the old string list when workers sweep")。
  - **Patch 1**: anchor (`pf_yield_ty` 直後 / `emit_method_signature` / `emit_method` 冒頭) が健在で
    **`git rebase --onto` がクリーンに通過** (E256 以来の手動 re-plant 不要)。追加 **48 行は
    E389/E350/E278 と byte-identical** (added-lines SHA-256 `bc9d05f1…`、削除 0 = 純粋 additive)。
    旧 pin `22bf2181` は `backup-c-emit-ir-22bf2181` に退避。
  - **一時ビルド修正は不要** (pristine が gcc 14 でクリーンビルド、3 回連続)。
  - **byte-identity 検証** (repos/spinel の同一内容 tree で実施、`src/codegen.c` は LOCKSTEP 一致を確認):
    pristine (`83d1315d` 素) と patched で **`make test -k` の stamp が完全一致
    (2887 stamps / 2879 PASS、非 PASS 8 は同一集合)**。非 pass は `rbs-seed-test` (FAIL、upstream issue
    #3203 継続) + `block_tail_prelude_inside` / `ffi_struct` / `fiber_kill` / `monitor_reentrant` /
    `thread_condvar` / `thread_gc_start` (E389 に無かった新顔、pristine でも ERR) / `thread_kill_raise` /
    `thread_mutex` (ERR) = upstream / container-env 起因、本 patch 無関係。
    env-gate control: env 未設定で `sp_helper` は static 定義 (extern 0/body 2)、
    `SPINEL_EXTERN_METHODS=helper` で extern 宣言 + body 0。
  - **spinel-ebpf 側の追随は 0 行** (E389 の `sp_ffi_bin_len` 改名のような API 変更なし)。
    in-process codegen (`spinel_upstream_contract.h` の読み取り面) は +381 でも無傷、
    新 upstream object への link がそのまま通った。
  - **fixture drift 0**: 187 fixtures を再生成して `.ast` / `.ir` とも 1 バイトも変化なし
    (E389 の fzl 追加のような良性 drift すら無し)。
  - **ゲート実測**: golden **PASS=146 / DIFF=0 / refused=34**、stage2 **MATCH=146 / MISMATCH=0 /
    refusals CHECKED=34**、host unit 45 ファイル全 pass、form_verify **IDENTICAL=3 / DRIFT=0**。
  - **公開フォーク `repos/spinel` (yuskesh/spinel) も同日同 base に同期・push 済** (master ff +
    c-emit-ir re-plant、旧 published は `backup-c-emit-ir-b6a440b3`)。
  - 上流 +381 の中身 (GC 世代別マーキング機構・poly/boxed 面の大拡張・csv package・socket
    keepalive 定数・compile 時 NoMethodError 等) は E436 Edoc 参照。

## 現在当てている patch (branch `c-emit-ir`, base `83d1315d`)

> **[ADR-011 Stage 2] 恒久的な機能 hook は Patch 1 の 1 つだけ**(env-gate、未指定時の出力は
> upstream と byte-identical):
> - **Patch 1** `SPINEL_EXTERN_METHODS`(transparent dispatch opt-in 時のみ)。`src/codegen.c` +48 行。
>   **これが唯一の恒久 hook**であり、upstream PR 候補。境界戦略 (ADR-004) の対象はこれ。
> - **~~一時ビルド修正 (1 行、`src/codegen_call.c`)~~ は E350 (2026-07-21) で drop 済**。upstream tip
>   `d746d051` の `emit_class_new_call()` は `return 1;` を持ち gcc 14 でクリーンにビルドできる。
>   ⇒ **現 footprint = 恒久 footprint = `src/codegen.c` の Patch 1 単一ファイルのみ** (詳細は下記「一時ビルド修正」節)。
> - **Patch A(`--emit-ir`)は ADR-011 Stage 2 で廃止**。eBPF codegen が `Compiler` 構造体を
>   in-process で直読するようになり(`build_ir_text` を spinel-ebpf 側 `cc_build_ir_text` に移設)、
>   upstream の IR text シリアライザは不要になった。`build_ir_text` / `ir_emit_parent` /
>   `SPINEL_EMIT_IR` ブロック / `--emit-ir` flag を削除。`ir_is_free_fn` / `ir_is_verdict_handler`
>   は Patch 1 が使うため存置。
> - **Patch 2(sp_net)は E193 で externalize 済み → 廃止**。`lib/sp_net.{c,h}` は **100% upstream 逐語**。
>
> テストは **1793 pass / 1 fail / 5 error (1799 stamps)**。env 未設定で **pristine (tip + 一時ビルド修正のみ)
> と完全一致**、E278 で確認。非 pass の 6 件はいずれも本 patch 無関係の upstream / container-env 起因
> (pristine で同一に再現): `string_each_char_returns_self` (fail) は container locale 由来の UTF-8 inspect 差、
> `fiber_kill` / `ffi_struct` / `thread_condvar` / `thread_kill_raise` / `thread_mutex` (error) は
> upstream のテスト。
>
> **既知事象: `make test` は pristine でも完走しない** (tip 移行とは直交、`f9058dd3` でも同一に再現)。
> upstream の `rbs-seed-test` が `pinned container: C did not compile` で FAIL するため。
> 実体は **spinel が生成した C** が gcc 14 で通らないこと: `static sp_PolyArray * cst_EMPTY = NULL;`
> に対し `cst_EMPTY = sp_IntArray_new();` を代入 → `-Wincompatible-pointer-types` = permerror
> (clang では warning)。**これも upstream 側の課題**で spinel-ebpf の patch とは無関係。
> ⇒ 上記の数値は Makefile と同じ集計ロジック (`build/test-results/*.ok` の `^PASS`/`^FAIL`/`^ERR` を
> `grep -l` で数える) を、`TEST_TARGETS` + `PKG_TEST_TARGETS` を `make -k` で直接駆動して取得している。

### 一時ビルド修正 (Patch ではない / upstream バグの一時キャリー、E278〜E350 で drop)

> **✅ 2026-07-21 (E350) で drop 済**。upstream tip `d746d051` の `emit_class_new_call()` が
> `return 1;` を持つようになり、`spnlbuild` (gcc 14) で `bin/spinel` がクリーンにビルドできることを実測。
> E278 で 1 週間の carry を予定 (再評価 2026-07-22) していたが、それより前に upstream 側で解消された。
> **現 footprint = 恒久 footprint = `src/codegen.c` の Patch 1 単一ファイルのみ**。以下は経緯の記録 (歴史)。

> **これは spinel-ebpf の機能 hook ではなかった。** upstream の素のバグを、upstream が直すまでの間だけ
> ローカルに持っていたもの。ADR-004 の「境界」議論の対象外 (機能を足していないため)。

- **対象**: `src/codegen_call.c:2881`、`static int emit_class_new_call()` 内
- **改変**: `return;` → **`return 1;`** (1 行、これ以上でも以下でもない)
- **なぜ必要**: tip `5a247bb4` は **gcc 14 でビルドできない**。値を返す関数の裸 `return;` は
  gcc 14 が `-Wreturn-mismatch` を **permerror** 扱いにするため。`spnlbuild` (debian:trixie) の
  `cc` は gcc 14 なので、これが無いと `make all` が通らない = 全ゲートが回せない。
- **なぜ `return 1;` が正しい**: 呼び出し側 (`src/codegen_call.c:8748`) は
  `if (emit_class_new_call(c, id, b)) return;` と **戻り値で「処理済みか」を分岐**しており、
  同関数内の他の「処理済み」パスは **すべて `return 1;`** (7 箇所)。この 1 箇所だけが outlier。
  値を返さず関数末尾に落ちると戻り値が不定になり、「処理済み」判定が壊れて **二重 emit** の恐れ。
  つまり単なる警告潰しではなく **実バグの修正**。
- **なぜ upstream で顕在化していない**: `common.mk` の `CFLAGS` に **`-Wno-all`** があり、同診断は
  **clang 19 でも gcc 13 でも warning 止まり**。落ちるのは **gcc 14+ だけ** (permerror に昇格)。
  「gcc/clang 非対称」でなく **gcc 14+ 固有**の挙動 (L-method-001)。
- **upstream 状況**: 混入は `31eca166c` (2026-07-11 12:47、"eighth conformance wave, part four")。
  **issue / PR は無い** (2026-07-15 時点)。**今回は報告しない方針** (ユーザー決定)。
- **drop 条件 (充足済)**: upstream が `return 1;` を入れたら削除、という条件を **E350 (2026-07-21) で充足**。
  tip `d746d051` の `src/codegen_call.c` は `emit_class_new_call()` 内に `return 1;` を持つ (実測、gcc 14 で
  `bin/spinel` クリーンビルド) ため **carry を削除**。再評価予定日 (2026-07-22) より前に upstream で解消された。
- **参考**: 同時期の姉妹バグ `lib/sp_bigint.c` の `sp_sprintf` 暗黙宣言 (`-Wint-conversion`) は
  **upstream で既に修正済み** (`3e8d8fda`、issue **#2063 CLOSED**)。よって carry 不要。
  この 2 件は混入と修正の順序が入れ替わっており (非単調)、bisect の first bad
  (`c2838254`) は **tip の原因ではない**。E278 の教訓を参照。

### Patch A: `--emit-ir` / `SPINEL_EMIT_IR` — C コンパイラ → 旧 SPINEL-IR v1 シリアライザ

- **目的**: ADR-009 移行の中核。co-process (`src/spinel_ebpf/*`) は依然 **SPINEL-IR v1**
  (analyze→codegen の join point の text 形式) を読む。C コンパイラの `Compiler` 構造体
  (scopes[] / classes[] / ntype[] …) を旧 Ruby analyzer が吐いていた IR text に書き戻す。
- **対応**: [ADR-009](../../docs/adr/ADR-009_c_compiler_integration_emit_ir.md)
- **改変箇所**:
  - `src/codegen.c`: `build_ir_text(Compiler *c)` (`@meth_*` / `@cls_*` / `SN`/`ST` locals /
    `@needs_*` flags を emit)、`ir_is_free_fn` / `ir_is_verdict_handler` / `ir_emit_parent` helper。
    `codegen_program` 末尾で `getenv("SPINEL_EMIT_IR")` → `build_ir_text` → ファイル出力。
    正規化: `initialize`→`void` / poly・unknown param→`int` / poly・unknown return は
    verdict-attach handler なら `int` 他は `nil` / poly ivar→`int` / class separator は
    旧形式 (`;` / `%7C` percent-encode) に合わせる。
  - `src/main.c`: `--emit-ir` flag (→ `SPINEL_EMIT_IR` env、`--emit-rbs` を mirror)。
  - AST は既存 `--dump-ast --no-line-map` を流用 (co-process の AST parser がそのまま読む)。
- **breadth 検証**: 97 fixture を旧 Ruby IR と diff し **90 byte-identical + 7 cosmetic**
  (tracing handler の discarded-return 表現差のみ、機能等価)。
- **互換性**: `SPINEL_EMIT_IR` 未設定で出力なし (通常 codegen に影響しない)。
- **適用日**: 2026-06-17 (commits `5d940260` / `87b5d239` / `05b2f8c7` / `a019d126`)

### Patch 1: `SPINEL_EXTERN_METHODS` — external method definitions (汎用 hook)

- **目的 (汎用)**: 指定した top-level method を **body 無しの `extern` 宣言**にし、定義を別 TU に
  委ねる「external method definitions」機能。separate compilation / 手書き C 実装 / 別ランタイム
  への委譲に使える汎用機構で、**eBPF 固有のロジックを含まない**。spinel-ebpf はこれを transparent
  native→eBPF dispatch (E028) に使う (`<base>_dispatch.c` が `bpf_prog_test_run` で解決)。
- **対応 Edoc**: [E023 (設計)](../../docs/experiments/E023_native_to_ebpf_dispatch_design.md),
  [E028 (実装)](../../docs/experiments/E028_transparent_native_to_ebpf_dispatch.md),
  [E251 (汎用化)](../../docs/experiments/E251_patch1_generalize_extern_methods.md)
- **改変箇所** (`src/codegen.c`、**48 行・追加のみ**):
  - `toplevel_method_p(s)` / `extern_method_p(s)`: top-level method かつ `$SPINEL_EXTERN_METHODS`
    (comma 区切り) に name が居るか。汎用名・eBPF 非依存。
  - `emit_method_signature`: 対象を **canonical value ABI** で `extern <rt> sp_<name>(mrb_int lv_...)`
    出力 (rt = concrete int なら `mrb_int`、他は `void`)。**分析依存の自然シグネチャでなく固定 ABI** に
    するので外部定義側 (dispatch shim) が whole-program 推論を再現せず link 整合できる。
  - `emit_method`: 対象の body emission を skip (prototype loop が extern 宣言を担う)。
- **E251 汎用化 (2026-06-21)**: eBPF 固有だった `ir_is_verdict_handler` (attach prefix 一覧) と
  `ir_extern_ret_ctype` を撤去。verdict-handler は dispatch 対象 (非 attach method) では常に dead で、
  撤去しても挙動不変。命名/コメントを upstream-neutral 化 ⇒ **upstream PR 可能な汎用 hook に**。79→48 行。
- **挙動差分**: 未設定で完全に既存挙動 (byte-identical)。`SPINEL_EXTERN_METHODS="foo,bar"` 設定時のみ
  `foo`/`bar` の body が消え `extern mrb_int sp_foo(...);` 宣言になる。
- **検証**: `make test` 991/1 (env 未設定で pristine と同一)、`14_calls --ebpf-dispatch` で
  `quad(3)=12` / `six_times(5)=30` (extern 宣言 + dispatch shim link + BPF 実行)、golden 90 / stage2 90。
- **適用日**: 2026-06-17 (commit `099a1d0c`、旧 Ruby 版 Patch 1 を C codegen へ移植)。
  **2026-06-21 (E250)**: base `5a9bd8d2 → f5e1a22a` 同期に伴い `src/codegen.c` の新位置
  (emit_method_cname / emit_method_signature / emit_method / ty_tag_into 付近) へ手動 re-plant。
  ロジック無変更、footprint は引き続き `src/codegen.c` の 1 ファイルのみ。
  **2026-06-28 (E256)**: base `f5e1a22a → 2d7dbb09` 同期 (今日時点の upstream tip、+346)。anchor 健在で
  `git apply` クリーン、`git rebase --onto` で再 plant (手動 re-plant 不要)。ロジック・footprint 無変更。
  **2026-07-15 (E278)**: base `2d7dbb09 → 5a247bb4` (+1199)。position drift で `git apply` 不可、
  `patch -p1 --fuzz=3` で機械的に載る。**2026-07-21 (E350)**: base `5a247bb4 → d746d051` (+538)、
  `git apply` クリーン。**2026-08-01 (E389)**: base `d746d051 → 53feb9df` (+484)。upstream が anchor 間に
  `pf_yield_ty()` を挿入 (+ 空行削除) し `emit_method` の冒頭も変わったため `git apply` / `--fuzz=3` とも
  不可 → **手動 re-plant** (E250 と同型)。**追加 48 行は毎回 byte-identical** (SHA-256 `bc9d05f1…`)。

### Patch 2 (廃止): `lib/sp_net.{c,h}` HTTP server 向け薄 shim → E193 で externalize

- **廃止理由 (ADR-010 / E193, 2026-06-18)**: sp_net 拡張は upstream ソースに置く必要がなく、FFI シンボル名で
  リンク解決されるため spinel-ebpf 所有 TU に移せる。`lib/sp_net.{c,h}` を **100% upstream 逐語に revert**
  (`git diff origin/master -- lib/sp_net.c lib/sp_net.h` = 空)。これで upstream への改変は Patch A + Patch 1 のみ。
- **移動先**: `src/runtime/sp_net_ext.c`(spinel-ebpf 所有、`bin/spinel-ebpf` の `extra_sources` で全 binary にリンク、
  `-lutil` 既存)。中身: `sp_net_read_line(+_reset/_buffered_remaining)` / `file_size` / `sendfile` /
  `epoll_*` / `reap_nb` / `autoreap_on` / `sp_pty_*` / `sp_ws_*`(E180/183/184/186/190)。
- **dormant フックの解消**: upstream の `sp_net_close`/`sp_net_recv_some` を編集する代わりに、新 TU に
  **別名の read_line 対応版** `sp_net_rl_close`(reset+close)/ `sp_net_rl_recv_some`(buffered remainder を
  drain してから upstream `sp_net_recv_some` に委譲、E187 の `/api/exec` hang 修正を維持)を用意。read_line を
  使う server(M001/M002/M003*/observability/sendfile_demo/*)は `sp_net_rl_*` を FFI。**upstream 関数は無改変・逐語**。
  `epoll_wait_one` の term 判定は static `sp_net_term_flag` でなく公開 API `sp_net_shutdown_requested()` を使用。
- **検証 (E193)**: `make test` 893 pass、host unit 全 pass、M001 GET 200/health/404、serve_deck_term の
  `POST /api/exec`(over-read body → `rl_recv_some` drain)が hang せず 0.003s 応答、graceful shutdown は
  upstream 逐語(E192 で C ハーネス検証済)。
- **対応 Edoc**: E193(externalize)。元機能の Edoc: E033/E098/E180/E183/E184/E186/E187/E190

### Patch 4 (廃止): `SPINEL_EBPF_SOFT_FAIL`

- 旧 Ruby codegen で `sk.snd_cwnd += v` (CallOperatorWriteNode) / `XDP::PASS`
  (ConstantPathNode) の native 側 `exit(1)` を warn+no-op に降格していた hook。
- **C コンパイラでは不要**: C analyzer/codegen は解決不能 receiver / 未知 ConstantPath を
  native 側で `exit(1)` せず通すため、`bin/spinel-ebpf` は C path で本 env を設定しない。
  eBPF lowering は従来どおり spinel-ebpf の `codegen_bpf.rb` が担当。
- ([E070](../../docs/experiments/E070_receiver_dot_accessor.md) /
  [E072](../../docs/experiments/E072_module_style_constants.md) の表現は C path でも動作。)
