class Screen
  attr_accessor :width
  attr_writer :depth
  attr_reader :label

  def initialize
    @width = 320
    @depth = 8
    @label = "screen"
    @fullscreen = false
  end

  def fullscreen=(v)
    @fullscreen = v
  end

  def resize
    42
  end

  def apply(key, value)
    case key
    when :fullscreen
      self.fullscreen = value if respond_to?(:fullscreen=)
      puts "fullscreen set"
    when :width
      self.width = value if respond_to?(:width=)
      puts "width set"
    end
  end

  def probe
    puts respond_to?(:resize)
    puts respond_to?(:width)
    puts respond_to?(:width=)
    puts respond_to?(:depth=)
    puts respond_to?(:label)
    puts respond_to?(:label=)
    puts respond_to?(:missing_method)
    puts respond_to?(:to_s)
  end

  def self.build
    puts respond_to?(:build)
    puts respond_to?(:nope)
    new
  end
end

s = Screen.build
s.apply(:fullscreen, true)
s.apply(:width, 640)
s.probe
