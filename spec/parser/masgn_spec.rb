def test_case
{"RawParseTree"=>
  [:masgn,
   [:array, [:lasgn, :a], [:lasgn, :b]],
   [:array, [:vcall, :c], [:vcall, :d]]],
 "Ruby"=>"a, b = c, d",
 "RubyParser"=>
  s(:masgn,
   s(:array, s(:lasgn, :a), s(:lasgn, :b)),
   s(:array, s(:call, nil, :c, s(:arglist)), s(:call, nil, :d, s(:arglist))))}
end
