//
// FILE            wsTransport.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// ws.so - WebSocket (RFC 6455) as a transport of the broker's API (doc/websocket.md).
//
// What this plugin knows: the handshake, the frames, its connections. What it does not: the messages -
// each text message goes to the broker as it is (TransportHost.message), and the broker's answers and
// notifications come back as text to send (TransportDriver.send).
//
// Version 1: one thread per connection, blocking reads; text frames only (a binary one closes the
// connection, 1003); the subprotocol ngsi-ld.json, or none.
//
#include <errno.h>                                   // errno, EINTR
#include <fcntl.h>                                   // fcntl, O_NONBLOCK
#include <pthread.h>                                 // pthread_*
#include <stdbool.h>                                 // bool
#include <stdint.h>                                  // uint8_t, uint64_t
#include <stdio.h>                                   // snprintf
#include <stdlib.h>                                  // malloc, realloc, free
#include <string.h>                                  // memcpy, strlen, strstr, strncmp
#include <strings.h>                                 // strncasecmp
#include <sys/socket.h>                              // shutdown, setsockopt
#include <sys/time.h>                                // struct timeval
#include <time.h>                                    // nanosleep
#include <unistd.h>                                  // read, write

#include <openssl/evp.h>                             // EVP_EncodeBlock
#include <openssl/sha.h>                             // SHA1

#include "plugin/TransportDriver.h"                  // TransportDriver, TransportHost



#define WS_GUID          "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"   // RFC 6455 § 1.3
#define WS_MESSAGE_MAX   (16 * 1024 * 1024)                       // a message bigger than this closes the connection (1009)



// -----------------------------------------------------------------------------
//
// WsConn - one connection
//
typedef struct WsConn
{
  int            connId;
  int            fd;
  void         (*closeFn)(void*);
  void*          closeArg;
  char*          extra;                              // what the client sent behind the upgrade request - read first
  int            extraLen;
  int            extraPos;
  bool           closing;                            // a close frame sent: no more writes
  struct WsConn* next;
} WsConn;

static TransportHost*   hostP     = NULL;
static pthread_mutex_t  connMutex = PTHREAD_MUTEX_INITIALIZER;    // the list AND every write: one writer at a time
static WsConn*          connList  = NULL;
static int              connCount = 0;



// -----------------------------------------------------------------------------
//
// readFull - n bytes: first what came behind the request, then the socket
//
static bool readFull(WsConn* cP, uint8_t* buf, uint64_t n)
{
  uint64_t got = 0;

  while ((got < n) && (cP->extraPos < cP->extraLen))
    buf[got++] = (uint8_t) cP->extra[cP->extraPos++];

  while (got < n)
  {
    ssize_t r = read(cP->fd, &buf[got], n - got);

    if ((r < 0) && (errno == EINTR))
      continue;
    if (r <= 0)
      return false;

    got += (uint64_t) r;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// writeAll -
//
static bool writeAll(int fd, const uint8_t* buf, uint64_t n)
{
  uint64_t done = 0;

  while (done < n)
  {
    ssize_t w = write(fd, &buf[done], n - done);

    if ((w < 0) && (errno == EINTR))
      continue;
    if (w <= 0)
      return false;

    done += (uint64_t) w;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// frameWrite - one unfragmented frame, unmasked (a server never masks, § 5.1); under connMutex
//
static bool frameWrite(WsConn* cP, uint8_t opcode, const char* payload, uint64_t len)
{
  uint8_t hdr[10];
  int     hlen = 2;

  if (cP->closing)
    return false;

  hdr[0] = 0x80 | opcode;                            // FIN

  if (len < 126)
    hdr[1] = (uint8_t) len;
  else if (len < 65536)
  {
    hdr[1] = 126;
    hdr[2] = (uint8_t) (len >> 8);
    hdr[3] = (uint8_t) len;
    hlen   = 4;
  }
  else
  {
    hdr[1] = 127;
    for (int i = 0; i < 8; i++)
      hdr[2 + i] = (uint8_t) (len >> (56 - 8 * i));
    hlen = 10;
  }

  bool ok = writeAll(cP->fd, hdr, hlen) && ((len == 0) || writeAll(cP->fd, (const uint8_t*) payload, len));

  if (opcode == 0x8)
    cP->closing = true;

  return ok;
}



// -----------------------------------------------------------------------------
//
// closeWith - a close frame with this status (§ 7.4)
//
static void closeWith(WsConn* cP, int status)
{
  char payload[2] = { (char) (status >> 8), (char) status };

  pthread_mutex_lock(&connMutex);
  frameWrite(cP, 0x8, payload, 2);
  pthread_mutex_unlock(&connMutex);
}



// -----------------------------------------------------------------------------
//
// connThread - a connection's frames, read until it closes; its messages to the broker
//
static void* connThread(void* arg)
{
  WsConn*  cP      = (WsConn*) arg;
  char*    msg     = NULL;
  uint64_t msgLen  = 0;
  bool     inMsg   = false;

  hostP->opened(cP->connId);

  for (;;)
  {
    uint8_t  hdr[2];
    uint8_t  ext[8];
    uint8_t  mask[4];

    if (readFull(cP, hdr, 2) == false)
      break;

    bool     fin    = (hdr[0] & 0x80) != 0;
    uint8_t  opcode = hdr[0] & 0x0F;
    bool     masked = (hdr[1] & 0x80) != 0;
    uint64_t len    = hdr[1] & 0x7F;

    if (len == 126)
    {
      if (readFull(cP, ext, 2) == false)
        break;
      len = ((uint64_t) ext[0] << 8) | ext[1];
    }
    else if (len == 127)
    {
      if (readFull(cP, ext, 8) == false)
        break;
      len = 0;
      for (int i = 0; i < 8; i++)
        len = (len << 8) | ext[i];
    }

    if (masked == false)                             // § 5.1: a client masks every frame
    {
      closeWith(cP, 1002);
      break;
    }

    if ((len > WS_MESSAGE_MAX) || (msgLen + len > WS_MESSAGE_MAX))
    {
      closeWith(cP, 1009);
      break;
    }

    if (readFull(cP, mask, 4) == false)
      break;

    char* payload = (char*) malloc(len + 1);

    if ((payload == NULL) || (readFull(cP, (uint8_t*) payload, len) == false))
    {
      free(payload);
      break;
    }

    for (uint64_t i = 0; i < len; i++)
      payload[i] ^= (char) mask[i & 3];
    payload[len] = 0;

    if (opcode == 0x8)                               // close: answered with its status, and done (§ 5.5.1)
    {
      pthread_mutex_lock(&connMutex);
      frameWrite(cP, 0x8, payload, (len >= 2) ? 2 : 0);
      pthread_mutex_unlock(&connMutex);
      free(payload);
      break;
    }

    if (opcode == 0x9)                               // ping: pong, with its payload
    {
      pthread_mutex_lock(&connMutex);
      frameWrite(cP, 0xA, payload, len);
      pthread_mutex_unlock(&connMutex);
      free(payload);
      continue;
    }

    if (opcode == 0xA)                               // pong
    {
      free(payload);
      continue;
    }

    if (opcode == 0x2)                               // binary: not in version 1
    {
      free(payload);
      closeWith(cP, 1003);
      break;
    }

    if (((opcode == 0x1) && inMsg) || ((opcode == 0x0) && (inMsg == false)) || ((opcode != 0x0) && (opcode != 0x1)))
    {
      free(payload);
      closeWith(cP, 1002);
      break;
    }

    //
    // A text frame, or a continuation of one: the message, once complete, to the broker
    //
    char* grown = (char*) realloc(msg, msgLen + len + 1);

    if (grown == NULL)
    {
      free(payload);
      break;
    }

    msg = grown;
    memcpy(&msg[msgLen], payload, len);
    msgLen        += len;
    msg[msgLen]    = 0;
    inMsg          = true;
    free(payload);

    if (fin)
    {
      hostP->message(cP->connId, msg, (int) msgLen);
      free(msg);
      msg    = NULL;
      msgLen = 0;
      inMsg  = false;
    }
  }

  free(msg);

  //
  // Gone: the broker forgets it (and deletes its subscriptions), then the socket goes
  //
  hostP->closed(cP->connId);

  pthread_mutex_lock(&connMutex);

  WsConn* prevP = NULL;

  for (WsConn* p = connList; p != NULL; prevP = p, p = p->next)
  {
    if (p == cP)
    {
      if (prevP == NULL)
        connList = p->next;
      else
        prevP->next = p->next;
      --connCount;
      break;
    }
  }

  cP->closing = true;
  pthread_mutex_unlock(&connMutex);

  cP->closeFn(cP->closeArg);
  free(cP->extra);
  free(cP);

  return NULL;
}



// -----------------------------------------------------------------------------
//
// handshake - § 4.2.2: Sec-WebSocket-Accept = base64(SHA-1(key + GUID))
//
static bool handshake(const char* (*header)(const char* key), void (*respHeader)(const char* key, const char* value), int* statusP, const char** detailP)
{
  const char* key      = header("Sec-WebSocket-Key");
  const char* version  = header("Sec-WebSocket-Version");
  const char* protocol = header("Sec-WebSocket-Protocol");

  if ((key == NULL) || (strlen(key) > 64))
  {
    *statusP = 400;
    *detailP = "a WebSocket upgrade needs a Sec-WebSocket-Key";
    return false;
  }

  if ((version == NULL) || (strcmp(version, "13") != 0))
  {
    *statusP = 400;
    *detailP = "Sec-WebSocket-Version 13 only";
    return false;
  }

  //
  // The subprotocol: ngsi-ld.json, or none asked for (the same)
  //
  if (protocol != NULL)
  {
    if (strstr(protocol, "ngsi-ld.json") == NULL)
    {
      *statusP = 400;
      *detailP = "the subprotocol ngsi-ld.json only (Sec-WebSocket-Protocol)";
      return false;
    }

    respHeader("Sec-WebSocket-Protocol", "ngsi-ld.json");
  }

  char          keyGuid[128];
  unsigned char sha[SHA_DIGEST_LENGTH];
  unsigned char accept[64];

  snprintf(keyGuid, sizeof(keyGuid), "%s%s", key, WS_GUID);
  SHA1((const unsigned char*) keyGuid, strlen(keyGuid), sha);
  EVP_EncodeBlock(accept, sha, SHA_DIGEST_LENGTH);

  respHeader("Upgrade", "websocket");
  // The HTTP server owns the generic Connection: Upgrade response header.
  // Adding it here as well makes libmicrohttpd merge the two values into
  // "Upgrade, Upgrade", which strict browser clients reject as a bad
  // WebSocket handshake.
  respHeader("Sec-WebSocket-Accept", (const char*) accept);

  return true;
}



// -----------------------------------------------------------------------------
//
// take - the socket after the 101: blocking, a send timeout, a thread of its own
//
static bool take(int connId, int fd, const char* extra, int extraLen, void (*closeFn)(void*), void* closeArg)
{
  WsConn* cP = (WsConn*) calloc(1, sizeof(WsConn));

  if (cP == NULL)
  {
    closeFn(closeArg);
    return false;
  }

  cP->connId   = connId;
  cP->fd       = fd;
  cP->closeFn  = closeFn;
  cP->closeArg = closeArg;

  if (extraLen > 0)
  {
    cP->extra = (char*) malloc(extraLen);
    if (cP->extra != NULL)
    {
      memcpy(cP->extra, extra, extraLen);
      cP->extraLen = extraLen;
    }
  }

  //
  // Blocking, as this connection's thread reads it - and a send that waits for a client that stopped
  // reading gives up after 5 s instead of holding every other connection's notifications behind it
  //
  int flags = fcntl(fd, F_GETFL, 0);

  if (flags >= 0)
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

  struct timeval tv = { 5, 0 };

  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  pthread_mutex_lock(&connMutex);
  cP->next = connList;
  connList = cP;
  ++connCount;
  pthread_mutex_unlock(&connMutex);

  pthread_attr_t attr;
  pthread_t      tid;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  bool ok = (pthread_create(&tid, &attr, connThread, cP) == 0);

  pthread_attr_destroy(&attr);

  if (ok == false)
  {
    pthread_mutex_lock(&connMutex);
    connList = cP->next;
    --connCount;
    pthread_mutex_unlock(&connMutex);

    closeFn(closeArg);
    free(cP->extra);
    free(cP);
  }

  return ok;
}



// -----------------------------------------------------------------------------
//
// wsSend - a text message to a connection; any thread
//
static bool wsSend(int connId, const char* text, int len)
{
  bool ok = false;

  pthread_mutex_lock(&connMutex);

  for (WsConn* cP = connList; cP != NULL; cP = cP->next)
  {
    if (cP->connId == connId)
    {
      ok = frameWrite(cP, 0x1, text, (uint64_t) len);
      break;
    }
  }

  pthread_mutex_unlock(&connMutex);
  return ok;
}



// -----------------------------------------------------------------------------
//
// init / stop
//
static bool init(TransportHost* _hostP, char* errorBuf, int errorBufSize)
{
  (void) errorBuf;
  (void) errorBufSize;

  hostP = _hostP;
  return true;
}

static void stop(void)
{
  //
  // Every connection's read ends - its thread closes it (a going-away close frame first, § 7.4.1) and
  // tells the broker; waited for, 2 s at most
  //
  pthread_mutex_lock(&connMutex);

  for (WsConn* cP = connList; cP != NULL; cP = cP->next)
  {
    char payload[2] = { (char) (1001 >> 8), (char) (1001 & 0xFF) };

    frameWrite(cP, 0x8, payload, 2);
    shutdown(cP->fd, SHUT_RD);
  }

  pthread_mutex_unlock(&connMutex);

  for (int i = 0; i < 200; i++)
  {
    pthread_mutex_lock(&connMutex);
    int n = connCount;
    pthread_mutex_unlock(&connMutex);

    if (n == 0)
      break;

    struct timespec ts = { 0, 10 * 1000 * 1000 };
    nanosleep(&ts, NULL);
  }
}



// -----------------------------------------------------------------------------
//
// transportRegister - the one symbol the broker looks for
//
void transportRegister(TransportDriver* driverP)
{
  driverP->abiVersion = TRANSPORT_ABI_VERSION;
  driverP->name       = "ws";
  driverP->upgrade    = "websocket";
  driverP->init       = init;
  driverP->handshake  = handshake;
  driverP->take       = take;
  driverP->send       = wsSend;
  driverP->stop       = stop;
}
