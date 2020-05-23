-- Construct a function which just sets the current color
return function()
  return function(st)
    st[1]:fill(st[4],st[5],st[6],st[7])
  end
end
