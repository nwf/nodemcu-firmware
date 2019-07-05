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

// Module for Simple Network Time Protocol (SNTP) packet processing;
// see lua_modules/sntp/sntp.lua for the user-friendly bits of this.

#include "module.h"
#include "lauxlib.h"
#include "lmem.h"
#include "os_type.h"
#include "osapi.h"
#include "lwip/udp.h"
#include "stdlib.h"
#include "user_modules.h"
#include "lwip/dns.h"
#include "task/task.h"
#include "user_interface.h"

#define max(a,b) ((a < b) ? b : a)

#define NTP_PORT 123
#define NTP_ANYCAST_ADDR(dst)  IP4_ADDR(dst, 224, 0, 1, 1)

#if 0
# define sntppkt_dbg(...) dbg_printf(__VA_ARGS__)
#else
# define sntppkt_dbg(...)
#endif

typedef struct
{
  uint32_t sec;
  uint32_t frac;
} ntp_timestamp_t;

static const uint32_t NTP_TO_UNIX_EPOCH = 2208988800ul;

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
} __attribute__((packed)) ntp_frame_t;

typedef struct {
    int64_t   delta;
    uint32_t  cached_delay;
    uint32_t  txsec;
    uint32_t  delay_frac;
    uint32_t  root_delay;
    uint32_t  root_dispersion;
    uint8_t   LI;
    uint8_t   stratum;
} ntp_response_t;

static uint64_t
sntppkt_div1m(uint64_t n) {
  uint64_t q1 = (n >> 5) + (n >> 10);
  uint64_t q2 = (n >> 12) + (q1 >> 1);
  uint64_t q3 = (q2 >> 11) - (q2 >> 23);

  uint64_t q = n + q1 + q2 - q3;

  q = q >> 20;

  // Ignore the error term -- it is measured in pico seconds
  return q;
}

static uint32_t
sntppkt_us_to_frac(uint64_t us) {
  return sntppkt_div1m(us << 32);
}

static const uint32_t MICROSECONDS      = 1000000;

static uint32_t
sntppkt_frac16_to_us(uint64_t frac) {
  return (frac * MICROSECONDS) >> 16;
}

/*
 * Convert sec/usec to a Lua string suitable for depositing into a SNTP packet
 * buffer.  This is a little gross, but it's not the worst thing a C
 * programmer's ever done, I'm sure.
 */
static int
sntppkt_make_ts(lua_State *L) {
  ntp_timestamp_t ts;
  uint32_t usec;

  ts.sec  = htonl(luaL_checkinteger(L, 1) + NTP_TO_UNIX_EPOCH) ;
  usec    =       luaL_checkinteger(L, 2)  ;
  ts.frac = htonl(sntppkt_us_to_frac(usec));

  lua_pushlstring(L, (const char *)&ts, sizeof(ts));
  return 1;
}

/*
 * Process a SNTP packet as contained in a Lua string, given a cookie timestamp
 * and local clock second*usecond pair.  Generates a ntp_response_t userdata
 * for later processing or a string if the server is telling us to go away.
 */
static int
sntppkt_proc_pkt(lua_State *L) {
  const char  *pkts;
  size_t       pkts_len;

  uint32_t     now_sec;
  uint32_t     now_usec;

  ntp_timestamp_t *cookie;
  size_t           cookie_len;

  ntp_response_t *ntpr;

  // make sure we have an aligned copy to work from
  // XXX nwf: is this necessary?
  ntp_frame_t  pktb;

  now_usec = luaL_checkinteger(L, 4);
  now_sec  = luaL_checkinteger(L, 3);

  luaL_checktype(L, 2, LUA_TSTRING);
  cookie   = (ntp_timestamp_t*) lua_tolstring(L, 2, &cookie_len);
  if (cookie_len != sizeof(*cookie)) {
    luaL_error(L, "Bad cookie");
  }

  luaL_checktype(L, 1, LUA_TSTRING);
  pkts = lua_tolstring(L, 1, &pkts_len);
  if (pkts_len != sizeof(pktb)) {
    luaL_error(L, "Bad packet length");
  }
  os_memcpy (&pktb, pkts, sizeof(pktb));

  if (memcmp((const char *)cookie, (const char *)&pktb.origin, sizeof (*cookie))) {
    /* bad cookie; return nil */
    return 0;
  }

  /* KOD? */
  if (pktb.LI == 3) {
    lua_pushlstring(L, (const char *)&pktb.refid, 4);
    return 1;
  }

  ntpr = lua_newuserdata(L, sizeof(ntp_response_t));
  luaL_getmetatable(L, "sntppkt.resp");
  lua_setmetatable(L, -2);

  ntpr->LI              = pktb.LI;
  ntpr->stratum         = pktb.stratum;
  ntpr->root_delay      = ntohl(pktb.root_delay);
  ntpr->root_dispersion = ntohl(pktb.root_dispersion);

  /* Heavy time lifting time */

  pktb.origin.sec  = ntohl(pktb.origin.sec);
  pktb.origin.frac = ntohl(pktb.origin.frac);
  pktb.recv.sec    = ntohl(pktb.recv.sec);
  pktb.recv.frac   = ntohl(pktb.recv.frac);
  pktb.xmit.sec    = ntohl(pktb.xmit.sec);
  pktb.xmit.frac   = ntohl(pktb.xmit.frac);

  ntpr->txsec = pktb.xmit.sec - NTP_TO_UNIX_EPOCH;

  uint64_t ntp_recv   = (((uint64_t) pktb.recv.sec               ) << 32)
                        + pktb.recv.frac;
  uint64_t ntp_origin = (((uint64_t) pktb.origin.sec             ) << 32)
                        + pktb.origin.frac;
  uint64_t ntp_xmit   = (((uint64_t) pktb.xmit.sec               ) << 32)
                        + pktb.xmit.frac;
  uint64_t ntp_dest   = (((uint64_t) now_sec + NTP_TO_UNIX_EPOCH ) << 32)
                        + sntppkt_us_to_frac(now_usec);

  ntpr->delta = ((int64_t) ntp_recv - ntp_origin) / 2
              + ((int64_t) ntp_xmit - ntp_dest  ) / 2;

  ntpr->delay_frac = ((int64_t)ntp_dest - ntp_origin - ntp_xmit + ntp_recv) >> 16;

  ntpr->cached_delay    = ntpr->root_delay * 2 + ntpr->delay_frac;

  return 1;
}

/*
 * Left-biased selector of a "preferred" NTP response.  Note that preference
 * is rather subjective!
 *
 * Lua does not make it straightforward to return an existing userdata
 * object, so instead we merely return a boolean indicating whether the
 * second argument is superior to the first.
 */

static int
sntppkt_pick_resp(lua_State *L) {

  ntp_response_t *a = luaL_checkudata(L, 1, "sntppkt.resp");
  ntp_response_t *b = luaL_checkudata(L, 2, "sntppkt.resp");
  int biased = 0;

  biased = lua_toboolean(L, 3);

  /*
   * If we're "biased", prefer the second structure if the delay less than
   * 3/4ths of the delay in the first.  An unbiased comparison just uses
   * the raw delay values.
   */
  if (biased) {
    lua_pushboolean(L, a->cached_delay * 3 > b->cached_delay * 4);
  } else {
    lua_pushboolean(L, a->cached_delay     > b->cached_delay    );
  }
  return 1;
}

static void
field_from_number(lua_State *L, const char * field_name, lua_Number value) {
  lua_pushnumber(L, value);
  lua_setfield(L, -2, field_name);
}

/*
 * Inflate a NTP response into a Lua table
 */
static int
sntppkt_read_resp(lua_State *L) {
  ntp_response_t *r = luaL_checkudata(L, 1, "sntppkt.resp");

  lua_createtable(L, 0, 6);

  /* For large corrections, don't bother exposing fine values */
  int d40 = r->delta >> 40;
  if (d40 != 0 && d40 != -1) {
    field_from_number(L, "offset_s", r->delta >> 32);
  } else {
    field_from_number(L, "offset_us", (r->delta * MICROSECONDS) >> 32);
  }

  field_from_number(L, "delay_us", sntppkt_frac16_to_us(r->delay_frac));
  field_from_number(L, "root_delay_us", sntppkt_frac16_to_us(r->root_delay));
  field_from_number(L, "root_dispersion", r->root_dispersion);
  field_from_number(L, "leapind", r->LI);
  field_from_number(L, "stratum", r->stratum);

  return 1;
}

LROT_BEGIN(sntppkt_resp)
LROT_END(sntppkt_resp, sntppkt_resp, 0)

static int
sntppkt_init(lua_State *L)
{
  luaL_rometatable(L, "sntppkt.resp"   , LROT_TABLEREF(sntppkt_resp   ));
  return 0;
}

// Module function map
LROT_BEGIN(sntppkt)
  LROT_FUNCENTRY( make_ts  , sntppkt_make_ts   )
  LROT_FUNCENTRY( proc_pkt , sntppkt_proc_pkt  )
  LROT_FUNCENTRY( pick_resp, sntppkt_pick_resp )
  LROT_FUNCENTRY( read_resp, sntppkt_read_resp )
LROT_END( sntppkt, NULL, 0 )

NODEMCU_MODULE(SNTPPKT, "sntppkt", sntppkt, sntppkt_init);
