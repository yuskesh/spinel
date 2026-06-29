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
     - **★真因を確定(2026-06-21 第2次調査)= proc_ret の post-widen fixpoint 伝播不整合**(当初「side-channel 非再入」と推測したが誤り):
       - `.call` の return 読み出しは proc の **caller-side `proc_ret`** で int slot vs `_sp_proc_poly_ret` を分岐。これが proc **本体の実 emit ret** と一致しないと 0 を返す。
       - **失敗方向①(naive widen=全 proc_ret→poly)**: `->(a,b){a+b}`(a,b は block-param 除外で int、本体は `return sp_int_add`=**int return**)で caller が side-channel(未設定)を読み 0。multi_arg_lambda 等 **11 lambda が退行**。
       - **失敗方向②(literal 限定で proc_ret 再計算=`proc_ret_of`)**: multi_arg は直る(本体 int→proc_ret int)が、**`c = make_counter()`(メソッドが返す proc)** は proc_ret が make_counter の `ret_proc_ret`(未 widen=int)由来で、literal ループが取りこぼし → proc_closure が再退行。
       - **= proc_ret は複数サイトの fixpoint**(`x=proc{}` analyze_pass.c:1124 / proc param :1218 / method `ret_proc_ret` :3719 / compose・to_proc)。**widen 後にこの fixpoint 全体を再収束させる必要**があり、naive widen も部分再計算も不可。`proc_node_ret`=本体最終式の infer(`analyze_util.c:334`)。
     - **★★解決済み(ed0c0d68, 927→932, 回帰0)= option 1(ABI 変更不要)**:
       - ①**poly capture cell**: `sp_RbVal *_cell_x` + `sp_cell_scan_rbval`(sp_gc.h)+ cap-struct フィールド `sp_RbVal*`(codegen.c)+ proc-capture 制限に TY_POLY 許可。read/write 既存経路で OK(lvalue deref + emit_assign の poly box arm)。
       - ②**args int-slot 公開**: `emit_proc_call_args` で poly arg を `sp_poly_to_i` で int slot にも置く(int proc param=block-param 除外 が args[k] を読むため)。
       - ③**proc_ret/ret_proc_ret の post-widen focused fixpoint**(analyze.c, finalize infer の後):method scope ret_proc_ret(proc_ret_of で本体から)+ `x=proc{}`/`x=factory()` の proc_ret を収束ループで再導出。**proc-return metadata のみ更新、widen slot 型は不変。**
       - **★決め手 = fixpoint 後に `for id: infer_type(c,id)` で node-type cache を再構築**。codegen の `.call` は `comp_ntype(call node)`(node cache)を読むので、proc_ret を更新しても cache が stale だと `.call` が古い int slot 経路を出す。再 infer は scope local 型を読むだけで変えないので widen 不変。
       - 結果: proc / proc_closure / case_local_in_lambda / enum_for_each / lowered_block_given が緑、**proc の 6→0 FAIL も解消**。当初推測の「side-channel 非再入」「ABI 全面改修」は**いずれも不要だった**(真因は node-cache の refresh 漏れ)。
  2. **node-cache stale**: `emit_hash_key` 等が `comp_ntype(key)` の widen 前 int を読み coerce を飛ばす(int-keyed hash の poly key=bundle_class_06)。local read は `scope_local()->type` が正、generic ヘルパーは node 型依存。中央解決要。
  3. **932→939 の安全グループ landed**(全 g_promote_mode gate 内、default 992/0/0+optcarrot 59662 維持): bound-method 直接 `.call` の arg を `emit_arg_or_default` で param 型 coerce(method/i960)/ ClassVariableWriteNode 推論を cvar 型返し=二重 box 解消(cvar_method_first_assign)/ poly-receiver dispatch の poly arg→scalar param unbox(8313, `<=>` 前進)/ splat 展開要素を poly param へ box(splat_call)/ **`<=>` を ret widen 除外**(小 int しか返さず overflow 無し→caller の `(a<=>b)<cmp>0` が int 比較で通る、lrama_features)/ **typed-array ret を body の PolyArray に追従**(post-widen fixpoint の loop3=method ret を body 末尾/return の PolyArray から再導出、multi_return)。
  **残 ERR=34(939 時点)の散在 one-off / 深い codegen バグ**:
     - **ptr_array**: method ret は PolyArray 化したが caller の `local = method()` の **local slot 型が IntArray のまま**(infer_write_types は widen 前確定)= method-ret→local 型の伝播 cascade 要。
     - **poly-receiver dispatch の arg coerce 双方向**: `8313` で poly arg→scalar param の unbox arm 追加済(`<=>` 等、回帰0)。だが user `<=>`(comparable_between_user_type 等 3)は**第2ブロッカー = between?/clamp の arg-hoist g_pre 再入バグ**で残る:`sp_Temp * _t7 = sp_RbVal _t10 = ...`(`Temp.new(5)` の boxed arg hoist が外側 arg-temp decl の初期化子内に混入)。emit_dispatch(codegen_fold.c:3389)で arg を側 buffer 評価→prelude flush 後に temp decl、の順序修正が要る。
     - **closure_capture_float 系(3, sp_runtime.h:187 `mrb_int=sp_RbVal`)**: poly cell 導入で capture が通るようになったが、proc 本体の captured 変数の**算術が `sp_int_add(poly cell, 1)`**(comp_ntype が int、cell は poly)。proc 本体 scope の captured-var 型一致が要る。
     - **bound-method `.call` 直接 dispatch の raw arg**(method=sp_double / i960=sp_dbl / splat_call=sp_mix / bound_method_single_eval=sp_poly_puts / cvar_method_first_assign=sp_box_int): poly param へ int arg を box せず。
     - **B6 poly→int**(bm_instance_eval=ivar-write-as-expr / poly_dispatch_* / instance_exec break/next/trampoline_result / bundle_class_10 等)、**B1 int→poly**(instance_eval_block_params/bundle_misc_b/block2/bundle_classd_04)、binary op(pattern_pin `==`/bundle_class_05 `<<`/lrama_features `<`)、return mismatch(compile_time_define_method_int / multi_return・ptr_array=PolyArray vs IntArray)、invalid init(i916/op_assign_user_operator)、hash key(bundle_class_06=emit_hash_key の comp_ntype stale)、sp_box_str int-conv(recursive_implicit_yield_value)、sp_Integer undeclared(class_method_open_class_call)、float aggregate(float_round_nonliteral)、ld error(fiber_capture_no_leak_to_sibling/i1007)、front-end(clamp_bounds/integer_div_by_zero)。各々 1-2 テスト、個別診断要。
     - **FAIL=19** はコンパイル通るが出力差、別途。
- **手法**: `/tmp/ptest.sh <name>`(単体)、`/tmp/perragg.sh`→`/tmp/perr4.txt`(全 ERR first-error)、`/tmp/ptest_lastc`(最後の cfile)。中央 coerce=`emit_int_expr`(poly→int)/`emit_float_expr`/`sp_poly_as_bigint`/`emit_boxed`+`emit_boxed_text`(→poly)/`emit_unbox_text`(poly→scalar)。
- **次の一手**: (1) 残 B4 one-off を `emit_int_expr`/`sp_poly_as_bigint` で個別 coerce(中央化できる site は emit_int_expr に寄せる)。(2) closure-capture front-end(capture 変数 poly 対応 or widen 除外)。(3) FAIL=21 の出力差(proc 等)。手法=`/tmp/ptest.sh <name>`、`/tmp/perragg.sh`→`/tmp/perr2.txt`、`/tmp/ptest_lastc` に最後の cfile。**poly→int は `emit_int_expr`、poly→float は `emit_float_expr`、poly→bigint は `sp_poly_as_bigint` が既存の中央ヘルパー。**

- **2026-06-21 続行(939→959、ERR 34→15、12 commit、全 g_promote_mode gate 内で default 992/0/0 + optcarrot 59662 維持)**:
  - **pattern_pin(22d351d2)**: scalar scrutinee に対する pinned poly 値(`in ^x`、x widen)を `emit_unbox_text` で unbox。
  - **poly cell op-assign(f5..)**: captured poly cell(`sp_RbVal *n`、proc 内 `n += 1`)の compound-assign を `sp_poly_<op>`/bitwise re-box へ。closure_capture_float/block_forward_value_callable/bundle_classd_13。
  - **array-local cascade(78872692)= post-widen fixpoint step (4)**: map メソッド ret や array literal が PolyArray 化したとき `local = value` の IntArray/StrArray/FloatArray slot を TY_POLY_ARRAY に追従。ptr_array。
  - **poly `<=>`(e34530e2)**: poly operand の `<=>` を `sp_poly_cmp` へ(user class の `<=>` に誤 dispatch して boxed-int payload を user-ptr cast → recursion segfault を回避)。bundle_classd_50。
  - **between? arg-hoist(f447..)**: Comparable#between? の self/lo/hi temp を、各 operand を local buffer で先に emit してから decl prefix を書く(operand 自身の g_pre hoist が decl 行を割らないよう順序入替)。comparable_between_user_type。
  - **unary `-@`/`+@`/`~` on poly(analyze_infer)**: poly receiver の unary を poly/int に解決(method-dispatch unify が user class の `-@` に bind して obj 型を cascade するのを防ぐ)。unary_operator_methods。
  - **poly-dispatch scalar slot unbox(8217)**: length-like poly dispatch の int slot に、widen された user method ret(poly)を `emit_unbox_text` で下げる(box 方向の逆)。poly_dispatch_builtin_all/poly_dispatch_ptr_array/bundle_class_10/bundle_classd_28。
  - **instance_exec result slot を break/next 値で sizing(codegen+analyze)**: `next val+1`(poly)が trailing `999`(int)より広いとき result temp を union 型に。`ie_splice_value_ty`(codegen)/`ie_block_break_next_ty`(analyze)で break/next 値型を unify、`g_ie_res_poly` flag で scalar break/next/last を box。instance_exec_next/instance_exec_break。
  - **nested destructure box(codegen_stmt)**: `a, (b, c), d = 1, [2,3], 4` の inner array element を poly target slot に box。bundle_misc_b。
  - **obj op-assign user-operator arg box(codegen_stmt)**: `slot OP= rhs`(obj slot + user OP)の rhs temp を OP param が poly のとき box。op_assign_user_operator + sibling。
  - **loop{} poly result(codegen_call)**: expression-position `loop{ break v }` の result temp を `default_value()` で init(NULL→sp_box_nil())+ `g_ie_res_poly` で scalar break を box。i916。
  - **constant cascade(analyze)= step (5)**: `COUNT = obj.m` でメソッド ret が widen したとき int 定数 slot を poly に追従。bare_module_body_cmethod_call。
  - **959→965+ 続行(さらに 9 commit、default 992/0/0+optcarrot 59662 維持)**:
    - **define_method subst literal を poly-return tail で box**: `define_method("m_#{n}"){ n }` 合成メソッドの ret が widen → subst literal(int)を box。tail box 判定を subst node の型で sizing。compile_time_define_method_int。
    - **poly-ivar attr-writer infer=poly**: `obj.attr = v` の値は C 代入結果=boxed poly。infer が rhs(int)型を返していた → ivar poly なら poly 返し。instance_eval 末尾 setter の result temp sizing 一致。bm_instance_eval。
    - **instance_eval ivar-write を rebound class で解決**: splice 内 block scope は class_id 無し → `@v=42` の ivar 型 UNKNOWN(box skip + infer rhs 型)。g_ie_class_id(codegen)/an_ie_class_id(analyze)で解決。instance_eval_block_params。
    - **instance_eval/exec call node 自身を rebound scope で再 infer**: post-fixpoint 再 infer は body だけ更新、call node 型は stale → truthiness consumer が int-nil form を poly に。`infer_type(c,id)` 追加。bundle_classd_04。
    - **value-form hash `[]=` の poly key を unbox**: `h[k]=v`(式)の key が emit_expr 生 → typed-key hash setter に poly。emit_hash_key 経由化。bundle_class_06(-Werror 検出、make test は lenient で見逃す)。
    - **Float#round/ceil/floor/truncate の poly ndigits unbox**: `(double)(poly)` aggregate error → emit_int_expr。float_round_nonliteral。
  - **★ make-test promote count は -Werror 無しで lenient + 並列 compile timeout で flaky**(同一 binary 再実行で ±1 揺れる)。各 fix の確証は `/tmp/ptest.sh`(-Werror, deterministic)。**default 992/0/0 + optcarrot 59662 が hard gate、両者は全 commit で緑維持**。
  - **965→971 続行(さらに 6 commit、ERR 9→3、default 992/0/0+optcarrot 59662 維持)**:
    - **instance_exec block param を自 block scope で解決**: forward-block(`&b` が別 site の literal に解決)の param が call site scope の無関係な `a` を読み slot 型誤認 → `comp_scope_of(reqs[p])`(param node の scope)で解決。is_exec/tramp の poly→int unbox が発火。instance_exec_trampoline_result。
    - **reopened-primitive の poly dispatch receiver unbox**: `(sp_Integer*)_t.v.p` は非 struct → `.v.i`/`.v.f`/`.v.s`/`(sp_sym)` で union field 読み。class_method_open_class_call。
    - **poly local `%=` → sp_poly_mod**: op-assign arm が +,-,*,/ のみ → `%` 追加。integer_div_by_zero。
    - **poly clamp**: `sp_poly_clamp`(runtime, tag-dispatch int/float + NaN/order check 既存 helper 再利用)+ infer poly.clamp→poly + codegen の 2-arg/range form。clamp_bounds。
    - **`sp_gc_mark_rbval` typo → `sp_mark_rbval`**: fiber poly-capture scan が未定義シンボル参照 = ld error。promote の poly fiber capture で初露見。i1007 / fiber_capture_no_leak_to_sibling。
  - **★残 ERR=3(971 時点、deterministic、全て raw mrb_int carrier ABI family = 構造的ブロッカー)**:
    - **block2**: no-block の inline yield が poly value-if の then 分岐で `_t2 = 0`(int)を poly slot へ。yield emit を `sp_box_nil()` 化しても value-if/inline 経路が `0` を出す（emit と value-if の二段。yield emit 単独修正では不可、inline+value-if の branch-default を追う要）。
    - **bound_method_single_eval**: object-bound `.call` の ABI が `mrb_int(*)(void*, mrb_int...)` 固定。promote で target method は poly param/ret に widen → args を param 型に box + ret cast を target 型にする協調修正要(return cast 単独は ERR→FAIL 悪化、revert 済)。
    - **recursive_implicit_yield_value**: lowered-yield の raw mrb_int carrier が string/int 両用 block で型判別不能 → `sp_box_str(mrb_int)` 誤選択。carrier に型 tag を持たせる ABI 変更要。
    - **FAIL=19** はコンパイル通るが出力差(attr=promote-mode crash 等、別途)。

  **★セッション総括(940→971、ERR 33→3)**: emission-site の box/unbox coercion + post-widen fixpoint の cascade(array-local step4 / 定数 step5)+ instance_exec/eval の rebound-scope 解決(ivar-write/call-node 再 infer/forward-block param scope)+ poly dispatch の双方向 coerce + runtime helper 追加(sp_poly_clamp)で系統的に潰した。**hard gate(default 992/0/0 + optcarrot 59662)は全 commit で緑**。make-test promote count は -Werror 無し + 並列 timeout で flaky、確証は `/tmp/ptest.sh`。

- **971→973 続行(ERR 3→1、残 1 = recursive_implicit_yield_value のみ)**:
  - **block2 CLOSED**: no-block の inline yield が poly value-if/inline tail で `0`(int)を poly slot へ。**真因 = `emit_tail_value` が文字列 `"sp_box_nil()"` を `emit_ret_nil(g_ret_type)`(=scalar)に書き換える特例**(g_result_var の poly を無視)。2 段修正: yield emit を `comp_ntype==POLY` で `sp_box_nil()` 化 + `emit_tail_value` を `g_result_poly` 時は as-is pass-through。
  - **bound_method_single_eval CLOSED**: object-bound `.call` が `mrb_int(*)(void*, mrb_int...)` 固定 ABI で、widen 後の poly param/ret を truncate(0 返し)。**target が静的解決できる時はその実 return/param C 型で fn pointer を cast + `emit_arg_or_default` で arg coerce**。default mode は非 widen で同一コード(回帰なし)。
  - **★残 ERR=1 = recursive_implicit_yield_value(深い multi-pass ABI、本セッションで root cause 確定・修正は scope 外と判断)**:
    - 症状: `countdown` が string block(`{"done"}`)と int block(`{42}`)両方から呼ばれ、return 型は poly に unify。だが lowered-yield の `yield`=`sp_proc_call(__yblk__)` は raw mrb_int を返し、call-site block proc(`_proc_N`)も raw 値(int は素・string は ptr→mrb_int cast)を返すため、`return yield` の box で string/int を判別できず `sp_box_str(mrb_int)` 誤選択。
    - **修正の必要 3 点(個別には実装、しかし pass 順序で不成立)**: ①`method_call_ret`/`yield_value_type` を lowered-yield で全 call-site unify(現状は「per-call-site 特殊化」前提で first block 型を break-採用 → 単一コンパイルの実態と不一致)②call-site block proc を poly-ret 化(`_sp_proc_poly_ret` に box、codegen.c:1084 の `blk_ret==POLY` 機構が既存)③lowered yield emit が `_sp_proc_poly_ret` を読む。
    - **★ブロッカー = pass 順序**: `blk_ret` を計算する `infer_return_types`(analyze_pass.c:3699)時点で **lowered-yield 化(`blk_param="__yblk__"` 設定、analyze.c:1992+)がまだ走っておらず**、countdown の `blk_param` が NULL → blk_ret 計算自体が lowered-yield scope を一切見ない(debug 確認: 該当 pass で blk_param 付き scope が 0 件)。よって ②の前提(blk_ret==POLY)が成立しない。**真の修正は blk_ret 計算を lowering 後に動かす(or lowering pass 内で計算)+ ②③ の協調**で、analyzer の pass 順序に踏み込む構造変更。siblings(proc/block2/forward_args_block 等)は本試行で回帰なしを確認済だが、本丸は未収束のため revert・clean 維持。
    - **FAIL=19** はコンパイル通るが出力差(attr=promote-mode crash 等、別途)。

**2026-06-20 第3試行 = 方針 (A) full uniform widen を実行(branch `promote-full-widen-experiment` に保全):**
matz 洞察「poly は int(inline)と bigint(heap)両方を 1 値で保持するので、legacy の全 bigint(全 heap)より効率犠牲を抑えて全 widen できる」を受け、**全 int slot → poly** を実装(analyze.c の fixpoint 後・node-type cache 前、`g_promote_mode` gate、24 行)。param/return/local/ivar/cvar の `TY_INT`→`TY_POLY`。
- **本丸 #1 が動いた**: `def mul(x,y)=x*y; mul(10**15,10**15)` = 10**30、`mul(3,4)=12`(小 int は inline poly)。MRI 一致。**approach 実証完了**。
- **default gate 無傷**(992/0/0、g_promote_mode gate)。
- **promote suite 993→831(32 fail / 130 error)**= 境界バケツが表面化。**全エラーは poly↔int 境界の coercion 欠如**(codegen が型一致前提)。
- 単純代入境界は **emit_assign に既に poly-box arm あり**(codegen_stmt.c:512 `lv->type==TY_POLY → emit_boxed`)。残るは以下の **境界バケツ(C error 頻度順、`/tmp/promerr.txt` 集計)**:
  1. **代入 `sp_RbVal = mrb_int/long`(265)**= op_assign(`x+=1`)・ivar/cvar write・宣言初期化など emit_assign 以外の代入経路。各 write emit に poly-box。
  2. **比較 `sp_RbVal <= / >= / < / > / == mrb_int`(~60)**= poly recv の比較を `sp_poly_lt/cmp/eq` に回す(scalar 比較 codegen が int 前提)。
  3. **increment `wrong type argument to increment`(28)**= `lv_x++`(loop counter 等)が poly slot。`lv_x = sp_poly_add(lv_x, sp_box_int(1))` 化。
  4. **builtin arg coerce(~40)**: `sp_IntArray_push`(14)/`sp_poly_to_s`(11)/`sp_poly_lt`(9)/`sp_bigint_new_int`(5)/`sp_poly_to_i`(4)/`sp_range_include`(4)/`sp_str_sub_range_r`(4)= poly arg を builtin の int param へ。call-arg coerce。
  5. **const/cvar static init 非定数(21)**= `static ... = sp_box_int(...)`(非定数式)。cvar/const の poly 初期化を runtime init へ移すか、int 保持して read 時 box。
  6. **mrb_int = sp_RbVal(22)/ return mismatch(~10)**= builtin 戻り(int)を poly slot へ、または poly 値を int return 型へ。widen し残した境界。
  7. **float `aggregate where floating-point expected`(10)**= poly を float 文脈へ。`sp_poly_to_f` coerce。
- **次の一手**: バケツ 1(op_assign/ivar/cvar write の poly-box)→ 2(比較)→ 3(increment)の順で、各バケツ後に promote suite を再計測(`rm -rf build/test-results && make test SPINEL_INT_OVERFLOW=promote OPT=-O1`)。emit_assign の poly arm パターンを各 write/compare/return site に横展開する作業。branch から cherry-pick して再開。
- **判断**: これは計画通り複数セッションの big-bang。master は clean(2a の 993)維持、完遂後にまとめて昇格。approach は確定(本丸動作・効率優位)。

**2026-06-21 バケツ1(for-range loop counter)実装の決定的知見:**
`emit_for` の TY_RANGE arm に修正(端点を独立に `sp_poly_to_i` coerce + poly counter は fresh `mrb_int` temp 駆動&毎 iter `lv_<vn>=sp_box_int(_tc)` で box、early return で `else` 回避)。branch `promote-full-widen-experiment` の commit **bda23978** に保全。
- **修正は correct**: `for i in 1..10`(=55)+ poly 端点版 `for i in 1..f`(=55)とも MRI 一致、BUILD_OK。端点 coerce は counter の poly 性と独立に必要(concrete int counter + poly 端点 `n` でも `mrb_int _t=lv_n` が壊れる)。
- **だが promote pass は 831 横ばい**(error→fail 1件のみ移動)。**∵各 failing テストに複数の poly↔int 境界が重なる→1バケツ単独では緑にならない**(for-range を直しても同テスト内の次の境界=代入/比較/builtin-arg 等でコンパイル続行不能)。
- **戦略的結論**: **バケツ逐次 commit は中間で pass を増やさない**。緑にするには (1) 1テストを縦に全境界完遂(pass+1、境界カタログ確証)か (2) 全境界(代入両方向/比較/increment/builtin-arg/const-init/return/float)を**一括**で潰す真の big-bang。次セッションは for-range fix(bda23978)を土台に、まず 1 テスト縦完遂で全境界の実装パターンを確立してから横展開が安全。

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
