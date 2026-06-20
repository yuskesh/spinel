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
