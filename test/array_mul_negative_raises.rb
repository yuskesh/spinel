# Array#* with a negative count raises ArgumentError (was: silently []).
w = %w[a] * 0
p w
begin
  %w[a] * -3
rescue ArgumentError => e
  puts "AE2: #{e.message}"
end
