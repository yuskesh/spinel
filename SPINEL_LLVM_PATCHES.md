# spinel-llvm patches against yuskesh/spinel-fork (`c-emit-ir`)

このファイルは [CLAUDE.md](https://github.com/yuskesh/spinel-llvm/blob/main/CLAUDE.md) (spinel-llvm 側) の「vendor/spinel は原則 read-only。変更が必要に
なったら spinel-ebpf の `SPINEL_EBPF_PATCHES.md` 方式に従う」に基づき、spinel-llvm
プロジェクトが `vendor/spinel` に当てている改変を一元管理する。書式と規律は姉妹
プロジェクト spinel-ebpf の [`SPINEL_EBPF_PATCHES.md`](SPINEL_EBPF_PATCHES.md)
を踏襲している。

## 改変方針 (要約)

- **opt-in のみ**: `-D` / 環境変数で明示的に有効化したときだけ新挙動が走る
- **追加のみ (additive)**: 既存行の削除・書き換えをしない。footprint は追加行数で数える
- **フラグ未設定時は出力バイト一致**: 生成 C・プリプロセス済みトークン列・
  コンパイル済みバイナリのすべてで upstream と一致することを毎回実測する
- **安全境界を先に書く**: パッチが壊しうる意味論を先に特定し、ゲートの適用範囲として明文化する
- **upstream PR を目指す**: 汎用に書けた hook は本家に PR

## ベース (upstream)

- **fork**: yuskesh/spinel-fork branch `c-emit-ir` @ `8f65a5b5`
  ("sync upstream 53feb9df→83d1315d (+381), rebase-clean re-plant Patch 1")
- **upstream master**: matz/spinel `83d1315d`
- fork には spinel-ebpf の **Patch 1 `SPINEL_EXTERN_METHODS`** (`src/codegen.c` +48 行) が
  既に載っている。spinel-llvm の footprint は**それとは別**に数える。
- **upstream sync は意図的に保留中** ([P001](../docs/plan/P001_llvm_backend_roadmap.md) 末尾の決定)。

## 現在当てている patch

| # | 名前 | ファイル | 追加行 | ゲート | 状態 |
|---|---|---|---|---|---|
| 1 | `SP_UNMASKED_SETJMP` — mask-free 非局所脱出 | `lib/spinel_rt.h` | **+20 / −0** | `-DSP_UNMASKED_SETJMP` (既定 OFF) | コミット済み (aa84459c) |
| 2 | `SP_STRLEN_DEDUP` — `s[i]` の `sp_str_length` 二重計算除去 | `lib/sp_str.c` | **+17 / −0** | `-DSP_STRLEN_DEDUP` (既定 OFF) | 本コミット |

**現 footprint = 恒久 footprint = `lib/spinel_rt.h` (Patch 1) + `lib/sp_str.c` (Patch 2) の 2 ファイル・計 +37 行。**
一時ビルド修正は無い (pristine `8f65a5b5` が Apple clang 21 / macOS 26.6.1 でクリーンにビルドできる)。

---

### Patch 1: `SP_UNMASKED_SETJMP` — `setjmp/longjmp` → `_setjmp/_longjmp`

- **目的**: Darwin (BSD) の `setjmp`/`longjmp` は**シグナルマスクを保存・復元する** =
  setjmp 1 回・longjmp 1 回ごとに `sigprocmask(2)` を払う。Spinel の大域脱出は
  4 系統すべて (例外 / throw・catch / valued break / 非 lambda `Proc#return`)
  この対を通るため、非局所脱出の重いプログラムでは wall の大半がここに消える。
  `_setjmp`/`_longjmp` は同じジャンプでマスクに触らない版。
- **対応 Edoc**: [E004](../docs/experiments/E004_unmasked_setjmp.md)
  (前段: [E003](../docs/experiments/E003_thinlto_3way_bench.md) §5.2 が
  「非局所脱出の伸び代は LTO ではなく `setjmp`→`_setjmp` にある」と名指しした)
- **改変箇所** (`lib/spinel_rt.h`、最初の `#include <setjmp.h>` (旧 41 行目) の直後、
  **+20 行・追加のみ**):

  ```c
  #if defined(SP_UNMASKED_SETJMP)
  #undef setjmp
  #undef longjmp
  #define setjmp  _setjmp
  #define longjmp _longjmp
  #endif
  ```

  (残り 14 行は理由と境界を書いたコメント + 前後の空行)
- **なぜヘッダ 1 枚で足りるか (= なぜ additive のままでいられるか)**:
  - `longjmp(...)` の呼び出しは **`lib/spinel_rt.h` の 12 箇所のみ**
    (6701 / 6824 / 7056 / 7058 / 7132 / 7134 / 7209 / 7211 / 7265 / 7268 / 7269 / 7270)。
    `lib/*.c` に `longjmp(` は **0 件**。
  - `setjmp(...)` は **codegen が生成 TU へ emit** する (13 emit site: `src/codegen_expr.c:2719`,
    `src/codegen_call.c:8108/9191/9283/21699`, `src/codegen_fold.c:5028`,
    `src/codegen_iter.c:1840`, `src/codegen.c:2053/2058/5855`,
    `src/codegen_stmt.c:5284/5439/8497`)。
  - `spinel_rt.h` を `#include` する TU は **生成 TU 只 1 つ**
    (`src/codegen.c:6637` が emit)。生成 TU はヘッダを**最初に**取り込むので、
    ヘッダ内の `#define` が emit された setjmp すべてに届く。
  - ⇒ **`src/` (codegen) には 1 行も触らない**。触れば additive でなくなる
    (生成 C が変わる = バイト一致ゲートを自力で壊す) ので、この形が必須条件だった。
  - 唯一の例外 `lib/sp_fiber.c:330` の `setjmp(base)` はヘッダを含まない TU なので
    **素の `setjmp` のまま**残る。その buffer は `spinel_rt.h` 側の `_longjmp` で
    読まれるが、これは「保存済みマスクを読まずに捨てる」= 安全側。危険な逆向き
    (`_setjmp` の buffer を素の `longjmp` が読む) は `lib/*.c` に `longjmp(` が
    0 件なので発生しえない。
- **境界 (安全性解析の結論、`docs/experiments/results/E004/safety_analysis.md`)**:

  > **不可**: `Signal.trap(sig) { ... }` に渡した **Proc が大域脱出する** (raise / throw /
  > valued break / `Proc#return`) プログラム。Spinel は `signal(3)` で登録した C ハンドラ
  > `sp_sig_c_handler` (`lib/sp_cold.c:2120-2127`) の**中で直接 Ruby ブロックを実行する**ため、
  > そこから longjmp するとマスクが復元されず、**そのシグナルが以後恒久的にブロックされる**。
  > 実測で再現済み (C プローブ + 実 Spinel バイナリ、`trap_boundary.txt`)。
  >
  > **可 (影響なし)**: 例外 / throw・catch / valued break / `Proc#return` の通常経路、
  > Thread・Mutex・Queue・`SPINEL_PREEMPT_SIGNAL` (SIGURG プリエンプションハンドラは
  > `SP_SAFEPOINT_SET(1)` するだけで longjmp しない、`lib/sp_sched.c:147`)、
  > Fiber / Enumerator、`Signal.trap(sig, "IGNORE"/"DEFAULT"/コマンド文字列)`、
  > `Signal.trap("EXIT") { }` (atexit 経由 = 通常コンテキスト)、
  > sp_net の SIGTERM/SIGINT ハンドラ (フラグのみ)、`SPINEL_GC_VERIFY` の SIGSEGV レポータ。
  >
  > ツリー全体で `sigprocmask` 直接呼び出しは 0 件、`pthread_sigmask` は 1 件
  > (`lib/sp_sched.c:947`、sysmon スレッドの恒久ブロック、setjmp スコープと交わらない)、
  > `sigaltstack` / `sigsetjmp` / `siglongjmp` は 0 件。
  > **マスク復元に依存しているのは上記 1 経路だけ**。

- **有効化 (make の knob)**:

  ```bash
  # 単体バイナリ: 生成 TU にだけ define を渡す (ランタイムアーカイブは無改造で可)
  bin/spinel app.rb --cc='cc -DSP_UNMASKED_SETJMP' -o app

  # スイート / ツリー全体。PCH は $(OPT) をパス名に畳むため、空白を含む COPT を
  # 渡すと PCH パスが割れる。PCH_ROOT をコマンドラインで上書きして隔離する
  # (コマンドライン変数は Makefile の `:=` にも勝つ)。E002 と同じ罠回避。
  make -k test-run COPT="-O2 -DSP_UNMASKED_SETJMP" PCH_ROOT=build/pch/clangO2-unmasked
  ```

  `COPT` は `bin/spinel` 自体・`libspinel_rt{,_mt}.a`・PCH・生成 TU のすべてに届くが、
  `spinel_rt.h` を含まない TU では **inert** (`src/` も `lib/*.c` もヘッダを含まない)。
  実質的に効くのは **生成 TU と PCH だけ**。

  > **`PCH_ROOT` を明示上書きする理由**: Makefile は
  > `PCH_ROOT := build/pch/$(CC_KIND)$(subst -,,$(OPT))` と `$(OPT)` をパス名に畳むため、
  > **空白を含む `COPT` を渡すと PCH パスに空白が入り Make の 2 語に割れる**。
  > コマンドライン変数は Makefile の `:=` にも勝つので上書きで隔離できる
  > (E002 が踏んだ PCH の罠の変種)。

- **挙動差分**: 未設定で完全に既存挙動 (バイト一致、下記)。設定時のみ 4 系統の
  大域脱出が `_setjmp`/`_longjmp` を通る。

- **検証 (2026-08-15, Apple M1 Max / macOS 26.6.1 / Apple clang 21.0.0)**:

  | ゲート | 結果 |
  |---|---|
  | 生成 C (`bin/spinel -S`) 4 本 pre/post | **4/4 バイト一致** |
  | `cc -E -P` (トークン列) on `#include "spinel_rt.h"` の 2 行 TU | **バイト一致** (9583 行) |
  | `cc -E` (行マーカー込み、同一パスで比較) | 差分 167 行、**うち `# N ".../spinel_rt.h"` 以外は 0** (= +20 行のオフセットのみ) |
  | `bm_fib` の `-O2` Mach-O バイナリ pre/post | **バイト一致** (`68a1f90f…`)、stdout も一致 |
  | positive control: `nm -u` フラグ ON | `__setjmp` / `__longjmp` (OFF は `_setjmp` / `_longjmp`) |
  | positive control: `otool -tV` 呼び出しサイト数 | **setjmp 11 / longjmp 10** = `tools/bench/cases/README.md` の凍結基準値と一致 (機構は 1 つも消えていない) |
  | `bm_unwind` stdout sha256 (ON/OFF) | 両方 `0288437…f14d` = 凍結値と一致 |
  | フルスイート (フラグ ON) | **2887 stamps / 2887 PASS / 0 FAIL / 0 ERR** = E001/E002 の同一ホストのベースラインと完全一致 |
  | 危険面のテスト個別確認 (`signal_*` 2 / `thread_*` 20 / `mutex_*`+`monitor_*` 3 / `fiber_*` 8 / `catch_throw_*`+`ensure`+`at_exit_*` 6) | **全 39 本 PASS** |
  | trap 境界 (positive control) | フラグ OFF は CRuby と一致、ON は 2 回目の USR1 が発火しない (既知境界の再現) |

- **性能 (`bm_unwind` @ 250000、hyperfine 1.20.0 `--warmup 3 --runs 10 --shell=none`、
  `caffeinate -i`、同一 hyperfine 起動で 2 構成)**:

  | 構成 | wall min | wall mean ± σ | user | sys | sys / mean wall |
  |---|---|---|---|---|---|
  | フラグ OFF (`setjmp`) | 1.9082 s | 1.9223 ± 0.0230 | 1.1973 | 0.7221 | 37.6% |
  | フラグ ON (`_setjmp`) | **0.2601 s** | 0.2618 ± 0.0014 | **0.2584** | **0.0023** | 0.9% |
  | Δ | **−86.4% (7.34x)** | | **−78.4%** | **−99.7%** | |

  sys は予想通り消えたが、**user も 78% 落ちた** — E001 の C マイクロベンチ
  (3M 往復で user 0.95 s / sys 0.74 s) と符合する。`sigprocmask` はユーザ側の
  コストも大きい。参考: CRuby は同 probe で 0.86 s
  (`tools/bench/cases/README.md`) — **フラグ ON の Spinel が CRuby の 3.3x 速い**
  (OFF では 2.2x 遅かった)。

- **upstream PR 可能性**: **高い**。
  - eBPF/LLVM 固有のロジックを含まない汎用の性能 hook で、名前も upstream-neutral。
  - 追加のみ 20 行・1 ファイル・既定 OFF で、未設定時のバイト一致を実測済み。
  - 議論になるのは境界 (`Signal.trap` ブロックからの大域脱出) の扱い方。
    upstream 側でより良い形にするなら `sp_sig_c_handler` (`lib/sp_cold.c`) で
    proc 呼び出しの前後にマスクを退避・復元して境界を消す案がある
    (CRuby はそもそもハンドラ内で Ruby を走らせず遅延配送する)。
    ただしそれは `lib/sp_cold.c` を触るので**本 patch の additive 性からは外れる** —
    PR では「まず opt-in で入れ、境界解消は別 PR」を提案するのが筋。
  - Linux/glibc での挙動は**未検証** (下記)。PR 前に確認が要る。

- **未検証事項 (honest boundary)**:
  - **Darwin (Apple M1 Max / macOS 26.6.1 / Apple clang 21) でしか検証していない。**
    Linux/glibc では `setjmp` がマクロだったり `_setjmp` の可搬性が違ったりするので、
    `#undef setjmp` を置いてはいるが**未実測**。
  - `lib/sp_fiber.c:330` の setjmp は素のまま (= fiber 生成ごとに 1 回だけ
    `sigprocmask` が残る)。取りに行っていない (回数が無視できる、かつ
    footprint を 1 ファイルに保つため)。
  - ThinLTO 経路 (`tools/llvm/`) との組み合わせは未測定。
  - 計測は 1 セッション・1 回の hyperfine (10 runs)。σ は run 間のばらつきのみ。
    計測中マシンは静音化されていない (load average ~3、別エージェントが並行作業)。
    ただし効果量 (7.34x) がノイズの桁を大きく超えている。

- **適用日**: 2026-08-15 (E004)。**コミットしていない** — working tree のみ。

---

### Patch 2: `SP_STRLEN_DEDUP` — `sp_str_char_at_or_nil` が求めた長さを `sub_range` に渡す

- **目的**: `str[i]` (単一整数添字) は **同じ文字列の文字数を 2 回数える**。
  `sp_str_char_at_or_nil` が境界判定のために `sp_str_length(s)` を呼び、
  その直後に呼ぶ `sp_str_sub_range` が**同じ `s` に対してもう一度** `sp_str_length(s)` を呼ぶ
  (`lib/sp_str.c:926,929`)。UTF-8 の文字数え直しは長さキャッシュ (`sp_str_lcache`) が
  効いてもハッシュ引き + `SP_GC_ROOT_STR` の push/pop + call/ret を払う。
  文字列を 1 文字ずつ舐めるループでは、これが実行時間の 1 割に届く。
- **対応 Edoc**: [E007](../docs/experiments/E007_strlen_dedup.md)
  (前段: [E005](../docs/experiments/E005_profiling_first.md) の
  `results/E005/candidates_assessment.md` §2 が Time Profiler で
  「`sp_str_length` が bm_huffman の自己時間 36.6%、うち呼び出し元 `sp_str_sub_range` の
  318 サンプルは同一文字列に対する重複」と名指しした)
- **改変箇所** (`lib/sp_str.c`、929 行目の `sp_str_char_at_or_nil` を
  `#if defined(SP_STRLEN_DEDUP)` / `#else` / `#endif` で挟む。**+17 行・追加のみ**、
  元の 1 行は `#else` 側に**無改造で**残る):

  ```c
  #if defined(SP_STRLEN_DEDUP)
  /* (13 行のコメント: 同値性の根拠と profile 由来) */
  const char*sp_str_char_at_or_nil(const char*s,mrb_int i){SP_GC_ROOT_STR(s);if(!s)sp_nil_recv("[]");mrb_int cl=sp_str_length(s);if(i<0)i+=cl;if(i<0||i>=cl)return NULL;return sp_str_sub_range_len(s,cl,i,1);}
  #else
  (upstream 929 行目そのまま)
  #endif
  ```

- **なぜ 1 行の呼び先変更で足りるか (= なぜ新しい関数を足さずに済むか)**:
  - **`sp_str_sub_range_len(s, cl, start, len)` は upstream に既にある**
    (`lib/sp_str.c:930`、宣言 `lib/sp_str.h:152`)。「文字数を引数で受け取る `sub_range`」
    そのものである。
  - upstream 自身が **`sp_str_sub_range_r` (931 行) でこのイディオムを使っている** —
    自分で `cl` を 1 回求めて `sub_range_len` に渡す形。
  - ⇒ **929 行だけがそのイディオムから漏れていた**。本 patch は
    漏れを塞ぐだけで、新しい関数も新しい code path も作らない。
  - `src/` (codegen) には 1 行も触らない。`sp_str_char_at_or_nil` の emit site
    (`src/codegen_call.c:4659/4661/20986/21081/21082`, `src/codegen_call_recv.c:6118`) は
    シグネチャが変わらないので**生成 C は不変**。

- **境界 (安全性解析の結論、`docs/experiments/results/E007/safety_analysis.md`)**:

  > **意味論の境界は無い。** 呼び出し地点では
  > (P1) `s != NULL`、(P2) `cl == sp_str_length(s)`、(P3) `0 <= i < cl`、(P4) `len == 1`
  > がすべて成立しており、この下で `sp_str_sub_range(s,i,1)` が余分に実行する文は
  > **`if(!s)sp_nil_recv()` (P1 より no-op) と冗長な `sp_str_length` の 2 つだけ**で、
  > 残りの本体は 2 関数で**逐語的に同一のテキスト**である (逐条表は解析文書 §2)。
  >
  > (P2) が無条件に成立する理由: 1 回目と 2 回目の `sp_str_length` の間に走るのは
  > **整数演算・分岐・シャドウスタックへの push だけで、アロケーションが 1 件も無い**。
  > ⇒ **GC は起こりえず、`s` の指す先も内容も変わらない。**
  > `sp_str_length` 自身の副作用 (`sp_str_lcache` の更新) はポインタ同一性で引く
  > プロセス内キャッシュで、**戻り値を変えず、Ruby から観測できない**。
  >
  > `SP_GC_ROOT_STR` の push 回数も **2 回のまま変わらない**
  > (`sp_str_sub_range_len` も先頭に同じ root を持つ) ので、root 収支は自動的に対称。
  >
  > それでも **opt-in ゲートにしてある** — 意味論の境界があるからではなく、
  > (a) 台帳の一貫性、(b) 「未設定時バイト一致」という検証手段を手放さないため、
  > (c) 回帰時に 1 フラグで切り戻せるため。

- **有効化 (make の knob)**:

  ```bash
  # ツリー全体 (アーカイブが再ビルドされる = Patch 1 と違い、ここが本体)
  make -k test-run COPT="-O2 -DSP_STRLEN_DEDUP" PCH_ROOT=build/pch/clangO2-strlendedup

  # 単体バイナリで効かせるには **ランタイムアーカイブ側**に define が要る。
  # bin/spinel app.rb --cc='cc -DSP_STRLEN_DEDUP' では効かない (生成 TU にしか届かない)。
  make lib/libspinel_rt.a COPT="-O2 -DSP_STRLEN_DEDUP" && bin/spinel app.rb -o app
  ```

  > **Patch 1 との重要な違い**: Patch 1 (`lib/spinel_rt.h`) は生成 TU にだけ届くヘッダなので
  > `--cc` で渡せた。Patch 2 は `lib/sp_str.c` = **`libspinel_rt.a` のメンバー**なので、
  > 効かせるにはアーカイブを再ビルドする必要がある。
  > `PCH_ROOT` をコマンドラインで上書きする理由は Patch 1 と同じ (E002 の PCH の罠)。

- **挙動差分**: 無し。未設定でバイト一致 (下記)、設定時も出力は完全に同一
  (`bm_huffman` / `bm_fasta` の stdout sha256 が E001 の凍結値と ON/OFF どちらも一致)。
  変わるのは実行時間だけ。

- **検証 (2026-08-15, Apple M1 Max / macOS 26.6.1 / Apple clang 21.0.0)**:

  | ゲート | 結果 |
  |---|---|
  | **`build/sp_str.o` (make と同一 cmdline、`libspinel_rt.a` メンバー) pre/post** | **バイト一致** (`ff85590f…`)。**これが主ゲート** — Patch 2 はアーカイブメンバーなので `.o` が真の検査対象 |
  | **`build/mt/sp_str.o` (`-DSP_THREADS`、`libspinel_rt_mt.a` メンバー) pre/post** | **バイト一致** (`3fe2f515…`) — スレッド版アーカイブも落としていない |
  | `cc -E -P` (トークン列) | **バイト一致** (`9d230cb8…`, 3295 行)。差分 **0 行** (条件分岐が同一ファイル内なので行オフセットすら動かない) |
  | `git diff --stat` | **1 file changed, 17 insertions(+), 0 deletions(-)** = additive 厳守 |
  | 生成 C (`bin/spinel -S`) | 不変 (`src/` 無改造。`bin/spinel` も再ビルド不要) |
  | positive control: `llvm-objdump -d -r` の `_sp_str_char_at_or_nil` | **命令オフセットが 1 バイトもずれず** (`def8`/`df20`/`df4c` 完全一致)、差は BRANCH26 の**シンボル名 1 つだけ** (`_sp_str_sub_range` → `_sp_str_sub_range_len`)。GC root の push/pop も両方に残存 |
  | positive control: リンク後バイナリ | フラグ ON の `bm_huffman` で実際に `_sp_str_sub_range_len` が呼ばれている |
  | `bm_huffman` / `bm_fasta` stdout sha256 (ON/OFF) | **4/4 が E001 `bench/checksums.tsv` の凍結値と一致** |
  | フルスイート (フラグ ON、`make -k -j8 test-run COPT="-O2 -DSP_STRLEN_DEDUP" PCH_ROOT=build/pch/clangO2-strlendedup`、事前に `clean-test-results`) | **2887 stamps / 2887 PASS / 0 FAIL / 0 ERR** = E001/E002/E004 の同一ホストのベースラインと完全一致。付随レグも `rbs-test 13 pass` / `rbs-seed-test pass`、`EXIT=0` |
  | 文字列面のテスト個別確認 (`str_*` 26 / `string*` 94 / `slice*` 3 / `bigint*` 12 / `bignum*` 10) | **全 145 本 PASS** |

- **性能 (hyperfine 1.20.0 `--warmup 3 --runs 10 --shell=none`、`caffeinate -i`、
  各ペアを同一 hyperfine 起動でインターリーブ、stock cc-O2 パイプライン)**:

  | benchmark | 構成 | wall min | wall mean ± σ | user mean | Δ(min) |
  |---|---|---|---|---|---|
  | `bm_huffman` 40000000 | OFF | 1.4091 s | 1.4389 ± 0.0314 | 1.4063 | — |
  | | **ON** | **1.2800 s** | 1.3127 ± 0.0253 | **1.2810** | **−9.16% (1.101x)** |
  | `bm_fasta` 2000000 (レグ検査) | OFF | 1.0534 s | 1.0579 ± 0.0031 | 1.0507 | — |
  | | ON | 1.0438 s | 1.0462 ± 0.0020 | 1.0387 | −0.91% (閾値 3% 未満 = 無風) |

  E005 は「≒14%」と見積もっていたが、**その 14% は 606 サンプル
  (`sp_str_sub_range` 318 + `sp_str_char_at_or_nil` 288) を両方消した場合の値**である。
  後者は `str[i]` が nil を返すかの判定 (`i>=cl`) に必要で**消せない**。
  安全に消せるのは 318 サンプル = `sp_str_length` 自己時間の 27.8% =
  ベンチ全体の **≈10.2%** で、**実測 9.16% はこの事前予想とよく合う**。
  台帳としては **「E005 の 14% は上限であって取り分ではなかった」**が正しい記録。

- **upstream PR 可能性**: **非常に高い。Patch 1 より高い。**
  - **意味論の境界がゼロ**。Patch 1 は `Signal.trap` ブロックからの大域脱出という
    実挙動差を持っていたが、Patch 2 は「同じ答えを 1 回で出す」だけ。
  - 置換先 `sp_str_sub_range_len` は **upstream 自身の関数**で、
    **upstream 自身が隣の `sp_str_sub_range_r` で同じ使い方をしている**。
    つまり PR は新機能ではなく「イディオムの適用漏れの修正」として出せる。
  - upstream に出すなら **ゲートを外して無条件にするのが筋** (opt-in である理由が
    spinel-llvm 側の台帳規律だけなので)。その形なら +1 行 −1 行の差分になる。
  - eBPF/LLVM 固有のロジックを一切含まない。

- **未検証事項 (honest boundary)**:
  - **Darwin (Apple M1 Max / macOS 26.6.1 / Apple clang 21) でしか測っていない。**
    意味論に境界が無いので移植性の懸念は Patch 1 より小さいが、
    効果量 (9%) は `sp_str_length` の相対コストに依存するので他環境では別の値になる。
  - **`sp_str_length` の重複はこの 1 箇所しか直していない。**
    同種の重複は `sp_str_index_from` / `sp_str_rindex_from` / `sp_str_ljust2` など
    `cl = sp_str_length(s)` を持つ関数が更に別関数を呼ぶ形で残っている可能性があるが、
    **走査していない**。E005 のプロファイルが名指ししたのはこの 1 本だけ。
  - **`sp_str_lcache` の状態は変わる** (登録試行が 1 回減る)。観測可能な意味論では
    ないが、「別の文字列の長さ計算が遅くなる」という**性能上の**副作用は理論上ありうる。
    `bm_fasta` を no-regression 検査に入れたのはこの穴を踏むためで、−0.91% (改善側)
    だったが、**文字列ベンチ 2 本しか見ていない**。
  - **E001 凍結 8 本のうち 2 本 (huffman / fasta) しか計測していない。**
    残り 6 本 (binary_trees / nbody / spectral_norm / ao_render / fannkuch / pidigits) は
    `str[i]` を踏まないので効果ゼロのはずだが、**確認していない**。
  - **ThinLTO 経路 (`tools/llvm/`) との組み合わせは未測定。** E005 は
    ThinLTO 後に `sp_str_length` の自己時間が 36.6% → 39.9% に**増えた**と観測しており、
    ThinLTO 下での取り分は本測定 (cc-O2) と同じとは限らない。
  - **計測は 1 セッション・1 回の hyperfine (10 runs)。** σ は run 間のばらつきのみ。
    マシンは静音化していない (別エージェントが並行作業中)。
    効果量 9.16% は σ (2.2% / 1.9%) の 4 倍以上あり判定は動かないが、
    **±1% の精度は無い**。
  - **フラグ ON のフルスイートは 1 本のみ実測**。同セッションでの OFF 再走はしていない
    (基準は E001/E002/E004 の 2887 PASS / 0 FAIL / 0 ERR)。
  - **スイートは `str[i]` を大量に踏むが、`sp_str_lcache` のミス側 (非 cacheable /
    binary 文字列) を狙って踏むテストは特定していない。** 効果が最大になるのは
    その形なので、「一番効く形が最もよくテストされている」保証は無い。
  - ベンチ用のフラグ ON アーカイブは `make COPT=...` のフルリビルドではなく
    **`sp_str.o` だけを `ar r` で差し替えて**作った (交絡因子の排除のため)。
    `SP_STRLEN_DEDUP` を参照するファイルが `lib/sp_str.c` のみなのでフルリビルドと
    等価だが、**「本番と同じビルド経路で計測した」わけではない**
    (フルスイートの方は本番経路 = `make COPT=...` で走らせている)。

- **適用日**: 2026-08-15 (E007)。fork へ Patch 2 としてコミット済み。
