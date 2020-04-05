// Module for TLS

#include "module.h"

#if defined(CLIENT_SSL_ENABLE) && defined(LUA_USE_MODULES_NET)

#include "lauxlib.h"
#include "platform.h"
#include "lmem.h"

#include <ctype.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <ctype.h>

#include "mem.h"
#include "lwip/ip_addr.h"
#include "espconn.h"
#include "sys/espconn_mbedtls.h"
#include "lwip/err.h"
#include "lwip/dns.h"

#include "mbedtls/debug.h"
#include "user_mbedtls.h"

#ifdef HAVE_SSL_SERVER_CRT
#include HAVE_SSL_SERVER_CRT
#else
__attribute__((section(".servercert.flash"))) unsigned char tls_server_cert_area[INTERNAL_FLASH_SECTOR_SIZE];
#endif

__attribute__((section(".clientcert.flash"))) unsigned char tls_client_cert_area[INTERNAL_FLASH_SECTOR_SIZE];

typedef struct {
  struct espconn pesp_conn;
  int self_ref;
  int cb_connect_ref;
  int cb_reconnect_ref;
  int cb_disconnect_ref;
  int cb_sent_ref;
  int cb_receive_ref;
  int cb_dns_ref;
  uint8_t refcount;   /* References held by other bits of C */
} tls_socket_ud;

static int tls_socket_create( lua_State *L ) {
  tls_socket_ud *ud = (tls_socket_ud*) lua_newuserdata(L, sizeof(tls_socket_ud));

  bzero(&ud->pesp_conn, sizeof(ud->pesp_conn));

  ud->self_ref =
  ud->cb_connect_ref =
  ud->cb_reconnect_ref =
  ud->cb_disconnect_ref =
  ud->cb_sent_ref =
  ud->cb_receive_ref =
  ud->cb_dns_ref = LUA_NOREF;

  ud->refcount = 0;

  luaL_getmetatable(L, "tls.socket");
  lua_setmetatable(L, -2);

  return 1;
}

/*
 * Disconnect and unhook this socket from the Lua side of the world.  Lua...
 *
 *   - may have dropped all their references to this socket, in which case,
 *   this is getting pretty near the last time we'll ever hear of this object
 *   (except the tls_socket_delete __gc metamethod, below).
 *
 *   - may retain a reference to this socket; we'll re-allocate the tcp state
 *   in tls_socket_connect, below.
 *
 * It is, therefore, important that if any code references ud after calling
 * this method that they be very sure that a reference is held on the Lua
 * stack!
 */
static void tls_socket_cleanup(tls_socket_ud *ud) {
  NODE_DBG("tls_socket_cleanup %p w=%d\n", ud, ud->refcount);

  /* Wait for C to drop its references (DNS or espconn) */
  if (ud->refcount != 0)
   return;

  if (ud->pesp_conn.proto.tcp) {
    free(ud->pesp_conn.proto.tcp);
    ud->pesp_conn.proto.tcp = NULL;
  }

  int selfref = ud->self_ref;
  ud->self_ref = LUA_NOREF;
  luaL_unref(lua_getstate(), LUA_REGISTRYINDEX, selfref);
}

/*
 * Call the "last gasp" callbacks and tear down the socket state, returning it
 * to its pre-connect state.
 *
 * Like `net`, make "disconnection" get everything unless there's a
 * "reconnection" callback registered, in which case, "disconnection" gets only
 * the ordinary disconnection events (i.e., those with no errstr).
 */
static void tls_socket_last_call(tls_socket_ud *ud, const char *errstr) {
  int cbref = ud->cb_disconnect_ref;
  if ((errstr != NULL) && (ud->cb_reconnect_ref != LUA_NOREF)) {
    cbref = ud->cb_reconnect_ref;
  }

  NODE_DBG("tls_socket_last_call %p %d '%s'\n", ud, cbref,
           errstr ? errstr : "No error");

  if (cbref != LUA_NOREF) {
    lua_State *L = lua_getstate();
    lua_rawgeti(L, LUA_REGISTRYINDEX, cbref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->self_ref);
    if (errstr != NULL) {
      lua_pushstring(L, errstr);
    } else {
      lua_pushnil(L);
    }
    tls_socket_cleanup(ud);
    lua_call(L, 2, 0);
  } else {
    tls_socket_cleanup(ud);
  }
}

static void tls_socket_onconnect( struct espconn *pesp_conn ) {
  tls_socket_ud *ud = (tls_socket_ud *)pesp_conn;

  NODE_DBG("tls_socket_onconnect %p w=%d\n", ud, ud->refcount);

  if (ud->cb_connect_ref != LUA_NOREF) {
    lua_State *L = lua_getstate();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_connect_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->self_ref);
    lua_call(L, 1, 0);
  } else if ((ud->cb_disconnect_ref == LUA_NOREF) &&
             (ud->cb_sent_ref == LUA_NOREF) &&
             (ud->cb_receive_ref == LUA_NOREF)) {
    espconn_secure_disconnect(&ud->pesp_conn);
  }
}

static void tls_socket_ondisconnect( struct espconn *pesp_conn ) {
  tls_socket_ud *ud = (tls_socket_ud *)pesp_conn;

  NODE_DBG("tls_socket_ondisconnect %p w=%d\n", ud, ud->refcount);

  ud->refcount--; // ESP goo has released handle
  tls_socket_last_call(ud, NULL);
}

static void tls_socket_onreconnect( struct espconn *pesp_conn, s8 err ) {
  const char* reason = "Unknown error";
  switch (err) {
    case(ESPCONN_MEM): reason = "Out of memory"; break;
    case(ESPCONN_TIMEOUT): reason = "Timeout"; break;
    case(ESPCONN_RTE): reason = "Routing problem"; break;
    case(ESPCONN_ABRT): reason = "Connection aborted"; break;
    case(ESPCONN_RST): reason = "Connection reset"; break;
    case(ESPCONN_CLSD): reason = "Connection closed"; break;
    case(ESPCONN_HANDSHAKE): reason = "SSL handshake failed"; break;
    case(ESPCONN_SSL_INVALID_DATA): reason = "SSL application invalid"; break;
  }

  tls_socket_ud *ud = (tls_socket_ud *)pesp_conn;

  NODE_DBG("tls_socket_onreconnect %p w=%d e=%d(%s)\n", ud, ud->refcount, err, reason);

  ud->refcount--; // ESP goo has released handle
  tls_socket_last_call(ud, reason);
}

static void tls_socket_onrecv( struct espconn *pesp_conn, char *buf, u16 length ) {
  tls_socket_ud *ud = (tls_socket_ud *)pesp_conn;

  NODE_DBG("tls_socket_onrecv %p w=%d\n", ud, ud->refcount);

  if (ud->cb_receive_ref != LUA_NOREF) {
    lua_State *L = lua_getstate();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_receive_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->self_ref);
    lua_pushlstring(L, buf, length);
    lua_call(L, 2, 0);
  } else if ((ud->cb_disconnect_ref == LUA_NOREF) &&
             (ud->cb_sent_ref == LUA_NOREF)) {
    espconn_secure_disconnect(&ud->pesp_conn);
  }
}

static void tls_socket_onsent( struct espconn *pesp_conn ) {
  tls_socket_ud *ud = (tls_socket_ud *)pesp_conn;

  NODE_DBG("tls_socket_onsent %p w=%d\n", ud, ud->refcount);

  if (ud->cb_sent_ref != LUA_NOREF) {
    lua_State *L = lua_getstate();
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_sent_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->self_ref);
    lua_call(L, 1, 0);
  } else if ((ud->cb_disconnect_ref == LUA_NOREF) &&
             (ud->cb_receive_ref == LUA_NOREF)) {
    espconn_secure_disconnect(&ud->pesp_conn);
  }
}

static void tls_socket_ondns( const char* domain, ip_addr_t *ip_addr, void *arg) {
  tls_socket_ud *ud = arg;
  NODE_DBG("tls_socket_ondns %p w=%d\n", ud, ud->refcount);

  ud->refcount--;

  ip_addr_t addr;
  if (ip_addr) addr = *ip_addr;
  else addr.addr = 0xFFFFFFFF;
  lua_State *L = lua_getstate();
  if (ud->cb_dns_ref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->cb_dns_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ud->self_ref);
    if (addr.addr == 0xFFFFFFFF) {
      lua_pushnil(L);
    } else {
      char tmp[20];
      sprintf(tmp, IPSTR, IP2STR(&addr.addr));
      lua_pushstring(L, tmp);
    }
    lua_call(L, 2, 0);
  }

  if ((ud->cb_disconnect_ref == LUA_NOREF) &&
      (ud->cb_connect_ref == LUA_NOREF) &&
      (ud->cb_sent_ref == LUA_NOREF) &&
      (ud->cb_receive_ref == LUA_NOREF)) {
    /* Directly call last_call here, not _disconnect because we're not connected */
    tls_socket_last_call(ud, "No callbacks");
    return;
  }

  if (addr.addr == 0xFFFFFFFF) {
    tls_socket_last_call(ud, "DNS failure");
  } else {
    os_memcpy(ud->pesp_conn.proto.tcp->remote_ip, &addr.addr, 4);

    /* Additionally referenced by ESP goo until disconn or reconn callback */
    ud->refcount++;
    int res = espconn_secure_connect(&ud->pesp_conn);
    switch(res) {
    case ESPCONN_OK:
      return;
    default:
      return tls_socket_onreconnect(&ud->pesp_conn, res);
    }
  }
}

static int tls_socket_connect( lua_State *L ) {
  tls_socket_ud *ud = (tls_socket_ud *)luaL_checkudata(L, 1, "tls.socket");

  NODE_DBG("tls_socket_connect %p w=%d\n", ud, ud->refcount);

  if (ud->pesp_conn.proto.tcp) {
    return luaL_error(L, "already connected");
  }

  u16 port = luaL_checkinteger( L, 2 );
  if (port == 0)
    return luaL_error(L, "invalid port");

  size_t il;
  const char *domain = luaL_checklstring( L, 3, &il );
  if (domain == NULL)
    return luaL_error(L, "invalid domain");

  /*
   * Anchor this socket in the lua registry while callbacks exist.  This might
   * OOM if the registry needs to expand.  As such, do this before we allocate,
   * below, and unwire if the allocation fails.
   */
  if (ud->self_ref == LUA_NOREF) {
    lua_pushvalue(L, 1);  // copy to the top of stack
    ud->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  ud->pesp_conn.proto.udp = NULL;
  ud->pesp_conn.proto.tcp = (esp_tcp *)calloc(1,sizeof(esp_tcp));
  if(!ud->pesp_conn.proto.tcp){
    tls_socket_cleanup(ud);
    return luaL_error(L, "not enough memory");
  }
  ud->pesp_conn.type = ESPCONN_TCP;
  ud->pesp_conn.state = ESPCONN_NONE;
  ud->pesp_conn.proto.tcp->remote_port = port;
  espconn_regist_connectcb(&ud->pesp_conn, (espconn_connect_callback)tls_socket_onconnect);
  espconn_regist_disconcb(&ud->pesp_conn, (espconn_connect_callback)tls_socket_ondisconnect);
  espconn_regist_reconcb(&ud->pesp_conn, (espconn_reconnect_callback)tls_socket_onreconnect);
  espconn_regist_recvcb(&ud->pesp_conn, (espconn_recv_callback)tls_socket_onrecv);
  espconn_regist_sentcb(&ud->pesp_conn, (espconn_sent_callback)tls_socket_onsent);

  ud->refcount++;
  ip_addr_t addr;
  err_t err = dns_gethostbyname(domain, &addr, tls_socket_ondns, ud);
  if (err == ERR_OK) {
    tls_socket_ondns(domain, &addr, ud);
  } else if (err != ERR_INPROGRESS) {
    tls_socket_ondns(domain, NULL, ud);
  }

  return 0;
}

static int tls_socket_on( lua_State *L ) {
  tls_socket_ud *ud = (tls_socket_ud *)luaL_checkudata(L, 1, "tls.socket");

  size_t sl;
  const char *method = luaL_checklstring( L, 2, &sl );
  int *cbp;

       if (strcmp(method, "connection"   ) == 0) { cbp = &ud->cb_connect_ref   ; }
  else if (strcmp(method, "disconnection") == 0) { cbp = &ud->cb_disconnect_ref; }
  else if (strcmp(method, "reconnection" ) == 0) { cbp = &ud->cb_reconnect_ref ; }
  else if (strcmp(method, "receive"      ) == 0) { cbp = &ud->cb_receive_ref   ; }
  else if (strcmp(method, "sent"         ) == 0) { cbp = &ud->cb_sent_ref      ; }
  else if (strcmp(method, "dns"          ) == 0) { cbp = &ud->cb_dns_ref       ; }
  else {
    return luaL_error(L, "invalid method");
  }

  if (lua_isfunction(L, 3)) {
    lua_pushvalue(L, 3);  // copy argument (func) to the top of stack
    luaL_unref(L, LUA_REGISTRYINDEX, *cbp);
    *cbp = luaL_ref(L, LUA_REGISTRYINDEX);
  } else if (lua_isnil(L, 3)) {
    luaL_unref(L, LUA_REGISTRYINDEX, *cbp);
    *cbp = LUA_NOREF;
  } else {
    return luaL_error(L, "invalid callback function");
  }

  return 0;
}

static int tls_socket_send( lua_State *L ) {
  tls_socket_ud *ud = (tls_socket_ud *)luaL_checkudata(L, 1, "tls.socket");
  size_t sl;
  const char* buf = luaL_checklstring(L, 2, &sl);

  if(ud->pesp_conn.proto.tcp == NULL) {
    NODE_DBG("not connected");
    return 0;
  }

  espconn_secure_send(&ud->pesp_conn, (void*)buf, sl);
  return 0;
}

static int tls_socket_hold( lua_State *L ) {
  tls_socket_ud *ud = (tls_socket_ud *)luaL_checkudata(L, 1, "tls.socket");

  if(ud->pesp_conn.proto.tcp == NULL) {
    NODE_DBG("not connected");
    return 0;
  }

  espconn_recv_hold(&ud->pesp_conn);

  return 0;
}
static int tls_socket_unhold( lua_State *L ) {
  tls_socket_ud *ud = (tls_socket_ud *)luaL_checkudata(L, 1, "tls.socket");
  if(ud->pesp_conn.proto.tcp == NULL) {
    NODE_DBG("not connected");
    return 0;
  }

  espconn_recv_unhold(&ud->pesp_conn);

  return 0;
}

static int tls_socket_getpeer( lua_State *L ) {
  tls_socket_ud *ud = (tls_socket_ud *)luaL_checkudata(L, 1, "tls.socket");

  if(ud->pesp_conn.proto.tcp && ud->pesp_conn.proto.tcp->remote_port != 0){
    char temp[20] = {0};
    sprintf(temp, IPSTR, IP2STR( &(ud->pesp_conn.proto.tcp->remote_ip) ) );
    lua_pushstring( L, temp );
    lua_pushinteger( L, ud->pesp_conn.proto.tcp->remote_port );
  } else {
    lua_pushnil( L );
    lua_pushnil( L );
  }
  return 2;
}

static int tls_socket_close( lua_State *L ) {
  tls_socket_ud *ud = (tls_socket_ud *)luaL_checkudata(L, 1, "tls.socket");

  NODE_DBG("tls_socket_close %p\n", ud);

  if (ud->pesp_conn.proto.tcp) {
    /*
     * This, eventually, results in calling the "last-call" callbacks through
     * the ESP glue (on a different task).  That path will eventually crawl up
     * to tls_socket_cleanup(), so don't do the cleanup here, so that we do
     * fire off the callbacks on the posted task.  See the note in
     * tls_socket_cleanup() about its lack of reentrancy.
     */
    espconn_secure_disconnect(&ud->pesp_conn);
  }
  return 0;
}

static int tls_socket_delete( lua_State *L ) {
  tls_socket_ud *ud = (tls_socket_ud *)luaL_checkudata(L, 1, "tls.socket");

  NODE_DBG("tls_socket_delete %p\n", ud);

  luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_connect_ref);
  ud->cb_connect_ref = LUA_NOREF;
  luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_disconnect_ref);
  ud->cb_disconnect_ref = LUA_NOREF;
  luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_reconnect_ref);
  ud->cb_reconnect_ref = LUA_NOREF;
  luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_dns_ref);
  ud->cb_dns_ref = LUA_NOREF;
  luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_receive_ref);
  ud->cb_receive_ref = LUA_NOREF;
  luaL_unref(L, LUA_REGISTRYINDEX, ud->cb_sent_ref);
  ud->cb_sent_ref = LUA_NOREF;

  /*
   * self-ref must have already been dropped, else we won't end up here,
   * because there's still a reference to us.  Don't attempt to drop it.
   *
   * Since the only way for self-ref to have been dropped is for
   * tls_socket_cleanup() to have been called, there's no need to contemplate
   * the pesp_conn.proto.tcp dynamic allocatin, either.
   */

  return 0;
}

// Returns NULL on success, error message otherwise
static const char *append_pem_blob(const char *pem, const char *type, uint8_t **buffer_p, uint8_t *buffer_limit, const char *name) {
  char unb64[256];
  memset(unb64, 0xff, sizeof(unb64));
  int i;
  for (i = 0; i < 64; i++) {
    unb64["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
  }

  if (!pem) {
    return "No PEM blob";
  }

  // Scan for -----BEGIN CERT
  pem = strstr(pem, "-----BEGIN ");
  if (!pem) {
    return "No PEM header";
  }

  if (strncmp(pem + 11, type, strlen(type))) {
    return "Wrong PEM type";
  }

  pem = strchr(pem, '\n');
  if (!pem) {
    return "Incorrect PEM format";
  }
  //
  // Base64 encoded data starts here
  // Get all the base64 data into a single buffer....
  // We will use the back end of the buffer....
  //

  uint8_t *buffer = *buffer_p;

  uint8_t *dest = buffer + 32 + 2;  // Leave space for name and length
  int bitcount = 0;
  int accumulator = 0;
  for (; *pem && dest < buffer_limit; pem++) {
    int val = unb64[*(uint8_t*) pem];
    if (val & 0xC0) {
      // not a base64 character
      if (isspace(*(uint8_t*) pem)) {
	continue;
      }
      if (*pem == '=') {
	// just ignore -- at the end
	bitcount = 0;
	continue;
      }
      if (*pem == '-') {
	break;
      }
      return "Invalid character in PEM";
    } else {
      bitcount += 6;
      accumulator = (accumulator << 6) + val;
      if (bitcount >= 8) {
	bitcount -= 8;
	*dest++ = accumulator >> bitcount;
      }
    }
  }
  if (dest >= buffer_limit || strncmp(pem, "-----END ", 9) || strncmp(pem + 9, type, strlen(type)) || bitcount) {
    return "Invalid PEM format data";
  }
  size_t len = dest - (buffer + 32 + 2);

  memset(buffer, 0, 32);
  strcpy(buffer, name);
  buffer[32] = len & 0xff;
  buffer[33] = (len >> 8) & 0xff;
  *buffer_p = dest;
  return NULL;
}

static const char *fill_page_with_pem(lua_State *L, const unsigned char *flash_memory, int flash_offset, const char **types, const char **names)
{
  uint8_t  *buffer = luaM_malloc(L, INTERNAL_FLASH_SECTOR_SIZE);
  uint8_t  *buffer_base = buffer;
  uint8_t  *buffer_limit = buffer + INTERNAL_FLASH_SECTOR_SIZE;

  int argno;

  for (argno = 1; argno <= lua_gettop(L) && types[argno - 1]; argno++) {
    const char *pem = lua_tostring(L, argno);

    const char *error = append_pem_blob(pem, types[argno - 1], &buffer, buffer_limit, names[argno - 1]);
    if (error) {
      luaM_free(L, buffer_base);
      return error;
    }
  }

  memset(buffer, 0xff, buffer_limit - buffer);

  // Lets see if it matches what is already there....
  if (memcmp(buffer_base, flash_memory, INTERNAL_FLASH_SECTOR_SIZE) != 0) {
    // Starts being dangerous
    if (platform_flash_erase_sector(flash_offset / INTERNAL_FLASH_SECTOR_SIZE) != PLATFORM_OK) {
      luaM_free(L, buffer_base);
      return "Failed to erase sector";
    }
    if (platform_s_flash_write(buffer_base, flash_offset, INTERNAL_FLASH_SECTOR_SIZE) != INTERNAL_FLASH_SECTOR_SIZE) {
      luaM_free(L, buffer_base);
      return "Failed to write sector";
    }
    // ends being dangerous
  }

  luaM_free(L, buffer_base);

  return NULL;
}

// Lua: tls.cert.auth(PEM data [, PEM data] )
// Lua: tls.cert.auth(true / false)
static int tls_cert_auth(lua_State *L)
{
  if (ssl_client_options.cert_auth_callback != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, ssl_client_options.cert_auth_callback);
    ssl_client_options.cert_auth_callback = LUA_NOREF;
  }
  if (lua_isfunction(L, 1)) {
    ssl_client_options.cert_auth_callback = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushboolean(L, true);
    return 1;
  }
  if (lua_type(L, 1) != LUA_TNIL) {
    platform_print_deprecation_note("tls.cert.auth's old interface", "soon");
  }

  int enable;

  uint32_t flash_offset = platform_flash_mapped2phys((uint32_t) &tls_client_cert_area[0]);
  if ((flash_offset & 0xfff) || flash_offset > 0xff000 || INTERNAL_FLASH_SECTOR_SIZE != 0x1000) {
    // THis should never happen
    return luaL_error( L, "bad offset" );
  }

  if (lua_type(L, 1) == LUA_TSTRING) {
    const char *types[3] = { "CERTIFICATE", "RSA PRIVATE KEY", NULL };
    const char *names[2] = { "certificate", "private_key" };
    const char *error = fill_page_with_pem(L, &tls_client_cert_area[0], flash_offset, types, names);
    if (error) {
      return luaL_error(L, error);
    }

    enable = 1;
  } else {
    enable = lua_toboolean(L, 1);
  }

  bool rc;

  if (enable) {
    // See if there is a cert there
    if (tls_client_cert_area[0] == 0x00 || tls_client_cert_area[0] == 0xff) {
      return luaL_error( L, "no certificates found" );
    }
    rc = espconn_secure_cert_req_enable(ESPCONN_CLIENT, flash_offset / INTERNAL_FLASH_SECTOR_SIZE);
  } else {
    rc = espconn_secure_cert_req_disable(ESPCONN_CLIENT);
  }

  lua_pushboolean(L, rc);
  return 1;
}

// Lua: tls.cert.verify(PEM data [, PEM data] )
// Lua: tls.cert.verify(true / false)
static int tls_cert_verify(lua_State *L)
{
  if (ssl_client_options.cert_verify_callback != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, ssl_client_options.cert_verify_callback);
    ssl_client_options.cert_verify_callback = LUA_NOREF;
  }
  if (lua_isfunction(L, 1)) {
    ssl_client_options.cert_verify_callback = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushboolean(L, true);
    return 1;
  }
  if (lua_type(L, 1) != LUA_TNIL) {
    platform_print_deprecation_note("tls.cert.verify's old interface", "soon");
  }

  int enable;

  uint32_t flash_offset = platform_flash_mapped2phys((uint32_t) &tls_server_cert_area[0]);
  if ((flash_offset & 0xfff) || flash_offset > 0xff000 || INTERNAL_FLASH_SECTOR_SIZE != 0x1000) {
    // THis should never happen
    return luaL_error( L, "bad offset" );
  }

  if (lua_type(L, 1) == LUA_TSTRING) {
    const char *types[2] = { "CERTIFICATE", NULL };
    const char *names[1] = { "certificate" };
    const char *error = fill_page_with_pem(L, &tls_server_cert_area[0], flash_offset, types, names);
    if (error) {
      return luaL_error(L, error);
    }

    enable = 1;
  } else {
    enable = lua_toboolean(L, 1);
  }

  bool rc;

  if (enable) {
    // See if there is a cert there
    if (tls_server_cert_area[0] == 0x00 || tls_server_cert_area[0] == 0xff) {
      return luaL_error( L, "no certificates found" );
    }
    rc = espconn_secure_ca_enable(ESPCONN_CLIENT, flash_offset / INTERNAL_FLASH_SECTOR_SIZE);
  } else {
    rc = espconn_secure_ca_disable(ESPCONN_CLIENT);
  }

  lua_pushboolean(L, rc);
  return 1;
}

#if defined(MBEDTLS_DEBUG_C)
static int tls_set_debug_threshold(lua_State *L) {
  mbedtls_debug_set_threshold(luaL_checkint( L, 1 ));
  return 0;
}
#endif


LROT_BEGIN(tls_socket, NULL, LROT_MASK_GC_INDEX)
  LROT_FUNCENTRY( __gc, tls_socket_delete )
  LROT_TABENTRY(  __index, tls_socket )
  LROT_FUNCENTRY( connect, tls_socket_connect )
  LROT_FUNCENTRY( close, tls_socket_close )
  LROT_FUNCENTRY( on, tls_socket_on )
  LROT_FUNCENTRY( send, tls_socket_send )
  LROT_FUNCENTRY( hold, tls_socket_hold )
  LROT_FUNCENTRY( unhold, tls_socket_unhold )
  LROT_FUNCENTRY( getpeer, tls_socket_getpeer )
LROT_END(tls_socket, NULL, LROT_MASK_GC_INDEX)


LROT_BEGIN(tls_cert, NULL, LROT_MASK_INDEX)
  LROT_TABENTRY( __index, tls_cert )
  LROT_FUNCENTRY( verify, tls_cert_verify )
  LROT_FUNCENTRY( auth, tls_cert_auth )
LROT_END(tls_cert, NULL, LROT_MASK_INDEX)


LROT_BEGIN(tls, NULL, 0)
  LROT_FUNCENTRY( createConnection, tls_socket_create )
#if defined(MBEDTLS_DEBUG_C)
  LROT_FUNCENTRY( setDebug, tls_set_debug_threshold )
#endif
  LROT_TABENTRY( cert, tls_cert )
LROT_END(tls, NULL, 0)


int luaopen_tls( lua_State *L ) {
  luaL_rometatable(L, "tls.socket", LROT_TABLEREF(tls_socket));
  return 0;
}

NODEMCU_MODULE(TLS, "tls", tls, luaopen_tls);
#endif
