class MyHash < Hash; end

class NewHash < Hash
  def initialize(*args)
    args.each_with_index do |val, index|
      self[index] = val
    end
  end
end

class DefaultHash < Hash
  def default(key)
    100
  end
end

class ToHashHash < Hash
  def to_hash() { "to_hash" => "was", "called!" => "duh." } end
end

module HashSpecs
  def self.empty_frozen_hash
    @empty ||= {}
    @empty.freeze
    @empty
  end
  
  def self.frozen_hash
    @hash ||= {1 => 2, 3 => 4}
    @hash.freeze
    @hash
  end
end
