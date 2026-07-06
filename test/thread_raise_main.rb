# Thread#raise aimed at the MAIN thread from a sibling must not crash the
# process (it used to NULL-deref: main has no fiber, so sp_thread_deliver
# passed NULL to the inject setters). Spinel documents delivery-to-main as
# unsupported (a no-op; see docs/limitations.md), so the sleep completes
# undisturbed. Under CRuby the raise IS delivered and the rescue swallows it.
# Either way the output is "ok" -- the test pins "does not crash", not the
# delivery semantics.
Thread.report_on_exception = false
begin
  t = Thread.new { Thread.main.raise("to-main") }
  t.join
  sleep 0.2
rescue => e
  # CRuby lands here; spinel's no-op never raises.
end
puts "ok"
