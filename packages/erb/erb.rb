# Spinel lib: erb
#
# Minimal ERB placeholder. Full ERB requires eval which is AOT-incompatible.
# For lrama, output.rb should be rewritten to use direct string generation.

class ERB
  def initialize(input, opts)
    @input = input
    @filename = ""
  end

  def filename=(name)
    @filename = name
  end

  def result_with_hash(vars)
    @input
  end
end
