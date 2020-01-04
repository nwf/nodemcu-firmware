###########################
NodeMCU Testing Environment
###########################

Herein we define the environment our testing framework expects to see when it
runs.  It is composed of two ESP8266 devices, each capable of holding an entire
NodeMCU firmware, LFS image, and SPIFFS file system, as well as additional
peripheral hardware.  It is designed to fit comfortably on a breadboard and so
should be easily replicated and integrated into any firmware validation
testing.

The test harness runs from a dedicated host computer, which is expected to have
reset- and programming-capable UART links to both ESP8266 devices, as found on
almost all ESP8266 boards with USB to UART adapters, but the host does not
necessarily need to use USB to connect, so long as TXD, RXD, DTR, and RTS are
wired across.

Peripherals
###########

I2C Bus
=======

There is an I2C bus hanging off DUT 0.  Attached hardware is used both as tests
of modules directly and also to facilitate testing other modules (e.g., gpio).

MCP23017: I/O Expander
----------------------

At address 0x20.  An 16-bit tristate GPIO expander, this chip is used to test
I2C, GPIO, and ADC functionality.  This chip's interconnections are as follows:

+---------+-------------------------------------------------------------------+
| /RESET  | DUT0 reset.  This resets the chip whenever the host computer      |
|         | resets DUT 0 over its serial link (using DTR/RTS).                |
+---------+-------------------------------------------------------------------+
| B 0     | 4K7 resistor to DUT 0 ADC.                                        |
+---------+-------------------------------------------------------------------+
| B 1     | 2K2 resistor to DUT 0 ADC.                                        |
+---------+-------------------------------------------------------------------+
| B 3     | Power for LCD backpack                                            |
+---------+-------------------------------------------------------------------+
| B 5     | DUT1 GPIO16/WAKE via 4K7 resitor                                  |
+---------+-------------------------------------------------------------------+
| B 6     | DUT0 GPIO13 via 4K7 resistor and DUT1 GPIO15 via 4K7 resistor     |
+---------+-------------------------------------------------------------------+
| B 7     | DUT0 GPIO15 via 4K7 resistor and DUT1 GPIO13 via 4K7 resistor     |
+---------+-------------------------------------------------------------------+

Notes:

* DUT 0's ADC pin is connected via a 2K2 reistor to this chip's port B, pin 1
  and via a 4K7 resistor to port B, pin 0.  This gives us the ability to
  produce approximately 0 (both pins low), 1.1 (pin 0 high, pin 1 low), 2.2
  (pin 1 high, pin 0 low), and 3.3V (both pins high) on the ADC pin.

* Port B pins 6 and 7 sit on the UART cross-wiring between DUT 0 and DUT 1.
  The 23017 will be tristated for inter-DUT UART tests, but these 

* Port B pins 2 and 4, as well as all of port A, remain available for
  expansion.

* The interrupt pins are not yet routed, but could be.  We reserve DUT 0
  GPIO 2 for this purpose with the understanding that the 23017's
  interrupt functionality will be disabled (INTA, INTB set to open-drain,
  GPINTEN set to 0) when not explicitly under test.

PCF8574-based LCD backpack
--------------------------

At bus address 0x27, if present.  This is used to test the ``liquidcrystal``
Lua module (and, of course, further exercises the I2C bus).

1-Wire
======

DS1820: Temperature Sensor
--------------------------

The test environment contains two of this chip (or DS18S20 or DS18B20, which
are roughly equivalent for our purposes), used to test 1-wire functionality
(``ow`` C module, including device discovery) and the DS18B20 Lua module.  The
1-wire bus is on DUT 0, and these devices are also connected directly to power.

.. todo::

   It would make sense to augment the 1-Wire testing facility to include
   bus-drive power, perhaps via the MCP23017, especially if we ever augment
   the driver as per https://github.com/nodemcu/nodemcu-firmware/issues/1995

ESP8266 Device 0 Connections
############################

+---------+---------------------------------------------------------+
| ESP     |                                                         |
+---------+---------------------------------------------------------+
| GPIO 0  | Used to enter programming mode; otherwise unused in     |
|         | test environment.                                       |
+---------+---------------------------------------------------------+
| GPIO 1  | Primary UART transmit; reserved for host communication  |
+---------+---------------------------------------------------------+
| GPIO 2  | 1-Wire                                                  |
|         | [+ reserved for 23017 INT[AB] connections]              |
+---------+---------------------------------------------------------+
| GPIO 3  | Primary UART recieve; reserved for host communication   |
+---------+---------------------------------------------------------+
| GPIO 4  | I2C SDA                                                 |
+---------+---------------------------------------------------------+
| GPIO 5  | I2C SCL                                                 |
+---------+---------------------------------------------------------+
| GPIO 6  | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 7  | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 8  | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 9  | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 10 | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 11 | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 12 | HSPI MISO                                               |
+---------+---------------------------------------------------------+
| GPIO 13 | Secondary UART RX; device 2 GPIO 15, I/O expander B 6.  |
|         | Also used as HSPI MOSI for SPI tests                    |
+---------+---------------------------------------------------------+
| GPIO 14 | HSPI CLK                                                |
+---------+---------------------------------------------------------+
| GPIO 15 | Secondary UART TX; device 2 GPIO 13, I/O expander B 7   |
|         | Also used as HSPI /CS for SPI tests                     |
+---------+---------------------------------------------------------+
| GPIO 16 |                                                         |
+---------+---------------------------------------------------------+
| ADC 0   | Resistor divider with MCP23017                          |
+---------+---------------------------------------------------------+

ESP8266 Device 2
################

+---------+---------------------------------------------------------+
| ESP     |                                                         |
+---------+---------------------------------------------------------+
| GPIO 0  | Used to enter programming mode; otherwise unused in     |
|         | test environment.                                       |
+---------+---------------------------------------------------------+
| GPIO 1  | Primary UART transmit; reserved for host communication  |
+---------+---------------------------------------------------------+
| GPIO 2  | [Reserved for WS2812]                                   |
+---------+---------------------------------------------------------+
| GPIO 3  | Primary UART recieve; reserved for host communication   |
+---------+---------------------------------------------------------+
| GPIO 4  |                                                         |
+---------+---------------------------------------------------------+
| GPIO 5  |                                                         |
+---------+---------------------------------------------------------+
| GPIO 6  | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 7  | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 8  | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 9  | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 10 | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 11 | [Reserved for on-chip flash]                            |
+---------+---------------------------------------------------------+
| GPIO 12 | HSPI MISO                                               |
+---------+---------------------------------------------------------+
| GPIO 13 | Secondary UART RX; DUT 0 GPIO 15, I/O exp B 7 via 4K7   |
|         | Also used as HSPI MOSI for SPI tests                    |
+---------+---------------------------------------------------------+
| GPIO 14 | HSPI CLK                                                |
+---------+---------------------------------------------------------+
| GPIO 15 | Secondary UART TX; DUT 0 GPIO 13, I/O exp B 6 via 4K7   |
|         | Also used as HSPI /CS for SPI tests                     |
+---------+---------------------------------------------------------+
| GPIO 16 | I/O expander B 5 via 4K7 resistor, for deep-sleep tests |
+---------+---------------------------------------------------------+
| ADC 0   |                                                         |
+---------+---------------------------------------------------------+

Test Plan
#########

These modules are purely software or use strictly on-device hardware and can be
tested by pushing lua to the DUT.  These tests may be found in ``lua_tests``.

+-------------+---------------------------------------------------------------+
| bit         |                                                               |
+-------------+---------------------------------------------------------------+
| bloom       |                                                               |
+-------------+---------------------------------------------------------------+
| color_utils |                                                               |
+-------------+---------------------------------------------------------------+
| crypto      |                                                               |
+-------------+---------------------------------------------------------------+
| encoder     |                                                               |
+-------------+---------------------------------------------------------------+
| file        | ``mispec_file.lua``                                           |
+-------------+---------------------------------------------------------------+
| pipe        |                                                               |
+-------------+---------------------------------------------------------------+
| pixbuf      | ``mispec_pixbuf_*.lua``                                       |
+-------------+---------------------------------------------------------------+
| rtcfifo     |                                                               |
+-------------+---------------------------------------------------------------+
| rtcmem      |                                                               |
+-------------+---------------------------------------------------------------+
| sjson       |                                                               |
+-------------+---------------------------------------------------------------+
| struct      |                                                               |
+-------------+---------------------------------------------------------------+

These time-related modules will be tested using on-device Lua but will use a
host-side ``expect`` script to ensure that clocks are ticking:

+----------+------------------------------------------------------------------+
| cron     |                                                                  |
+----------+------------------------------------------------------------------+
| rtctime  |                                                                  |
+----------+------------------------------------------------------------------+
| tmr      |                                                                  |
+----------+------------------------------------------------------------------+

These modules require connectivity to off-device network endpoints and so are
managed by dedicated ``expect`` scripts:

+----------+------------------------------------------------------------------+
|   http   |                                                                  |
+----------+------------------------------------------------------------------+
|   mdns   |                                                                  |
+----------+------------------------------------------------------------------+
|   mqtt   |                                                                  |
+----------+------------------------------------------------------------------+
|   net    | ``test-net-host.expect``, ``test-net-intermodule.expect``        |
+----------+------------------------------------------------------------------+
|   sntp   |                                                                  |
+----------+------------------------------------------------------------------+
|   tls    |                                                                  |
+----------+------------------------------------------------------------------+
| websocket|                                                                  |
+----------+------------------------------------------------------------------+
|   wifi   |                                                                  |
+----------+------------------------------------------------------------------+

Some modules require hardware support, detailed below:

+------------+----------------------------------------------------------------+
|    adc     | Resistor ladder off I2C I/O expander;                          |
|            | tested on-device in ``lua_tests/adc.lua``                      |
+------------+----------------------------------------------------------------+
|   gpio     | Cross-wired GPIO; I2C I/O expander                             |
|            | tested on-device in ``lua_tests/gpio.lua``                     |
+------------+----------------------------------------------------------------+
|    i2c     | I2C I/O expander.                                              |
|            | Note that this is implicitly tested by several other tests!    |
+------------+----------------------------------------------------------------+
|    ow      | DS1820 sensors;                                                |
|            | tested on-device in ``lua_tests/ow.lua``                       |
+------------+----------------------------------------------------------------+
|   uart     | Cross-wired UARTs                                              |
|            | Requires cross-module synchronization; ``test-uart.expect``    |
+------------+----------------------------------------------------------------+

These modules seem to require hardware and do not yet have a test plan (note
that some hardware, such as addressable LEDs, may not be possible to test in an
easily automated way):

* ads1115
* adxl345
* am2320
* apa102
* bme280
* bme680
* bmp085
* dht
* hdc1080
* hmc5883l
* hx711
* l3g4200d
* mcp4725
* rfswitch
* rotary
* si7021
* tcs34725
* tm1829
* tsl2561
* u8g2
* ucg
* ws2801
* ws2812
* xpt2046

No test plan yet exists for these modules:

* coap
* dcc
* enduser_setup
* gdbstub
* gpio_pulse
* sigma_delta
* node
* pcm
* perf
* pwm
* pwm2
* softuart
* somfy
* spi
* switec
* wps

ESP8266 Device 1
################
>>>>>>> 1019f965... Test environment docs

