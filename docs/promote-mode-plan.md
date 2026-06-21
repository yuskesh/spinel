# `--int-overflow=promote` 完全動作計画

## ゴール

`SPINEL_INT_OVERFLOW=promote` (`-DSP_INT_OVERFLOW_MODE_PROMOTE`) で:
- `make test` 全 pass
- `make bench` 全 pass
- `make optcarrot` PASS + checksum 59662 (Makefile から `--int-overflow=wrap` pin 除去)

## 現状 (master HEAD, 2026-06-19 再計測 — 旧 `bc7e1a1` 表は obsolete)

| Target | promote 結果 |
|---|---|
| `make test SPINEL_INT_OVERFLOW=promote`(front-end も cc も promote driven) | **966/966 pass** |
| `make bench` 相当 | **57/57 pass** |
| optcarrot (`-DSP_INT_OVERFLOW_MODE_PROMOTE`) | **コンパイル成功 + checksum 59662** |

→ 5月の error バケット(bit-ops/index/FFI 等、optcarrot 72 errors)は **既に全て解消済み**。境界変換は実装済み。default gate(966 + optcarrot 59662 + bench 57)は `g_promote_mode` gate により不変。

## 進捗 (2026-06-19)

- **bigint `**`/`<<`/`>>` のコード生成バグ修正**(92e8d6dc): `**` は codegen 未配線で int path に落ち、shift は `TY_BIGINT` 推論されず "unsupported call"。runtime の `sp_bigint_shl/shr` も `to_int` で 64bit に truncate していた。3 演算とも任意精度に。
- **ブロックループ蓄積の bigint 昇格**(9295160d): `infer_bigint_loop_locals` を `times`/`each`/`upto`/`downto`/`step`/`loop`/`each_with_index` ブロックに拡張(`g_promote_mode` gate)。`f=1; 25.times{|i| f=f*(i+1)}` が promote で MRI 一致。付随して emit_op_assign の **captured-cell path が int 専用**だったバグ(bigint cell に `sp_int_mul` で truncate)を修正。
- **Makefile promote テスト配線**: `make test SPINEL_INT_OVERFLOW=promote` が front-end に `--int-overflow=promote`、cc に `-D...PROMOTE` を渡すように(以前は test list を filter するだけ=promote codegen path 未検証)。promote-only テストは `test/promote_*.rb` 命名で raise/wrap から除外。**promote 全 suite 966/0/0**。

## 残る本質的 gap (2026-06-19 更新)

昇格機構は **`g_promote_mode` 下の `infer_bigint_loop_locals`**(while + block loop の self-ref multiply 検出)。runtime macro には promote arm が無い(`sp_int_add` は overflow で raise)ので、**静的に "無限増殖" と判定できる local だけ**が bigint 化される。

残り raise になるパターン:
- **関数引数経由**(`def mul(x,y)=x*y; mul(10**15,10**15)`)= **静的検出は原理的に不可能**。runtime promote が要る=legacy の全 int→bigint widen(下記「完成路」)。これが最後の本丸。
- **リテラル累乗 / bigint リテラル**(`10**30`, `100000000000000000000`)→ INT_MAX に飽和。**AST が int64 値しか保持しない**(IntegerNode "value" が int)ので parser 段で decimal 文字列を保持する別 feature が要る。promote 機構とは独立。
- `+`/`<<` の単純蓄積(`*`/`**` のみ self-ref 検出対象)。

既知の周辺バグ(promote 無関係・default でも再現): **`1.upto(N){ ... }` の no-block-param ブロックが body を drop**(`h=0; 1.upto(5){h+=1}; puts h` → 0)。upto codegen の別件。

## 採用方針: poly-based(SP_TAG_BIGINT)— 2026-06-19 matz 決定

legacy の「全 int → bigint widen」ではなく、**poly 値(`sp_RbVal`)に bigint を載せる**方向を採用(matz 判断)。理由: (a) `SP_TAG_BIGINT` 追加は promote と無関係に異種コンテナ/リテラル bug を直す独立価値がある、(b) 小さい int は unbox のまま=Ruby の fixnum→bignum と同じで legacy 全 bigint より実用的、(c) コア投資を bug 修正と共有。

### 進捗(landed)
- **Increment 1 (ce6995c8)**: `SP_TAG_BIGINT`(`v.p`=GC bigint)。`sp_box_bigint` + emit_boxed/unbox の TY_BIGINT arm + GC mark + poly consumer 全 arm(puts/to_s/inspect/class_name/to_i/to_f/numeric_p/eq/cmp/hash-key)+ codegen `emit_bigint_operand`(poly operand を `sp_poly_as_bigint` で narrow)。→ `[1,"x",big]` / `{k:big}` / bigint hash-key 動作。clang+ASAN 検証。
- **Increment 2a (48863f34)**: `sp_poly_add/sub/mul` が bigint operand を bigint 演算へ(全モード、`bigint+int`→0 bug 修正)+ **promote で int+int overflow→bigint 昇格**(`SP_INT_OVERFLOW_MODE_PROMOTE` #ifdef gate、default/wrap は wrap 維持)。→ **既に poly 型の値**は promote で runtime 昇格する。

### 残: Increment 2b(本丸 big-bang)= int→poly widen under promote
**既に poly な値**は 2a で昇格するが、関数引数など**静的に int な slot** は poly でないので overflow しても昇格できない。最後の山は promote モードで該当 int slot を **poly に widen**する analyze 変更。widen 先が bigint(legacy)でなく **poly** な点が違い: 小さい値は unbox のまま、overflow 時だけ 2a の機構で box。

**2026-06-19 試行の知見(revert 済、master は 2a の promote 972 のまま):**
- `promote_widen_arith_params`(analyze.c の re-narrow 後 hook): `def` メソッド param のうち `+`/`-`/`*`/`**` の直接 operand を poly 化(block param・非 DefNode scope は除外)→ **c2 = `def mul(x,y)=x*y; mul(10^12,10^12)` が 1e24 で動いた**(MRI 一致)。
- しかし promote suite が **972→947(25 err/3 fail)**に退行。退行の本質: codegen は型一致を前提に coercion を挿入しないので、widen した poly param が int slot に出会う所が全部境界化:
  - `mrb_int = sp_RbVal`(poly param → int local / cvar 代入、両方向)
  - `yield x` で poly を int block param へ(`n = poly`)
  - `sp_double(poly)`(別メソッド呼びの int param へ coercion 無し)
  - `mrb_int == sp_RbVal`(比較)/ cvar init が非定数
- 伝播 pass を増やす(infer_block_params/ivar/cvar/param/return を post-widen で回す)と境界が**ずれるだけ**(whack-a-mole)。
- **結論**: 2b は「全 poly↔int 境界 site で coercion を挿入する codegen sweep」(代入両方向 / yield / call-arg / 比較 / cvar)を要する真の big-bang。専用セッションで境界バケツを 1 つずつ。あるいは sound な uniform widen(全 int→poly + 境界 coercion 完備)。c2 が動く事は確認済みなので方式自体は正しい、残りは境界網羅。

**2026-06-19 第2試行(保守 gate 方式・revert 済、master=2a の 972):**
保守 gate を重ねて非退行を狙ったが収束せず、**根本 ripple** を特定:
- 実装した gate: (1) DefNode メソッドのみ・block param 除外、(2)`scan_param_use`=param の**全 read が `+`/`-`/`*`/`**` の直接 operand**の時のみ widen(yield/call-arg/index/比較/代入に1つでも出たら不可)、(3)`method(:sym)` で address-taken なメソッドは除外、(4)伝播は infer_write_types+infer_return_types のみ(full re-run は無関係 slot を撹乱)。
- これで block2/method/i960/bound_method 系は緑、**c2 維持**。だが pattern_pin/class_var_read/proc 等が残存。
- **根本原因 = return-type ripple**: param を poly 化すると arith 結果=メソッド戻り値が poly 化し、**その戻り値を int 文脈で消費する全 caller が壊れる**:
  - `^(double(7))` pin 比較(`mrb_int == sp_RbVal`)
  - `x = double(7)`(int local へ)/ `@@total = @@total + n`(int cvar へ、static init 非定数も)
  - `&blk` メソッドの signature 不一致(`undefined reference to sp_with_logging`)
- poly→int coercion で消す案は **promote を台無し**(昇格値を truncate)。正解は消費 slot 側も poly に widen(=full propagation)+ codegen が poly 非対応な site(cvar static init / pin)を coerce。
- **= caller call-site の tolerance 解析 か whole-program coercion sweep が必須**。gate だけでは「結果が puts 等 poly-tolerant 文脈でしか消費されないメソッド」まで絞らないと非退行にできず、それは c2 級しか残らない。
- 次セッション指針: (A) full propagation を入れ、sur面化する codegen site を 1 つずつ coerce(cvar poly init / pin poly 比較 / 各 int-consume を poly widen)。(B) または call-site tolerance gate(戻り値が int 文脈で消費されるメソッドは widen しない)を実装。試行コードは git 履歴(本 commit 直前)に無し=revert 済、本節が完全な再現手順。

**2026-06-20 第3試行 続行 = 境界 site の横展開で 831→859(+28)、4 commit、default 992/0/0 維持:**
full int→poly widen pass(79709207)を土台に、ERR(=C compile fail の境界 coercion gap)を emission-site 単位で潰した。各 fix は poly slot を検出したら rvalue を `emit_boxed`(node 版)/`emit_boxed_text`(型+テキスト版)で box する単一パターン。**「逐次 commit は緑を増やさない」という前回の悲観は誤り**で、単一 site のバケツ(cvar static / instance_exec / range.each)は直接複数テストを回復した(マルチ境界テストだけが横並び依存)。
- **cvar static init(298a0e56, +2)**: `static sp_RbVal cvar = default_value(TY_POLY)` が `sp_box_nil()`(関数呼び=非定数初期化子)。`{SP_TAG_NIL,0,{0}}` 定数 aggregate を emit(civ と同パターン)。default の genuine-poly cvar の latent bug も解消。codegen.c:2742。
- **instance_exec/instance_eval block-param(dbc46864, +16)**: lifted block の `lv_<param> = <arg>` 束縛が int arg を poly slot へ。numbered/requireds(直接 arg・auto-splat 要素・trampoline arg)/keyword の 4 経路で slot poly なら `emit_boxed`。codegen_call.c:5268+。
- **(a..b).each fusion(07e55f26, +3)**: loop var を int 直駆動。poly counter は fresh `mrb_int` で駆動し毎 iter `lv_i=sp_box_int(_tc)`(emit_for と同パターン)。codegen_call.c:10749。
- **masgn target boxing(694f3f89, +7)**: scalar-RHS local(+typed-zero rest default)/tuple-element local・cvar/typed-array destructure get/trailing rights get を box。codegen_stmt.c。
- **leading-splat masgn(91c022ce, +1)**: `*xs, last=[..]` の post-splat fixed target を temp 経由 box。splat_destructure 緑。codegen_stmt.c:3672。(promote 860 到達)
- **★決定的洞察**: widen pass は `scope->locals[].type`/`ivar_types`/`cvar_types` だけ書き換え、**node-type cache は更新しない**。ゆえに `comp_ntype(c, target_node)` は **widen 前のスカラ型(int 等)を返す**。slot が poly かは必ず `scope_local(scope, name)->type`(or ivar/cvar の type 配列)で判定すること。rvalue を box する型は `comp_ntype(value)`(真の値型)で取る。この non-update のおかげで rest default の typed-zero(`default_value(int)="0"`)も復元でき、`emit_boxed_text(int,"0")`=`sp_box_int(0)` で quirk 一致。
- **★block-param widen 除外(0ed24068, 860→901、+41!)= 本セッション最大の勝因**: block-param 反復(`[1,2,3,4].filter_map{|x| x*2 if x.even?}`)は最大グループだった。**真因**: widen は block-param の scope local を poly 化するが、block emitter は use_shadow で param を element 型(int)に retype して body を再 infer する。だが **`even?` 等メソッド呼びの inference は receiver を cache から読むだけで再 infer しない**(`*` 演算子は operand を infer_type で更新する)ので、`x*2`=int / `x.even?`=poly に混在し、`mrb_int lv_x` shadow が外側 `sp_RbVal lv_x`(widen)を二重宣言。**解=block-param を widen から除外**(`is_block_param` skip、analyze.c:2275)。block-param は反復コレクションの element 型で型付くべきで、widen は害だけ。method param は引き続き widen(promote の本旨)。compile error と一致する output-mismatch FAIL も一掃。**当初は「analyze で型を一貫 poly 化」を想定したが、逆に「block-param は widen しない」が正解だった。**
- **poly→scalar unbox(29dbedea, 901→909)**: block-param を int に保ったことで、poly source(widen された yield 値・ivar・trampoline temp)を int block-param へ束縛する逆方向が必要に。emit_block_invoke の yield 束縛 + instance_exec の is_exec/tramp 束縛に `bt scalar && at poly → emit_unbox_text` arm を追加(box arm の対称)。yield/forward_args_block/block_forward_*/inlined_yield_self_locals/instance_exec_mixed_args 緑。
- **残 ERR=64 / FAIL=20(`/tmp/perr2.txt` の手法で再取得)**: **B4 builtin-arg coerce(~19)**= poly arg を builtin の int/typed param へ(`sp_bigint_new_int`/`sp_range_include`/`sp_str_sub_range(_r)`/user `<=>`=`_lt_set_gt`/`sp_collect_s` 等、call-arg site で散在 coerce)。**B6 残(poly→int 代入)**= まだ拾い切れてない束縛 site。**B1 残(int→poly 代入、8)**。**closure capture(front-end, 5)**=`unsupported closure capturing a non-integer variable`(widen で capture 変数が poly 化→closure が非 int capture 非対応、front-end 制限)。他に return mismatch(3)/invalid binary operands(4)/invalid initializer(2)/`unsupported p argument`(clamp)等。**FAIL=20** はコンパイル通るが出力差(例: `proc` が proc 戻り値で `6`→`0`)、別途調査。
- **range/string builtin の poly arg coerce(17fa2d32, 909→912)**: `sp_range_include`(include?/case-as-value/when range-membership の 3 site)と string slice/index(`sp_str_sub_range[_r]`/`sp_str_char_at_or_nil`)の int 引数を poly のとき `sp_poly_to_i`(string 側は `emit_int_expr` で一律 coerce)。range/case/truncate_module_method 緑。**poly 経路のみ変化、default 不変**。
- **自走バッチ(912→927、+15)**: masgn/pattern 系の box(pattern-match destructure+capture、leading-splat、cvar op-assign→`sp_poly_<op>`+inference 修正、begin/rescue tail box)+ **index/count 引数の一括 `emit_int_expr` 化**(array/string index・fetch・count・op-assign の `mrb_int _t=<idx>` 約25 site、poly のみ coerce で default 不変)+ bigint slot poly 代入→`sp_poly_as_bigint` + range/string-index builtin の poly arg coerce。各 commit は `g_promote_mode` 経路のみ変化、default 992/0/0 + optcarrot 59662 維持。
- **★罠(記録)**: `emit_tail_value` の `g_ret_type==POLY` 無条件 box は **explicit-return 経路と二重 box** で begin_rescue/forward_args_block を回帰(spot-test では PASS だが full-suite で発覚)→ revert。**共有 tail/return 経路の変更は full-suite 必須**(spot-test 不可)。
- **残 ERR=44 / FAIL=22 の構造的ブロッカー**:
  1. **proc/closure の poly 対応(closure-capture ~5 ERR + proc 6→0 等 FAIL)— ★(A) 単独 land 不可を実証(2026-06-21)**:
     - **proc ABI は表面 mrb_int だが poly side-channel を既に持つ**: `_sp_proc_poly_args[16]`(poly 引数)/`_sp_proc_poly_ret`(poly 戻り値)。proc 本体は `_proc_N(void*_cap, mrb_int argc, mrb_int *args)`。
     - **(A) を実装・計測**: ①poly capture cell(`sp_RbVal *_cell_x` + `sp_cell_scan_rbval` + cap-struct フィールド `sp_RbVal*` + proc-capture 制限に poly 許可)②**proc local の `proc_ret` も widen**(本体 scope の ret は widen 済だが caller 側 `lv->proc_ret` が未 widen → `.call` が side-channel を読まず raw mrb_int=0 を返す)③`emit_proc_call_args` の int slot に `sp_poly_to_i` 公開(int proc param が args[k] を読むため)。
     - **個別には proc_closure / case_local_in_lambda が PASS**になったが、**full-suite で 927→920(PASS -7, FAIL +9)に純減**。
     - **真因 = (A) は (B) と結合**: `proc_ret` を widen すると多数の `.call` が side-channel を読むようになるが、**`_sp_proc_poly_ret` は単一グローバル(非再入)**。ネスト/再帰 proc 呼び出しが内側で外側のスロットを上書き → 既存 PASS が FAIL 化。**→ (A) を入れる前に (B) 再入対応(side-channel をスタック化、or 戻り値を呼び出し式内で即読み)が必須**。未コミット破棄済(927 維持)。
     - **正しい順序 = (B) を先に**: `_sp_proc_poly_ret` を depth-indexed stack 化(or `({sp_proc_call(...); _sp_proc_poly_ret;})` で即時読みを保証)→ その上で (A) poly-cell + proc_ret widen + args int-slot 公開を入れる。これで初めて net-positive になるはず。**poly-cell/proc_ret-widen/args-slot の 3 点セット + (B) を一括で入れる**こと(部分適用は退行)。
  2. **node-cache stale**: `emit_hash_key` 等が `comp_ntype(key)` の widen 前 int を読み coerce を飛ばす(int-keyed hash の poly key=bundle_class_06)。local read は `scope_local()->type` が正、generic ヘルパーは node 型依存。中央解決要。
  3. **散在 one-off**: user `<=>`(`sp_*__lt_set_gt` の arg、`expected expression before sp_RbVal` 構造的)、bound-method `.call` 直接 dispatch の raw arg(sp_double/dbl/mix)、instance_exec break/next/trampoline_result の poly 結果→int、各種 builtin arg。
- **手法**: `/tmp/ptest.sh <name>`(単体)、`/tmp/perragg.sh`→`/tmp/perr4.txt`(全 ERR first-error)、`/tmp/ptest_lastc`(最後の cfile)。中央 coerce=`emit_int_expr`(poly→int)/`emit_float_expr`/`sp_poly_as_bigint`/`emit_boxed`+`emit_boxed_text`(→poly)/`emit_unbox_text`(poly→scalar)。
- **次の一手**: (1) 残 B4 one-off を `emit_int_expr`/`sp_poly_as_bigint` で個別 coerce(中央化できる site は emit_int_expr に寄せる)。(2) closure-capture front-end(capture 変数 poly 対応 or widen 除外)。(3) FAIL=21 の出力差(proc 等)。手法=`/tmp/ptest.sh <name>`、`/tmp/perragg.sh`→`/tmp/perr2.txt`、`/tmp/ptest_lastc` に最後の cfile。**poly→int は `emit_int_expr`、poly→float は `emit_float_expr`、poly→bigint は `sp_poly_as_bigint` が既存の中央ヘルパー。**

### (参考)legacy 方式
legacy backend(`legacy/spinel_codegen.rb` L55, L3037-3203)は promote で全 int slot を `sp_Bigint*` に widen(method ABI = `(void*, sp_Bigint*...) -> sp_Bigint*`)。採用せず(上記理由)。

**実装の難所:**
1. **mode 伝達**: C 版は `codegen_program(nt)` に overflow mode が渡らない(main.c:355、現状 `-D` のみ)。`g_promote_mode` global を main.c で設定し analyze/codegen が読む必要。
2. **big-bang 性**: 全 int を bigint 化すると、埋まっていない境界が新たに表面化しうる。部分 widen は promote コンパイルを壊すので、全境界を同時に通す必要(=現 optcarrot が通る範囲を超えた path で再度バケツ潰し)。
3. **optcarrot perf 退行リスク**: ブロックループ検出を素朴に足すと optcarrot の `.times` 内 `x=x*y` が大量 bigint 化し壊滅的に遅くなる(while 限定はこの回避が理由と思われる)。全 int widen でも promote optcarrot は大幅減速(opt-in なので許容、gate は wrap pin で不変)。

**推奨**: 専用の集中作業として (1) mode 伝達 →(2) 全 int→bigint widen pass(promote gate)→(3) 表面化する境界を潰す → promote で test/bench/optcarrot 再確認、の順。default/wrap は promote gate により不変なので gate 回帰リスクは promote 経路に限定。

## (以下、2026-05 当時の Error バケット分類 — 多くは解消済み、参考)

## Error バケット分類

### Bucket A: Bit operators on `sp_Bigint *` (LARGEST — optcarrot の 80%)

```
error: invalid operands to binary & (have 'sp_Bigint *' and 'long long int')
error: wrong type argument to bit-complement
```

Ruby の `a & b`, `a | b`, `a ^ b`, `a << n`, `a >> n`, `~a` で operands が bigint の時 C 演算子が使えない。 optcarrot は CPU emulator なので bit-op が大量。

**修正**: runtime helpers + codegen 分岐:
- `sp_bigint_and(a, b)`, `_or`, `_xor`, `_shl`, `_shr`, `_not`, `_inv`
- `compile_operator_expr` / `compile_bitop_expr` で bigint operands を検出 → helper call emit
- 混在 (bigint + int_literal) → 一方を unbox / box して揃える

### Bucket B: Builtin container helpers with bigint args

```
sp_IntArray_push(a, bigint_lv)   ← needs sp_bigint_to_int
sp_IntArray_get(a, bigint_idx)
sp_FloatArray_set(a, idx, bigint_val)  ← unbox + cast to mrb_float
sp_IntStrHash_set(h, bigint_key, val)
```

memory に partial fix 言及あり。 残るのは `_push` / `_get` / `_set` の特定 site と FloatArray の bigint→float cast。

### Bucket C: 反復 / index で bigint

```
range.each { |i| arr[i] = ... }   ← i bigint、 arr[i] で unbox 必要
n.times { |i| ... }
```

block param が bigint で IntArray index 等に使われる shape。 既に部分対応済みだが取りこぼし。

### Bucket D: FFI / typed user method call の poly → bigint coerce

```
sp_Counter_cls_double(poly_val)   ← param bigint、 arg poly
sp_mandelbrot(bigint, bigint)     ← FFI param float、 arg bigint
```

`compile_typed_call_args` で poly arg → bigint param の coerce を追加 (mirror of #639 IVW unbox)。 FFI :float / :int arg は bigint で `sp_bigint_to_int` → cast。

### Bucket E: Bigint slot init / return from poly

```
sp_Bigint * x = poly_expr          ← .v.p extract 必要
return poly_expr                   (function return bigint)
```

`compile_expr_for_expected_type` の poly→bigint arm が無い / 不完全。

### Bucket F: Bigint → string 変換漏れ

```
puts bigint_val
"prefix" + bigint_val
sp_int_to_s(bigint_val)            ← should be sp_bigint_to_s
```

memory に「文字列補間 / str_concat / str << の bigint→string 変換 (sp_bigint_to_s)」とあり partial fix 済み。 残りの emit site を探す。

### Bucket G: Test/bench infrastructure issues

promote と無関係に表面化する shape:
- `int_eq_nil_strict` — int slot が bigint 化したので nil check の挙動が変わる
- `pattern_pin` — pin pattern が int を期待してるが bigint で型不一致
- `const_self_ref_init_warns` — 出力 format 変化 (`0` vs `0LL`?)

これらは個別調査。

## Phases (実装順)

### Phase 1 — Bit-op support for bigint (大ブロッカー解消)

最大の塊。 これで optcarrot の 80% errors が消える。

1.1 `lib/sp_runtime.h`: 新 helpers
- `sp_bigint_and(a, b)`, `_or(a, b)`, `_xor(a, b)` — 既存 GMP wrapper 経由
- `sp_bigint_shl(a, n)`, `_shr(a, n)` — n は mrb_int
- `sp_bigint_not(a)` — 1's complement (符号反転 + 1 で signed と同じ)

1.2 `lib/sp_bigint.c`: GMP `mpz_and` / `mpz_ior` / `mpz_xor` / `mpz_mul_2exp` / `mpz_fdiv_q_2exp` / `mpz_com` ラップ

1.3 `spinel_codegen.rb`:
- `compile_operator_expr` で `op in {"&", "|", "^", "<<", ">>"}` && (lt == bigint || rt == bigint) → helper
- `~` の単項 (`compile_call_expr` で recv bigint mname `~`) → `sp_bigint_not`
- mixed (bigint + int literal): literal を `sp_bigint_new_int` で box して helper、 OR 両方 unbox して C 演算子 → re-box

1.4 `optcarrot` で動作確認、 checksum 59662 維持確認

期待: optcarrot promote PASS、 test promote +20〜30 cases

### Phase 2 — Builtin container helpers の bigint unbox 取りこぼし

2.1 `sp_IntArray_push` emit site: arg が bigint なら `sp_bigint_to_int` を wrap
2.2 `sp_IntArray_get` / `_set` / `_index` 同様
2.3 `sp_FloatArray_set` の val が bigint なら `(mrb_float)sp_bigint_to_int(...)` で cast
2.4 `sp_*IntHash_*` で key/val bigint の unbox

期待: bench bm_loops_times / bm_matmul / bm_fannkuch PASS。 test promote +10〜15 cases

### Phase 3 — Typed user method call の poly→bigint coerce

3.1 `compile_typed_call_args` (positional + kwarg):
- 引数の static type が `poly` で param expected が `bigint` → `(arg).v.p` 経由
- mirror of #639 の IVW unbox path

3.2 FFI :float / :int arg with bigint:
- `compile_ffi_func_call` で arg が bigint なら `sp_bigint_to_int` → cast

期待: nullable_poly_hash_arg_unbox 等 +5 tests、 bm_mandel_term / bm_so_mandelbrot 系 PASS

### Phase 4 — Bigint slot init / return from poly

4.1 `compile_expr_for_expected_type`: at == "poly" && expected == "bigint" → `(... ).v.p`
4.2 `compile_return_stmt`: rt == "poly" && method return == "bigint" → 同様 unbox

期待: 残 test promote errors +5

### Phase 5 — Bigint → string 変換取りこぼし

5.1 `puts bigint`, string interpolation `"x=#{bigint}"`, `<<` chain の emit site で bigint operand → `sp_bigint_to_s` 経由

期待: bench bm_csv_process / bm_jekyll_lite 系 PASS

### Phase 6 — 個別 test failure 調査

残 test failures を個別に潰す。 各々小さな shape:
- `int_eq_nil_strict` — bigint slot で nil compare 挙動
- `pattern_pin` — pattern matching の bigint scrutinee
- `const_self_ref_init_warns` — output format 微差

期待: test promote 100% pass

### Phase 7 — Bench 残り + Makefile 整理

7.1 残 bench failures 個別に潰す
7.2 Makefile から `optcarrot` 用 `--int-overflow=wrap` pin 除去 (or `--int-overflow=$(SPINEL_INT_OVERFLOW)` defaulting to wrap だが promote でも通る)

期待: bench promote 100% pass

## 検証フロー (各 phase で)

```bash
# default mode は壊さない
rm -rf build/test-results/
make -j8 test 2>&1 | tail -2   # 631/0/0 維持
make optcarrot 2>&1 | tail -3  # checksum 59662 維持

# promote mode 進捗
rm -rf build/test-results/
SPINEL_INT_OVERFLOW=promote make \
  CFLAGS="-O2 -Wno-all -Wno-unknown-warning-option -Wno-alloc-size-larger-than -DSP_INT_OVERFLOW_MODE_PROMOTE" \
  -j8 test 2>&1 | tail -2

SPINEL_INT_OVERFLOW=promote make \
  CFLAGS="-O2 -Wno-all -Wno-unknown-warning-option -Wno-alloc-size-larger-than -DSP_INT_OVERFLOW_MODE_PROMOTE" \
  -j8 bench 2>&1 | tail -2

# optcarrot single
SPINEL_INT_OVERFLOW=promote ./spinel --int-overflow=promote build/optcarrot-single.rb -o /tmp/op_p
/tmp/op_p
```

## 効果量予想 (累積)

| Phase | test | bench | optcarrot |
|---|---|---|---|
| 現状 | 557/52/22 | 36/7/14 | C compile fail |
| 1完了 | ~580/45/15 | ~45/5/7 | **PASS** (checksum 59662) |
| 2完了 | ~595/40/8 | ~52/3/2 | PASS |
| 3完了 | ~610/15/4 | ~55/2/0 | PASS |
| 4完了 | ~620/8/2 | ~56/1/0 | PASS |
| 5完了 | ~625/5/0 | ~57/0/0 | PASS |
| 6完了 | **631/0/0** | 57/0/0 | PASS |
| 7完了 | 631/0/0 | **57/0/0** | PASS (Makefile clean) |

## 工数概算

- Phase 1 (bit-ops): 2-4 hours
- Phase 2 (containers): 1-2 hours
- Phase 3 (call args): 1-2 hours
- Phase 4 (return/slot): 0.5-1 hour
- Phase 5 (string conv): 0.5-1 hour
- Phase 6 (個別 test): 2-4 hours (varies)
- Phase 7 (bench + Makefile): 1-2 hours

**合計**: 8-16 hours focused work

## 進め方の注意

- **default を壊さない**: 各 commit 直後に `make test` (default 631/0/0) + `make optcarrot` (wrap pin、 checksum 59662) を回す
- **commit を小刻みに**: bucket 1 つ ≒ 1 commit (memory `feedback_commit_often`)
- **inline 関数 vs macro**: `sp_int_*` は macro。 新規 helper も同じ pattern で (memory `feedback_promote_mode_inline_pitfall`)
- **stale cache 注意**: promote_int_to_bigint_globally 後の cache invalidation 漏れに警戒
- **記録**: 各 phase の commit 後、 残り errors / fails の数を memo して進捗トラック

## Risks

1. **optcarrot は --int-overflow=wrap が最速**: promote だと bigint helper call overhead で fps 低下が予想される。 Makefile のデフォルトは wrap のままにする (promote は opt-in mode という設計に沿う)
2. **bigint memory pressure**: 全 int slot が heap-allocated bigint になる → GC 圧と allocator churn。 既存 GC で対応できるか測定要
3. **既存 default test の non-determinism**: 不変なはずだが、 stale cache invalidation 経由で稀に regression が出る可能性あり。 毎 commit で default test 必須
4. **新 helper の signedness**: GMP の `mpz_and` 等は arbitrary-precision で signed 自然サポート。 spinel 側で signedness 維持確認

## 開始 phase

**Phase 1 (bit-ops)** を最優先。 optcarrot を unblock し、 影響が最も大きい。 着手後、 各 helper を 1 つずつ追加 → optcarrot で動作確認 → commit。
