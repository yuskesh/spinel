# Bundled tests:
#   - kw_nil_default_string_call (show -> t_kw_show)
#   - optional_string_param_method (parse -> t_osp_parse)
#   - poly_box_int_array (kind_of -> t_pbia_kind_of)
#   - poly_or_returns_value (pick / pick_int -> t_porv_*)
#   - each_with_object_seed_shadow (f -> t_ewoss_f)
#   - fetch_nil_default_int_lv (f -> t_fndi_f)
#   - no_keywords_param (add -> t_nkp_add)
#   - sym_case (classify -> t_sc_classify)
#
# Each inner top-level def is renamed with a per-test prefix to
# avoid cross-test collisions when the bundle is compiled.

# === kw_nil_default_string_call ===
def t_kw_show(x: nil)
  puts x
end

def t_kw_nil_default_string_call
  t_kw_show(x: "hi")
  t_kw_show(x: nil)
end
t_kw_nil_default_string_call

# === optional_string_param_method ===
def t_osp_parse(source, file_path = nil)
  if file_path
    puts file_path.length
  else
    puts source.length
  end
end

def t_optional_string_param_method
  t_osp_parse("abc", "name.rb")
  t_osp_parse("abc")
  t_osp_parse("longer", "another_name.rb")
end
t_optional_string_param_method

# === poly_box_int_array ===
def t_pbia_kind_of(a)
  a.nil? ? "nil" : "something"
end

def t_poly_box_int_array
  puts t_pbia_kind_of(Object.new)
  puts t_pbia_kind_of([1, 2, 3])
end
t_poly_box_int_array

# === poly_or_returns_value ===
def t_porv_pick(a, b)
  a || b
end

def t_porv_pick_int(a, b)
  a || b
end

def t_poly_or_returns_value
  puts t_porv_pick(nil, "fallback")
  puts t_porv_pick("actual", "fallback")
  puts t_porv_pick_int(0, 5)
  puts(nil || "direct")
end
t_poly_or_returns_value

# === each_with_object_seed_shadow ===
def t_ewoss_f
  obj = 42
  out = [1, 2, 3].each_with_object([]) { |x, obj| obj.push(x * 2) }
  puts out[0]   # 2
  puts out[1]   # 4
  puts out[2]   # 6
  puts obj      # 42
end
t_ewoss_f

# === fetch_nil_default_int_lv ===
def t_fndi_f(opts = {})
  v = opts.fetch(:k, nil)
  v.nil? ? "missing" : "found"
end

def t_fetch_nil_default_int_lv
  puts t_fndi_f
end
t_fetch_nil_default_int_lv

# === no_keywords_param ===
def t_nkp_add(a, b, **nil)
  a + b
end

def t_no_keywords_param
  puts t_nkp_add(1, 2)
  puts t_nkp_add(10, 20)
end
t_no_keywords_param

# === sym_case ===
def t_sc_classify(sym)
  case sym
  when :red then "warm"
  when :orange then "warm"
  when :blue then "cool"
  when :green then "cool"
  else "unknown"
  end
end

def t_sym_case
  puts t_sc_classify(:red)
  puts t_sc_classify(:orange)
  puts t_sc_classify(:blue)
  puts t_sc_classify(:purple)
end
t_sym_case
