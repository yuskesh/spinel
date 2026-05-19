# Shared helpers used by both spinel_analyze.rb and
# spinel_codegen.rb. Extracted to avoid byte-for-byte
# duplication between the two compiler passes. Both passes
# already share node-table accessors (@nd_type / @nd_name /
# @nd_arguments / etc.) and class-table accessors
# (cls_find_method / cls_method_return / etc.) by virtue of
# both defining a `class Compiler`; the methods here only
# depend on that shared surface.
#
# To add a helper here it must:
# - depend only on instance vars / methods that exist on
#   BOTH the analyze-side and codegen-side Compiler
# - not perform pass-specific side effects (emit, push narrow
#   stack only via methods both sides define, etc.)
# - have identical semantics in both passes (drift between
#   the two would re-introduce the original bug)
class Compiler

 # ---- Nil-guard narrow helpers (#550) ----

 # `<lv>.nil?` predicate. Returns the LV name; otherwise "".
  def parse_nil_predicate(pred_id)
    if pred_id < 0
      return ""
    end
    if @nd_type[pred_id] != "CallNode"
      return ""
    end
    if @nd_name[pred_id] != "nil?"
      return ""
    end
    recv = @nd_receiver[pred_id]
    if recv < 0 || @nd_type[recv] != "LocalVariableReadNode"
      return ""
    end
    @nd_name[recv]
  end

 # Body ends with a definite scope exit. Used by the nil-guard
 # narrow (issue #550) to identify guards whose continuation
 # only fires when the predicate held. Recognizes:
 # - `return X` (ReturnNode)
 # - `raise ...` / `throw ...` (CallNode)
 # - `break` / `next` (BreakNode / NextNode) -- both unwind
 #   the iteration / loop, so the narrow applies to the rest
 #   of the enclosing block.
  def body_definitely_exits?(body_id)
    if body_id < 0
      return 0
    end
    stmts_r = get_stmts(body_id)
    if stmts_r.length == 0
      return 0
    end
    last = stmts_r[stmts_r.length - 1]
    if @nd_type[last] == "ReturnNode"
      return 1
    end
    if @nd_type[last] == "BreakNode" || @nd_type[last] == "NextNode"
      return 1
    end
    if @nd_type[last] == "CallNode" && (@nd_name[last] == "raise" || @nd_name[last] == "throw")
      return 1
    end
    0
  end

 # Given the rhs of the most recent write to a local variable
 # whose nil? was just checked, return the type the variable
 # narrows to after the nil-exit. Currently recognizes
 # `<string>.index(needle)` / rindex / find_index returning
 # int-or-nil; the non-nil arm is mrb_int. Returns "" when the
 # writer's shape isn't a known int-or-nil source so the caller
 # leaves the type alone. Issue #550.
  def infer_nil_guard_narrow_type(expr_id)
    if expr_id < 0
      return ""
    end
    if @nd_type[expr_id] != "CallNode"
      return ""
    end
    mname_eg = @nd_name[expr_id]
    if mname_eg != "index" && mname_eg != "rindex" && mname_eg != "find_index"
      return ""
    end
    recv_eg = @nd_receiver[expr_id]
    if recv_eg < 0
      return ""
    end
    rt_eg = infer_type(recv_eg)
    if rt_eg == "string" || rt_eg == "mutable_str"
      return "int"
    end
 # Array#index family on int_array now returns int? (sentinel-
 # encoded). After `return if h.nil?` the live arm sees the value
 # as a plain int, same as the String#index narrow above.
    if rt_eg == "int_array"
      return "int"
    end
    ""
  end

 # Recognize `return X if h.nil?` shape; return the LV name or
 # "". Caller threads the stmt list separately into
 # scan_back_writer_narrow_for to derive the narrow type. (Two
 # separate calls instead of one [var, type] return because
 # spinel-self's inference widens an array-return into poly,
 # cascading into push_type_narrow's param signature.)
 # Issue #550.
  def parse_nil_guard_var(nid)
    if nid < 0
      return ""
    end
    if @nd_type[nid] != "IfNode"
      return ""
    end
    body_i = @nd_body[nid]
    if body_definitely_exits?(body_i) == 0
      return ""
    end
    sub_i = @nd_subsequent[nid]
    else_i = @nd_else_clause[nid]
    if sub_i >= 0 || else_i >= 0
      return ""
    end
    parse_nil_predicate(@nd_predicate[nid])
  end

  def scan_back_writer_narrow_for(stmts_list, before_idx, varname)
    j = before_idx - 1
    while j >= 0
      stmt = stmts_list[j]
      if @nd_type[stmt] == "LocalVariableWriteNode" && @nd_name[stmt] == varname
        return infer_nil_guard_narrow_type(@nd_expression[stmt])
      end
      j = j - 1
    end
    ""
  end

 # ---- Poly-recv dispatch helpers (#549) ----

 # For a `<poly>.<mname>(args)` site, return the static C type
 # the dispatch result temp should have. Returns "poly" when
 # the surviving cls_id arms can't agree on a single scalar.
 # nid + arg_types let the helper apply the same arm-suppression
 # logic the emit loop uses (param-incompat + observed-class
 # narrow), so unreachable arms don't widen the result.
  def poly_dispatch_return_type(mname, nid = -1, arg_types = nil)
    if arg_types == nil
      arg_types = "".split(",")
    end
    if mname == "[]"
      @needs_rb_value = 1
      return "poly"
    end
 # Hash-shape preserving methods on a poly recv carrying hash
 # storage. The result temp must be sp_RbVal so the per-cls_id
 # arm's `tmp = sp_box_obj(...)` lands. Without this, the temp
 # is mrb_int and the assignment silently no-ops, leaving every
 # downstream consumer reading the int 0 default.
    if mname == "dup" || mname == "each" || mname == "to_h" || mname == "merge"
      @needs_rb_value = 1
      return "poly"
    end
 # `fetch` on a poly recv: runtime cls_id picks between user-class
 # `fetch` arms and built-in Hash-variant arms (StrIntHash /
 # StrStrHash / StrPolyHash). Each arm's value type differs (int /
 # string / poly) so the result temp must be sp_RbVal -- without
 # widening, the temp's static C type comes from the first user
 # class's `fetch` return and other arms' rhs fails to compile
 # against it. Sibling to `[]` widening above; same rationale.
    if mname == "fetch"
      @needs_rb_value = 1
      return "poly"
    end
 # Source-level narrow set from ivar observations: when the
 # receiver reads from an ivar whose observed type set is all
 # obj_X (no poly / no primitives), the dispatch can only ever
 # land on those classes. Combined with the param-incompat
 # check below, this prunes arms whose return type would
 # otherwise widen the dispatch result to sp_RbVal even though
 # they can never fire at runtime. Issues #549, #531.
    narrow_set = poly_dispatch_narrow_class_set(nid)
 # Built-in string methods that compile_poly_method_call also
 # lowers via a SP_TAG_STR arm -- their result temp needs to be
 # at least string-typed so the per-tag dispatch assignment
 # doesn't try to store const char * into a mrb_int slot. If a
 # user class also defines the method with a different return
 # type, the per-class loop below escalates to "poly".
    if mname == "gsub" || mname == "sub"
      ci_s = 0
      diverges = 0
      while ci_s < @cls_names.length
        if narrow_set != "" && poly_dispatch_class_in_set(narrow_set, ci_s) == 0
          ci_s = ci_s + 1
        else
          if cls_find_method(ci_s, mname) >= 0 && poly_dispatch_arm_param_compat(ci_s, mname, arg_types) == 1
            urt = cls_method_return(ci_s, mname)
            if urt != "" && urt != "string"
              diverges = 1
              ci_s = @cls_names.length
            end
          end
          ci_s = ci_s + 1
        end
      end
      if diverges == 1
        @needs_rb_value = 1
        return "poly"
      end
      return "string"
    end
 # Setters: mname ends with "=" and at least one class has an
 # attr_writer for the bare name. Return type is the ivar type
 # (Ruby returns the rhs from `x = v`); without this, the result
 # tmp's C type defaults to `mrb_int` and `tmp = rhs` mismatches
 # for non-int slots.
    setter_bname = ""
    if mname.length > 1 && mname[mname.length - 1] == "="
      setter_bname = mname[0, mname.length - 1]
    end
    common = ""
    ci = 0
    while ci < @cls_names.length
      if narrow_set != "" && poly_dispatch_class_in_set(narrow_set, ci) == 0
        ci = ci + 1
      else
        rt = ""
        if cls_find_method(ci, mname) >= 0
 # Skip arms whose param types can't accept the dispatch
 # site's arg types -- mirrors the arm_incompat check in
 # the emit loop so the result-type union doesn't see
 # return types from arms the runtime can't reach.
          if poly_dispatch_arm_param_compat(ci, mname, arg_types) == 0
            rt = ""
          else
            rt = cls_method_return(ci, mname)
          end
        elsif cls_has_attr_reader(ci, mname) == 1
 # An attr_reader returns the ivar type. .
          rt = cls_ivar_type(ci, "@" + mname)
        elsif setter_bname != "" && cls_has_attr_writer(ci, setter_bname) == 1
 # An attr_writer setter returns the ivar's type.
          rt = cls_ivar_type(ci, "@" + setter_bname)
        end
        if rt != ""
          if common == ""
            common = rt
          elsif common != rt
            return "poly"
          end
        end
        ci = ci + 1
      end
    end
    common == "" ? "int" : common
  end

 # Returns a comma-separated list of class indices the dispatch
 # site can possibly land on, derived from the receiver ivar's
 # observed type set. Returns "" when no narrowing is safe
 # (non-ivar receiver, partial observation, or any non-obj
 # observation like "poly" / "int" / "string"). Issue #549.
  def poly_dispatch_narrow_class_set(nid)
    if nid < 0
      return ""
    end
    if @current_class_idx < 0
      return ""
    end
    recv_id = @nd_receiver[nid]
    if recv_id < 0
      return ""
    end
    if @nd_type[recv_id] != "InstanceVariableReadNode"
      return ""
    end
    iname = @nd_name[recv_id]
    obs = cls_ivar_observed_types_for(@current_class_idx, iname)
    if obs == ""
      return ""
    end
    out = ""
    parts = obs.split(",")
    k = 0
    while k < parts.length
      t = parts[k]
      if t == ""
 # blank slot -- uninformative, skip
      elsif is_obj_type(t) == 1
        cname = t[4, t.length - 4]
        cidx = find_class_idx(cname)
        if cidx >= 0
          s_idx = cidx.to_s
          if out == ""
            out = s_idx
          else
 # dedup
            already = 0
            seen = out.split(",")
            sk = 0
            while sk < seen.length
              if seen[sk] == s_idx
                already = 1
              end
              sk = sk + 1
            end
            if already == 0
              out = out + "," + s_idx
            end
          end
        end
      else
 # Any non-obj observation ("poly", "int", "string", etc.)
 # means the ivar can hold values whose dispatch is broader
 # than the obj-class set we can enumerate. Bail out so we
 # don't unsoundly prune.
        return ""
      end
      k = k + 1
    end
    out
  end

  def poly_dispatch_class_in_set(set, ci)
    if set == ""
      return 1
    end
    target = ci.to_s
    parts = set.split(",")
    k = 0
    while k < parts.length
      if parts[k] == target
        return 1
      end
      k = k + 1
    end
    0
  end

 # Mirrors the arm_incompat check inside compile_poly_method_call's
 # emit loop. Returns 1 when the arm's param types are compatible
 # with the dispatch site's arg types, 0 when at least one slot
 # has a base-type mismatch outside the int/symbol/bool family.
  def poly_dispatch_arm_param_compat(ci, mname, arg_types)
    midx = cls_find_method_direct(ci, mname)
    owner_idx = ci
    if midx < 0
      owner_name = find_method_owner(ci, mname)
      if owner_name != ""
        owner_idx = find_class_idx(owner_name)
        if owner_idx >= 0
          midx = cls_find_method_direct(owner_idx, mname)
        end
      end
    end
    if midx < 0
      return 1
    end
    arm_ptypes = cls_meth_ptypes_get(owner_idx, midx)
    pk = 0
    while pk < arm_ptypes.length && pk < arg_types.length
      at_b = base_type(arg_types[pk])
      pt_b = base_type(arm_ptypes[pk])
      if at_b != "" && pt_b != "" && at_b != "poly" && pt_b != "poly" && at_b != pt_b
        if (at_b == "int" && pt_b == "symbol") || (at_b == "symbol" && pt_b == "int") ||
           (at_b == "int" && pt_b == "bool")   || (at_b == "bool"   && pt_b == "int")
 # compatible
        else
          return 0
        end
      end
      pk = pk + 1
    end
    1
  end

end
