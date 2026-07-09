# Array#insert / #delete_at on a poly array (heterogeneous elements): insert
# shifts boxed slots and boxes the inserted value; delete_at returns the
# removed element.
arr = [1, "two"]
arr.insert(1, :mid)
p arr.size
p arr[1]
removed = arr.delete_at(0)
p removed
p arr.size
arr.insert(-1, 9.5)
p arr[arr.size - 1]
# boxed (TY_POLY) index: a poly-array element read must unbox to mrb_int, not
# pass an sp_RbVal into the mrb_int slot.
idxs = [0, "x"]
arr.insert(idxs[0], :head)
p arr[0]
