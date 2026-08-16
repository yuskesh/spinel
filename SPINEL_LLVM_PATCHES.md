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
| 2 | `SP_STRLEN_DEDUP` — `s[i]` の `sp_str_length` 二重計算除去 | `lib/sp_str.c` | **+17 / −0** | `-DSP_STRLEN_DEDUP` (既定 OFF) | コミット済み (b01289c3) |
| 4 | `SP_INT64` — `mrb_int` をポインタ幅から切り離す | `lib/sp_types.h`, `lib/sp_core.h` | **+53 / −0** | `-DSP_INT64` (既定 OFF) | 本コミット |

**現 footprint = 恒久 footprint = `lib/spinel_rt.h` (Patch 1) + `lib/sp_str.c` (Patch 2) +
`lib/sp_types.h` / `lib/sp_core.h` (Patch 4) の 4 ファイル・計 +90 行。**

> **Patch 3 (bigint GC 会計) は意図的な欠番** — E008 の安全性分析が未ルート中間値の
> サイレント破壊を実証したため実装中止。番号は再利用しない。
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

---

### Patch 4: `SP_INT64` — `mrb_int` をポインタ幅から切り離して `int64_t` に固定する

- **目的**: `lib/sp_types.h:90` の `typedef intptr_t mrb_int;` は「`mrb_int` は
  ポインタ幅に従う」と決めている。64bit ホストでは何も起きないが、**wasm32 (Cloudflare
  Workers 向けターゲット) では Ruby の `Integer` が 32bit になる**。
  問題はそれ自体ではなく、**Spinel の型推論が 64bit ホスト上で走ること**である:
  - 字句解析はリテラルが bigint になる閾値を **`LLONG_MAX`** で判定する
    (`src/spinel_parse.c:355,398`)。
  - `**` / `<<` のオーバーフロー判定はホストの **`long long` + `__builtin_mul_overflow`**
    で行う (`src/analyze_infer.c:515-522, 526-532`)。
  - 定数畳み込みも `long long`、シフト幅の上限は **`>= 64` 決め打ち**
    (`src/codegen_expr.c:376,406,410`)。
  - **`src/` にターゲット幅のパラメータが 1 つも無い** (`-m32` も triple も
    `sizeof(intptr_t)` の参照も 0 件)。

  結果として `2**40` は `TY_INT` と型付けされ `sp_int_pow(2LL, 40LL)` として吐かれ、
  ターゲットの `mrb_int` = `int32_t` で **`RangeError: integer overflow in **`** になる
  (発生源は `lib/sp_cold.c:2445`)。CRuby は `1099511627776` を返す。
  リテラル `1234567890123` は生成 C にそのまま焼かれ、**静かに `1912276171` に化ける**。

  本パッチはターゲット側の幅をホストに合わせることで、**このズレという問題カテゴリごと消す。**

- **対応 Edoc**: E010 (**未執筆** — 生データは spinel-llvm 側の
  `docs/experiments/results/E010/` にある。台帳へ反映する際に Edoc のパスを埋めること)
  (前段: [E009](../docs/experiments/E009_wasm_workers_inventory.md) §11 —
  「本当の壁は整数幅だった」「意味を変えるのはこの 1 項目だけ」)

- **なぜ wasm32 で i64 が「安い」のか (E009 の判定を覆した 2 点)**:

  1. **`sp_RbVal` のレイアウトは動かない。** E009 §11 は緩和策②を
     「additive では済まない — `sp_RbVal` のレイアウトに影響する」と判定していたが、
     ```c
     /* lib/sp_gc.h:55 */
     typedef struct { int tag; int cls_id;
                      union { mrb_int i; const char *s; mrb_float f; mrb_bool b; void *p; } v; } sp_RbVal;
     ```
     `union` の最大メンバーは **`mrb_float` = `double` = 8 バイト**であり `mrb_int` ではない。
     **native / wasm32-i32 / wasm32-i64 の 3 構成すべてで `sizeof(sp_RbVal) == 16`。**
  2. **性能も落ちない。** wasm32 は本物の 32bit CPU と違い **i64 ALU をネイティブに持ち**、
     V8 はそれを 64bit レジスタで実行する。実測 (node v24.12.0):
     `fib(35)` w32 50.0 ms / **w64 49.9 ms**、30M 回ループ w32 80.2 ms / **w64 80.3 ms** —
     **差は測定限界以下 (±0.5%)**。E009 §11 の「i64 演算がエミュレーションになり性能は落ちる」は
     **実測で棄却された**。

- **改変箇所 (2 ファイル・+53 行・追加のみ)**:

  **(a) `lib/sp_types.h`** — 85-92 行の幅ガードと typedef を `#if defined(SP_INT64)` /
  `#else` / `#endif` で挟む。**元の 8 行は `#else` 側に無改造で残る**。+39 行:

  ```c
  #if defined(SP_INT64)
  #undef  INTPTR_MAX
  #define INTPTR_MAX INT64_MAX
  #undef  INTPTR_MIN
  #define INTPTR_MIN INT64_MIN
  typedef int64_t  mrb_int;
  typedef uint64_t mrb_uint;
  typedef double   mrb_float;
  typedef bool     mrb_bool;
  #else
  (upstream 85-92 行そのまま)
  #endif
  ```
  (残り 26 行は動機と境界を書いたコメント)

  **(b) `lib/sp_core.h`** — `#include <stddef.h>` の直後に追加。+14 行:

  ```c
  #if defined(SP_INT64)
  #include "sp_types.h"
  #define intptr_t mrb_int
  #endif
  ```
  (残り 10 行はコメント)

- **なぜ typedef 1 行では足りないのか (= なぜ `INTPTR_MAX` を再定義するのか)**:
  - `INTPTR_MAX` / `INTPTR_MIN` は **ツリー内 約 45 箇所** で使われているが、
    **1 件もポインタ限界として使われていない**。すべて `mrb_int` の限界である:
    beginless/endless Range のセンチネル (`sp_range.h:37-61`, `sp_re.c:958-959`,
    `spinel_rt.h:740-747, 2457-2465, 3263-3297, 7920`)、`SP_INT_NIL` (`sp_types.h:115`)、
    オーバーフロー保護 (`sp_core.c:433-435`, `sp_array.c:40-47`, `sp_format.c:280-345`)、
    幅セレクタ (`spinel_rt.h:290` の `sp_i64_to_int`, `sp_core.c:25` の
    `SP_INT_POW10_LIMIT`, `sp_format.c:274`)。
  - **codegen (`src/`) を 1 行も触らずに済むのはこの形のおかげ**である。
    `src/codegen_expr.c:882,884` ほか約 20 箇所の `INTPTR_MIN`/`INTPTR_MAX` は
    **生成 C へ吐く文字列リテラル**なのでターゲット側の定義に追随する。
    Patch 1 と同じ「codegen に触れば additive でなくなる」制約をここでも守っている。
  - `lib/sp_core.h` の 21 プロトタイプは `intptr_t` を **`mrb_int` の綴りとして**使っている
    (ヘッダ冒頭のコメントが自らそう書いている: "Signatures use intptr_t / double directly
    (== mrb_int / mrb_float)")。ここを追随させないと、生成 TU が i64 を渡すのに
    `libspinel_rt.a` 側が i32 で受ける **静かな ABI 割れ**になる (リンクは通ってしまう)。
    `#define intptr_t mrb_int` は `lib/sp_core.h` を含む **3 TU
    (`sp_core.c` / `sp_time.c` / 生成 TU) にしか届かない**。`lib/sp_sched.c` の
    `(void*)(intptr_t)wid` (本物のポインタ用途) は `sp_core.h` を含まないので無影響。
  - `lib/sp_core.c:21` の独自 `typedef intptr_t mrb_int;` は
    `typedef mrb_int mrb_int;` に展開され、C11 の冗長再宣言として合法。

- **境界 (安全性解析の結論、`docs/experiments/results/E010/couplings_audit.md`)**:

  > **ポインタを `mrb_int` に詰める既存の 6 群はすべて安全。** wasm32 のポインタは 32bit で、
  > i64 の `mrb_int` はこれをゼロ拡張で保持し `(uintptr_t)` へ戻すとき下位 32bit を取る。
  > リトルエンディアンなので GC の closure cell (`sp_gc.h:373-383` の
  > `*(void**)p` による下位半分読み) も正しい。`sp_BoundMethod.fn` (`sp_proc.h:64`) に
  > 関数ポインタを入れる形、`#hash` の `(uintptr_t)` 混ぜ込み、`sp_sym`、
  > 配列長フィールドも同様。
  >
  > **Patch 4 が「ついでに直す」もの 5 件**: `sp_IntArray_ffi_data` (`sp_array.c:575`) の
  > `mrb_int*` → `const int64_t*` **無変換 reinterpret** (32bit では型 punning バグ)、
  > `SP_INT_NIL` の 32bit 既知限界 (`sp_types.h:100-114`)、
  > `spinel_rt.h:3772` の `depth = INT64_MAX` が `-1` に化ける件 (`Array#flatten` 深さ無制限)、
  > `spinel_rt.h:4412` の `(mrb_int)1<<40` のシフト幅超過 UB、
  > そして canonical value ABI が `i32(i32)` から **`i64(i64)` = native / eBPF と同じ**に戻ること。
  >
  > **残る意味論のズレは 1 件だけ — `Marshal`。** `lib/sp_marshal.c` は
  > `long` 幅で符号化する (`:45,110,175,226,232`)。wasm32 では `long` = 4 バイトなので
  > `mrb_int` が 64bit でも dump/load が i32 で切れる。実測:
  > `Marshal.load(Marshal.dump(1099511627776))` が native `1` / w32 `0` / **w64 `0`**。
  > **Option A (フラグ無し) でも同じ値を返す**ので Patch 4 が新たに壊しているわけではないが、
  > 「native と意味論が一致する」という主張の唯一の例外である。修正は `long` → `mrb_int` の
  > 置換 5 箇所で、**既存行の書き換えになるので本パッチの additive 性からは外れる** — Patch 5 案件。
  >
  > **未踏の理論上の穴 1 件**: `spinel_rt.h:1356,1366,2882` は `uintptr_t` を
  > 「符号なし `mrb_int`」の意味で使っている。wasm32 では `uintptr_t` が 32bit のままなので
  > `n >= 32` のシフトが UB になる。ただし本実験の 3 プローブはどれもここに到達しなかった
  > (codegen は定数シフトを `uint64_t` 明示、変数シフトを `sp_int_shl_ck`、
  > `x <<= const` を生の C シフトに落とす)。恒久化するなら `mrb_uint` への置換が要る。

- **有効化 (make の knob)**:

  ```bash
  # wasm32 (本来の用途)。ランタイム 28 TU と生成 TU の *両方* に渡す。
  # 片方だけだと sp_RbVal が同じ 16 バイトなのでリンクは通り、値だけ静かに壊れる。
  clang --target=wasm32-wasip1 --sysroot=$SYS -O2 -DSP_INT64 -c lib/*.c ...
  bin/spinel app.rb -c && clang --target=wasm32-wasip1 -DSP_INT64 -c app.c

  # ツリー全体 (native。回帰確認用)
  make -k test-run COPT="-O2 -DSP_INT64" PCH_ROOT=build/pch/clangO2-int64
  ```

  > **Patch 1 / 2 との違い**: Patch 1 (`spinel_rt.h`) は生成 TU にだけ届くので `--cc` で渡せた。
  > Patch 2 (`sp_str.c`) はアーカイブメンバーなのでアーカイブの再ビルドが要った。
  > **Patch 4 は両方**に届かないと ABI が割れる — 最も渡し漏れが危険なフラグである。
  > 恒久化するなら `bin/spinel` の driver フラグ (`--int-width=64` 等) にして
  > アーカイブ選択と `-D` を同時に決める形が正しい (**未実装**)。
  > `PCH_ROOT` を明示上書きする理由は Patch 1 / 2 と同じ (E002 の PCH の罠)。

- **挙動差分**: 未設定で完全に既存挙動 (バイト一致、下記)。設定時のみ `mrb_int` /
  `mrb_uint` / `INTPTR_MAX` / `INTPTR_MIN` / `sp_core.h` の ABI が 64bit 固定になる。
  **64bit ホストでは意味論の差分ゼロ** (差分ラン 531 テストで 0 行差)。

- **検証 (2026-08-15, Apple M1 Max / macOS 26.6.1 / clang 22.1.8 + wasi-libc 33 / node v24.12.0)**:

  | ゲート | 結果 |
  |---|---|
  | **`cc -E -P` (トークン列) — `sp_types.h` / `sp_core.h` / `spinel_rt.h` の 3 TU、フラグ未設定** | **3/3 バイト一致** (161 / 155 / **9583** 行。行数も動かない = `#if` で丸ごと消えている) |
  | **`.o` ハッシュ (make と同一 cmdline) — `sp_core.o` / `sp_cold.o` / `sp_array.o` / `sp_format.o` / `sp_alloc.o`** | **5/5 バイト一致**。主ゲートは `sp_core.o` (Patch 4 が触った `sp_core.h` を含む唯一のアーカイブメンバー) |
  | `git diff --numstat` | **2 files changed, 53 insertions(+), 0 deletions(-)** = additive 厳守 |
  | 生成 C (`bin/spinel -c`) | 不変 (`src/` 無改造) |
  | **native 差分ラン: `test/*.rb` 名前順 500 本、専用アーカイブ、フラグ ON/OFF** | **両側 498 PASS / 2 FAIL、`diff` 0 行**。2 FAIL は `.args`/ARGV を渡さないハーネスの人工物でフラグ非依存 |
  | **native 差分ラン: 幅に敏感な 31 本** (`integer_literal_no_int32_overflow` / `int_overflow_op_assign` / `pack_endian_modifiers` / `bignum_*` / `marshal_roundtrip` ほか) | **両側 31/31 PASS、`diff` 0 行** |
  | **wasm32 (a) `2**40`** | フラグ OFF: `RangeError`, exit 1 / **フラグ ON: `1099511627776`** |
  | **wasm32 (b) E009 の `rich.rb`** | **native とバイト一致** (フラグ OFF でも一致) |
  | **wasm32 (c) `int_width_probe.rb`** | フラグ OFF: 1 行目で raise / **フラグ ON: 5 行すべて native と一致** |
  | **wasm32 (d) `bm_unwind` (48bit checksum)** | フラグ OFF: `RangeError: integer overflow in *`, stdout 0 B / **フラグ ON: ARGV 20000・250000 の両方で native とバイト一致** (`checksum 58736352199621`) |
  | **wasm32: 凍結ベンチ 10 本 (E001 の 8 本 + `bm_unwind`/`bm_dispatch`)** | フラグ OFF **7/10** (`bm_huffman`/`bm_unwind`/`bm_dispatch` が RangeError) / **フラグ ON 10/10 が native とバイト一致** |
  | wasm32: 静かな narrowing プローブ 6 行 | フラグ OFF は 4 行が静かに誤答 + 1 行 raise / **フラグ ON は 6/6 一致** |
  | positive control: `Integer#size` | native 8 / wasm32 フラグ OFF **4** / **フラグ ON 8** |
  | negative control: native で `-DSP_INT64` を付けた `.o` | 32 個中 **31 個バイト一致、`sp_cold.o` のみ差分**。原因は Darwin arm64 で `intptr_t`=`long`、`int64_t`=`long long` が**幅は同じでも別型** = **TBAA ディスクリプタが変わる**こと。プリプロセス後の差は `…L` → `…LL` のサフィックスのみ (値は同一) |

- **性能・サイズ (node v24.12.0、単発 3 回。**load average 25-34 の汚染下**)**:

  | 指標 | フラグ OFF (i32) | フラグ ON (i64) | Δ |
  |---|---|---|---|
  | `fib(35)` | 49.7 / 50.1 / 50.1 ms | 50.2 / 49.7 / 49.8 ms | **±0.2% (差なし)** |
  | 30M 回ループ | 79.9 / 80.9 / 79.9 ms | 80.0 / 80.4 / 80.6 ms | **±0.5% (差なし)** |
  | `rich.wasm` raw / gzip | 291,763 B / **105,174 B** | 292,588 B / **106,325 B** | **+0.28% / +1.09%** |
  | モジュールサイズ (12 プログラム) | — | — | raw **+0.17〜1.27%** / gzip **+0.55〜1.81%** |
  | 起動 (`rich.wasm` compile / instantiate) | 0.5 ms / 1.2 ms | 0.5-0.6 ms / 1.2 ms | 不変 |
  | 線形メモリ: `fib` / `rich` / `bm_fasta` | 0.88 / 0.88 / 1.44 MB | 0.88 / 0.88 / 1.44 MB | **±0** |
  | 線形メモリ: 200 万要素の `Array<Integer>` | **17.81 MB** | **34.81 MB** | **+17 MB (1.95x)** |

  E009 の差別化点「105 KB gzip = Workers の 1 MB 制限の約 10%」は **106 KB になるだけ**。
  **唯一の実コストは整数配列を大量に持つプログラムのヒープが約 2 倍になること。**

- **upstream PR 可能性**: **中〜高。ただしこの形のままでは出せない。**
  - 汎用性は高い — 「`mrb_int` をポインタ幅から切り離す」は wasm32 に限らず
    **任意の 32bit ターゲットで Ruby の意味論を保ちたい場合の唯一の答え**であり、
    eBPF/LLVM 固有のロジックを含まない。upstream 自身が `sp_types.h:78-114` で
    32bit プロファイルの限界 (`SP_INT_NIL` の衝突、64bit 値の narrowing) を
    自覚しているので、議論の土台は既にある。
  - しかし `#undef INTPTR_MAX` と `#define intptr_t mrb_int` は
    **「未設定時バイト一致」を守るための計測用の形**であって恒久形ではない。PR にするなら:
    1. `SP_INT_MAX` / `SP_INT_MIN` を導入し `INTPTR_MAX`/`INTPTR_MIN` の約 45 用途を置換
    2. `sp_core.h` の 21 プロトタイプを `intptr_t` → `mrb_int` に
    3. `lib/sp_marshal.c` の `long` → `mrb_int` (5 箇所)
    4. `spinel_rt.h:1356,1366,2882` の `uintptr_t` → `mrb_uint`
    5. `spinel_rt.h:3772` の `INT64_MAX` と `:4412` の `1<<40`、
       `sp_int_shl_ck` (`:4592`) の `63` 決め打ちを幅追随に
       — **これは Option A を採る場合でも必要なバグ修正である**
  - **より本質的な upstream 提案は別にある**: コンパイラにターゲット幅パラメータを入れること。
    `src/spinel_parse.c` の `pm_int_overflows`、`src/analyze_infer.c` の
    `infer_int_pow_overflows` / `infer_int_shl_overflows`、`src/codegen_expr.c` の
    `fold_int_node` の **4 箇所**にターゲット幅を通せば、32bit プロファイルを選んだときに
    `2**40` が正しく `TY_BIGINT` に昇格する。Patch 4 と排他ではなく、
    **「32bit を選ぶ自由」を残したいなら必須**の作業である。

- **未検証事項 (honest boundary)**:
  - **wasm 上でスイートを 1 本も走らせていない。** wasm 側の検証は
    プログラム 20 本 (`fib` / `fib35` / `loopy` / `rich` / `int_width_probe` /
    `silent_probe` / `coupling_probe` / `shl_probe` / `shl2_probe` / `memhog` +
    凍結ベンチ 10 本) の stdout 比較のみ。WASI ファイルシステムが要るスイート実走は次の実験の仕事
    (E009 の同じ boundary を引き継いでいる)。
  - **native 差分ランは 2856 本中 531 本 (500 + 31)。** 全数ではない。
    しかも `make test-run` の本番経路ではなく**専用アーカイブ + 手組みリンク**である
    (`vendor/spinel/build` と `lib/*.a` を汚さないため)。`.args` サイドカー・
    `.err.expected`・`SP_THREADS` アーカイブ選択・PCH は再現していない。
  - **`SP_THREADS` ビルドを 1 度もコンパイルしていない。** 静的には安全と判定した
    (`#define intptr_t` が届く TU は 3 つで、`sp_sched.c` は含まれない) が未実測。
  - **`workerd` 上で 1 度も走らせていない。** node の `node:wasi` のみ。
    E009 は workerd まで到達しているので、そこは回帰していることになる。
  - **Linux / x86_64、および wasm 以外の実 32bit ターゲット (ARM Cortex-M 等) で未検証。**
    Patch 4 の効果は「32bit ターゲットで `mrb_int` を 64bit にする」ことなので、
    **i64 ALU を持たない本物の 32bit CPU では性能ペナルティが出るはず** — wasm32 の
    「タダ」という結論をそのまま持ち出してはならない。
  - **計測は hyperfine 無し・σ 無し・並行負荷あり (load 25-34)。**
    サイズは決定的なので信頼できるが、時間は「差が測定限界以下」という定性判定まで。
  - **メモリは線形メモリの終了時サイズを 4 形状で読んだだけ** (ピークでも RSS でもない)。
  - **`Marshal` の残ズレは修正していない** (上記境界)。
  - **`-DSP_INT64` の渡し漏れを検出する仕組みが無い。** 片側だけに渡すと
    リンクは通り実行時に値だけ壊れる。driver フラグ化が要る。

- **適用日**: 2026-08-15 (E010)。**コミットしていない — working tree のみ、既定 OFF。**

---

## バグ修正コミット (非ゲート)

このセクションは **Patch 1/2/4 とは種類の違う改変**を記録する。
Patch 群が**性能・移植性のための opt-in 実験**であるのに対し、
ここに載るのは **upstream の健全性バグに対する修正**である。

### 方針 — なぜ soundness 修正はゲートしないのか

冒頭の「改変方針」(opt-in のみ / additive only / **フラグ未設定時は出力バイト一致**) は
**性能・移植性パッチの規律であって、バグ修正には適用しない**。理由は 3 つある。

1. **ゲート = 既定で wrong code。** 「フラグを立てたときだけ正しい GC ルートを張る」
   という形は、裏返せば「**既定では未ルートのまま誤答する**」という意味になる。
   ゲートは「新しい挙動を選択的に入れる」ための道具であって、
   「正しさを選択的に入れる」ための道具ではない。
   `SP_UNMASKED_SETJMP` を OFF にしたビルドは**遅いだけ**だが、
   `SP_GC_ROOT` を OFF にしたビルドは**間違っている**。この差は本質的である。
2. **「未設定時バイト一致」という検証手段が原理的に成立しない。**
   この規律が意味を持つのは「ゲートを外せば upstream と 1 バイトも変わらない」ことを
   毎回実測できるからだが、バグ修正の**目的そのもの**が出力を変えること
   (誤答 → 正答、SIGSEGV → 完走) である。バイト一致は満たせないし、
   満たしてしまったら修正が効いていない。
   代わりの検証は **(a) 全件スイート緑 / (b) ストレス下の赤集合が単調に減ること /
   (c) 元から通っていたプログラムの stdout がバイト一致すること** の 3 点で行う
   ((c) だけが「バイト一致」の生き残りで、**対象がバイナリではなく出力**である点が違う)。
3. **行き先が違う。** Patch 1/2/4 は「fork にだけ載る spinel-llvm の都合」だが、
   バグ修正は **upstream (matz/spinel) に返すべきもの**である。
   fork に載せるのは upstream PR が通るまでの繋ぎであって、
   `docs/upstream_candidates.md` の該当行と 1:1 で対応させる必要がある。
   **したがってゲートしないが、台帳には必ず記録する** — 記録の目的は
   「無効化できるようにすること」ではなく、**追跡可能性 (どのコミットが何を直したか) と
   upstream PR へのマッピング**である。

**additive only は結果的にほぼ守られている**が、それは規律としてではなく
バグの性質による — 未ルート値に `SP_GC_ROOT` を足す修正は定義上加算的である。
E020 の codegen 側だけが既存行を書き換えており (`emit_expr` 直置き →
ルート付き一時への lowering)、これは**同じファイル内の `Hash#slice` が既に書いている形**に
揃えたものである。

**逆に、ゲートの byte-identity 性質は本セクションの修正によって壊れていない** —
Patch 1/2/4 のフラグ OFF ビルドは修正の前後で `.o` バイト一致を保つ (§検証)。
バグ修正は Patch 群と**直交している**。

### 一覧

| # | 名前 | 層 | ファイル | ± 行 | Edoc | upstream 台帳 |
|---|---|---|---|---|---|---|
| **B1** | インライン化 yielding `initialize` が構築中インスタンスを未 root | codegen | `src/codegen_call.c` | **+10 / −0** | [E019](../docs/experiments/E019_set_enumerable_gc_corruption.md) | **S4 / A1** |
| **B2** | `Hash#merge` の両オペランドが未 root | codegen + runtime | `src/codegen_call_recv.c`, `lib/spinel_rt.h` | **+29 / −8** | [E020](../docs/experiments/E020_hash_predicates_missing_root.md) | **S5** (= S2 と同族) |
| **B3** | `sp_program_name` (`$0`) がルート集合に居ない | runtime | `lib/spinel_rt.h` | **+10 / −0** | [E021](../docs/experiments/E021_stress_failure_taxonomy.md) §4 | **S3** |
| **B4** | `sp_box_brat` の `num`/`den` が未 root | runtime | `lib/sp_cold.c` | **+10 / −0** | [E021](../docs/experiments/E021_stress_failure_taxonomy.md) §3e (C1) | **S2** |
| **B5** | `sp_brat_to_s` / `sp_brat_inspect` の文字列中間値が未 root | runtime | `lib/sp_cold.c` | **+14 / −3** | [E021](../docs/experiments/E021_stress_failure_taxonomy.md) §3e (C2) | **S2** |
| **B6** | `sp_MatchData_inspect` の `sp_String` builder が未 root | runtime | `lib/sp_re.c` | **+8 / −1** | [E021](../docs/experiments/E021_stress_failure_taxonomy.md) §3c (D1) | **S8** (D のランタイム側) |

**合計 +81 / −12、5 ファイル** (`src/codegen_call.c` / `src/codegen_call_recv.c` /
`lib/spinel_rt.h` / `lib/sp_cold.c` / `lib/sp_re.c`)。
**ゲート無し = 常時有効。** 恒久 footprint の勘定は Patch 群 (+90 行) とは**別に**数える。

さらに **回帰テスト 2 本**を `test/` に追加している (下記 §回帰テスト)。

### 共通の根因 — 「未ルートの窓」

6 件すべてが**同じ 1 つの形**である:

> **GC が管理するオブジェクトが、C のローカル変数・関数実引数・無名一時のいずれかにしか
> 存在しない状態で、その先で割り当て (= 収集点) を踏む。**
> collector はシャドウスタック (`sp_gc_roots[]`) / 停止中の fiber / globals hook しか
> 走査しないので、C スタック上のそのポインタは**見えない**。
> 収集が窓に入るとオブジェクトは回収され、
> 以降の読み書きは解放済みメモリに対して行われる。

違うのは**どの emit site / どの関数が窓を開けているか**だけで、
修正はすべて「窓の手前で `SP_GC_ROOT` を張る」に落ちる。
既定の閾値 (256 KiB) では窓に収集が入る確率が低いので**普段は発現しない**が、
`SPINEL_GC_STRESS=1` (閾値 2048 バイト)・大きな割り当てを含む本体・
codegen 摂動のいずれかで確実に発現する。**ストレスはバグを作らず、窓を開けるだけである。**

---

### B1: インライン化 yielding `initialize` が構築中インスタンスを未 root

- **根因 (1 行)**: `src/codegen_call.c:159` `emit_ctor_yield_inline()` が
  `sp_X_new()` の直後に `SP_GC_ROOT` を張らないまま `initialize` の本体を展開するため、
  **構築中のインスタンス自身**が本体の割り当てで回収され、
  文式のあとに呼び出し元が張るルートが**死んだオブジェクトのスロット**になる。
- **Edoc**: [E019](../docs/experiments/E019_set_enumerable_gc_corruption.md)
  (`docs/experiments/results/E019/root_cause.md` が機構、`blast_radius.txt` が影響半径)
- **改変**: `src/codegen_call.c` **+10 / −0** (実質 1 行 + 9 行のコメント)。
  同じ関数がインライン本体の**ローカルには**既に `SP_GC_ROOT` を張っており、
  **インスタンスだけが漏れていた**。`is_val` (value type) は by-value の構造体で
  GC ポインタではないので除外する。
- **なぜゲートしないか**: 発現条件は「`initialize` が `yield` を持つクラスの `new`」で
  **`Set` 固有ではない** (`def initialize; …; yield self; end` 形のユーザークラス全部)。
  ゲート OFF は「そのクラス群を既定で壊しておく」ことを意味する。
- **検証**:
  - 影響半径は**全数**で測ってある — `test/*.rb` 2856 本を修正前後のコンパイラで C に落として
    1 本ずつ `cmp`: **生成 C が変わるのは 51 本 (1.8%)・追加 247 行・削除 0 行・
    追加行は `SP_GC_ROOT(_tN);` の 1 種類のみ** = **厳密に加算的**。
  - `set_*`/`enum*` 83 本: verifier abort **12 → 3**、赤 **18 → 10**、新しい赤 **0**。
  - E021 が独立に S4 の 44 件へ当て直し、**A1 クラスタ 8 件が 8/8 PASS**。
  - **E024 (本統合)**: ストレス全件で **A1 クラスタ 8 件が 8/8 緑**
    (E021 の `xcheck_e019_patch.tsv` と 1 件も違わない)。
    新規回帰テスト `test/ctor_yield_inline_self_rooted.rb` が
    **既定閾値で修正前 SIGSEGV 10/10 → 修正後 PASS**。
- **upstream**: `docs/upstream_candidates.md` **S4** (クラスタ A1)。PR 適性 **高**、ゲート不要。

### B2: `Hash#merge` の両オペランドが未 root

- **根因 (1 行)**: `src/codegen_call_recv.c:5137` が `emit_expr(recv)` を C の実引数位置に
  直接置くため、`h.merge(x).merge(y)` (多引数 merge が lower される形でもある) の
  **内側 merge の戻り値**がどこからもルートされず、外側 merge 冒頭の `sp_XHash_new()` が
  それを回収し、`calloc` が**同じブロックをゼロ埋めで返す** (`outer == inner`)。
- **Edoc**: [E020](../docs/experiments/E020_hash_predicates_missing_root.md)
  (`results/E020/root_cause.md` / `alloc_trace.txt` が `outer == inner` の実証)
- **改変**: `src/codegen_call_recv.c` **+27 / −6** (同変種 merge `:5137` と
  異変種 str merge `:5112` の 2 箇所を**両オペランドをルート付き一時に落とす
  statement-expression** に変える) + `lib/spinel_rt.h` **+2 / −2**
  (`sp_StrPolyHash_merge` / `sp_SymPolyHash_merge` に `SP_GC_ROOT(a); SP_GC_ROOT(b);`)。
  **本セクション唯一の「既存行の書き換え」**だが、
  移行先は**同じファイルの `Hash#slice` (`:5170`) と bigint bitwise が既に書いている形**である。
  兄弟の `sp_PolyPolyHash_merge` / `sp_SymPolyHash_update` は既に root を持っており、
  この 2 本だけが欠けていた = **設計判断ではなく漏れ**。
- **なぜゲートしないか**: **プラグイン不要・生成 C 無編集の 3 行の純 Ruby が
  無改造 stock を `-O0`〜`-O3` すべてで落とす** (`{a: 1, b: 2, c: 3}` が `{c: 3}` になる)。
  ゲート OFF = `Hash#merge` の連鎖が既定でサイレント誤答する。
- **検証**:
  - blast radius: 2856 件の `-S` 出力比較で**バイト一致 2772 / 一時番号ずれのみ 64 /
    実質差分 20**、実質差分の**変更行は 1 行残らず `Hash_merge` を含む**。
  - plain 全件 **2887 pass / 0 fail / 0 error** (LIB のみ / CG+LIB の 2 構成とも)。
  - stress 下、生成 C が変わった 84 件を直接対照: **FAIL 16 → 15**、
    `hash_ctor_merge_clone_transform` が **FAIL 5/5 → PASS 5/5**、**PASS → FAIL は 0**。
  - **E024 (本統合)**: ストレス全件でも `hash_ctor_merge_clone_transform` が緑。
    回帰テスト `test/hash_merge_chain_rooted.rb` を suite に常設。
- **upstream**: **S5**。S2 (bigint オペランド) と**同じ emit 族の穴**なので、
  報告は S2 と 1 本にまとめる。PR 材料 (純 Ruby 再現体・既定閾値で落ちる回帰テスト・
  パッチ・blast radius・全件緑) は揃っている。

### B3: `sp_program_name` (`$0`) がルート集合に居ない

- **根因 (1 行)**: `main()` が `sp_program_name = sp_str_dup_external(argv[0])` で
  str-heap から確保するのに `sp_re_mark_globals` がそれを mark しないため、
  **最初の `sp_str_sweep` が回収**し、以降の `$0` / `$PROGRAM_NAME` 読み出しが
  すべて use-after-free になる。
- **Edoc**: [E021](../docs/experiments/E021_stress_failure_taxonomy.md) §4
  (前段の機構は [E017](../docs/experiments/E017_gcroots_soundness_fix.md) §R2)
- **改変**: `lib/spinel_rt.h` **+10 / −0** (`sp_re_mark_globals` に
  `sp_mark_string(sp_program_name);` の **1 行** + 9 行のコメント)。
  同関数には **`sp_argv` を全く同じ理由で mark する既存コード**が隣にあり
  (`:1165` のコメントが「`sp_str_dup_external` した argv 文字列は mark しないと
  最初の sweep で消える」と明言)、**`$0` はその隣に並ぶだけ**である。
  E017 が挙げたもう 1 案 (codegen が `$0` を巻き上げる) は読み出し側の対症療法で、
  `$0` を読む経路が増えるたびに再発するので採らない。
- **なぜゲートしないか**: 初期値の rodata `SPL("")` 以外、**`$0` を読むプログラムは
  既定で壊れている**。摂動 (`-fno-unroll-loops` 単独) で落ちる程度の薄氷である。
- **検証**: 系統 cc・stress 下 5 回走行で正答した回数 —
  stock **1/5** (摂動下 **0/5**) → パッチ適用 **5/5 (摂動の有無によらず)**。
  クラスタは 1 件で閉じている (S4 の 44 件に `$0` を読むテストが無いので追加の効果は 0)。
  **E024 (本統合)**: ストレス全件を回すと `literal_string_markers` が base で赤・fix で緑になり、
  **S4 の 44 件の「外」に居た 1 件が実際に落ちた** — E021 §3e の「S3 は 0 件」は
  「44 件の中では 0 件」の意味だったことがはっきりした。
- **upstream**: **S3**。

### B4: `sp_box_brat` の `num`/`den` が未 root

- **根因 (1 行)**: `lib/sp_cold.c:2959` `sp_box_brat()` が `num`/`den` を
  C の実引数にしか置かないまま末尾で `sp_gc_alloc` を呼ぶ
  (関数中の他の `sp_bigint_*` はすべて `sp_gc_alloc_nogc` なので**そこが唯一の収集点**)。
  回収されると `sp_bigint_finalizer` が `mpz_clear` + `free(limbs)` し、
  直後の `p->num = num` が**解放済みポインタを publish** する。
- **Edoc**: [E021](../docs/experiments/E021_stress_failure_taxonomy.md) §3e (クラスタ C1)
- **改変**: `lib/sp_cold.c` **+10 / −0** (`SP_GC_ROOT(num); SP_GC_ROOT(den);` の 1 行 + コメント)。
- **なぜゲートしないか**: 症状が **`(0/0)` と印字**するサイレント誤答である。
  数字が機構を語っており (`mpz_clear` 済みの limb を読んでいる)、
  「フラグ次第で有理数が 0/0 になる」ビルドを既定にする理由が無い。
  `sp_brat_*_poly` の全呼び出し元をここ 1 箇所でまとめて覆える。
- **検証**: stress 下 2 件が緑 (E021 `validation_runtime_patches.tsv`)。
  **E024 (本統合)**: ストレス全件で `bignum_rational_arith` /
  `rational_bigint_and_complex_zero` の 2 件が緑 (E021 の測定と一致)。
  **`bignum_rational_basic` / `bignum_receiver_methods` は赤のまま** —
  E008 §2.1 が挙げた `lib/sp_bigint.c` の `gcd`/`lcm` 中間値が未着手だからで、想定どおり。
- **upstream**: **S2**。E008 が予言した族の**実際に発火している site** の 1 つ
  (発火していたのは codegen の算術式ではなくランタイム内部だった)。

### B5: `sp_brat_to_s` / `sp_brat_inspect` の文字列中間値が未 root

- **根因 (1 行)**: `lib/sp_cold.c:3010` が
  `ns = sp_bigint_to_s(r->num)` を root しないまま次の `sp_bigint_to_s(r->den)` を呼ぶため、
  **分子の文字列が分母の整形中に回収**され、`"(/3)"` (分子だけ空) と印字される。
- **Edoc**: [E021](../docs/experiments/E021_stress_failure_taxonomy.md) §3e (クラスタ C2)
- **改変**: `lib/sp_cold.c` **+14 / −3** — 中間値を名前付きにして
  `SP_GC_ROOT_STR` を各段に張る (`sp_brat_inspect` も同型なので同時に直す)。
  **本セクションで既存行を書き換えるもう 1 件**だが、
  書き換えは「入れ子式を 1 段ずつ let に開く」だけで意味論は同じ。
- **なぜゲートしないか**: B4 と同じ — サイレント誤答であり、既定で正しくあるべき。
- **検証**: stress 下 `bigint_to_s_is_a_heap_string` が緑
  (E021 `validation_runtime_patches.tsv`)。**E024 (本統合)**: ストレス全件でも同じ 1 件が緑。
- **upstream**: **S2** (B4 と 1 本で出す)。

### B6: `sp_MatchData_inspect` の `sp_String` builder が未 root

- **根因 (1 行)**: `lib/sp_re.c:190` の builder `sp_String *b = sp_String_new(...)` が
  未 root のままループ内で `sp_str_inspect` / `sp_str_substr` (= 割り当て) を呼ぶため、
  `b` とそのデータバッファが回収され、次の `sp_String_append` が
  解放済みポインタを realloc して **macOS libmalloc が SIGABRT** する。
- **Edoc**: [E021](../docs/experiments/E021_stress_failure_taxonomy.md) §3c (クラスタ D1)
- **改変**: `lib/sp_re.c` **+8 / −1** (`SP_GC_ROOT(b);` の 1 行 + コメント)。
  ランタイムの同型 `sp_IntArrayPtrArray_inspect` (`lib/spinel_rt.h`) は
  **既に `SP_GC_ROOT(s)` を持っている** — ここだけが揃っていなかった。
- **なぜゲートしないか**: プロセスが落ちる。
- **検証**: stress 下 `regex_value_as_match_arg` が緑。
  **E024 (本統合)**: ストレス全件でも同じ 1 件が緑。
- **upstream**: **S8** の (D) のうち**ランタイム側**。
  **codegen 側の同型 (`sp_obj_inspect_sw`) は未修正**である
  (root を足すと `object_default_display` の SIGABRT が SIGSEGV に**変わる** =
  そのテストにはもう 1 つ別のバグがある)。**ここは意図的に閉じていない。**

---

### 回帰テスト (`test/` への追加 2 本)

台帳の他のセクションと違い、バグ修正は**スイートに恒久的な番人を残す**。
どちらも **`SPINEL_GC_STRESS` 無し = 既定閾値で修正前に落ちる** ことを条件に選んだ
(スイートにストレスレーンが無いので、ストレス専用の再現体は番人にならない)。

| テスト | 守る修正 | 修正前 (既定閾値) | 修正後 | 実行時間 |
|---|---|---|---|---|
| `test/hash_merge_chain_rooted.rb` | **B2** | `true true true true` (**FAIL**、5/5 決定的) | `false false false false` **PASS** | 0.03 s |
| `test/ctor_yield_inline_self_rooted.rb` | **B1** | **SIGSEGV (rc=139)、10/10 決定的** | `false` **PASS** | 0.005 s |

- `hash_merge_chain_rooted.rb` は E020 の成果物をそのまま採用した
  (`results/E020/proposed_test_hash_merge_chain_rooted.rb`)。
  ループ + 位相をずらす `pad = Array.new(i % 11)` で窓を掃く形。
- `ctor_yield_inline_self_rooted.rb` は **E024 で新規に作った**。
  E019 の最小再現 (3 行、`Set` を使うもの) は**ストレス専用**で既定閾値では落ちないため
  番人にならない — `sp_Set` がプール化クラスで recycle がペイロードを温存するからである
  (E019 §5)。そこで E019 自身の結論「**`Set` 固有ではない。
  `def initialize; …; yield self; end` 形のユーザークラスは全部候補**」を使い、
  **プール化されていないユーザークラス**で、かつ `initialize` の本体が
  既定閾値 (256 KiB) を自力で跨ぐだけ割り当てる形にした。
  結果は**プールに庇われないぶん E019 の再現体より強い** (誤答ではなく SIGSEGV)。
  反復数 20/40/60/100 × `n` の増分 0/3/7/13 の **16 通りすべてで修正前 SIGSEGV** =
  位相依存のナイフエッジではない (E019 の `Set` 版を既定閾値で落とす試みは
  `M=4000` でだけ落ち `M=6000/8000` では落ちないナイフエッジだった。
  **その版は採用していない。**)。
- **E019 の元の 3 行再現体 (`results/E019/minimal_repro.rb`) は `test/` に入れていない。**
  ストレス専用でスイートに該当レーンが無く、入れても常に緑になるだけで番人にならない
  (E008 の `test/bigint_operand_rooted.rb` が抱えた問題と同じ)。
  記録としては本台帳の B1 と E019 の Edoc に残る。

### 検証 (統合、E024)

作業ツリー: `vendor/spinel` worktree `fix/gc-roots-batch1` @ `1aee3d55` + 6 パッチ。
Apple M1 Max / macOS 26.6.1 / **系統 cc (Apple clang) のみ、LLVM プラグイン無し、
ThinLTO 無し**、`make -j4`。

| ゲート | 結果 |
|---|---|
| **パッチ適用** | 6 本すべて `git apply` **クリーン** (競合 0。`fix_brat_to_s` が同一ファイル内の `fix_box_brat` の +10 行ぶん offset するのみ) |
| **plain 全件 `make -j4 test`** | **2889 pass / 0 fail / 0 error** (2887 + 新設の回帰テスト 2 本) |
| **回帰テストの噛み合い (bite-proof)** | 2 本とも**既定閾値で**修正前に落ち、修正後に通る。しかも **1 本 = 1 修正**に噛む (E019 だけ外す / E020 だけ外す の 2×2 が対角に割れる) |
| **stress 全件 A/B** | **赤 46 → 31 (−15)、新しい赤 0 本**。消えたのは A1 **8** (E021 の `xcheck_e019_patch.tsv` の PASS 8 件と完全一致) / E **1** / C1 **2** / C2 **1** / D1 **1** / **S3 1 (予告外)** + 新設テスト 1。残った 31 件はすべて E021 が既に「直らない」と分類済みのもの (S9 11 / A2 3 / B 2 / C1 残り 2 / D2 1 / 残渣 12) |
| **挙動不変 (凍結ベンチ 10 本 + `bm_unwind`/`bm_dispatch`)** | 凍結ベンチ 10 本 + `bm_unwind`/`bm_dispatch` の stdout が **10/10 バイト一致**。しかも `benchmark/` 由来の 8 本は **E001 の凍結 sha256 とも一致** (Phase 0 から 1 バイトも動いていない)。バイナリは 10 本とも変わる (`lib/spinel_rt.h` が全生成 TU に入るヘッダなので当然) |
| **ゲートの byte-identity 保存** | flag OFF で `.o` バイト一致が **3 本とも成立**。`SP_STRLEN_DEDUP` は台帳 Patch 2 の凍結値 **`ff85590f…` そのもの**。`SP_INT64` は LP64 ホストでは flag ON でも `.o` 一致 = **構成上の no-op** (効くのは wasm32) だが、前処理済みトークン列に **215 行の差**が出るのでゲートは生きている |

詳細は [E024](../docs/experiments/E024_vendor_fixes_integration.md) と
`docs/experiments/results/E024/`。

### 未修正で残しているもの (honest boundary)

本セクションは「同族のバグを全部直した」とは主張しない。
**E024 のストレス全件 A/B で、適用後もなお 31 件が赤である** (適用前は 45 件 + 新設テスト 1)。
内訳はすべて既知:

| クラスタ | 件数 | 状態 |
|---|---:|---|
| **S9** (入れ子配列の行が潰れる族) | **11** | 未根因特定 |
| **A2** (蓄積配列 / yield しないコンストラクタ) | **3** | 根因証明済み・修正なし → S8 |
| **B** (capture セルの scan が `NULL`) | **2** | 根因証明済み・修正なし → S8 |
| **C1 の残り** | **2** | E008 §2.1 の `gcd`/`lcm` 中間値が未着手 |
| **D2** (`sp_obj_inspect_sw`) | **1** | root を足すと SIGABRT → SIGSEGV に変わる別バグ同居 |
| 未分類の残渣 | **12** | E021 §4 の残渣 |

**新しい赤は 1 本も増えていないが、ストレス下のスイートは緑にならない。**
本セクションは「赤を単調に減らした」以上のことは主張しない。

**性能は 1 度も測っていない。** 6 本すべてがシャドウスタックの push/pop を増やす方向で、
B1 は生成 C に 247 行の `SP_GC_ROOT` を足す。**出荷判定にベンチは含まれていない**
(E021 §5.5 が挙げた宿題は E024 でも未消化)。

**プラグイン (`SpinelGCRoots`) / ThinLTO は 1 度も通していない。**
検証はすべて系統 cc (Apple clang) である。
