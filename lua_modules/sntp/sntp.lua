local MAX_SERVER_ATTEMPTS = 2
local SNTP_TIMEOUT        = 5000

  -- sk and fk are our Success and Failure Kontinuations, resp.
return {
new = function(serv, sk, fk, now)

  if     type(serv) == "string" then serv = {serv}
  elseif serv == nil            then serv =
    {
      "0.nodemcu.pool.ntp.org",
      "1.nodemcu.pool.ntp.org",
      "2.nodemcu.pool.ntp.org",
      "3.nodemcu.pool.ntp.org",
    }
  elseif type(serv) ~= "table"  then error "Bad server table"
  end

  if type(sk) ~= "function" then
    error "Bad success continuation type"
  end
  if fk ~= nil and type(fk) ~= "function" then
    error "Bad failure continuation type"
  end
  if now ~= nil and type(now) ~= "function" then
    error "Bad clock type"
  end
  now = now or (rtctime and rtctime.get)
  if now == nil then error "Need clock function" end

  local _tmr -- contains the currently running timer, if any
  local _udp -- the socket we're using to talk to the world

  local _kod = {} -- kiss of death flags accumulated accoss syncs
  local _pbest -- best server from prior pass

  local _six -- index of the server in serv to whom we are speaking
  local _sat -- number of times we've tried to reach this server
  local _res -- the best result we've got so far
  local _best -- best server this pass, for updating _pbest

  -- Shut down the state machine
  --
  -- upvals: _tmr, _udp, _six, _sat, _res, _best
  local function stop()
    print("sntp", "stop")

    local res, best = _res, _best

    _six = nil
    _sat = nil
    _res = nil
    _best = nil

    -- stop any time-based callbacks and drop tmr
    if _tmr then
      _tmr:unregister()
      _tmr = nil
    end

    -- stop any UDP callbacks and drop the socket
    if _udp then
      _udp:on("receive", nil)
      _udp:on("sent"   , nil)
      _udp:on("dns"    , nil)
      _udp = nil
    end

    return res and sntppkt.read_resp(res), best
  end

  local nextServer
  local doserver

  -- Try communicating with the current server
  --
  -- upvals: SNTP_TIMEOUT, now, _tmr, _udp, _best, _kod, _pbest, _res, _six
  local function hail(ip)
    print("sntp", "hail", ip)

    _tmr:alarm(SNTP_TIMEOUT, tmr.ALARM_SINGLE, function()
      print("sntp", "hail-tmr")
      _udp:on("sent", nil)
      _udp:on("receive", nil)
      return doserver("timeout")
    end)

    -- XXX merely for diagnostics
    _udp:on("sent", function()
      print("sntp", "udp-on-sent")
      _udp:on("sent", nil)
    end)

    local txts = sntppkt.make_ts(now())

    _udp:on("receive",
     -- upvals: now, ip, _tmr, _best, _kod, _pbest, _res, _six
     function(skt, d, port, rxip)
      print("sntp", "udp-on-recv", rxip, port)

      -- many things constitute bad packets; drop with tmr running
      if rxip ~= ip and rxip ~= "224.0.1.1" then return end -- wrong peer
      if port ~= 123 then return end                        -- wrong port
      if #d   <   48 then return end                        -- too short

      local pkt = sntppkt.proc_pkt(d, txts, now())

      if pkt == nil then
        -- sntppkt can also reject the packet for having a bad cookie;
        -- this is important to prevent processing spurious or delayed responses
        return
      elseif type(pkt) == "string" then
        print("sntp", "udp-on-recv", "goaway", pkt)
        if pkt == "DENY" then -- KoD packet
          if _kod[_six] then
            if fk then fk("kod", serv[_six]) end
            table.remove(_kod, _six)
            table.remove(serv, _six)
            _six = _six - 1 -- nextServer will add one
          else
            if fk then fk("goaway", serv[_six], pkt) end
          end
        else
          if fk then fk("goaway", serv[_six]) end
        end
        return nextServer()
      end

      _kod[_six] = nil

      if _pbest == serv[_six] then
        -- this was our favorite server last time; if we don't have a
        -- result or if we'd rather this one than the result we have...
        if not _res or not sntppkt.pick_resp(pkt, _res, true) then
          _res = pkt
          _best = _pbest
        end
      else
        -- this was not our favorite server; take this result if we have no
        -- other option or if it compares favorably to the one we have, which
        -- might be from our favorite from last pass.
        if not _res or sntppkt.pick_resp(_res, pkt, _pbest == _best) then
          _res = pkt
          _best = serv[_six]
        end
      end

      _tmr:unregister()
      skt:on("receive", nil) -- skt == _udp
      skt:on("sent", nil)
      return nextServer()
     end)

    return _udp:send(123, ip,
      -- '#' == 0x23: version 4, mode 3 (client), no LI
      "#\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
      .. txts)
  end

  -- upvals: _sat, _six, _udp
  function doserver(err)
    if _sat == MAX_SERVER_ATTEMPTS then
      if fk then fk(err, serv[_six]) end
      return nextServer()
    end
    _sat = _sat + 1

    return _udp:dns(serv[_six], function(skt, ip)

      -- XXX Because stop() always drops the socket, having nuked its DNS
      -- callback, we should never attempt to resurrect a stale scan if DNS
      -- results come back after stop()-ing the state machine.  If that turns
      -- out not to be true, then we can do something like this to check that
      -- the callback is being run in the correct context.
      --
      -- -- if skt ~= _udp then
      -- --   print("sntp", "udp-on-dns", "stale socket")
      -- --   return
      -- -- end

      print("sntp", "udp-on-dns", ip)
      skt:on("dns", nil) -- skt == _udp
      if ip == nil then return doserver("dns") else return hail(ip) end
    end)
  end

  -- Move on to the next server or finish a pass
  --
  -- upvals: fk, serv, sk, _best, _pbest, _res, _sat, _six
  function nextServer()
    if _six >= #serv then
     -- XXX Finished the entire pass; call success or failure as indicated
     if _res then
       _pbest = _best
       local res = _res
       local best = _best
       stop()
       return sk(sntppkt.read_resp(res), best)
     else
       stop()
       if fk then return fk("all", #serv) else return end
     end
    end

    print("sntp", "next", _six)

    _six = _six + 1
    _sat = 0
    return doserver()
  end

  -- Poke all the servers and invoke the user's callbacks
  --
  -- upvals: _tmr, _udp, _six
  local function sync()
    stop()
    _udp = net.createUDPSocket()
    _tmr = tmr.create()
    _udp:listen() -- on random port
    _six = 0
    nextServer()
  end

  return { sync = sync, stop = stop }
  
end,

-- A utility function which applies a result to the rtc
update_rtc = function(res)
  local off_s, off_us = 0, 0
  if res.offset_s then
    off_s = res.offset_s
  elseif res.offset_us then
    off_s, off_us = res.offset_us / 1000000, res.offset_us % 1000000
  end
  local now_s, now_us = rtctime.get()
  local new_s, new_us = now_s + off_s, now_us + off_us
  if new_us > 1000000 then
    new_s  = new_s  + 1
    new_us = new_us - 1000000
  end
  rtctime.set(new_s, new_us)
end

}
