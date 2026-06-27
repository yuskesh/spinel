# Regression: mapping an untyped block result into a poly array must not emit
# sp_box_int(sp_box_nil()) (an mrb_int slot fed an already-boxed value).
# `rows` is RBS-typed Array[Hash[String, untyped]]; `@model` is an unassigned
# (untyped) ivar, so `@model.build(row)` is untyped. The map result is a poly
# array; collecting the untyped value must box it correctly (yield nil), not
# wrap it in sp_box_int. The RBS-only adapter stubs to empty rows, so the
# observable result is the length (0); before the fix this failed to compile.
module DB
  class << self
    def all
      rows = adapter.select_rows("select 1")
      rows.map { |row| @model.build(row) }
    end
  end
end

puts DB.all.length
