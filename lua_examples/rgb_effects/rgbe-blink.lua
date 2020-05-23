-- Blink, toggling all channels between 0 and the current color
--
-- Uses an upval to hold the display state between rounds

return function()
  local x = false

  return function(st)
    x = ~x
    if x then
      st[1]:fill(st[4],st[5],st[6],st[7])
    else
      st[1]:fill(0,0,0,st[7] and 0)
  end
end
