# case/when with lambda predicates: Proc#=== calls the lambda with the
# scrutinee, so its parameter takes the scrutinee's type -- including a
# Class-valued scrutinee that can't ride the integer proc-call ABI.

class Story
end

class Comment
end

class W
  def self.go(x)
    case x.class
    when ->(b) { b == Story }
      "story"
    when ->(b) { b == Comment }
      "comment"
    else
      "other"
    end
  end
end

puts W.go(Story.new)
puts W.go(Comment.new)
puts W.go(42)

# statement-position case, int scrutinee
def f(n)
  case n
  when ->(v) { v > 10 }
    puts "big"
  when ->(v) { v.even? }
    puts "even"
  else
    puts "other"
  end
end
f(11)
f(4)
f(3)

# a proc held in a variable still dispatches Proc#===
big = ->(v) { v > 5 }
case 7
when big then puts "big7"
else puts "no"
end
case 2
when big then puts "big2"
else puts "no2"
end
