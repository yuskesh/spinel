class Renderer
  attr_accessor :skip_background_fill

  def initialize
    @skip_background_fill = false
  end
end

class Window
  def initialize
    @renderer = Renderer.new
    @show_debug = false
  end

  def button_down(id)
    case id
    when 1
      @show_debug = !@show_debug
    when 2
      @renderer.skip_background_fill = !@renderer.skip_background_fill
      puts "Background fill: #{@renderer.skip_background_fill ? 'OFF' : 'ON'}"
    end
  end
end

w = Window.new
w.button_down(2)
w.button_down(1)
w.button_down(2)
w.button_down(3)
puts "done"
