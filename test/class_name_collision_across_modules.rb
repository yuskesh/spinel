# Same leaf-name classes in different modules must stay distinct. They share a
# flat sp_<Name> C namespace keyed by the leaf name, so before the qualifying
# pass `Web::Response` and `Chat::Response` collapsed onto one struct + method
# set (the last `initialize` won), leaving the other class's ivars unset -- the
# seed-then-delete `@cookies` here would be NULL and `push` would SIGSEGV.
# Each class must get its own module-qualified identity, while a genuine reopen
# of the same qualified class still merges.
module Web
  class Response
    def initialize
      @cookies = [""]        # seed to pin the str_array element type
      @cookies.delete_at(0)  # then empty it
    end
    def add(c)
      @cookies.push(c)
    end
    def first
      @cookies[0]
    end
  end
end

# Reopening Web::Response (same qualified name) must MERGE, not create a new class.
module Web
  class Response
    def tag
      "web"
    end
  end
end

module Chat
  class Response
    def initialize
      @content = "hi"
    end
    def content
      @content
    end
  end
end

module Llm
  module Inner
    class Response
      def initialize
        @x = 3
      end
      def x
        @x
      end
    end
  end
end

r = Web::Response.new
r.add("sid")
puts r.first              # sid  (cookies array was allocated, not NULL)
puts r.tag               # web  (reopen merged in)
puts Chat::Response.new.content      # hi   (distinct class)
puts Llm::Inner::Response.new.x.to_s # 3    (3-deep distinct class)

# A bare class reference inside a module body resolves lexically.
module Web
  class User
    def go
      Response.new.tag   # bare Response -> Web::Response
    end
  end
end
puts Web::User.new.go    # web
