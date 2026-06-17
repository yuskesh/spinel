# spinel-doctor: one health report for a spinel program. Each leg is
# independent; the behavior leg is skipped cleanly when CRuby is absent.
#
# Usage: spinel-doctor [--only a,b] [--skip a,b] [--behavior] [--quiet] app.rb
#
# Legs:
#   build       compile to a binary; report compile / codegen / C-build failure
#   unsupported codegen gaps that degrade to a stub (stderr "unsupported ...")
#   unresolved  calls that silently degrade to nil/0 (SPINEL_WARN_UNRESOLVED)
#   requires    non-relative `require`s spinel treats as native / no-op
#   behavior    (opt) compiled output vs CRuby; needs `ruby` on PATH

require_relative "tool_common"

# Does this leg run, given --only / --skip?
def leg_on(name, only, skip)
  return false if skip.include?(name)
  return true if only.length == 0
  only.include?(name)
end

# Print a leg's header and its finding lines. Returns the finding count.
def report(name, sev, lines, quiet)
  if lines.length == 0
    puts "  [ok]   " + name if !quiet
    return 0
  end
  mark = "[warn]"
  mark = "[ERR] " if sev == "error"
  mark = "[info]" if sev == "info"
  puts mark + " " + name + " (" + lines.length.to_s + ")"
  lines.each { |l| puts "         " + l }
  lines.length
end

# Keep only the interesting diagnostic lines from a compile run.
def grep_kind(out, needle)
  hits = []
  out.split("\n").each { |l|
    hits.push(l.strip) if l.include?(needle)
  }
  hits
end

def main
  only = []
  skip = []
  want_behavior = false
  quiet = false
  src = ""
  i = 0
  while i < ARGV.length
    a = ARGV[i]
    if a == "--only"
      i = i + 1
      only = ARGV[i].split(",") if i < ARGV.length
    elsif a == "--skip"
      i = i + 1
      skip = ARGV[i].split(",") if i < ARGV.length
    elsif a == "--behavior"
      want_behavior = true
    elsif a == "--quiet"
      quiet = true
    elsif a == "-h" || a == "--help"
      puts "usage: spinel-doctor [--only a,b] [--skip a,b] [--behavior] [--quiet] app.rb"
      puts "  legs: build unsupported unresolved requires behavior"
      exit(0)
    else
      src = a
    end
    i = i + 1
  end
  die("usage: spinel-doctor [options] app.rb", 2) if src.length == 0
  die("spinel-doctor: no such file: " + src, 2) if !File.exist?(src)

  sp = find_spinel
  cobj = tmp_path("doctor", src, ".c")
  bin = tmp_path("doctor", src, ".bin")
  errs = 0

  puts "spinel-doctor: " + src

  # One codegen-only run with unresolved warnings on feeds three legs.
  diag = sh("SPINEL_WARN_UNRESOLVED=1 " + sp + " " + src + " -c -o " + cobj)
  cg_ok = $sh_status == 0

  if leg_on("unsupported", only, skip)
    errs = errs + report("unsupported", "error", grep_kind(diag, "unsupported "), quiet)
  end
  if leg_on("unresolved", only, skip)
    errs = errs + report("unresolved", "warn", grep_kind(diag, "warning: unresolved"), quiet)
  end

  if leg_on("requires", only, skip)
    reqs = []
    File.read(src).split("\n").each { |line|
      s = line.strip
      if s.start_with?("require ") && !s.start_with?("require_relative")
        reqs.push(s)
      end
    }
    report("requires", "info", reqs, quiet)
  end

  if leg_on("build", only, skip)
    out = sh(sp + " " + src + " -o " + bin + " --line-map")
    if $sh_status == 0
      report("build", "error", [], quiet)
    else
      lines = []
      out.split("\n").each { |l| lines.push(l.strip) if l.strip.length > 0 }
      errs = errs + report("build", "error", lines, quiet)
    end
  end

  if leg_on("behavior", only, skip) && (want_behavior || have_cmd("ruby"))
    if !have_cmd("ruby")
      puts "  [skip] behavior (needs ruby)" if !quiet
    elsif !File.exist?(bin)
      puts "  [skip] behavior (build produced no binary)" if !quiet
    else
      ref = sh("ruby " + src)
      got = sh(bin)
      if ref == got
        report("behavior", "error", [], quiet)
      else
        errs = errs + report("behavior", "error",
          ["compiled output differs from CRuby (diff the two runs to localize)"], quiet)
      end
    end
  end

  puts ""
  if errs == 0
    puts "spinel-doctor: clean"
    exit(0)
  end
  puts "spinel-doctor: " + errs.to_s + " finding(s) at error severity"
  exit(1)
end

main
