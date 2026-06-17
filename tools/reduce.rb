# spinel-reduce: delta-debug (ddmin) a degrading program down to a minimal
# input that still reproduces a chosen failure. Re-runs spinel only.
#
# Usage: spinel-reduce [--oracle build|unsupported|unresolved]
#                      [--oracle-cmd 'CMD {}'] [-o OUT] app.rb
#
#   --oracle <leg>     built-in interestingness (default: build = spinel fails)
#   --oracle-cmd CMD   custom test: "{}" is replaced by the candidate file;
#                      the candidate is interesting when CMD exits 0
#
# Tip: flatten multi-file programs first (spinel-flatten) so reduce has one
# input to shrink.

require_relative "tool_common"

# Is the candidate (an array of source lines) still interesting?
def interesting(lines, tmp, sp, oracle, ocmd)
  File.write(tmp, lines.join("\n") + "\n")
  if ocmd.length > 0
    sh(ocmd.gsub("{}", tmp))
    return $sh_status == 0
  end
  if oracle == "build"
    sh(sp + " " + tmp + " -o " + tmp + ".bin")
    return $sh_status != 0
  end
  needle = "unsupported "
  needle = "warning: unresolved" if oracle == "unresolved"
  out = sh("SPINEL_WARN_UNRESOLVED=1 " + sp + " " + tmp + " -c -o " + tmp + ".c")
  out.include?(needle)
end

# Classic ddmin: shrink `lines` while `interesting` holds, increasing
# granularity when no single chunk-complement reproduces.
def ddmin(lines, tmp, sp, oracle, ocmd)
  n = 2
  while lines.length >= 2
    size = lines.length / n
    size = 1 if size < 1
    start = 0
    reduced = false
    while start < lines.length
      stop = start + size
      comp = []
      k = 0
      while k < lines.length
        comp.push(lines[k]) if k < start || k >= stop
        k = k + 1
      end
      if comp.length > 0 && comp.length < lines.length && interesting(comp, tmp, sp, oracle, ocmd)
        lines = comp
        n = n - 1
        n = 2 if n < 2
        reduced = true
        start = 0
      else
        start = start + size
      end
    end
    if !reduced
      break if n >= lines.length
      n = n * 2
      n = lines.length if n > lines.length
    end
  end
  lines
end

def main
  oracle = "build"
  ocmd = ""
  outfile = ""
  src = ""
  i = 0
  while i < ARGV.length
    a = ARGV[i]
    if a == "--oracle"
      i = i + 1
      oracle = ARGV[i] if i < ARGV.length
    elsif a == "--oracle-cmd"
      i = i + 1
      ocmd = ARGV[i] if i < ARGV.length
    elsif a == "-o"
      i = i + 1
      outfile = ARGV[i] if i < ARGV.length
    elsif a == "-h" || a == "--help"
      puts "usage: spinel-reduce [--oracle build|unsupported|unresolved] [--oracle-cmd 'CMD {}'] [-o OUT] app.rb"
      exit(0)
    else
      src = a
    end
    i = i + 1
  end
  die("usage: spinel-reduce [options] app.rb", 2) if src.length == 0
  die("spinel-reduce: no such file: " + src, 2) if !File.exist?(src)

  sp = find_spinel
  tmp = tmp_path("reduce", src, ".rb")
  lines = []
  File.read(src).split("\n").each { |l| lines.push(l) }

  if !interesting(lines, tmp, sp, oracle, ocmd)
    die("spinel-reduce: the input is not interesting under this oracle (nothing to reduce).", 1)
  end

  $stderr.puts "spinel-reduce: " + lines.length.to_s + " lines, oracle=" + (ocmd.length > 0 ? "cmd" : oracle)
  mini = ddmin(lines, tmp, sp, oracle, ocmd)
  text = mini.join("\n") + "\n"
  $stderr.puts "spinel-reduce: reduced to " + mini.length.to_s + " lines"

  if outfile.length > 0
    File.write(outfile, text)
    $stderr.puts "spinel-reduce: wrote " + outfile
  else
    print text
  end
  exit(0)
end

main
