# A class method specialized into subclasses (Base.find -> Article.find /
# Comment.find) must not cause an unrelated class's same-named class method
# (Sqlite.find) to be DCE'd as a "transplanted source". The transplant check
# must match the class hierarchy, not just the method name -- otherwise the
# adapter's find is called but never emitted (undefined reference).

class Sqlite
  def self.find(table, id)
    "sql-#{table}-#{id}"
  end
end
module AR
  class << self
    attr_accessor :adapter
  end
end
AR.adapter = Sqlite
class Base
  def self.table_name
    "base"
  end
  def self.find(id)
    AR.adapter.find(table_name, id)
  end
end
class Article < Base
  def self.table_name
    "articles"
  end
end
class Comment < Base
  def self.table_name
    "comments"
  end
end
puts Article.find(5)
puts Comment.find(9)
puts Base.find(1)
