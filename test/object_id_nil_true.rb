# The immediate values nil, false and true have fixed, distinct object_ids.
p nil.object_id
p false.object_id
p true.object_id
p [nil.object_id, true.object_id]
p(nil.object_id == false.object_id)
p(false.object_id == true.object_id)
p(nil.object_id == true.object_id)
