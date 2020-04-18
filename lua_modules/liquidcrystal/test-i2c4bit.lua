-- Run LiquidCrystal through some basic tests
--
--[[   The simplest method of testing leaves everything in RAM and is just

  TCLLIBPATH=./tests/expectnmcu expect ./tests/tap-driver.expect \
    ./lua_tests/mispec.lua \
    ./lua_modules/liquidcrystal/liquidcrystal.lua \
    ./lua_modules/liquidcrystal/lc-i2c4bit.lua \
    ./lua_modules/liquidcrystal/test-i2c4bit.lua

Better to put mispec and LiquidCrystal in LFS, and then just run

  TCLLIBPATH=./tests/expectnmcu expect ./tests/tap-driver.expect \
    ./lua_modules/liquidcrystal/test-i2c4bit.lua

--]]


require "mispec"

local metalcd
local metaback
local backend
local lcd

describe("lc-i2c4bit", function(it)

  it:initialize(function()
    collectgarbage() print("HEAP init", node.heap())
  end)

  it:cleanup(function() end)

  it:should("import constructor", function()
    metalcd = require "liquidcrystal"
    collectgarbage() print("HEAP constructor imported ", node.heap())
    ok(metalcd, "constructor")
  end)

  it:should("import backend", function()
    metaback = require "lc-i2c4bit"
    collectgarbage() print("HEAP backend imported ", node.heap())
    ok(metaback, "backend")
  end)

  it:should("construct backend", function()
    backend = metaback({
     address = 0x27,
     id  = 0,
     speed = i2c.SLOW,
     sda = 4,
     scl = 5,
    })
    collectgarbage() print("HEAP backend built", node.heap())
  end)

  it:should("construct lcd", function()
    lcd = metalcd(backend, false, true, 20)
    collectgarbage() print("HEAP lcd built", node.heap())
  end)

  it:should("release backend", function()
    backend = nil
    collectgarbage() print("HEAP backend released", node.heap())
  end)

  it:should("custom character", function()
    lcd:customChar(0, { 0x1F, 0x15, 0x1B, 0x15, 0x1F, 0x10, 0x10, 0x0 })
    ok(0x1B == lcd:readCustom(0)[3], "read back custom character")
  end)

  it:should("draw", function()
    lcd:cursorMove(0)
    lcd:write("abc")
    lcd:cursorMove(10,1)
    lcd:write("de")
    lcd:cursorMove(10,2)
    lcd:write("fg")
    lcd:cursorMove(12,3)
    lcd:write("hi\000")
    lcd:cursorMove(18,4)
    lcd:write("jk")
  end)

  it:should("read back 'a'", function() lcd:home()           ok(0x61 == lcd:read()) end)
  it:should("read back 'e'", function() lcd:cursorMove(11,1) ok(0x65 == lcd:read()) end)
  it:should("read back 'g'", function() lcd:cursorMove(11,2) ok(0x67 == lcd:read()) end)
  it:should("read back 'i'", function() lcd:cursorMove(13,3) ok(0x69 == lcd:read()) end)
  it:should("read back  0" , function() lcd:cursorMove(14,3) ok(0x00 == lcd:read()) end)
  it:should("read back 'k'", function() lcd:cursorMove(19,4) ok(0x6B == lcd:read()) end)

  it:should("not be busy", function() tmr.delay(5) ok(lcd:busy() == false) end)

  it:should("update home", function()
    lcd:home() lcd:write("l")
    lcd:home() ok(0x6C == lcd:read())
  end)

  it:should("clear", function()
    lcd:clear()
    lcd:home() ok(0x20 == lcd:read())
  end)

end)

mispec.run()
