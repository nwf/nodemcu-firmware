local I2C4bit = {}

-- command bits
local LCD_RS = 0x1
local LCD_RW = 0x2
local LCD_EN = 0x4
local LCD_BACKLIGHT = 0x8

local function send4bitI2C(value, cmd, bus_args, backlight)
   local hi = bit.band(value, 0xf0)
   local lo = bit.lshift(bit.band(value, 0xf), 4)
   if backlight then
      cmd = bit.bor(cmd, LCD_BACKLIGHT)
   end
   i2c.start(bus_args.id)
   i2c.address(bus_args.id, bus_args.address, i2c.TRANSMITTER)
   i2c.write(bus_args.id, bit.bor(hi, bit.bor(cmd, LCD_EN)))
   i2c.write(bus_args.id, bit.bor(hi, bit.band(cmd, bit.bnot(LCD_EN))))
   i2c.write(bus_args.id, bit.bor(lo, bit.bor(cmd, LCD_EN)))
   i2c.write(bus_args.id, bit.bor(lo, bit.band(cmd, bit.bnot(LCD_EN))))
   i2c.stop(bus_args.id)
end

I2C4bit.command = function(self, value)
   send4bitI2C(value, 0x0, self._bus_args, self._backlight)
end

I2C4bit.write = function(self, value)
   send4bitI2C(value, LCD_RS, self._bus_args, self._backlight)
end

I2C4bit.backlight = function(self, on)
   self._backlight = on
   self._backend.command(self, 0x0)
end

I2C4bit.init = function(self)
   i2c.setup(self._bus_args.id, self._bus_args.sda, self._bus_args.scl, self._bus_args.speed)
   self._backlight = true
   -- init sequence from datasheet
   self._backend.command(self, 0x33)
   self._backend.command(self, 0x32)
end

return I2C4bit
