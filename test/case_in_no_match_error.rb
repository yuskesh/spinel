begin
  case 5
  in 3
    puts "three"
  in 4
    puts "four"
  end
rescue NoMatchingPatternError => e
  puts "caught int miss"
end

# Match still works
case 5
in 3
  puts "three"
in 5
  puts "matched five"
end

# With else: no raise
case 99
in 1
  puts "one"
else
  puts "else clause"
end

# String scrutinee
begin
  case "x"
  in "y"
  end
rescue NoMatchingPatternError => e
  puts "caught str miss"
end

# Symbol scrutinee
begin
  case :foo
  in :bar
  end
rescue NoMatchingPatternError => e
  puts "caught sym miss"
end

# StandardError parent catches NoMatchingPatternError
begin
  case 7
  in 1
  in 2
  end
rescue StandardError => e
  puts "caught via parent"
end
