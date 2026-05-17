#!/usr/bin/env ruby
# Spike harness: run spinel_analyze twice on the same AST -- once
# without a seed file, once with -- and print the diff for the
# Box class's @cls_meth_ptypes / @cls_meth_returns / @cls_ivar_types
# rows. The diff is the evidence that RBS seeding flowed through to
# the analyzer's type tables.
#
# Run from the spinel repo root after `make parse`:
#
#   ruby experiments/rbs/harness.rb
#
# Exit 0 = the expected diff was observed (Box#relabel's param
# changed from int to string). Exit 1 = either the harness couldn't
# run the pipeline or the diff didn't show the expected change.

require "fileutils"
require "tmpdir"

SPINEL_ROOT = File.expand_path("../..", __dir__)
FIXTURE_RB = File.join(__dir__, "box.rb")
SEED_FILE  = File.join(__dir__, "box.seed")

PARSE_BIN    = File.join(SPINEL_ROOT, "spinel_parse")
ANALYZE_RB   = File.join(SPINEL_ROOT, "spinel_analyze.rb")

unless File.executable?(PARSE_BIN)
  abort "spinel_parse not built -- run `make parse` first"
end

Dir.mktmpdir("rbs_spike") do |tmp|
  ast = File.join(tmp, "box.ast")
  ir_off = File.join(tmp, "box_off.ir")
  ir_on  = File.join(tmp, "box_on.ir")

  ok = system(PARSE_BIN, FIXTURE_RB, ast, out: File::NULL)
  abort "parse failed" unless ok

  ok = system("ruby", ANALYZE_RB, ast, ir_off, out: File::NULL)
  abort "analyze (no seed) failed" unless ok

  ok = system("ruby", ANALYZE_RB, ast, ir_on, SEED_FILE, out: File::NULL)
  abort "analyze (with seed) failed" unless ok

  # Pluck the rows we care about. The IR format is one directive per
  # line; rows for the class tables are positional pipes, one slot per
  # user class. Box is the first (and only) user class in the fixture,
  # so we read slot 0.
  rows_of_interest = %w[
    @cls_names
    @cls_meth_names
    @cls_meth_ptypes
    @cls_meth_returns
    @cls_ivar_names
    @cls_ivar_types
  ]

  # IR row encoding: SA <name> <count> <row>. Within <row>, `|` is the
  # between-class separator and any class slot's content URL-encodes
  # its own `|`s as %7C. So per-method ptypes (separated by `|` in the
  # raw analyzer state) become %7C in the dump, and per-param types
  # stay comma-separated. Pluck the Box slot (index 0) and decode.
  def pluck(path, rows)
    out = {}
    File.foreach(path) do |line|
      line = line.chomp
      next unless line.start_with?("SA ")
      _, name, _count, rest = line.split(" ", 4)
      rest ||= ""
      out[name] = rest.split("|", -1)[0] if rows.include?(name)
    end
    out
  end

  off = pluck(ir_off, rows_of_interest)
  on  = pluck(ir_on,  rows_of_interest)

  puts "=" * 72
  puts "Box class IR rows (slot 0 of each table; %7C is encoded |)"
  puts "=" * 72
  puts "%-22s  %-22s -> %-22s" % ["row", "no --rbs", "with --rbs"]
  puts "-" * 72
  rows_of_interest.each do |k|
    a = off[k].to_s
    b = on[k].to_s
    marker = (a == b) ? "  " : "* "
    puts "%s%-20s  %-22s -> %-22s" % [marker, k, a.inspect, b.inspect]
  end
  puts "=" * 72

  # Box has methods (initialize, relabel, show). Within the class
  # slot, methods are separated by %7C and params by `,`. The
  # expected change: relabel(s)'s param flips from int to string.
  meth_names_off  = off["@cls_meth_names"].to_s.split(";")
  ptypes_off      = off["@cls_meth_ptypes"].to_s.split("%7C", -1)
  ptypes_on       = on["@cls_meth_ptypes"].to_s.split("%7C", -1)

  relabel_idx = meth_names_off.index("relabel")
  if relabel_idx.nil?
    abort "relabel not found in @cls_meth_names; fixture changed?"
  end

  before = ptypes_off[relabel_idx]
  after  = ptypes_on[relabel_idx]

  puts ""
  puts "Box#relabel param type: #{before.inspect} -> #{after.inspect}"

  if before == after
    abort "FAIL: seeding did not change Box#relabel's param type"
  end
  if after != "string"
    abort "FAIL: expected Box#relabel param to seed to 'string', got #{after.inspect}"
  end

  puts ""
  puts "PASS: RBS seeding flowed through to the analyzer's type tables."
end
