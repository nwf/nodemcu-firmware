# LiquidCrystal Module
| Since  | Origin / Contributor  | Maintainer  | Source  |
| :----- | :-------------------- | :---------- | :------ |
| 2019-12-01 | [Matsievskiy Sergey](https://github.com/seregaxvm) | [Matsievskiy Sergey](https://github.com/seregaxvm) | [liquidcrystal.lua](../../lua_modules/liquidcrystal/liquidcrystal.lua) |

This Lua module provides access to [Hitachi HD44780](https://www.sparkfun.com/datasheets/LCD/HD44780.pdf) based LCDs. It supports 4 bit and 8 bit GPIO interface, 4 bit [PCF8574](https://www.nxp.com/docs/en/data-sheet/PCF8574_PCF8574A.pdf) based I²C interface.

!!! note
	This module requires `bit` C module built into firmware. Depending on the interface, `gpio` or `i2c` module is also required. `tmr` is needed if one wishes to enable `busyloop` option.

### Require
```lua
lc = require("liquidcrystal")
```

### Release
```lua
liquidcrystal = nil
package.loaded["liquidcrystal"] = nil
```

## liquidcrystal.autoscroll
Autoscroll text when printing. When turned off, cursor moves and text stays still, when turned on, vice versa.

#### Syntax
`liquidcrystal.autoscroll(self, on)`

#### Parameters
- `self`: `liquidcrystal` instance.
- `on`: `true` to turn on, `false` to turn off.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:autoscroll(true)
```

## liquidcrystal.backlight
Control LCDs backlight.

#### Syntax
`liquidcrystal.backlight(self, on)`

#### Parameters
- `self`: `liquidcrystal` instance.
- `on`: `true` to turn on, `false` to turn off.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:backlight(true)
```

## liquidcrystal.blink
Control cursors blink mode.

#### Syntax
`liquidcrystal.blink(self, on)`

#### Parameters
- `self`: `liquidcrystal` instance.
- `on`: `true` to turn on, `false` to turn off.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:blink(true)
```

## liquidcrystal.clear
Clear LCD screen.

#### Syntax
`liquidcrystal.clear(self)`

#### Parameters
- `self`: `liquidcrystal` instance.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:clear()
```

## liquidcrystal.cursor
Control cursors highlight mode.

#### Syntax
`liquidcrystal.cursor(self, on)`

#### Parameters
- `self`: `liquidcrystal` instance.
- `on`: `true` to turn on, `false` to turn off.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:cursor(true)
```

## liquidcrystal.cursorLeft
Move cursor one character to the left.

#### Syntax
`liquidcrystal.cursorLeft(self)`

#### Parameters
- `self`: `liquidcrystal` instance.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:cursorLeft()
```

## liquidcrystal.cursorMove
Move cursor to position.

#### Syntax
`liquidcrystal.cursorMove(self, position)`

#### Parameters
- `self`: `liquidcrystal` instance.
- `position`: new cursor position index.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:cursorMove(20)
```

## liquidcrystal.cursorRight
Move cursor one character to the right.

#### Syntax
`liquidcrystal.cursorRight(self)`

#### Parameters
- `self`: `liquidcrystal` instance.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:cursorRight()
```

## liquidcrystal.display
Turn display on and off. Does not affect display backlight. Does not clear the display.

#### Syntax
`liquidcrystal.display(self, on)`

#### Parameters
- `self`: `liquidcrystal` instance.
- `on`: `true` to turn on, `false` to turn off.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:display(true)
```

## liquidcrystal.home
Reset cursor and screen position.

#### Syntax
`liquidcrystal.home(self)`

#### Parameters
- `self`: `liquidcrystal` instance.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:home()
```

## liquidcrystal.init
Function to setup the NodeMCU communication interface and configure LCD.

#### Syntax
`liquidcrystal.init(self, bus, bus_args, fourbitmode, oneline, eightdots, busyloop)`

#### Parameters
- `self`: `liquidcrystal` instance.
- `bus`: "GPIO" or "I2C".
- `bus_args`: interface configuration table.
  - for 4 bit `GPIO` interface table elements are:
	- `en`: IO connected to LCDs `EN` pin
	- `rs`: IO connected to LCDs `RS` pin
	- `bl`: IO controlling LCDs backlight. Optional
	- `0`: IO connected to LCDs `D4` pin
	- `1`: IO connected to LCDs `D5` pin
	- `2`: IO connected to LCDs `D6` pin
	- `3`: IO connected to LCDs `D7` pin
  - for 8 bit `GPIO` interface table elements are:
	- `en`: IO connected to LCDs `EN` pin
	- `rs`: IO connected to LCDs `RS` pin
	- `bl`: IO controlling LCDs backlight. Optional
	- `0`: IO connected to LCDs `D0` pin
	- `1`: IO connected to LCDs `D1` pin
	- `2`: IO connected to LCDs `D2` pin
	- `3`: IO connected to LCDs `D3` pin
	- `4`: IO connected to LCDs `D4` pin
	- `5`: IO connected to LCDs `D5` pin
	- `6`: IO connected to LCDs `D6` pin
	- `7`: IO connected to LCDs `D7` pin
  - for `I2C` interface table elements are:
	- `address`: 7 bit address of `PCF8574` chip
	- `id`: `i2c` bus id
	- `speed`: `i2c` communication speed
	- `sda`: IO connected to `PCF8574` `SDA` pin
	- `scl`: IO connected to `PCF8574` `SCL` pin
- `fourbitmode`: `true` to use 4 bit mode, `false` to use 8 bit mode
- `oneline`: `true` to use one line mode, `false` to use two line mode
- `eightdots`: `true` to use 5x8 dot font, `false` to use 5x10 dot font
- `busyloop`: `true` to insert `tmr.delay` for time consuming operations

!!! note
	LCDs `RW` pin must be pulled to the ground.

#### Returns
`nil`

#### Example (4 bit GPIO)
```lua
liquidcrystal:init("GPIO", {rs=0,en=1,[0]=2,[1]=3,[2]=4,[3]=5},true, true, false, true)
```

#### Example (8 bit GPIO)
```lua
liquidcrystal:init("GPIO", {rs=0,en=1,[4]=2,[5]=3,[6]=4,[7]=5,[0]=6,[1]=7,[2]=8,[3]=12},false, false, true, true)
```

#### Example (I2C)
```lua
liquidcrystal:init("I2C", {address=0x27,id=0,sda=1,scl=2,speed=i2c.SLOW},true, false, true, true)
```

## liquidcrystal.leftToRight
Print text left to right (default).

#### Syntax
`liquidcrystal.leftToRight(self)`

#### Parameters
- `self`: `liquidcrystal` instance.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:leftToRight()
```

## liquidcrystal.rightToLeft
Print text right to left.

#### Syntax
`liquidcrystal.rightToLeft(self)`

#### Parameters
- `self`: `liquidcrystal` instance.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:rightToLeft()
```

## liquidcrystal.scrollLeft
Move text to the left.

#### Syntax
`liquidcrystal.scrollLeft(self)`

#### Parameters
- `self`: `liquidcrystal` instance.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:scrollLeft()
```

## liquidcrystal.scrollRight
Move text to the right.

#### Syntax
`liquidcrystal.scrollRight(self)`

#### Parameters
- `self`: `liquidcrystal` instance.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:scrollRight()
```

## liquidcrystal.write
Print text.

#### Syntax
`liquidcrystal.write(self, str)`

#### Parameters
- `self`: `liquidcrystal` instance.
- `str`: string to print.

#### Returns
`nil`

#### Example
```lua
liquidcrystal:write("hello world")
```

