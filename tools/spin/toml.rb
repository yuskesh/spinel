# Minimal TOML reader for spin (M0). Covers the gem.toml subset: [tables],
# key = "string", key = { k = "v", ... } inline tables, and comments. This
# ships inside spin for bootstrap and graduates to the stdlib `toml` feature
# later (docs/spin.md §7). Storage is deliberately string->string only.

class TomlDoc
  def initialize
    @vals = { "" => "" }    # "table\x01key" => plain string value
    @inline = { "" => "" }  # "table\x01key\x01ik" => inline-table member
    @keys = { "" => "" }    # "table" => newline-joined key list
  end

  def self.parse(text)
    doc = TomlDoc.new
    doc.parse_text(text)
    doc
  end

  def parse_text(text)
    table = ""
    text.split("\n").each do |line0|
      line = strip_comment(line0).strip
      next if line == ""
      if line.start_with?("[") && line.end_with?("]")
        table = line[1..-2].strip
        next
      end
      eq = line.index("=")
      next if eq.nil?
      key = line[0..(eq - 1)].strip
      val = line[(eq + 1)..-1].strip
      remember(table, key, val)
    end
  end

  def strip_comment(line)
    out = ""
    in_str = false
    line.each_char do |ch|
      in_str = !in_str if ch == "\""
      break if ch == "#" && !in_str
      out += ch
    end
    out
  end

  def remember(table, key, val)
    have = @keys.key?(table) ? @keys[table] : ""
    @keys[table] = have == "" ? key : (have + "\n" + key)
    if val.start_with?("{") && val.end_with?("}")
      inner = val[1..-2]
      inner.split(",").each do |pair|
        peq = pair.index("=")
        next if peq.nil?
        ik = pair[0..(peq - 1)].strip
        iv = unquote(pair[(peq + 1)..-1].strip)
        @inline[table + "\x01" + key + "\x01" + ik] = iv
      end
      @vals[table + "\x01" + key] = ""
    else
      @vals[table + "\x01" + key] = unquote(val)
    end
  end

  def unquote(s)
    return s[1..-2] if s.length >= 2 && s.start_with?("\"") && s.end_with?("\"")
    s
  end

  # plain value, "" when absent (or when the value is an inline table)
  def get(table, key)
    k = table + "\x01" + key
    return "" unless @vals.key?(k)
    @vals[k]
  end

  # inline-table member, "" when absent
  def get_inline(table, key, ik)
    k = table + "\x01" + key + "\x01" + ik
    return "" unless @inline.key?(k)
    @inline[k]
  end

  def table_keys(table)
    return [] unless @keys.key?(table)
    joined = @keys[table]
    return [] if joined == ""
    joined.split("\n")
  end
end
