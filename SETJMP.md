# setjmp 維持方針の改善計画

`setjmp/longjmp` を維持する前提で、hot path の性能低下を最小化するための改善案を整理する。

## 前提

- Ruby の例外モデルは維持する
- `setjmp` を全面廃止しない
- フラグ方式で hot path に毎回チェックを入れる設計は避ける
- 改善対象は `spinel_codegen.rb` の生成形状と、必要最小限の runtime 側の分岐

## 優先順位

### 1. `volatile` の範囲を絞る

現在は `body_uses_setjmp?` が真になると、その関数内の非ポインタ局所変数を広く `volatile` にしている。

これは安全だが保守的すぎる。`volatile` が必要なのは、`setjmp` 以後に更新され、かつ `longjmp` 後も値を参照する変数だけである。

改善方針:

- `begin/rescue/retry/ensure` の境界をまたぐ値だけを `volatile` にする
- 単純な一時変数や、そのスコープ内で完結する値は通常変数のままにする
- 可能なら AST か簡易データフローで live range を絞る

期待効果:

- レジスタ退避の減少
- 不要な load/store の削減
- `setjmp` を使う関数のコード品質改善

### 2. `setjmp` を cold wrapper に寄せる

`begin/rescue/ensure` が直接 hot path に混ざると、成功経路の形が崩れやすい。

改善方針:

- hot な本体処理と、例外を受ける wrapper を分ける
- wrapper 側にだけ `setjmp` を置く
- 本体関数は通常のコードとして生成する

期待効果:

- hot path から `setjmp` 由来の影響を追い出せる
- 例外境界を局所化できる
- 最適化が効きやすくなる

### 3. `body_uses_setjmp?` の判定を精密化する

今の判定は安全側だが粗い。

改善方針:

- 本当に `setjmp` 境界をまたぐ値があるかを、もう少し細かく判定する
- `BeginNode` や `RetryNode` を見つけたら即座に広く volatile 化するのではなく、必要な局所に限定する
- 例外を投げるだけで値を保持しない経路は、できるだけ `volatile` 対象から外す

期待効果:

- `setjmp` の影響を受ける関数数を実質的に減らせる
- 無関係なローカルの最適化を壊しにくい

### 4. `SP_UNLIKELY` は補助として使う

`sp_exc_top > 0` などの分岐に `SP_UNLIKELY` を付けるのは有効だが、副次的効果に留まる。

改善方針:

- まず構造を直す
- その上で冷たい分岐にだけ branch hint を付ける

期待効果:

- 予測ミスの罰は少し減る
- ただし命令数そのものは減らない

## 実装順

1. `volatile` の適用範囲を見直す
2. `begin/rescue/ensure` の生成を cold wrapper 化できるか検討する
3. `body_uses_setjmp?` の判定を精密化する
4. 必要な箇所だけに `SP_UNLIKELY` を添える

## 判断基準

以下が満たせれば改善成功とみなす。

- `optcarrot` などの hot benchmark で FPS が回復する
- `make test` の結果が維持される
- `setjmp` の使用箇所は維持したまま、影響範囲だけが縮む
- 生成 C の形が読みやすく、将来の保守が破綻しない

