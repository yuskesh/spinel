# Stress test: ConstantPathNode resolution at depths beyond what
# typical Ruby code reaches. const_ref_flat_name and
# resolve_const_ref_name now have a depth=32 guard; this test
# exercises 6 levels (well below the cap, well above the 3-4
# levels seen in the rest of the regression suite) to catch
# off-by-one errors in the depth bookkeeping or any premature
# truncation.

module L1
  module L2
    module L3
      module L4
        module L5
          class L6
            def self.tag
              "L1::L2::L3::L4::L5::L6"
            end

            def echo(s)
              s + " <- " + L1::L2::L3::L4::L5::L6.tag
            end
          end
        end
      end
    end
  end
end

puts L1::L2::L3::L4::L5::L6.tag
puts L1::L2::L3::L4::L5::L6.new.echo("hi")
