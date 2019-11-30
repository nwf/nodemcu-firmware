local GPIO4bit = {}

local function send4bitGPIO(value, cmd, bus_args)
   if cmd then
      gpio.write(bus_args.rs, gpio.LOW)
   else
      gpio.write(bus_args.rs, gpio.HIGH)
   end
   local hi = bit.rshift(bit.band(value, 0xf0), 4)
   local lo = bit.band(value, 0xf)
   for i=0,3 do
      if bit.band(bit.lshift(1, i), hi) ~= 0 then
	 gpio.write(bus_args[i], gpio.HIGH)
      else
	 gpio.write(bus_args[i], gpio.LOW)
      end
   end
   gpio.write(bus_args.en, gpio.HIGH)
   gpio.write(bus_args.en, gpio.LOW)
   for i=0,3 do
      if bit.band(bit.lshift(1, i), lo) ~= 0 then
	 gpio.write(bus_args[i], gpio.HIGH)
      else
	 gpio.write(bus_args[i], gpio.LOW)
      end
   end
   gpio.write(bus_args.en, gpio.HIGH)
   gpio.write(bus_args.en, gpio.LOW)
end

GPIO4bit.command = function(self, value)
   send4bitGPIO(value, true, self._bus_args)
end

GPIO4bit.write = function(self, value)
   send4bitGPIO(value, false, self._bus_args)
end

GPIO4bit.backlight = function(self, on)
   if self._bus_args.bl ~= nil then
      if on then
	 gpio.write(self._bus_args.bl, gpio.HIGH)
      else
	 gpio.write(self._bus_args.bl, gpio.LOW)
      end
   end
end

GPIO4bit.init = function(self)
   for _,i in pairs(self._bus_args) do
      gpio.mode(i, gpio.OUTPUT)
   end
   -- init sequence from datasheet
   self._backend.command(self, 0x33)
   self._backend.command(self, 0x32)
end

return GPIO4bit
