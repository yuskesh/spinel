# Issue #646 修正計画

## 現象

```ruby
class App
  def initialize
    APP.something    # APP is being initialized RIGHT NOW
  end
end
APP = App.new
```

MRI: `NameError: uninitialized constant App::APP` を init 中に raise。
spinel: `cst_APP` は `NULL`。read は NULL を返し、`.something` で segfault。

reporter の実例 (tep#13) は transitive ケース:
```
APP = App.new
  → App#initialize
    → PG::Connection.new(...)
      → ... → Tep::Scheduler.scheduled_context?
        → Tep::APP.sched_current   # APP is still NULL
```

dispatch が compile-time に static resolve した結果、`cst_Tep_APP->iv_sched_current` が直接 emit され、NULL deref で SEGV。

## 現在の対応 (765b984)

直接 shape (`Class#initialize` が `CONST` を直接 read、ただし `CONST = Class.new` 形式) を compile-time に detect して clearer warning を emit。Runtime crash の plug は無し。

## ゴール

issue の expected behaviour:
1. **Best**: MRI 互換の `NameError` raise (option 1)
2. **Acceptable**: `nil` を返して method dispatch で NoMethodError (option 2)
3. **現状 / Worst**: silent partial-init pointer / SEGV (option 3)

option 1 を目指す。

## 修正案: per-const in-progress flag + read-side guard

### Phase 1: 直接 shape (`<C>` init reads `<X>` where `<X> = <C>.new`)

#### Analyzer 側 (spinel_analyze.rb)

新しい table `@const_init_class`:
- 要素: `@const_expr_ids[i]` が `<Class>.new(...)` shape なら `<Class>` の名前。そうでなければ `""`。
- IR export: `SA @const_init_class N value;value;...`

```ruby
def detect_const_init_classes
  @const_init_class = "".split(",")
  i = 0
  while i < @const_expr_ids.length
    init_cls = ""
    eid = @const_expr_ids[i]
    if eid >= 0 && @nd_type[eid] == "CallNode" && @nd_name[eid] == "new"
      recv = @nd_receiver[eid]
      if recv >= 0 && @nd_type[recv] == "ConstantReadNode"
        init_cls = @nd_name[recv]
      end
    end
    @const_init_class.push(init_cls)
    i = i + 1
  end
end
```

`generate_code` 内の早めのタイミング (precompute_all_scope_decls 後) で 1 回呼び出し。

#### Runtime 側 (lib/sp_runtime.h)

`sp_raise_cls("NameError", msg)` は既存。再利用する。新しいヘルパは不要。

#### Codegen 側 (spinel_codegen.rb)

1. **Per-const in-progress flag emit** (`emit_const_init` 周辺):
   - `@const_init_class[i]` が空でない const に対し、`static int sp_init_in_progress_<X> = 0;` を const 宣言と一緒に emit。

2. **Const init assignment を guard で包む** (`emit_const_init` 内):
   ```c
   sp_init_in_progress_X = 1;
   cst_X = sp_C_new();
   sp_init_in_progress_X = 0;
   ```

3. **Const read site で in-progress check** (`compile_expr` ConstantReadNode arm):
   - resolve した const X の `@const_init_class[ci]` が空でない場合のみ、wrap で emit:
   ```c
   ((sp_init_in_progress_X) ? (sp_raise_cls("NameError", "uninitialized constant X"), (sp_C *)NULL) : cst_X)
   ```
   - sp_raise_cls 後の cast は dead code だが C compile を通すために必要。

### Phase 2 (任意 - 最適化)

- 完全に static に "init 経路外" と判明する read site (e.g., main の APP = App.new の後 emit される statements) では in-progress check を skip。
- これにより hot path の overhead をゼロにできる (call graph 解析が必要)。
- Phase 1 だけでも MRI 互換 + 性能影響は branch 1 つ per const read のみ。許容範囲。

## 影響範囲

| ファイル | 変更内容 | 行数概算 |
|---|---|---|
| spinel_analyze.rb | `detect_const_init_classes` 追加 + `generate_code` 呼び出し + IR export | ~30 |
| spinel_codegen.rb | `@const_init_class` ロード + const decl で flag emit + assign を guard + read 側 wrap | ~50 |
| lib/sp_runtime.h | 変更不要 (sp_raise_cls 再利用) | 0 |
| test/ | 新規 regression test (NameError raise + rescue) | ~20 |

## 検証

```ruby
# test/const_self_ref_raises_name_error.rb
class App
  def initialize
    APP                    # ← raises NameError
  rescue NameError => e
    puts "caught: " + e.message
  end
end
APP = App.new
puts "after"
```

Expected:
```
caught: uninitialized constant APP
after
```

## エッジケース

1. **Transitive (`APP = build_app()` where build_app does `App.new`)**: Phase 1 では Catch しない。`@const_init_class[APP]` は `""` (build_app は CallNode `new` でも recv が ConstantReadNode でもない)。Phase 2 でも難しい (call graph + dispatch resolve が必要)。Reporter の workaround で対応可能。

2. **Class hierarchy: `APP = SubApp.new` で SubApp < App**: `@const_init_class[APP] = "SubApp"`。SubApp#initialize 内 + 継承された App#initialize 内の APP read は guard 対象。inheritance walk を `@const_init_class` のチェックに加える必要あり。Phase 1 では SubApp の直接 init のみ catch、parent init は miss する可能性 — phase 1.5 で追加するか。

3. **Module const (`APP = App.new` inside `module Tep`)**: const 名は `Tep_APP`。`@const_init_class` は qualified 名で record。標準パスで動く。

4. **`||=` re-init**: `APP ||= App.new` (rare)。`@nd_type[eid]` が ConstantOrWriteNode と違う。最初の `=` パスでのみ guard、`||=` は skip。妥当。

5. **Setter on partially-init recv (`APP.foo = bar`)**: const recv が NULL の attr_writer dispatch。setter は usually `cst_APP->iv_foo = ...` を emit → NULL deref で SEGV。Guard の wrap がここでも必要。compile_call_expr の `[]=` / attr_writer setter dispatch でも cst_X recv detect が要る。

## 段階的な実装

Phase 1 で issue #646 のメインケース (直接 shape) を MRI 互換に raise する。
Phase 1.5 で inheritance walk と setter path を追加。
Phase 2 (任意) で call graph による guard 削減。
Transitive case (build_app 経由) は inherently 難しく、document する方が pragmatic。

## 実装順序

1. `detect_const_init_classes` 追加、IR export 動作確認
2. Codegen で `@const_init_class` をロード、in-progress flag を decl
3. Const assignment を guard で包む emit
4. Const read site の wrap
5. Regression test 追加
6. setter path 追加 (Phase 1.5)
7. inheritance walk (Phase 1.5)
8. push + close #646
