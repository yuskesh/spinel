# Spinel stub: optparse
#
# Minimal OptionParser implementation for Spinel compilation.
# Supports the subset of OptionParser used by lrama:
#   - .on(flag, desc) { |v| ... }  (2-4 args with block)
#   - .banner=
#   - .separator(text)
#   - .on_tail(text)
#   - .parse!(argv)
#   - OptionParser::ParseError

class OptionParser
  class ParseError
    def initialize(msg)
      @message = msg
    end

    def message
      @message
    end

    def to_s
      @message
    end
  end

  def initialize
    @banner = ""
    @short_flags = []
    @long_flags = []
    @handlers = []
    @expects_value = []
  end

  def banner=(text)
    @banner = text
  end

  def separator(text)
  end

  def on_tail(text)
  end

  def on(a1, a2, &block)
    short = ""
    long = ""
    has_value = false
    if a1[0] == "-" && a1[1] == "-"
      long = a1
    else
      short = a1
    end
    if a2[0] == "-" && a2[1] == "-"
      long = a2
    end
    if long.include?("=")
      has_value = true
    end
    @short_flags.push(short)
    @long_flags.push(long)
    @handlers.push(block)
    @expects_value.push(has_value)
  end

  def parse!(argv)
    new_argv = []
    i = 0
    while i < argv.length
      arg = argv[i]
      matched = false
      j = 0
      while j < @handlers.length
        sf = @short_flags[j]
        lf = @long_flags[j]
        ev = @expects_value[j]
        if ev && lf != ""
          # extract base flag name before "="
          eq_pos = lf.index("=")
          if eq_pos
            base = lf[0, eq_pos]
          else
            base = lf
          end
          # --flag=VALUE form
          if arg.length > base.length && arg[0, base.length] == base && arg[base.length] == "="
            value = arg[(base.length + 1), (arg.length - base.length - 1)]
            @handlers[j].call(value)
            matched = true
          # --flag VALUE form
          elsif arg == base
            i += 1
            if i < argv.length
              @handlers[j].call(argv[i])
            end
            matched = true
          end
        else
          if arg == sf || arg == lf
            @handlers[j].call(true)
            matched = true
          end
        end
        j += 1
      end
      if !matched
        new_argv.push(arg)
      end
      i += 1
    end
    # replace argv contents with unmatched args
    argv.clear
    k = 0
    while k < new_argv.length
      argv.push(new_argv[k])
      k += 1
    end
  end

  def to_s
    @banner
  end
end
