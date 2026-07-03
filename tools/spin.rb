# spin — the Spinel project tool (M0: new/init/build/run/test/clean,
# path dependencies only, no network, no lockfile). Usage: docs/spin.md;
# design record: docs/internals/spin.md.

require_relative "spin/toml"

SPIN_USAGE = <<USAGE
usage: spin <command> [args]
  new <name> [--lib]   scaffold an application (or a library with --lib)
  init                 write a gem.toml into the current directory
  add <name> [--version C | --git URL [--ref R] | --path DIR]  add + lock
  search [term]        find gems in the index (name, latest, repo)
  remove <name>        drop a dependency + relock
  lock | fetch | vendor  resolve deps / warm the cache / copy into vendor/
  build [target..]     build bin/ executables into build/bin/
  run [target] [-- a]  build, then run one executable
  test [file..]        build and run test/*.rb against expectations
  clean                remove build/
  list [--json]        resolved dependency set (name, version, source)
  tree [--json]        dependency tree from this gem
USAGE

def spin_die(msg)
  $stderr.puts "spin: #{msg}"
  exit 1
end

# --- project discovery -------------------------------------------------------

def find_root(dir)
  d = dir
  while true
    return d if File.exist?(File.join(d, "gem.toml"))
    up = File.expand_path("..", d)
    return "" if up == d
    d = up
  end
end

def spinel_bin
  # spin ships beside the compiler: <dir-of-$0>/spinel
  me = File.expand_path($0)
  cand = File.join(File.expand_path("..", me), "spinel")
  return cand if File.exist?(cand)
  "spinel"  # PATH fallback
end


# --- index (M3) ----------------------------------------------------------------
# The index is a git repo, not a server (R5): gems/<name>.toml maps a name to
# its repo plus [[release]] version/ref entries. Selection is MVS: the LOWEST
# release satisfying every constraint gathered for the gem, so a build without
# a lock is still deterministic; gem.lock then pins the outcome.

def spin_index_url
  u = ENV["SPIN_INDEX"].to_s
  u == "" ? "https://github.com/matz/spinel-index" : u
end

def index_dir(offline)
  base = ENV["XDG_CACHE_HOME"].to_s
  base = File.join(ENV["HOME"].to_s, ".cache") if base == ""
  d = File.join(base, "spinel")
  Dir.mkdir(d) unless Dir.exist?(d)
  d = File.join(d, "index")
  return d if Dir.exist?(File.join(d, ".git"))
  spin_die("--offline: no cached index (spin fetch first)") if offline
  ok = system("git clone -q --depth 1 #{spin_index_url} #{d}")
  spin_die("cannot clone the index: " + spin_index_url) unless ok
  d
end

def index_refresh(offline)
  return if offline
  d = index_dir(false)
  system("git -C #{d} pull -q --ff-only 2>/dev/null")  # offline-tolerant: stale is usable
end

# "1.2.3" <=> "1.10" as numeric components; missing parts are 0
def vcmp(a, b)
  pa = a.split(".")
  pb = b.split(".")
  n = pa.length > pb.length ? pa.length : pb.length
  i = 0
  while i < n
    x = i < pa.length ? pa[i].to_i : 0
    y = i < pb.length ? pb[i].to_i : 0
    return -1 if x < y
    return 1 if x > y
    i += 1
  end
  0
end

# constraint: "*"/"" (any), "~> X.Y(.Z)" (pessimistic), ">= X", or exact "X.Y.Z"
def version_satisfies(v, cons)
  c = cons.strip
  return true if c == "" || c == "*"
  if c.start_with?("~>")
    floor = c[2..-1].to_s.strip
    return false if vcmp(v, floor) < 0
    parts = floor.split(".")
    return true if parts.length < 2
    ceil = ""
    i = 0
    while i < parts.length - 1
      ceil += "." unless ceil == ""
      ceil += (i == parts.length - 2 ? (parts[i].to_i + 1).to_s : parts[i])
      i += 1
    end
    return vcmp(v, ceil) < 0
  end
  return vcmp(v, c[2..-1].to_s.strip) >= 0 if c.start_with?(">=")
  vcmp(v, c) == 0
end

# MVS pick from gems/<name>.toml: lowest release satisfying the constraint.
# Returns "version\nrepo\nref" ("" when nothing matches).
def index_select(dep, cons, offline)
  gf = File.join(index_dir(offline), "gems", dep + ".toml")
  spin_die("not in the index: " + dep + " (spin add " + dep + " --git URL is the escape hatch)") unless File.exist?(gf)
  gdoc = TomlDoc.parse(File.read(gf))
  repo = gdoc.get("", "repo")
  spin_die("index entry for " + dep + " lacks a repo") if repo == ""
  n = gdoc.array_len("release")
  sel_v = ""
  sel_r = ""
  i = 0
  while i < n
    t = "release." + i.to_s
    v = gdoc.get(t, "version")
    r = gdoc.get(t, "ref")
    if v != "" && r != "" && version_satisfies(v, cons)
      if sel_v == "" || vcmp(v, sel_v) < 0
        sel_v = v
        sel_r = r
      end
    end
    i += 1
  end
  spin_die("no release of " + dep + " satisfies " + (cons == "" ? "*" : cons)) if sel_v == ""
  sel_v + "\n" + repo + "\n" + sel_r
end

# --- carried native C (M2) ----------------------------------------------------
# A gem may carry .c/.h sources (R6). spin compiles each .c once into the
# shared cache, keyed by (gem, version, toolchain), and hands the objects to
# spinel via --link; the compiler itself never touches gem C. Objects are
# project-independent (carried C is not specialized by inference).

def native_cc
  c = ENV["CC"].to_s
  c == "" ? "cc" : c
end

# The public runtime headers ship beside the compiler (dev tree: ../lib,
# installed tree: ./lib) -- same resolution the compiler itself uses.
def spinel_hdr_dir
  bin = spinel_bin
  return "" if bin == "spinel"
  d = File.expand_path("..", bin)
  a = File.join(d, "lib")
  return a if File.exist?(File.join(a, "sp_runtime.h"))
  b = File.join(File.expand_path("..", d), "lib")
  return b if File.exist?(File.join(b, "sp_runtime.h"))
  ""
end

# newline-packed .c paths (an [] accumulator arg would go poly-array and
# box the paths -- same tuple trap as dep_srcs)
def collect_c(dir)
  out = ""
  Dir.children(dir).each do |e|
    next if e.start_with?(".")
    next if e == "build" || e == "vendor" || e == "test"
    p2 = File.join(dir, e)
    if File.directory?(p2)
      out += collect_c(p2)
    elsif e.end_with?(".c")
      out += p2 + "\n"
    end
  end
  out
end

def newest_native_input(dir, newest)
  Dir.children(dir).each do |e|
    next if e.start_with?(".")
    next if e == "build" || e == "vendor" || e == "test"
    p2 = File.join(dir, e)
    if File.directory?(p2)
      newest = newest_native_input(p2, newest)
    elsif e.end_with?(".c") || e.end_with?(".h")
      m = File.mtime(p2).to_i
      newest = m if m > newest
    end
  end
  newest
end

def native_cache_dir(key)
  base = ENV["XDG_CACHE_HOME"].to_s
  base = File.join(ENV["HOME"].to_s, ".cache") if base == ""
  d = File.join(base, "spinel")
  Dir.mkdir(d) unless Dir.exist?(d)
  d = File.join(d, "native")
  Dir.mkdir(d) unless Dir.exist?(d)
  d = File.join(d, key)
  Dir.mkdir(d) unless Dir.exist?(d)
  d
end

# Compile one gem's carried C into the cache; returns the object list.
def native_objs_for(name, dir, version)
  cs = collect_c(dir)
  return [] if cs == ""
  odir = native_cache_dir(name + "-" + version + "-" + File.basename(native_cc))
  hdr = spinel_hdr_dir
  hnew = newest_native_input(dir, 0)
  objs = []
  cs.split("\n").each do |c|
    rel = c[dir.length + 1..-1].to_s
    o = File.join(odir, rel.gsub("/", "_")[0..-3] + ".o")
    if !File.exist?(o) || File.mtime(o).to_i < hnew
      cmd = native_cc + " -O2 -c #{c} -I #{dir}"
      cmd += " -I #{hdr}" if hdr != ""
      cmd += " -o #{o}"
      spin_die("native compile failed: " + rel + " (" + name + ")") unless system(cmd)
      puts "cc #{name}/#{rel}"
    end
    objs.push(o)
  end
  objs
end

# --- shared cache & git sources (M1) -----------------------------------------

def cache_gems_dir
  base = ENV["XDG_CACHE_HOME"].to_s
  base = File.join(ENV["HOME"].to_s, ".cache") if base == ""
  d = File.join(base, "spinel")
  Dir.mkdir(d) unless Dir.exist?(d)
  g = File.join(d, "gems")
  Dir.mkdir(g) unless Dir.exist?(g)
  g
end

def sh_read(cmd)
  tmp = "/tmp/spin_out_#{Process.pid}"
  system(cmd + " > " + tmp + " 2>/dev/null")
  out = File.exist?(tmp) ? File.read(tmp).strip : ""
  File.delete(tmp) if File.exist?(tmp)
  out
end

def gem_version_of(dir)
  mf = File.join(dir, "gem.toml")
  return "0.0.0" unless File.exist?(mf)
  v = TomlDoc.parse(File.read(mf)).get("gem", "version")
  v == "" ? "0.0.0" : v
end

# Fetch (or reuse) a git source; returns "dir\nversion\nsha".
def git_fetch(name, url, ref, want_sha)
  gems = cache_gems_dir
  # a previously locked SHA that is already cached wins (offline path)
  if want_sha != ""
    hits = Dir.glob(gems + "/" + name + "-*")
    hits.each do |h|
      stamp = File.join(h, ".spin-sha")
      next unless File.exist?(stamp)
      if File.read(stamp).strip == want_sha
        return h + "\n" + gem_version_of(h) + "\n" + want_sha
      end
    end
  end
  tmp = File.join(gems, ".fetch-" + name)
  system("rm -rf " + tmp)
  cloned = false
  if want_sha != ""
    # materialize the exact pinned/selected commit: fetch it directly
    # (works on file:// and GitHub); a server refusing SHA fetches falls
    # back to the full clone + checkout below.
    cloned = system("mkdir -p " + tmp) &&
             system("git -C " + tmp + " init -q 2>/dev/null") &&
             system("git -C " + tmp + " fetch -q --depth 1 " + url + " " + want_sha + " 2>/dev/null") &&
             system("git -C " + tmp + " checkout -q FETCH_HEAD 2>/dev/null")
    system("rm -rf " + tmp) unless cloned
  end
  unless cloned
    refarg = ref == "" ? "" : " --branch " + ref
    depth = want_sha == "" ? " --depth 1" : ""
    ok = system("git clone -q" + depth + refarg + " " + url + " " + tmp)
    spin_die("fetch failed: git clone " + url) unless ok
    if want_sha != ""
      okc = system("git -C " + tmp + " checkout -q " + want_sha)
      spin_die("fetch failed: " + name + " has no commit " + want_sha) unless okc
    end
  end
  sha = sh_read("git -C " + tmp + " rev-parse HEAD")
  spin_die("fetch failed: no HEAD sha for " + url) if sha == ""
  spin_die("fetch verify failed: wanted " + want_sha + ", got " + sha) if want_sha != "" && sha != want_sha
  ver = gem_version_of(tmp)
  final = File.join(gems, name + "-" + ver)
  system("rm -rf " + final)
  system("rm -rf " + File.join(tmp, ".git"))
  File.write(File.join(tmp, ".spin-sha"), sha + "\n")
  ok2 = system("mv " + tmp + " " + final)
  spin_die("fetch failed: cannot place " + final) unless ok2
  final + "\n" + ver + "\n" + sha
end

# --- gem.lock ----------------------------------------------------------------

def write_lock(root, lines)
  body = "# generated by spin lock -- diff me, don't edit me\n"
  lines.each { |l| body += l }
  File.write(File.join(root, "gem.lock"), body)
end

# Resolve all [dependencies] of the manifest at `root` (recursively through
# fetched gems), preferring SHAs recorded in gem.lock. Returns newline-packed
# records "name\tdir\tversion\tgit\tsha_or_path" joined by \n.
def resolve_deps(root, offline)
  root0 = root
  lf = File.join(root, "gem.lock")
  lock = TomlDoc.parse("")
  lock = TomlDoc.parse(File.read(lf)) if File.exist?(lf)
  out = ""
  seen = { "" => "" }
  queue = [root]
  qdirs = { root => "" }
  while queue.length > 0
    cur = queue.shift.to_s
    mf = File.join(cur, "gem.toml")
    next unless File.exist?(mf)
    toml = TomlDoc.parse(File.read(mf))
    toml.table_keys("dependencies").each do |dep|
      next if seen.key?(dep)
      seen[dep] = "1"
      pth = toml.get_inline("dependencies", dep, "path")
      url = toml.get_inline("dependencies", dep, "git")
      ref = toml.get_inline("dependencies", dep, "ref")
      if pth != ""
        d2 = File.expand_path(pth, cur)
        spin_die("dependency " + dep + ": path not found: " + pth) unless File.directory?(d2)
        out += dep + "\t" + d2 + "\t" + gem_version_of(d2) + "\tpath\t" + pth + "\n"
        queue.push(d2)
      elsif url != ""
        want = lock.get("lock." + dep, "ref")
        rec = ""
        if offline
          hit = ""
          Dir.glob(cache_gems_dir + "/" + dep + "-*").each do |h|
            st = File.join(h, ".spin-sha")
            hit = h if want != "" && File.exist?(st) && File.read(st).strip == want
          end
          if hit == ""
            Dir.glob(File.join(root0, "vendor/gems") + "/" + dep + "-*").each { |h| hit = h }
          end
          spin_die("--offline: " + dep + " not in cache or vendor (spin fetch/vendor first)") if hit == ""
          rec = hit + "\n" + gem_version_of(hit) + "\n" + want.to_s
        else
          rec = git_fetch(dep, url, ref, want)
        end
        parts = rec.split("\n")
        # want may be "" (no gem.lock yet): split drops the trailing empty
        # field, so read the SHA defensively rather than trusting parts[2].
        sha = parts.length > 2 ? parts[2] : ""
        out += dep + "\t" + parts[0] + "\t" + parts[1] + "\tgit\t" + url + "\x01" + sha + "\n"
        queue.push(parts[0])
      else
        # a plain string value is an index constraint: foo = "~> 1.2" / "*"
        cons = toml.get("dependencies", dep)
        sel = index_select(dep, cons, offline).split("\n")
        sel_v = sel[0]
        repo = sel[1]
        sel_r = sel.length > 2 ? sel[2] : ""
        # verify-not-select: a lock ref that still satisfies the constraint
        # pins the build; one that no longer does is reselected (spin lock
        # rewrites the pin).
        want = lock.get("lock." + dep, "ref")
        lv = lock.get("lock." + dep, "version")
        if want != "" && version_satisfies(lv, cons)
          sel_r = want
          sel_v = lv
        elsif want != ""
          $stderr.puts "spin: gem.lock pins " + dep + " " + lv + " outside " + cons + "; reselecting " + sel_v
        end
        rec = ""
        if offline
          hit = ""
          Dir.glob(cache_gems_dir + "/" + dep + "-*").each do |h|
            st = File.join(h, ".spin-sha")
            hit = h if sel_r != "" && File.exist?(st) && File.read(st).strip == sel_r
          end
          if hit == ""
            Dir.glob(File.join(root0, "vendor/gems") + "/" + dep + "-*").each { |h| hit = h }
          end
          spin_die("--offline: " + dep + " not in cache or vendor (spin fetch/vendor first)") if hit == ""
          rec = hit + "\n" + gem_version_of(hit) + "\n" + sel_r
        else
          rec = git_fetch(dep, repo, "", sel_r)
        end
        parts = rec.split("\n")
        sha = parts.length > 2 ? parts[2] : sel_r
        out += dep + "\t" + parts[0] + "\t" + sel_v + "\tindex\t" + repo + "\x01" + sha + "\x01" + cons + "\n"
        queue.push(parts[0])
      end
    end
  end
  out
end

# --- manifest ----------------------------------------------------------------

class Project
  attr_reader :root, :name, :dep_paths

  def initialize(root)
    @root = root
    toml = TomlDoc.parse(File.read(File.join(root, "gem.toml")))
    nm = toml.get("gem", "name")
    if nm == ""
      base = File.basename(root)
      base = base[7..-1] if base.start_with?("spinel-")
      nm = base
    end
    @name = nm
    @dep_paths = []
    @dep_records = resolve_deps(root, ENV["SPIN_OFFLINE"].to_s.length > 0)
    # tab-packed name\tdir\tversion records (an array of tuples would go
    # poly and poison the string params downstream)
    @dep_srcs = @name + "\t" + root + "\t" + gem_version_of(root)
    @dep_records.split("\n").each do |rec|
      next if rec == ""
      f = rec.split("\t")
      # prefer a vendored copy when present
      vd = File.join(root, "vendor/gems", f[0] + "-" + f[2])
      d = File.directory?(vd) ? vd : f[1]
      @dep_paths.push(d)
      @dep_srcs += "\n" + f[0] + "\t" + d + "\t" + f[2]
    end
  end

  # carried native C across the root gem and every resolved dep (M2)
  def native_objs
    objs = []
    @dep_srcs.split("\n").each do |s|
      f = s.split("\t")
      native_objs_for(f[0], f[1], f[2]).each { |o| objs.push(o) }
    end
    objs
  end

  def dep_records
    @dep_records
  end

  def bins
    out = []
    bd = File.join(@root, "bin")
    return out unless File.directory?(bd)
    Dir.glob(bd + "/*.rb").each do |path|
      out.push(File.basename(path)[0..-4])
    end
    out
  end

  def tests
    out = []
    td = File.join(@root, "test")
    return out unless File.directory?(td)
    Dir.glob(td + "/*.rb").each do |path|
      out.push(File.basename(path))
    end
    out
  end
end

# --- staleness (newest input mtime vs output mtime) --------------------------

def newest_mtime(dir, newest)
  Dir.children(dir).each do |e|
    next if e.start_with?(".")   # .git and friends
    p2 = File.join(dir, e)
    next if e == "build" || e == "vendor"
    if File.directory?(p2)
      newest = newest_mtime(p2, newest)
    elsif e.end_with?(".rb") || e.end_with?(".rbs") || e.end_with?(".c") || e.end_with?(".h") || e == "gem.toml"
      m = File.mtime(p2).to_i
      newest = m if m > newest
    end
  end
  newest
end

def inputs_mtime(prj)
  newest = newest_mtime(prj.root, 0)
  prj.dep_paths.each { |d| newest = newest_mtime(d, newest) }
  sb = spinel_bin
  newest = File.mtime(sb).to_i if File.exist?(sb) && File.mtime(sb).to_i > newest
  newest
end

# --- build -------------------------------------------------------------------

def compile(prj, entry, out, extra)
  # Inside a spin project the dependency universe is fully known (manifest +
  # lock), so an unresolvable require is a bug, not a maybe: flip the
  # compiler's require gate from warning to hard error. This also makes
  # stdlib features require-gated, i.e. CRuby-style `require "stringio"`
  # before use.
  cmd = "SPINEL_REQUIRE_GATE=1 #{spinel_bin} #{entry}"
  prj.dep_paths.each { |d| cmd += " -I #{d}" }
  cmd += " -I #{prj.root}"
  prj.native_objs.each { |o| cmd += " --link #{o}" }
  cmd += " #{extra}" if extra != ""
  cmd += " -o #{out}"
  ok = system(cmd)
  unless ok
    # close the add-a-gem loop: wrap the compiler's unsatisfiable-require error
    $stderr.puts "spin: build failed (hint: an unresolved require may need a dependency: spin add <name> --path <dir>)"
    exit 1
  end
end

def cmd_build(prj, targets, extra)
  bins = prj.bins
  spin_die("no bin/*.rb executables to build (a library is exercised via `spin test`)") if bins.empty?
  targets = bins if targets.empty?
  need = inputs_mtime(prj)
  Dir.mkdir(File.join(prj.root, "build")) unless Dir.exist?(File.join(prj.root, "build"))
  bindir = File.join(prj.root, "build/bin")
  Dir.mkdir(bindir) unless Dir.exist?(bindir)
  targets.each do |t|
    spin_die("no such executable: bin/#{t}.rb") unless bins.include?(t)
    out = File.join(bindir, t)
    if File.exist?(out) && File.mtime(out).to_i > need && extra == ""
      puts "build #{t} (up to date)"
      next
    end
    puts "build #{t}"
    compile(prj, File.join(prj.root, "bin/#{t}.rb"), out, extra)
  end
end

def cmd_run(prj, args)
  target = ""
  run_args = []
  seen_dd = false
  args.each do |a|
    if a == "--"
      seen_dd = true
    elsif seen_dd
      run_args.push(a)
    elsif target == ""
      target = a
    end
  end
  bins = prj.bins
  if target == ""
    spin_die("no executables in bin/") if bins.empty?
    spin_die("multiple executables (#{bins.join(', ')}): spin run <name>") if bins.length > 1
    target = bins[0]
  end
  cmd_build(prj, [target], "")
  cmd = File.join(prj.root, "build/bin", target)
  run_args.each { |a| cmd += " #{a}" }
  ok = system(cmd)
  exit(ok ? 0 : 1)
end

# --- test --------------------------------------------------------------------

def cmd_test(prj, files, regen)
  tests = prj.tests
  spin_die("no test/*.rb files") if tests.empty?
  tests = files.map { |f| File.basename(f) } unless files.empty?
  tdir = File.join(prj.root, "build/test")
  Dir.mkdir(File.join(prj.root, "build")) unless Dir.exist?(File.join(prj.root, "build"))
  Dir.mkdir(tdir) unless Dir.exist?(tdir)
  fails = 0
  tests.each do |t|
    src = File.join(prj.root, "test", t)
    spin_die("no such test: test/#{t}") unless File.exist?(src)
    exp = src + ".expected"
    inc = ""
    prj.dep_paths.each { |d| inc += " -I #{d}" }
    inc += " -I #{prj.root}"
    if regen
      system("ruby#{inc} #{src} > #{exp} 2>/dev/null")
      puts "regen #{t}"
      next
    end
    bin = File.join(tdir, t[0..-4])
    compile(prj, src, bin, "")
    outf = bin + ".out"
    system("#{bin} > #{outf} 2>&1")
    actual = File.exist?(outf) ? File.read(outf) : ""
    expected = ""
    if File.exist?(exp)
      expected = File.read(exp)
    else
      # no snapshot: diff directly against CRuby (the subset-parity check)
      cexp = bin + ".cruby"
      system("ruby#{inc} #{src} > #{cexp} 2>&1")
      expected = File.exist?(cexp) ? File.read(cexp) : ""
    end
    if actual == expected
      puts "ok   #{t}"
    else
      puts "FAIL #{t}"
      puts "--- expected"
      print expected
      puts "--- actual"
      print actual
      fails += 1
    end
  end
  puts "#{tests.length - fails}/#{tests.length} passed"
  exit 1 if fails > 0
end


# --- add / lock / fetch / vendor (M1) ------------------------------------------

def lock_from_records(prj)
  lines = []
  prj.dep_records.split("\n").each do |rec|
    next if rec == ""
    f = rec.split("\t")
    lines.push("\n[lock." + f[0] + "]\nversion = \"" + f[2] + "\"\n")
    if f[3] == "git" || f[3] == "index"
      us = f[4].split("\x01")
      lines.push("git = \"" + us[0] + "\"\nref = \"" + us[1] + "\"\n")
    else
      lines.push("path = \"" + f[4] + "\"\n")
    end
  end
  write_lock(prj.root, lines)
  puts "locked " + prj.dep_paths.length.to_s + " gem(s)"
end

def jq_str(s)
  out = ""
  s.split("").each do |ch|
    if ch == "\\" || ch == "\""
      out += "\\" + ch
    else
      out += ch
    end
  end
  "\"" + out + "\""
end

def cmd_list(prj, json)
  if json
    out = "["
    first = true
    prj.dep_records.split("\n").each do |rec|
      next if rec == ""
      f = rec.split("\t")
      src = f[3] == "path" ? f[4] : f[4].split("\x01")[0]
      out += "," unless first
      first = false
      out += "{\"name\":" + jq_str(f[0]) + ",\"version\":" + jq_str(f[2]) +
             ",\"kind\":" + jq_str(f[3]) + ",\"source\":" + jq_str(src) + "}"
    end
    puts out + "]"
  else
    prj.dep_records.split("\n").each do |rec|
      next if rec == ""
      f = rec.split("\t")
      src = f[3] == "path" ? f[4] : f[4].split("\x01")[0]
      puts f[0] + " " + f[2] + " (" + f[3] + " " + src + ")"
    end
  end
end

# name -> resolved dir, for walking each gem's own manifest
def tree_children(dir)
  mf = File.join(dir, "gem.toml")
  return [] unless File.exist?(mf)
  TomlDoc.parse(File.read(mf)).table_keys("dependencies")
end

def tree_walk(prj, name, dir, version, indent, seen, json)
  out = ""
  if json
    out = "{\"name\":" + jq_str(name) + ",\"version\":" + jq_str(version) + ",\"deps\":["
  else
    puts indent + name + " " + version
  end
  first = true
  tree_children(dir).each do |dep|
    # resolved location/version from the flat record set
    ddir = ""
    dver = ""
    prj.dep_records.split("\n").each do |rec|
      f = rec.split("\t")
      if f[0] == dep
        ddir = f[1]
        dver = f[2]
      end
    end
    next if ddir == ""
    if seen.include?("|" + dep + "|")
      puts indent + "  " + dep + " " + dver + " (...)" unless json
      next
    end
    sub = tree_walk(prj, dep, ddir, dver, indent + "  ", seen + "|" + dep + "|", json)
    if json
      out += "," unless first
      first = false
      out += sub
    end
  end
  json ? out + "]}" : ""
end

def cmd_tree(prj, json)
  r = tree_walk(prj, prj.name, prj.root, gem_version_of(prj.root), "", "|" + prj.name + "|", json)
  puts r if json
end

def cmd_add(root, name, url, ref, pth, cons)
  spin_die("usage: spin add <name> [--version C | --git URL [--ref R] | --path DIR]") if name == ""
  mf = File.join(root, "gem.toml")
  text = File.read(mf)
  spec = ""
  if pth != ""
    spec = "{ path = \"" + pth + "\" }"
  elsif url != ""
    spec = ref == "" ? "{ git = \"" + url + "\" }"
                     : "{ git = \"" + url + "\", ref = \"" + ref + "\" }"
  else
    # index form: bare name takes any release, --version narrows it
    index_refresh(false)
    spec = "\"" + (cons == "" ? "*" : cons) + "\""
  end
  line = name + " = " + spec + "\n"
  if text.include?("\n[dependencies]\n")
    text = text.sub("\n[dependencies]\n", "\n[dependencies]\n" + line)
  elsif text.start_with?("[dependencies]\n")
    text = "[dependencies]\n" + line + text[15..-1]
  else
    text += "\n[dependencies]\n" + line
  end
  File.write(mf, text)
  prj = Project.new(root)
  lock_from_records(prj)
  puts "added " + name
end

def cmd_search(term)
  index_refresh(false)
  d = File.join(index_dir(false), "gems")
  found = 0
  Dir.glob(d + "/*.toml").sort.each do |gf|
    nm = File.basename(gf)[0..-6]
    next unless term == "" || nm.include?(term)
    gdoc = TomlDoc.parse(File.read(gf))
    best = ""
    i = 0
    while i < gdoc.array_len("release")
      v = gdoc.get("release." + i.to_s, "version")
      best = v if best == "" || vcmp(v, best) > 0
      i += 1
    end
    puts nm + " " + best + " " + gdoc.get("", "repo")
    found += 1
  end
  puts "no matches" if found == 0
end

def cmd_remove(root, name)
  mf = File.join(root, "gem.toml")
  out = ""
  File.read(mf).split("\n").each do |l|
    out += l + "\n" unless l.strip.start_with?(name + " ") || l.strip.start_with?(name + "=")
  end
  File.write(mf, out)
  prj = Project.new(root)
  lock_from_records(prj)
  puts "removed " + name
end

def cmd_vendor(prj)
  vg = File.join(prj.root, "vendor")
  Dir.mkdir(vg) unless Dir.exist?(vg)
  vg = File.join(vg, "gems")
  Dir.mkdir(vg) unless Dir.exist?(vg)
  prj.dep_records.split("\n").each do |rec|
    next if rec == ""
    f = rec.split("\t")
    dst = File.join(vg, f[0] + "-" + f[2])
    system("rm -rf " + dst)
    system("cp -a " + f[1] + " " + dst)
    puts "vendored " + f[0] + "-" + f[2]
  end
end

# --- scaffold ----------------------------------------------------------------

APP_MANIFEST = <<TOML
# spin manifest — an application needs no [gem] identity.
# Add dependencies like:
#   [dependencies]
#   ansi = { path = "../spinel-ansi" }
TOML

def cmd_new(name, lib)
  spin_die("usage: spin new <name> [--lib]") if name == ""
  spin_die("#{name}: already exists") if File.exist?(name)
  Dir.mkdir(name)
  Dir.mkdir(File.join(name, "test"))
  if lib
    File.write(File.join(name, "gem.toml"),
               "[gem]\nname = \"#{name}\"\nversion = \"0.1.0\"\n\n# published repos are conventionally named spinel-#{name}\n")
    File.write(File.join(name, "#{name}.rb"), "# #{name}: library entry (require \"#{name}\")\n")
  else
    File.write(File.join(name, "gem.toml"), APP_MANIFEST)
    Dir.mkdir(File.join(name, "bin"))
    File.write(File.join(name, "bin/#{name}.rb"), "puts \"Hello from #{name}\"\n")
  end
  File.write(File.join(name, ".gitignore"), "/build/\n")
  system("git -C #{name} init -q")
  puts "created #{name}/#{lib ? " (library)" : ""}"
end

def cmd_init
  spin_die("gem.toml already exists") if File.exist?("gem.toml")
  File.write("gem.toml", APP_MANIFEST)
  puts "wrote gem.toml"
end

# --- main --------------------------------------------------------------------

cmd = ARGV.empty? ? "" : ARGV[0]
rest = []
i = 1
while i < ARGV.length
  rest.push(ARGV[i])
  i += 1
end

case cmd
when "new"
  lib = rest.include?("--lib")
  names = rest.reject { |a| a.start_with?("--") }
  cmd_new(names.empty? ? "" : names[0], lib)
when "init"
  cmd_init
when "add"
  root = find_root(Dir.pwd)
  spin_die("no gem.toml found") if root == ""
  nm = ""
  url = ""
  ref = ""
  pth = ""
  cons = ""
  i2 = 0
  while i2 < rest.length
    a = rest[i2]
    if a == "--git"
      i2 += 1
      url = rest[i2].to_s
    elsif a == "--ref"
      i2 += 1
      ref = rest[i2].to_s
    elsif a == "--path"
      i2 += 1
      pth = rest[i2].to_s
    elsif a == "--version"
      i2 += 1
      cons = rest[i2].to_s
    elsif !a.start_with?("--") && nm == ""
      nm = a
    end
    i2 += 1
  end
  cmd_add(root, nm, url, ref, pth, cons)
when "remove"
  root = find_root(Dir.pwd)
  spin_die("no gem.toml found") if root == ""
  cmd_remove(root, rest.empty? ? "" : rest[0])
when "lock", "fetch", "vendor"
  root = find_root(Dir.pwd)
  spin_die("no gem.toml found") if root == ""
  prj = Project.new(root)
  lock_from_records(prj) if cmd == "lock"
  puts "fetched " + prj.dep_paths.length.to_s + " gem(s)" if cmd == "fetch"
  cmd_vendor(prj) if cmd == "vendor"
when "search"
  cmd_search(rest.empty? ? "" : rest[0])
when "list", "tree"
  root = find_root(Dir.pwd)
  spin_die("no gem.toml found") if root == ""
  prj = Project.new(root)
  json = rest.include?("--json")
  cmd_list(prj, json) if cmd == "list"
  cmd_tree(prj, json) if cmd == "tree"
when "build", "run", "test", "clean"
  root = find_root(Dir.pwd)
  spin_die("no gem.toml found (run `spin init`, or `spin new <name>`)") if root == ""
  prj = Project.new(root)
  case cmd
  when "build"
    extra = ""
    if rest.include?("--")
      di = rest.index("--").to_i
      extra = rest[(di + 1)..-1].join(" ")
      rest = rest[0..(di - 1)]
    end
    cmd_build(prj, rest, extra)
  when "run"
    cmd_run(prj, rest)
  when "test"
    regen = rest.include?("--regen")
    files = rest.reject { |a| a.start_with?("--") }
    cmd_test(prj, files, regen)
  when "clean"
    system("rm -rf #{File.join(prj.root, 'build')}")
    puts "cleaned"
  end
when "--version"
  puts "spin (spinelgems M0)"
else
  puts SPIN_USAGE
  exit(cmd == "" || cmd == "help" || cmd == "--help" ? 0 : 3)
end
