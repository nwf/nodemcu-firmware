-- A more generic replacement for the old ws2812_effects library, which was,
-- in turn, based on https://github.com/kitesurfer1404/WS2812FX.
--
-- This is the front-end of the library, which uses pluggable effects behind
-- the scenes.

local M = { }

function M:init(buf, draw)
  -- avoid a cycle through the registry by collecting the mutable state
  local st = { buf -- buffer
             , 150 -- "speed"
             , 100 -- "delay"
             , 100 -- "brightness"
             , 0   -- green 
             , 0   -- red
             , 0   -- blue
             , nil -- white
             }
  -- st._cb will hold the callback set by the mode

  self._st  = st
  self._tmr = tmr.create()

  draw = draw or (ws2812 and ws2812.write)
              or (error "Please tell me how to draw")

    -- upvals: st, draw
  self._tick = function(stmr)
    if type(st._cb) == "function"
     then st._cb(st, stmr)   -- have tick cb; provide state and timer
          return draw(st[1])
    end
  end
end

function M:start() self._tmr:alarm(self._st[2], tmr.ALARM_AUTO, self._cb) end
function M:stop()  self._tmr:stop() end

function M:get_speed() return self._st[2] end
function M:set_speed(s)
  if s >= 0 and s <= 255
   then self._st[2] = s
   else error "Speed should be between 0 and 255"
  end
end

function M:get_delay() return self._st[3] end
function M:set_delay(d)
  if d >= 10
   then self._st[3] = d
   else error "Delay must be at leats 10"
  end
end

function M:set_brightness(b)
  if b >= 0 and b <= 255
   then self._st[4] = b
   else error "Brightness should be between 0 and 255"
  end
end

function M:set_color(g,r,b,w)
  self._st[5] = g
  self._st[6] = r
  self._st[7] = b
  self._st[8] = w
end

function M:set_mode(m)
  self._st._cb = require (("rgbe-%s"):format(m))()
end

return M
