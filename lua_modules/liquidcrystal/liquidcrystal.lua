local LiquidCrystal = {}

-- commands
local LCD_CLEARDISPLAY = 0x01
local LCD_RETURNHOME = 0x02
local LCD_ENTRYMODESET = 0x04
local LCD_DISPLAYCONTROL = 0x08
local LCD_CURSORSHIFT = 0x10
local LCD_FUNCTIONSET = 0x20
local LCD_SETCGRAMADDR = 0x40
local LCD_SETDDRAMADDR = 0x80

-- flags for display entry mode
local LCD_ENTRYRIGHT = 0x00
local LCD_ENTRYLEFT = 0x02
local LCD_ENTRYSHIFTINCREMENT = 0x01
local LCD_ENTRYSHIFTDECREMENT = 0x00

-- flags for display on/off control
local LCD_DISPLAYON = 0x04
local LCD_DISPLAYOFF = 0x00
local LCD_CURSORON = 0x02
local LCD_CURSOROFF = 0x00
local LCD_BLINKON = 0x01
local LCD_BLINKOFF = 0x00

-- flags for display/cursor shift
local LCD_DISPLAYMOVE = 0x08
local LCD_CURSORMOVE = 0x00
local LCD_MOVERIGHT = 0x04
local LCD_MOVELEFT = 0x00

-- flags for function set
local LCD_8BITMODE = 0x10
local LCD_4BITMODE = 0x00
local LCD_2LINE = 0x08
local LCD_1LINE = 0x00
local LCD_5x10DOTS = 0x04
local LCD_5x8DOTS = 0x00

-- defaults
LiquidCrystal._displayfunction = 0
LiquidCrystal._displaycontrol = 0
LiquidCrystal._displaymode = 0
LiquidCrystal._backlight = false

LiquidCrystal.init = function(self, bus, bus_args, fourbitmode, oneline, eightdots, busyloop)
   self._bus_args = bus_args
   self._busyloop = busyloop
   if bus == "I2C" then
      assert(fourbitmode, "8bit mode not supported")
      self._backend = dofile "i2c4bit.lua"
   elseif bus == "GPIO" then
      if fourbitmode then
	 self._backend = dofile "gpio4bit.lua"
      else
	 self._backend = dofile "gpio8bit.lua"
      end
   else
      error(string.format("%s backend is not implemented", bus))
   end

   if fourbitmode then
      self._displayfunction = bit.bor(self._displayfunction, LCD_4BITMODE)
   else
      self._displayfunction = bit.bor(self._displayfunction, LCD_8BITMODE)
   end

   if oneline then
      self._displayfunction = bit.bor(self._displayfunction, LCD_1LINE)
   else
      self._displayfunction = bit.bor(self._displayfunction, LCD_2LINE)
   end

   if eightdots then
      self._displayfunction = bit.bor(self._displayfunction, LCD_5x8DOTS)
   else
      self._displayfunction = bit.bor(self._displayfunction, LCD_5x10DOTS)
   end

   self._backend.init(self)
   self._backend.command(self, bit.bor(LCD_FUNCTIONSET, self._displayfunction))
   self._backend.command(self, bit.bor(LCD_ENTRYMODESET, self._displaymode))

   self:display(true)
   self:clear()
end

LiquidCrystal.clear = function(self)
   self._backend.command(self, LCD_CLEARDISPLAY)
   if self._busyloop then
      tmr.delay(2000)
   end
end

LiquidCrystal.home = function(self)
   self._backend.command(self, LCD_RETURNHOME)
   if self._busyloop then
      tmr.delay(2000)
   end
end

LiquidCrystal.cursorMove = function(self, position)
   self._backend.command(self, bit.bor(LCD_SETDDRAMADDR, position))
end

LiquidCrystal.display = function(self, on)
   if on then
      self._displaycontrol = bit.bor(self._displaycontrol, LCD_DISPLAYON)
   else
         self._displaycontrol = bit.band(self._displaycontrol, bit.bnot(LCD_DISPLAYON))
   end
   self._backend.command(self, bit.bor(LCD_DISPLAYCONTROL, self._displaycontrol))
end

LiquidCrystal.blink = function(self, on)
   if on then
      self._displaycontrol = bit.bor(self._displaycontrol, LCD_BLINKON)
   else
      self._displaycontrol = bit.band(self._displaycontrol, bit.bnot(LCD_BLINKON))
   end
   self._backend.command(self, bit.bor(LCD_DISPLAYCONTROL, self._displaycontrol))
end

LiquidCrystal.cursor = function(self, on)
   if on then
      self._displaycontrol = bit.bor(self._displaycontrol, LCD_CURSORON)
   else
      self._displaycontrol = bit.band(self._displaycontrol, bit.bnot(LCD_CURSORON))
   end
   self._backend.command(self, bit.bor(LCD_DISPLAYCONTROL, self._displaycontrol))
end

LiquidCrystal.cursorLeft = function(self)
   self._backend.command(self, bit.bor(LCD_CURSORSHIFT, LCD_CURSORMOVE, LCD_MOVELEFT))
end

LiquidCrystal.cursorRight = function(self)
   self._backend.command(self, bit.bor(LCD_CURSORSHIFT, LCD_CURSORMOVE, LCD_MOVERIGHT))
end

LiquidCrystal.scrollLeft = function(self)
   self._backend.command(self, bit.bor(LCD_CURSORSHIFT, LCD_DISPLAYMOVE, LCD_MOVELEFT))
end

LiquidCrystal.scrollRight = function(self)
   self._backend.command(self, bit.bor(LCD_CURSORSHIFT, LCD_DISPLAYMOVE, LCD_MOVERIGHT))
end

LiquidCrystal.leftToRight = function(self)
   self._displaymode = bit.bor(self._displaymode, LCD_ENTRYLEFT)
   self._backend.command(self, bit.bor(LCD_ENTRYMODESET, self._displaymode))
end

LiquidCrystal.rightToLeft = function(self)
   self._displaymode = bit.band(self._displaymode, bit.bnot(LCD_ENTRYLEFT))
   self._backend.command(self, bit.bor(LCD_ENTRYMODESET, self._displaymode))
end

LiquidCrystal.autoscroll = function(self, on)
   if on then
      self._displaymode = bit.bor(self._displaymode, LCD_ENTRYSHIFTINCREMENT)
   else
      self._displaymode = bit.band(self._displaymode, bit.bnot(LCD_ENTRYSHIFTINCREMENT))
   end
   self._backend.command(self, bit.bor(LCD_ENTRYMODESET, self._displaymode))
end

LiquidCrystal.write = function(self, str)
   for i=1,#str do
      self._backend.write(self, string.byte(str, i))
   end
end

LiquidCrystal.backlight = function(self, on)
   self._backend.backlight(self, on)
end

return LiquidCrystal
