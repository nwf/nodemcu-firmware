/*
 * Copyright 2015 Dius Computing Pty Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the
 *   distribution.
 * - Neither the name of the copyright holders nor the names of
 *   its contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @author Johny Mattsson <jmattsson@dius.com.au>
 * @author Nathaniel Wesley Filardo <nwfilardo@gmail.com>
 */

// Module for Simple Network Time Protocol (SNTP)

/*
 * State machine overview:
 *
 *  - Everything starts with the user invoking sntp_sync(), which specifies
 *     - the list of servers we're connecting to (or we use a default set)
 *     - an optional success callback
 *     - an optional error callback
 *     - whether or not we are repeating
 *    These values are stored into the global sntp state: the list of servers
 *    is a Lua array (ref'd via registry), the callbacks are in Lua (ref'd
 *    via the registry), and the repetition state is just a flag.  Each call to
 *    sntp_sync() will completely reset the state machine.
 *
 *  - At any moment, exactly one of the following things is happening:
 *
 *    - We are inside sntp_sync.
 *
 *    - We are idle; our callbacks are nil and no timers are running.
 *
 *    - We are awaiting the OS's task post mechanism to fire off a posted
 *      callback; this might be the repetition timer's firing with state as
 *      of the last call to sntp_sync().
 *
 *    - We are engaged in DNS resolution.  No timer is running because LwIP
 *      will call us back upon success or failure.  Because control flow is
 *      out of our hand, we have to guard against mis-understanding a possibly
 *      changed world.
 *
 *    - We are communicating with a SNTP server.  A timer is running in case
 *      the server doesn't get back to us.
 *
 *    - We have gotten an SNTP response and are processing, or we have timed
 *      out on this server (either from DNS or SNTP).  If this completes
 *      our traversal of the list of servers, we will post an OS task to call
 *      back to Lua.  If we are repeating, we will then engage the timer.
 *      If the RTC module is available, we will set the time.
 *
 *  - Within a pass through the list of servers, we will try each server up to
 *    MAX_ATTEMPTS times before deciding on its fate.  Each attempt will do a
 *    full DNS resolution and UDP contact; most likely, we will hit in the DNS
 *    cache, but this seems like it will keep the state machine simpler.
 *
 */

#include "module.h"
#include "lauxlib.h"
#include "lmem.h"
#include "os_type.h"
#include "osapi.h"
#include "lwip/udp.h"
#include "c_stdlib.h"
#include "user_modules.h"
#include "lwip/dns.h"
#include "task/task.h"
#include "user_interface.h"

#ifdef LUA_USE_MODULES_RTCTIME
#include "rtc/rtctime.h"
#endif

#include "pm/swtimer.h"

#define max(a,b) ((a < b) ? b : a)

#define NTP_PORT 123
#define NTP_ANYCAST_ADDR(dst)  IP4_ADDR(dst, 224, 0, 1, 1)

#define MAX_ATTEMPTS 5

#if 0
# define sntp_dbg(...) dbg_printf(__VA_ARGS__)
#else
# define sntp_dbg(...)
#endif

#define US_TO_FRAC(us)          (div1m(((uint64_t) (us)) << 32))
#define SUS_TO_FRAC(us)         ((((int64_t) (us)) << 32) / 1000000)
#define FRAC16_TO_US(frac)      ((((uint64_t) (frac)) * 1000000) >> 16)

typedef enum {
  NTP_NO_ERR = 0,
  /* These error codes are part of the defined API */
  NTP_DNS_ERR = 1,
  NTP_MEM_ERR = 2,
  NTP_SEND_ERR = 3,
  NTP_TIMEOUT_ERR = 4,
} ntp_err_t;

typedef struct
{
  uint32_t sec;
  uint32_t frac;
} ntp_timestamp_t;

typedef struct
{
  uint8_t mode : 3;
  uint8_t ver : 3;
  uint8_t LI : 2;
  uint8_t stratum;
  uint8_t poll;
  uint8_t precision;
  uint32_t root_delay;
  uint32_t root_dispersion;
  uint32_t refid;
  ntp_timestamp_t ref;
  ntp_timestamp_t origin;
  ntp_timestamp_t recv;
  ntp_timestamp_t xmit;
} ntp_frame_t;

#define SNTP_FLAG_USING_OFFSET  0x01
#define SNTP_FLAG_PENDING_LI    0x02

/*
 * XXX Ideally, we should package all the state up into a struct and change the
 * Lua API to be sntp:sync() rather than sntp.sync().  For the moment, things are
 * not in structures so that the compiler can re-shuffle the globals for better
 * packing.
 *
 * State lifecycle descriptors:
 *   (o) allocated once on module Open, held forever
 *   (p) allocated in Preflight
 *   (q) reset in Preflight
 *   (s) held by sntp_sync call
 *   (r) Reset by sntp_sync
 *   (t) set per packeT
 */

/* Global state machine parameters */

static os_timer_t sntp_timer;      // (o,q)
static task_handle_t sntp_task;    // (o) 

static int sntp_server_list_ref;   // (s) Lua table of servers; at most 32
static bool sntp_repeat;           // (s)
static int sntp_sync_cb_ref;       // (s)
static int sntp_err_cb_ref;        // (s)

static struct udp_pcb *sntp_pcb;   // (p) LWiP packet control
static struct pbuf *sntp_pbuf;     // (p) LWiP packet buffer

/* SNTP state */

static uint32_t sntp_kodbits;      // (r) per-server "STFU" flags
static uint8_t sntp_flags;         // SNTP_FLAG_*
static uint8_t sntp_offset;        // only if SNTP_FLAG_USING_OFFSET

static ntp_timestamp_t sntp_cookie; // (t)

/* Loop state */

static uint8_t sntp_servers;       // (q) maximum index into server_list_ref
static uint8_t sntp_server_index;  // (q) index into server table being tried
static uint8_t sntp_attempts;      // (q) remaining before next server

static int sntp_dns_ref;           // (q) Lua registry index of string being
                                   // resolved, if any.  Used to suppress stale
                                   // actions, as we cannot cancel LwIP's DNS
                                   // resolution.

static struct {
    int64_t   delta;
    ip_addr_t server;
    uint32_t  delay;
    uint32_t  delay_frac;
    uint32_t  root_maxerr;
    uint32_t  root_delay;
    uint32_t  root_dispersion;
    int       when;
    uint16_t  server_index;
    uint8_t   LI;
    uint8_t   stratum;    /* Doubles as presence flag (== 0 for no best) */
} sntp_best;              // (q) Best result observed so far this pass

static uint8_t sntp_last_server_index;  // sntp_best.server_pos from last time

static int32_t sntp_next_midnight;
static uint64_t sntp_pll_increment;


static void sntp_do_repeat (void *);
static void sntp_server_resolve (lua_State *);

#define PLL_A   (1 << (32 - 11))
#define PLL_B   (1 << (32 - 11 - 2))

static uint64_t
sntp_div1m(uint64_t n) {
  uint64_t q1 = (n >> 5) + (n >> 10);
  uint64_t q2 = (n >> 12) + (q1 >> 1);
  uint64_t q3 = (q2 >> 11) - (q2 >> 23);

  uint64_t q = n + q1 + q2 - q3;

  q = q >> 20;

  // Ignore the error term -- it is measured in pico seconds
  return q;
}

#ifdef LUA_USE_MODULES_RTCTIME
static void
sntp_get_zero_base_timeofday(struct rtc_timeval *tv) {
  uint32_t now = system_get_time();

  tv->tv_sec = now / 1000000;
  tv->tv_usec = now % 1000000;
}
#endif

/* Called at the end of every pass */
static void
sntp_pass_reset(void)
{
  sntp_dbg("sntp_pass_reset\n");

  os_timer_disarm (&sntp_timer);

  sntp_server_index = 1;
  sntp_attempts = MAX_ATTEMPTS;
  sntp_best.stratum = 0;
  sntp_cookie.sec = 0;
  sntp_cookie.frac = 0;
}

/*
 * Called at the end of a non-repeating synchronization and at the start of
 * sntp.sync to sweep away the old state machine's state.
 */
static void
sntp_cleanup(lua_State *L)
{
  sntp_dbg("sntp_cleanup\n");

  luaL_unref (L, LUA_REGISTRYINDEX, sntp_sync_cb_ref);
  sntp_sync_cb_ref = LUA_NOREF;

  luaL_unref (L, LUA_REGISTRYINDEX, sntp_err_cb_ref);
  sntp_err_cb_ref = LUA_NOREF;

  luaL_unref (L, LUA_REGISTRYINDEX, sntp_dns_ref);
  sntp_dns_ref = LUA_NOREF;

  sntp_last_server_index = 0;
  sntp_servers = 0;
}

/*
 * Called per-server as well as at the end of the loop
 *
 * srvix is the identity of the server whose error is being reported, or 0 for
 * none, in which case nil will be given to the Lua layer.
 */
static void
sntp_handle_error (lua_State *L, ntp_err_t err, int srvix)
{
  sntp_dbg("sntp_handle_error\n");

  if (srvix != 0)
    lua_rawgeti (L, LUA_REGISTRYINDEX, sntp_server_list_ref);

  lua_rawgeti (L, LUA_REGISTRYINDEX, sntp_err_cb_ref);
  if (!lua_isnoneornil (L, -1)) {
    sntp_dbg("sntp: handle_error no callback\n");
    lua_pop (L,1);
  } else {
    sntp_dbg("sntp: handle_error callback dispatch\n");
    lua_pushinteger (L, err);
    if (srvix != 0)
      lua_rawgeti (L, -3, srvix);
    else
      lua_pushnil (L);

    /*
     * XXX If memory is really dire, this might panic the system.
     * Maybe that's what we want?  Should this be a pcall instead?
     */
    lua_call (L, 2, 0);
  }

  if (srvix != 0)
    lua_pop (L,1);
}

/*
 * Called only at the end of the loop when we have at least one server
 * reporting success
 */
static void
sntp_handle_success (lua_State *L)
{
  const uint32_t MICROSECONDS = 1000000;

  sntp_dbg("sntp_handle_success\n");

  // Remember for next time
  sntp_last_server_index = sntp_best.server_index;

  // if we have rtctime, do higher resolution delta calc, else just use
  // the transmit timestamp
#ifdef LUA_USE_MODULES_RTCTIME
  struct rtc_timeval tv;
  rtctime_gettimeofday (&tv);
  if (tv.tv_sec == 0) {
    get_zero_base_timeofday(&tv);
  }
  tv.tv_sec += (int)(sntp_best.delta >> 32);
  tv.tv_usec += (int) ((MICROSECONDS * (sntp_best.delta & 0xffffffff)) >> 32);
  while (tv.tv_usec >= 1000000) {
    tv.tv_usec -= 1000000;
    tv.tv_sec++;
  }

  /* XXX It might make sense to allow Lua to turn this behavior off? */
  if (sntp_best.delta > SUS_TO_FRAC(-200000) && sntp_best.delta < SUS_TO_FRAC(200000)) {
    // Adjust rate
    // f is frequency -- f should be 1 << 32 for nominal
    sntp_dbg("delta=%d, increment=%d, ", (int32_t) sntp_best.delta, (int32_t) pll_increment);
    int64_t f = ((sntp_best.delta * PLL_A) >> 32) + pll_increment;
    pll_increment += (sntp_best.delta * PLL_B) >> 32;
    sntp_dbg("f=%d, increment=%d\n", (int32_t) f, (int32_t) pll_increment);
    rtctime_adjust_rate((int32_t) f);
  } else {
    rtctime_settimeofday (&tv);
  }
#endif

  /*
   * XXX If memory is really dire, this might panic the system.
   * Is that avoidable?
   */
  if (sntp_sync_cb_ref != LUA_NOREF)
  {
    lua_rawgeti(L, LUA_REGISTRYINDEX, sntp_sync_cb_ref);
#ifdef LUA_USE_MODULES_RTCTIME
    lua_pushnumber(L, tv.tv_sec);
    lua_pushnumber(L, tv.tv_usec);
    lua_pushstring(L, ipaddr_ntoa (&sntp_best.server));
    lua_newtable(L);
    int d40 = sntp_best.delta >> 40;
    if (d40 != 0 && d40 != -1) {
      lua_pushnumber(L, sntp_best.delta >> 32);
      lua_setfield(L, -2, "offset_s");
    } else {
      lua_pushnumber(L, (sntp_best.delta * MICROSECONDS) >> 32);
      lua_setfield(L, -2, "offset_us");
    }
#else
    int adjust_us = system_get_time() - sntp_best.when;
    int tv_sec = sntp_best.delta >> 32;
    int tv_usec = (int) (((sntp_best.delta & 0xffffffff) * MICROSECONDS) >> 32) + adjust_us;
    while (tv_usec >= 1000000) {
      tv_usec -= 1000000;
      tv_sec++;
    }
    lua_pushnumber(L, tv_sec);
    lua_pushnumber(L, tv_usec);
    lua_pushstring(L, ipaddr_ntoa (&sntp_best.server));
    lua_newtable(L);
#endif
    if (sntp_best.delay_frac > 0) {
      lua_pushnumber(L, FRAC16_TO_US(sntp_best.delay_frac));
      lua_setfield(L, -2, "delay_us");
    }
    lua_pushnumber(L, FRAC16_TO_US(sntp_best.root_delay));
    lua_setfield(L, -2, "root_delay_us");
    lua_pushnumber(L, FRAC16_TO_US(sntp_best.root_dispersion));
    lua_setfield(L, -2, "root_dispersion_us");
    lua_pushnumber(L, FRAC16_TO_US(sntp_best.root_maxerr + sntp_best.delay_frac / 2));
    lua_setfield(L, -2, "root_maxerr_us");
    lua_pushnumber(L, sntp_best.stratum);
    lua_setfield(L, -2, "stratum");
    lua_pushnumber(L, sntp_best.LI);
    lua_setfield(L, -2, "leap");
    lua_pushnumber(L, !!(sntp_flags & SNTP_FLAG_PENDING_LI));
    lua_setfield(L, -2, "pending_leap");

    lua_call (L, 4, 0);
  }
}

/* At the end of a synchronization attempt; TASK context! */
static void
sntp_pass_fini(lua_State *L)
{
  sntp_dbg("sntp_pass_fini\n");

  if (sntp_best.stratum == 0) {
    // This could be because none of the servers are reachable, or maybe we haven't been able to look
    // them up.
    sntp_server_index = 0; // Reset for next time.
    sntp_handle_error(L, NTP_TIMEOUT_ERR, 0);
  } else {
    sntp_handle_success(L);
  }

  sntp_pass_reset();
  if (sntp_repeat) {
    os_timer_setfn(&sntp_timer, sntp_do_repeat, L);
    os_timer_arm(&sntp_timer, 1000 * 1000, 1);
  } else {
    sntp_cleanup(L);
  }
}

/* Advance the state machine within a pass; IRQ context! */
static void
sntp_server_next(lua_State *L)
{
  sntp_dbg("sntp_server_next\n");

  if (--sntp_attempts == 0) {
    if (sntp_dns_ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, sntp_dns_ref);
      sntp_dns_ref = LUA_NOREF;
    }

    if (sntp_servers < ++sntp_server_index) {
      task_post_high(sntp_task, 0);
    } else {
      sntp_attempts = MAX_ATTEMPTS;
      /* Look up the current server index in the table */
      lua_rawgeti (L, LUA_REGISTRYINDEX, sntp_server_list_ref);
      lua_rawgeti (L, -1, sntp_server_index);
      sntp_dns_ref = luaL_ref(L, LUA_REGISTRYINDEX);
      sntp_server_resolve(L);
    }
  } else {
    sntp_server_resolve(L);
  }
}

/* A timeout while awaiting a server's response; IRQ context! */
static void
sntp_server_timeout(void *arg)
{
  sntp_dbg("sntp_server_timeout\n");
  sntp_server_next(arg);
}

/* Data received; IRQ context! */
static void
sntp_server_recv (void *arg, struct udp_pcb *pcb, struct pbuf *p,
                  struct ip_addr *addr, uint16_t port)
{
  sntp_dbg("sntp_server_recv\n");

  sntp_server_next(arg);
}

/* Attempt to contact the current server in the pass; might be IRQ context! */
static void
sntp_dosend(ip_addr_t *ipaddr, lua_State *L)
{
  sntp_dbg("sntp_server_dosend\n");

}

/* DNS answered or timed out; IRQ context! */
static void
sntp_onresolve(const char *name, ip_addr_t *ipaddr, void *arg)
{
  sntp_dbg("sntp_server_onresolve\n");

  lua_State *L = arg;

  if (ipaddr == NULL) {
    sntp_server_next(L);
  } else {
    sntp_dosend(ipaddr, L);
  }
}

/* Attempt to resolve the current server; IRQ context! */
static void
sntp_server_resolve(lua_State *L)
{
  const char *hostname;
  ip_addr_t ipaddr;

  sntp_dbg("sntp_server_resolve\n");

  lua_rawgeti (L, LUA_REGISTRYINDEX, sntp_dns_ref);
  hostname = luaL_checkstring(L, 1);

  switch(dns_gethostbyname(hostname, sntp_onresolve, &ipaddr, L)) {
    case ERR_OK:
    case ERR_INPROGRESS:
      /*
       * OK, we had to go to the Internet to get the answer; control flow is
       * almost entirely out of our hands.  Annoyingly, this means that there's
       * a window where the user might call sntp.sync() while we have a callback
       * pending!
       *
       * XXX
       */
       break;
    case ERR_ARG:
       /* Something has gone wrong */
       /* XXX */
  }
  :lua_pop(L, 1);

  if (err == ERR_INPROGRESS) {
      lua_pop(L, 1);
      break;  // Callback function sntp_dns_found will handle sntp_dosend for us
    } else if (err == ERR_ARG) {
      handle_error(L, NTP_DNS_ERR, hostname);
      lua_pop(L, 1);
      break;
    }


}

#if 0
OLD STUFF
static void sntp_dosend ()
{
  do {
    if (state->server_pos < 0) {
      os_timer_disarm(&state->timer);
      os_timer_setfn(&state->timer, on_timeout, NULL);
      SWTIMER_REG_CB(on_timeout, SWTIMER_RESUME);
        //The function on_timeout calls this function(sntp_dosend) again to handle time sync timeout.
        //My guess: Since the WiFi connection is restored after waking from light sleep, it would be possible to contact the SNTP server, So why not let it
      state->server_pos = 0;
    } else {
      ++state->server_pos;
    }

    if (state->server_pos >= server_count) {
      state->server_pos = 0;
      ++state->attempts;
    }

    if (state->attempts >= MAX_ATTEMPTS || state->attempts * server_count >= 8) {
      task_post_high(tasknumber, SNTP_HANDLE_RESULT_ID);
      return;
    }
  } while (serverp[state->server_pos].addr == 0 || (state->kodbits & (1 << state->server_pos)));

  sntp_dbg("sntp: server %s (%d), attempt %d\n", ipaddr_ntoa(serverp + state->server_pos), state->server_pos, state->attempts);

  struct pbuf *p = pbuf_alloc (PBUF_TRANSPORT, sizeof (ntp_frame_t), PBUF_RAM);
  if (!p) {
    task_post_low(tasknumber, NTP_MEM_ERR);
    return;
  }

  ntp_frame_t req;
  os_memset (&req, 0, sizeof (req));
  req.ver = 4;
  req.mode = 3; // client
#ifdef LUA_USE_MODULES_RTCTIME
  const uint32_t NTP_TO_UNIX_EPOCH = 2208988800ul;
  struct rtc_timeval tv;
  rtctime_gettimeofday (&tv);
  if (tv.tv_sec == 0) {
    get_zero_base_timeofday(&tv);
  }
  req.xmit.sec = htonl (tv.tv_sec - the_offset + NTP_TO_UNIX_EPOCH);
  req.xmit.frac = htonl (US_TO_FRAC(tv.tv_usec));
#else
  req.xmit.frac = htonl (system_get_time ());
#endif
  state->cookie = req.xmit;

  os_memcpy (p->payload, &req, sizeof (req));
  int ret = udp_sendto (state->pcb, p, serverp + state->server_pos, NTP_PORT);
  sntp_dbg("sntp: send: %d\n", ret);
  pbuf_free (p);

  // Ignore send errors -- let the timeout handle it

  os_timer_arm (&state->timer, 1000, 0);
}

static void on_timeout (void *arg)
{
  (void)arg;
  sntp_dbg("sntp: timer\n");
  sntp_dosend ();
}

static int32_t get_next_midnight(int32_t now) {
  return now + 86400 - the_offset - (now - the_offset) % 86400;
}

static void update_offset()
{
  // This may insert or remove an offset second -- i.e. a leap second
  // This can only happen if it is at midnight UTC.
#ifdef LUA_USE_MODULES_RTCTIME
  struct rtc_timeval tv;

  if (pending_LI && using_offset) {
    rtctime_gettimeofday (&tv);
    sntp_dbg("Now=%d, next=%d\n", tv.tv_sec - the_offset, next_midnight);
    if (next_midnight < 100000) {
      next_midnight = get_next_midnight(tv.tv_sec);
    } else if (tv.tv_sec - the_offset >= next_midnight) {
      next_midnight = get_next_midnight(tv.tv_sec);
      // is this the first day of the month
      // Number of days since 1/mar/0000
      // 1970 * 365 is the number of days in full years
      // 1970 / 4 is the number of leap days (ignoring century rules)
      // 19 is the number of centuries
      // 4 is the number of 400 years (where there was a leap day)
      // 31 & 28 are the number of days in Jan 1970 and Feb 1970
      int day = (tv.tv_sec - the_offset) / 86400 + 1970 * 365 + 1970 / 4 - 19 + 4 - 31 - 28;

      int century = (4 * day + 3) / 146097;
      day = day - century * 146097 / 4;
      int year = (4 * day + 3) / 1461;
      day = day - year * 1461 / 4;
      int month = (5 * day + 2) / 153;
      day = day - (153 * month + 2) / 5;

      // Months 13 & 14 are really Jan and Feb in the following year.
      sntp_dbg("century=%d, year=%d, month=%d, day=%d\n", century, year, month + 3, day + 1);

      if (day == 0) {
        if (pending_LI == 1) {
          the_offset ++;
        } else {
          the_offset --;
        }
      }
      pending_LI = 0;
    }
  }
#endif
}

static void record_result(int server_pos, ip_addr_t *addr, int64_t delta, int stratum, int LI, uint32_t delay_frac, uint32_t root_maxerr, uint32_t root_dispersion, uint32_t root_delay) {
  sntp_dbg("Recording %s: delta=%08x.%08x, stratum=%d, li=%d, delay=%dus, root_maxerr=%dus",
      ipaddr_ntoa(addr), (uint32_t) (delta >> 32), (uint32_t) (delta & 0xffffffff), stratum, LI, (int32_t) FRAC16_TO_US(delay_frac), (int32_t) FRAC16_TO_US(root_maxerr));
  // I want to favor close by servers as they probably have a more consistent clock,
  int delay = root_delay * 2 + delay_frac;
  if (state->last_server_pos == server_pos) {
    delay -= delay >> 2;               // 25% bonus to last best server
  }

  if (!state->best.stratum || delay < state->best.delay) {
    sntp_dbg("   --BEST\n");
    state->best.server = *addr;
    state->best.server_pos = server_pos;
    state->best.delay = delay;
    state->best.delay_frac = delay_frac;
    state->best.root_maxerr = root_maxerr;
    state->best.root_dispersion = root_dispersion;
    state->best.root_delay = root_delay;
    state->best.delta = delta;
    state->best.stratum = stratum;
    state->best.LI = LI;
    state->best.when = system_get_time();
  } else {
    sntp_dbg("\n");
  }
}

static void on_recv (void *arg, struct udp_pcb *pcb, struct pbuf *p, struct ip_addr *addr, uint16_t port)
{
  (void)port;
#ifdef LUA_USE_MODULES_RTCTIME
  // Ideally this would be done when we receive the packet....
  struct rtc_timeval tv;

  rtctime_gettimeofday (&tv);
  if (tv.tv_sec == 0) {
    get_zero_base_timeofday(&tv);
  }
#endif
  sntp_dbg("sntp: on_recv\n");

  if (!state || state->pcb != pcb)
  {
    // "impossible", but don't leak if it did happen somehow...
    udp_remove (pcb);
    pbuf_free (p);
    return;
  }

  if (!p)
    return;

  if (p->len < sizeof (ntp_frame_t))
  {
    pbuf_free (p);
    return; // not an ntp frame, ignore
  }

  // make sure we have an aligned copy to work from
  ntp_frame_t ntp;
  os_memcpy (&ntp, p->payload, sizeof (ntp));
  pbuf_free (p);
  sntp_dbg("sntp: transmit timestamp: %u, %u\n", ntp.xmit.sec, ntp.xmit.frac);

  // sanity checks before we touch our clocks
  ip_addr_t anycast;
  NTP_ANYCAST_ADDR(&anycast);
  if (serverp[state->server_pos].addr != anycast.addr && serverp[state->server_pos].addr != addr->addr)
    return; // unknown sender, ignore

  if (ntp.origin.sec  != state->cookie.sec ||
      ntp.origin.frac != state->cookie.frac)
    return; // unsolicited message, ignore

  if (ntp.LI == 3) {
    if (memcmp(&ntp.refid, "DENY", 4) == 0) {
      // KoD packet
      if (state->kodbits & (1 << state->server_pos)) {
        // Oh dear -- two packets rxed. Kill this entry
        serverp[state->server_pos].addr = 0;
      } else {
        state->kodbits |= (1 << state->server_pos);
      }
    }
    return; // server clock not synchronized (why did it even respond?!)
  }

  // clear kod -- we got a good packet back
  state->kodbits &= ~(1 << state->server_pos);

  os_timer_disarm(&state->timer);

  if (ntp.LI) {
    pending_LI = ntp.LI;
  }

  update_offset();

  ntp.origin.sec  = ntohl (ntp.origin.sec);
  ntp.origin.frac = ntohl (ntp.origin.frac);
  ntp.recv.sec  = ntohl (ntp.recv.sec);
  ntp.recv.frac = ntohl (ntp.recv.frac);
  ntp.xmit.sec  = ntohl (ntp.xmit.sec);
  ntp.xmit.frac = ntohl (ntp.xmit.frac);

  const uint64_t MICROSECONDS = 1000000ull;
  const uint32_t NTP_TO_UNIX_EPOCH = 2208988800ul;

  uint32_t root_maxerr = ntohl(ntp.root_dispersion) + ntohl(ntp.root_delay) / 2;

  bool same_as_last = state->server_pos == state->last_server_pos;

  // if we have rtctime, do higher resolution delta calc, else just use
  // the transmit timestamp
#ifdef LUA_USE_MODULES_RTCTIME
  ntp_timestamp_t dest;
  dest.sec = tv.tv_sec + NTP_TO_UNIX_EPOCH - the_offset;
  dest.frac = US_TO_FRAC(tv.tv_usec);

  uint64_t ntp_recv = (((uint64_t) ntp.recv.sec) << 32) + (uint64_t) ntp.recv.frac;
  uint64_t ntp_origin = (((uint64_t) ntp.origin.sec) << 32) + (uint64_t) ntp.origin.frac;
  uint64_t ntp_xmit = (((uint64_t) ntp.xmit.sec) << 32) + (uint64_t) ntp.xmit.frac;
  uint64_t ntp_dest = (((uint64_t) dest.sec) << 32) + (uint64_t) dest.frac;

  // Compensation as per RFC2030
  int64_t delta = (int64_t) (ntp_recv - ntp_origin) / 2 + (int64_t) (ntp_xmit - ntp_dest) / 2;

  record_result(same_as_last, addr, delta, ntp.stratum, ntp.LI, ((int64_t)(ntp_dest - ntp_origin - (ntp_xmit - ntp_recv))) >> 16, root_maxerr, ntohl(ntp.root_dispersion), ntohl(ntp.root_delay));

#else
  uint64_t ntp_xmit = (((uint64_t) ntp.xmit.sec - NTP_TO_UNIX_EPOCH) << 32) + (uint64_t) ntp.xmit.frac;
  record_result(same_as_last, addr, ntp_xmit, ntp.stratum, ntp.LI, (((int64_t) (system_get_time() - ntp.origin.frac)) << 16) / MICROSECONDS, root_maxerr, ntohl(ntp.root_dispersion), ntohl(ntp.root_delay));
#endif

  sntp_dosend();
}

#ifdef LUA_USE_MODULES_RTCTIME
static int sntp_setoffset(lua_State *L)
{
  the_offset = luaL_checkinteger(L, 1);

  struct rtc_timeval tv;
  rtctime_gettimeofday (&tv);
  if (tv.tv_sec) {
    next_midnight = get_next_midnight(tv.tv_sec);
  }

  using_offset = 1;

  return 0;
}

static int sntp_getoffset(lua_State *L)
{
  update_offset();
  lua_pushnumber(L, the_offset);

  return 1;
}
#endif

static void sntp_dolookups (lua_State *L) {
  // Step through each element of the table, converting it to an address
  // at the end, start the lookups. If we have already looked everything up,
  // then move straight to sending the packets.
  if ((state->list_ref == LUA_NOREF) || (state->list_ref == LUA_REFNIL)) {
    sntp_dosend();
    return;
  }

  lua_rawgeti(L, LUA_REGISTRYINDEX, state->list_ref);
  while (1) {
    int l;

    if (lua_objlen(L, -1) <= state->lookup_pos) {
      // We reached the end
      if (server_count == 0) {
        // Oh dear -- no valid entries -- generate an error
        // This means that all the arguments are invalid. Just pick the first
        lua_rawgeti(L, -1, 1);
        const char *hostname = luaL_checklstring(L, -1, &l);
        handle_error(L, NTP_DNS_ERR, hostname);
        lua_pop(L, 1);
      } else {
        sntp_dosend();
      }
      break;
    }

    state->lookup_pos++;

    lua_rawgeti(L, -1, state->lookup_pos);
    const char *hostname = luaL_checklstring(L, -1, &l);

    if (l>128 || hostname == NULL) {
      handle_error(L, NTP_DNS_ERR, hostname);
      lua_pop(L, 1);
      break;
    }
    err_t err = dns_gethostbyname(hostname, &serverp[server_count], sntp_dns_found, state);
    if (err == ERR_INPROGRESS) {
      lua_pop(L, 1);
      break;  // Callback function sntp_dns_found will handle sntp_dosend for us
    } else if (err == ERR_ARG) {
      handle_error(L, NTP_DNS_ERR, hostname);
      lua_pop(L, 1);
      break;
    }
    server_count++;
  }
  lua_pop(L, 1);
}

static char *state_init(lua_State *L) {
  state = (sntp_state_t *)c_malloc (sizeof (sntp_state_t));
  if (!state)
    return ("out of memory");

  memset (state, 0, sizeof (sntp_state_t));

  state->sync_cb_ref = LUA_NOREF;
  state->err_cb_ref = LUA_NOREF;
  state->list_ref = LUA_NOREF;

  state->pcb = udp_new ();
  if (!state->pcb)
    return ("out of memory");

  if (udp_bind (state->pcb, IP_ADDR_ANY, 0) != ERR_OK)
    return ("no port available");

  udp_recv (state->pcb, on_recv, L);

  state->server_pos = -1;
  state->last_server_pos = -1;

  return NULL;
}

static char *set_repeat_mode(lua_State *L, bool enable)
{
  if (repeat) {
    os_timer_disarm (&repeat->timer);
    luaL_unref (L, LUA_REGISTRYINDEX, repeat->sync_cb_ref);
    luaL_unref (L, LUA_REGISTRYINDEX, repeat->err_cb_ref);
    luaL_unref (L, LUA_REGISTRYINDEX, repeat->list_ref);
    luaM_free(L, repeat);
    repeat = NULL;
  }

  if (enable) {
    repeat = (sntp_repeat_t *) luaM_new(L, sntp_repeat_t);
    memset(repeat, 0, sizeof(repeat));
    lua_rawgeti(L, LUA_REGISTRYINDEX, state->sync_cb_ref);
    repeat->sync_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, LUA_REGISTRYINDEX, state->err_cb_ref);
    repeat->err_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, LUA_REGISTRYINDEX, state->list_ref);
    repeat->list_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    os_timer_setfn(&repeat->timer, on_long_timeout, NULL);
    SWTIMER_REG_CB(on_long_timeout, SWTIMER_RESUME);
      //The function on_long_timeout returns errors to the developer
      //My guess: Error reporting is a good thing, resume the timer.
    os_timer_arm(&repeat->timer, 1000 * 1000, 1);
  }

  return NULL;
}

static void on_long_timeout (void *arg)
{
  (void)arg;
  sntp_dbg("sntp: long timer\n");
  lua_State *L = lua_getstate ();
  if (!state) {
    if (!state_init(L)) {
      // Good.
      lua_rawgeti(L, LUA_REGISTRYINDEX, repeat->sync_cb_ref);
      state->sync_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
      lua_rawgeti(L, LUA_REGISTRYINDEX, repeat->err_cb_ref);
      state->err_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
      if (server_count == 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, repeat->list_ref);
        state->list_ref = luaL_ref(L, LUA_REGISTRYINDEX);
      }
      state->is_on_timeout = 1;
      sntp_dolookups(L);
    }
  }
}
#endif

static int
sntp_pass_preflight(lua_State *L)
{
  struct udp_pcb *pcb = NULL;
  struct pbuf *p = NULL;

  if (sntp_pcb == NULL) {
    pcb = udp_new();
    if (pcb == NULL)
      goto out;
  }

  if (sntp_pbuf == NULL) {
    p = pbuf_alloc (PBUF_TRANSPORT, sizeof (ntp_frame_t), PBUF_RAM);
    if (p == NULL)
      goto out;
  }

  if (pcb)
    sntp_pcb = pcb;

  if (p)
    sntp_pbuf = p;

  return 0;

out:
  if (pcb)
    udp_remove(pcb);

  if (p)
    pbuf_free(p);

   return -1;
}

static void
sntp_do_repeat (void *arg)
{
  lua_State *L = arg;

  if (sntp_pass_preflight(L) != 0) {
    /* Things must be really rough; try again in a bit */
    os_timer_setfn(&sntp_timer, sntp_do_repeat, L);
    os_timer_arm(&sntp_timer, 30 * 1000, 1);
    return;
  }
  
  sntp_pass_start(L);
}

/* sntp.sync (server or nil, syncfn or nil, errfn or nil, repeat bool) */
static int
sntp_sync (lua_State *L)
{

  /* 
   * This is a little out of order; we're going to preflight and reset the
   * state machine before we completely pave over the existing state.  This
   * way, errors don't change the existing state, in case that's already on
   * repeat and works.  Not great, but better than the alternative of
   * clobbering state, yeah?
   *
   * XXX If you disagree, move this down and call sntp_pass_reset and
   * sntp_cleanup on the exit path.
   */
  int pfc = sntp_pass_preflight(L);
  if (pfc != 0)
    return luaL_error (L, "SNTP preflight failed");

  sntp_pass_reset();
  sntp_cleanup(L);

  if (!lua_isnoneornil (L, 1)) {
    /* Replace the list of servers with the given set */
    if (lua_istable(L, 1)) {
      lua_pushvalue(L, 1);
      sntp_servers = lua_objlen(L, -1);
      /* Truncate the server list if it's overlong */
      for (; sntp_servers > 32; sntp_servers--) {
        lua_pushnil(L);
        lua_rawseti(L, 1, sntp_servers);
      }
    } else {
      size_t l;
      const char *hostname = luaL_checklstring(L, 1, &l);
      if (l > 128 || hostname == NULL)
        return luaL_error (L, "Bad hostname");

      /* Construct a singleton table containing the one server */
      lua_newtable(L);
      lua_pushnumber(L, 1);
      lua_pushstring(L, hostname);
      lua_settable(L, -3);
      sntp_servers = 1;
    }

    /* Drop the old server list and replace it */
    luaL_unref (L, LUA_REGISTRYINDEX, sntp_server_list_ref);
    sntp_server_list_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  if (!lua_isnoneornil (L, 2)) {
    lua_pushvalue (L, 2);
    sntp_sync_cb_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  }

  if (!lua_isnoneornil (L, 3)) {
    lua_pushvalue (L, 3);
    sntp_err_cb_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  }

  if (!lua_isnoneornil(L, 4)) {
    sntp_repeat = 1;
  }

  sntp_pass_start(L);
}

static void sntp_task_body(os_param_t param, uint8_t prio)
{
  (void) param; /* XXX lua_State *? */

  /*
   * We are done with LwIP state, so shed here; we grab them again, if
   * needed, in the next preflight, which will either be when the user
   * asks us to sync again or when the retry timer goes.  This way, we
   * aren't holding too many resources between syncs, and if there isn't
   * memory for a sync pass at any point, we can just keep trying again.
   */
  if (sntp_pcb) {
    udp_remove(sntp_pcb);
    sntp_pcb = NULL;
  }
  if (sntp_pbuf) {
    pbuf_free(sntp_pbuf);
    sntp_pbuf = NULL;
  }

  /*
   * If we aren't repeating, this will also release the refrences to the bits
   * of Lua state we're holding (server table, callbacks).
   */
  sntp_pass_fini(lua_getstate() /* XXX ICK! */);
}

static int
sntp_open(lua_State *L)
{
  int i;

  (void) L;
  sntp_task = task_get_id(sntp_task_body);

  // Load default NTP pool as a Lua table, to keep the state machine simple.
  // XXX Four entries is probably superfluous.  Maybe two instead?
  lua_newtable(L);
  for (i = 0; i < 4; i++) {
    char buf[64];

    lua_pushnumber(L, i + 1);
    c_sprintf(buf, "%d.nodemcu.pool.ntp.org", i);
    lua_pushstring(L, buf);
    lua_settable(L, -3);
  }
  sntp_server_list_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  return 0;
}

// Module function map
static const LUA_REG_TYPE sntp_map[] = {
  { LSTRKEY("sync"),  LFUNCVAL(sntp_sync)  },
#ifdef LUA_USE_MODULES_RTCTIME
  { LSTRKEY("setoffset"),  LFUNCVAL(sntp_setoffset)  },
  { LSTRKEY("getoffset"),  LFUNCVAL(sntp_getoffset)  },
#endif
  { LNILKEY, LNILVAL }
};

NODEMCU_MODULE(SNTP, "sntp", sntp_map, sntp_open);
