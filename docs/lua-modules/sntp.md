# SNTP Module
| Since  | Origin / Contributor  | Maintainer  | Source  |
| :----- | :-------------------- | :---------- | :------ |
| 2019-07-01 | [nwf](https://github.com/nwf) | [nwf](https://github.com/nwf) | [sntp.lua](../../lua_modules/sntp/sntp.lua) |

This is a user-friendly, Lua wrapper around the `sntppkt` module to facilitate
the use of SNTP.

## Constructor
```lua
sntp = (require "sntp").new(servers, success_cb, [failure_cb], [clock])
```

where

* `servers` specifies the name(s) of the (S)NTP server(s) to use; it may be...

  * a string, either a DNS name or an IPv4 address in dotted quad form,
  * an array of the above
  * `nil` to use some default `*.nodemcu.pool.ntp.org` servers.

* `success_cb` is called back at the end of a synchronization when at least one
  server replied to us.  It will be given two arguments: the preferred SNTP
  result and the name of the server whence that result came.

* `failure_cb` may be `nil` but, otherwise, is called back in two circumstances:

  * at the end of a pass during which no server could be reached.  In this case,
    the first argument will be the string "all" and the second will be the
    number of servers tried.

  * an individual server has failed in some way.  In this case, the first
    argument will be one of:

      * "dns" (if name resolution failed),
      * "timeout" (if the server failed to reply in time),
      * "goaway" (if the server refused to answer), or
      * "kod" ("kiss of death", if the server told us to stop contacting it entirely).

    In all cases, the name of the server is the second argument; in the
    "goaway" case, the third argument will contain the refusal string (e.g.,
    "RATE" for rate-limiting or "DENY" for kiss-of-death warnings

* `clock`, if given, should return two values describing the local clock in
  seconds and microseconds (between 0 and 1000000).  If not given, the module
  will fall back on `rtctime.get`; if `rtctime` is not available, a clock must
  be provided.

## SNTP object methods

### sntp.sync()
#### Syntax
`sntp:sync()`

Run a pass through the specified servers and call back as described above.

### sntp.stop()
#### Syntax
`sntp:stop()`

Abort any pass in progress; no more continuations will be called.  The current
preferred response and server name (i.e., the arguments to the success
callback, should the pass end now) are returned.

## Other module functions

The module contains some other utility functions beyond the SNTP object
constructor.

### update_rtc()
#### Syntax
`update_rtc(res)`

Given a result from a SNTP `sync` pass, update the local RTC through `rtctime`.
Attempting to use this function without `rtctime` support will raise an error.

## Example usage

```lua
sntpm = require "sntp"
sntp = sntpm.new(nil,
  function(res, serv)
    print("SNTP OK", serv)
    sntpm.update_rtc(res)
  end,
  function(err, srv, rply)
    if   err == "all"    then print("SNTP FAIL", #srv)
    elif err == "goaway" then print("SNTP server rejected us", srv, rply)
    else                      print("SNTP server unreachable", srv, err)
    end
  end)

-- Every five minutes, re-run SNTP
sntptmr = tmr.create()
sntptmr:alarm(3000000, tmr.ALARM_AUTO, sntp.sync)
```
