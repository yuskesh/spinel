# spin — the Spinel project tool (M0: new/init/build/run/test/clean,
# path dependencies only, no network, no lockfile). See docs/spin.md.

require_relative "spin/toml"

SPIN_USAGE = <<USAGE
usage: spin <command> [args]
  new <name> [--lib]   scaffold an application (or a library with --lib)
  init                 write a gem.toml into the current directory
  build [target..]     build bin/ executables into build/bin/
  run [target] [-- a]  build, then run one executable
  test [file..]        build and run test/*.rb against expectations
  clean                remove build/
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
  ""  # unreachable; keeps the return type a plain String (a while's value is nil)
end

def spinel_bin
  # spin ships beside the compiler: <dir-of-$0>/spinel
  me = File.expand_path($0)
  cand = File.join(File.expand_path("..", me), "spinel")
  return cand if File.exist?(cand)
  "spinel"  # PATH fallback
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
    toml.table_keys("dependencies").each do |dep|
      src = toml.get_inline("dependencies", dep, "path")
      ver = toml.get("dependencies", dep)
      if src != ""
        p2 = File.expand_path(src, root)
        spin_die("dependency #{dep}: path not found: #{src}") unless File.directory?(p2)
        @dep_paths.push(p2)
      elsif ver != ""
        spin_die("dependency #{dep}: only path dependencies are supported in M0 (git/index sources arrive with the lockfile milestone)")
      else
        spin_die("dependency #{dep}: needs a { path = \"..\" } source in M0")
      end
    end
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
  Dir.glob(dir + "/*").each do |p2|
    e = File.basename(p2)
    next if e == "build" || e == "vendor"
    if File.directory?(p2)
      newest = newest_mtime(p2, newest)
    elsif e.end_with?(".rb") || e.end_with?(".rbs") || e == "gem.toml"
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
  cmd = "#{spinel_bin} #{entry}"
  prj.dep_paths.each { |d| cmd += " -I #{d}" }
  cmd += " -I #{prj.root}"
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
    if regen
      system("ruby #{src} > #{exp} 2>/dev/null")
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
      system("ruby #{src} > #{cexp} 2>&1")
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
