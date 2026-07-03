v = ENV["SPINEL_NO_SUCH_1664"].to_s
puts v == ""
puts v.length
puts v.nil?
w = ENV["SPINEL_NO_SUCH_1664"]
puts w.nil?
ENV["SPINEL_T_1664"] = "val"
puts ENV["SPINEL_T_1664"].to_s
ENV["SPINEL_T_1664"] = nil
puts ENV["SPINEL_T_1664"].nil?
puts "x#{ENV["SPINEL_NO_SUCH_1664"]}y"
