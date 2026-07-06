# mspec_lite.rb -- a minimal, spinel-compilable subset of the mspec DSL.
#
# The ruby/spec harness (tools/rubyspec/) prepends this file to each extracted
# single-example program. It implements just enough of mspec for the extracted
# form: `x.should == y` (the dominant matcher: ~3400 uses in language/ alone),
# should_not, =~, and the be_*/raise_error forms core/ needs. Each assertion
# prints nothing on success; on failure it prints one MSPEC-FAIL line. The
# program's exit line (MSPEC-DONE pass=N fail=M) is what the runner parses.

$spec_pass = 0
$spec_fail = 0

class SpecExpectation
  def initialize(v, negate)
    @v = v
    @neg = negate
  end
  def report(ok, msg)
    ok = !ok if @neg
    if ok
      $spec_pass += 1
    else
      $spec_fail += 1
      puts "MSPEC-FAIL: #{msg}"
    end
    true
  end
  def ==(other)
    report(@v == other, "expected #{other.inspect}, got #{@v.inspect}")
  end
  def !=(other)
    report(@v != other, "expected not #{other.inspect}")
  end
  def =~(pat)
    report(!(@v =~ pat).nil?, "expected #{@v.inspect} =~ #{pat.inspect}")
  end
  def equal(other)
    report(@v.equal?(other), "expected same object")
  end
  def eql(other)
    report(@v.eql?(other), "expected eql")
  end
  def include(other)
    report(@v.include?(other), "expected #{@v.inspect} to include #{other.inspect}")
  end
  def be_true
    report(@v == true, "expected true, got #{@v.inspect}")
  end
  def be_false
    report(@v == false, "expected false, got #{@v.inspect}")
  end
  def be_nil
    report(@v.nil?, "expected nil, got #{@v.inspect}")
  end
  def be_empty
    report(@v.empty?, "expected empty")
  end
  def be_an_instance_of(cls)
    report(@v.instance_of?(cls), "expected instance of #{cls}")
  end
  def be_kind_of(cls)
    report(@v.is_a?(cls), "expected kind of #{cls}")
  end
  # Chain form (modern ruby/spec): -> { }.should.raise(E) is the same check.
  def raise(cls = nil, msg = nil)
    raise_error(cls, msg)
  end
  # -> { ... }.should raise_error(SomeError)  /  raise_error(SomeError, "msg")
  # v1 matches the exception CLASS BY NAME (exact), not by ancestry: spinel has
  # no is_a? on a rescued exception yet. A spec expecting a superclass therefore
  # over-reports FAIL (never under-reports PASS), which is the safe direction
  # for a measurement harness.
  def raise_error(cls = nil, msg = nil)
    raised = false
    ok = false
    begin
      @v.call
    rescue Exception => e
      raised = true
      ok = cls.nil? || e.class.to_s == cls.to_s
      ok = ok && (msg.nil? || e.message == msg)
    end
    report(raised && ok, raised ? "wrong exception raised" : "no exception raised")
  end
end

# mspec's flunk: an unconditional failure marker ("this line must not be
# reached", e.g. after a call that should have raised).
def flunk(msg = "flunked")
  $spec_fail += 1
  puts "MSPEC-FAIL: #{msg}"
end

class Object
  def should
    SpecExpectation.new(self, false)
  end
  def should_not
    SpecExpectation.new(self, true)
  end
end

# ScratchPad is handled by the EXTRACTOR as a textual rewrite onto a plain
# LOCAL variable (scratch_pad): each extracted example is one top-level
# program, so a local suffices and blocks close over it. Every module- or
# global-based implementation tripped a different compiler gap (module
# class-ivar array length, module-self global refs, Object-reopen killing
# top-level global declaration) -- all harness-found and cataloged.
