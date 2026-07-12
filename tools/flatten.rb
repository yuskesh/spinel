# spinel-flatten: inline a require_relative graph into one self-contained file,
# so spinel-reduce and bug reports operate on a single input. Resolves the same
# require_relative paths spinel does; cycle-safe; records each region's origin.
#
# Usage: spinel-flatten [-o OUT] app.rb

require_relative "tool_common"

# Extract the path from a `require_relative "x"` / `require_relative 'x'` line,
# or "" if the line is not a require_relative.
def require_target(line)
  s = line.strip
  return "" if !s.start_with?("require_relative")
  q = "\""
  i = s.index(q)
  if !i
    q = "'"
    i = s.index(q)
  end
  return "" if !i
  rest = s[i + 1, s.length - i - 1]
  j = rest.index(q)
  return "" if !j
  rest[0, j]
end

# Collapse "." and ".." segments so the same file always resolves to the same
# string. Without this, a file reached via two non-canonical paths (e.g.
# `a/../b.rb` and `b.rb`) compares unequal in `seen` and is inlined twice; with
# a require cycle that compounds into runaway re-inlining (the path keeps
# growing) and a flattened file many times larger than the program.
def canonicalize(path)
  # An absolute path's leading "/" splits off as an empty segment; remember it
  # and put it back, or an absolute entry resolves to a relative path that
  # File.exist? rejects. "/.." stays the root, so ".." never stacks when abs.
  abs = path.start_with?("/")
  stack = []
  path.split("/").each { |p|
    if p == "" || p == "."
      next
    elsif p == ".."
      if stack.length > 0 && stack[stack.length - 1] != ".."
        stack.pop
      elsif !abs
        stack.push(p)
      end
    else
      stack.push(p)
    end
  }
  abs ? "/" + stack.join("/") : stack.join("/")
end

# Resolve a require_relative target against the requiring file's directory,
# adding ".rb" when absent.
def resolve(from_file, target)
  base = dir_name(from_file) + "/" + target
  base = base + ".rb" if !base.end_with?(".rb")
  canonicalize(base)
end

def inline(real, out, seen)
  seen.each { |p| return if p == real }
  seen.push(real)
  if !File.exist?(real)
    out.push("# spinel-flatten: missing file: " + real)
    return
  end
  out.push("# >>> " + real)
  text = File.read(real)
  text.split("\n").each { |line|
    t = require_target(line)
    if t.length > 0
      inline(resolve(real, t), out, seen)
    else
      out.push(line)
    end
  }
  out.push("# <<< " + real)
end

def main
  src = ""
  outfile = ""
  i = 0
  while i < ARGV.length
    a = ARGV[i]
    if a == "-o"
      i = i + 1
      outfile = ARGV[i] if i < ARGV.length
    elsif a == "-h" || a == "--help"
      puts "usage: spinel-flatten [-o OUT] app.rb"
      puts "  inline the require_relative graph of app.rb into one file"
      exit(0)
    else
      src = a
    end
    i = i + 1
  end
  die("usage: spinel-flatten [-o OUT] app.rb", 2) if src.length == 0
  die("spinel-flatten: no such file: " + src, 2) if !File.exist?(src)

  out = []
  seen = []
  inline(src, out, seen)
  text = out.join("\n") + "\n"
  if outfile.length > 0
    File.write(outfile, text)
    $stderr.puts "spinel-flatten: wrote " + outfile
  else
    print text
  end
  exit(0)
end

main
