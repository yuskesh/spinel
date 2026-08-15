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
