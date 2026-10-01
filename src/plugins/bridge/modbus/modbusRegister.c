//
// FILE            modbusRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The Modbus TCP bridge - see doc/modbus-bridge.md.
//
// One Bridge is one Modbus server (host:port); one Channel is one value in it, and the endpoint IS
// the mapping, because a device says nothing about what its registers mean:
//
//   holding/40001?type=float32&order=CDAB&scale=0.01&unit=3&poll=1000&deadband=0.5
//   coil/12
//
// Modbus pushes nothing, so the plugin polls - one thread per Bridge, one request at a time - and
// reports a value only when it changed (beyond its deadband): a register read every second for an
// hour would otherwise be 3600 attribute writes, notifications and TRoE rows for one value.
//
// A write (publish) is queued and sent by the same thread: publish() runs on a broker thread inside
// the request that changed the attribute, and a Modbus round trip through a serial gateway can be
// 100 ms. Whether the device accepted it is logged - the seam has no way back for it yet
// (doc/modbus-bridge.md § 3.4).
//
// The configuration (the Bridge's member of the --bridgeConfig file):
//
//   "modbus": { "server": { "host": "10.0.0.5", "port": 502, "unit": 1, "pollMs": 1000, "timeoutMs": 500 },
//               "ngsild": { "topics": { "<endpoint>": { "entityId": ..., "entityType": ..., "attribute": ... } } } }
//
#include <pthread.h>                                  // pthread_*
#include <stdio.h>                                    // snprintf, fopen, fread
#include <stdlib.h>                                   // malloc, free, strtol, strtod
#include <string.h>                                   // strcmp, strdup, strncmp, strchr, memset, memcpy
#include <strings.h>                                  // strcasecmp
#include <stdint.h>                                   // uint8_t, uint16_t, uint32_t, uint64_t, int64_t
#include <stdbool.h>                                  // bool
#include <math.h>                                     // fabs
#include <errno.h>                                    // errno
#include <time.h>                                     // clock_gettime, nanosleep
#include <unistd.h>                                   // close
#include <netdb.h>                                    // getaddrinfo
#include <sys/socket.h>                               // socket, connect, send, recv, setsockopt
#include <sys/time.h>                                 // struct timeval
#include <netinet/in.h>                               // IPPROTO_TCP
#include <netinet/tcp.h>                              // TCP_NODELAY

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_V
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, BridgeRegisterFunc
#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*



// -----------------------------------------------------------------------------
//
// ModbusTable - the four tables a device exposes
//
typedef enum ModbusTable
{
  MbCoil,                                             // 1 bit, read/write    fc 1, 5
  MbDiscrete,                                         // 1 bit, read only     fc 2
  MbHolding,                                          // 16 bit, read/write   fc 3, 6, 16
  MbInput                                             // 16 bit, read only    fc 4
} ModbusTable;



// -----------------------------------------------------------------------------
//
// ModbusType - how the registers of one value are read
//
typedef enum ModbusType
{
  MbBool,
  MbInt16,
  MbUint16,
  MbInt32,
  MbUint32,
  MbFloat32,
  MbFloat64
} ModbusType;



// -----------------------------------------------------------------------------
//
// ModbusPoint - one Channel: where the value is, how to read it, and what was last reported
//
typedef struct ModbusPoint
{
  char*        endpoint;                              // as the broker names the Channel
  ModbusTable  table;
  int          address;                               // 0-based
  ModbusType   type;
  int          regs;                                  // registers (or bits: 1) the value takes
  char         order[5];                              // ABCD, CDAB, BADC, DCBA
  double       scale;
  double       offset;
  int          unit;
  int          pollMs;                                // 0: not polled (an outbound-only Channel)
  double       deadband;
  int64_t      dueMs;                                 // next poll, monotonic ms
  bool         reported;                              // a value has been reported
  double       last;                                  // ... and this was it
  char         lastJson[64];
  const char*  status;                                // "ok", "timeout", "illegalAddress", ... - last one reported
  uint64_t     generation;                            // unique per channelAdd: "is it still the same Channel?"
  bool         inUse;
} ModbusPoint;



// -----------------------------------------------------------------------------
//
// ModbusWrite - a queued write: the PDU is built at publish() time, sent by the I/O thread
//
typedef struct ModbusWrite
{
  char*                endpoint;
  int                  unit;
  uint8_t              pdu[260];
  int                  pduLen;
  struct ModbusWrite*  next;
} ModbusWrite;



// -----------------------------------------------------------------------------
//
// Module state
//
#define MODBUS_POINTS_MAX  512

static const BridgeBroker*  brokerP      = NULL;
static ModbusPoint          points[MODBUS_POINTS_MAX];
static pthread_mutex_t      mtx          = PTHREAD_MUTEX_INITIALIZER;
static ModbusWrite*         writeHead    = NULL;
static ModbusWrite*         writeTail    = NULL;
static pthread_t            ioThread;
static bool                 running      = false;
static int                  sock         = -1;
static uint16_t             transaction  = 0;
static uint64_t             generations  = 0;

static char                 host[256]    = "127.0.0.1";
static int                  port         = 502;
static int                  defaultUnit  = 1;
static int                  defaultPoll  = 1000;
static int                  timeoutMs    = 500;



// -----------------------------------------------------------------------------
//
// nowMs - monotonic milliseconds
//
static int64_t nowMs(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}



// -----------------------------------------------------------------------------
//
// nowNs - wall-clock nanoseconds, the time a value was read (Modbus has no timestamps)
//
static int64_t nowNs(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}



// -----------------------------------------------------------------------------
//
// typeFromName -
//
static bool typeFromName(const char* name, int nameLen, ModbusType* typeP, int* regsP)
{
  static const struct { const char* name; ModbusType type; int regs; } types[] =
  {
    { "bool",    MbBool,    1 },
    { "int16",   MbInt16,   1 },
    { "uint16",  MbUint16,  1 },
    { "int32",   MbInt32,   2 },
    { "uint32",  MbUint32,  2 },
    { "float32", MbFloat32, 2 },
    { "float64", MbFloat64, 4 }
  };

  for (unsigned int ix = 0; ix < sizeof(types) / sizeof(types[0]); ix++)
  {
    if (((int) strlen(types[ix].name) == nameLen) && (strncmp(types[ix].name, name, nameLen) == 0))
    {
      *typeP = types[ix].type;
      *regsP = types[ix].regs;
      return true;
    }
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// pointParse - an endpoint into a ModbusPoint; false (and logged) if it is not one
//
// table/address[?key=value&...] - the address is 0-based, or the 1-based 0xxxx/1xxxx/3xxxx/4xxxx form
// of the device manuals, recognised by its size (an address >= 10001 in its table's range).
//
static bool pointParse(const char* endpoint, ModbusPoint* pP)
{
  memset(pP, 0, sizeof(*pP));
  pP->scale    = 1;
  pP->unit     = defaultUnit;
  pP->pollMs   = defaultPoll;
  strcpy(pP->order, "ABCD");

  const char* slash = strchr(endpoint, '/');
  if (slash == NULL)
    return false;

  int tableLen = slash - endpoint;
  if      ((tableLen == 4) && (strncmp(endpoint, "coil", 4) == 0))      pP->table = MbCoil;
  else if ((tableLen == 8) && (strncmp(endpoint, "discrete", 8) == 0))  pP->table = MbDiscrete;
  else if ((tableLen == 7) && (strncmp(endpoint, "holding", 7) == 0))   pP->table = MbHolding;
  else if ((tableLen == 5) && (strncmp(endpoint, "input", 5) == 0))     pP->table = MbInput;
  else
    return false;

  char* end;
  long  address = strtol(slash + 1, &end, 10);
  if ((end == slash + 1) || (address < 0) || ((*end != 0) && (*end != '?')))
    return false;

  //
  // The manuals' 1-based form: 00001-09999 coils, 10001-19999 discrete inputs, 30001-39999 input
  // registers, 40001-49999 holding registers (and the 6-digit 400001 form)
  //
  if      ((pP->table == MbHolding)  && (address >= 400001) && (address <= 465536)) address -= 400001;
  else if ((pP->table == MbHolding)  && (address >= 40001)  && (address <= 49999))  address -= 40001;
  else if ((pP->table == MbInput)    && (address >= 300001) && (address <= 365536)) address -= 300001;
  else if ((pP->table == MbInput)    && (address >= 30001)  && (address <= 39999))  address -= 30001;
  else if ((pP->table == MbDiscrete) && (address >= 10001)  && (address <= 19999))  address -= 10001;

  if (address > 65535)
    return false;

  pP->address = (int) address;

  if ((pP->table == MbCoil) || (pP->table == MbDiscrete))
  {
    pP->type = MbBool;
    pP->regs = 1;
  }
  else
  {
    pP->type = MbUint16;
    pP->regs = 1;
  }

  for (const char* p = (*end == '?') ? end + 1 : end; *p != 0; )
  {
    const char* eq  = strchr(p, '=');
    const char* amp = strchr(p, '&');
    if (amp == NULL)
      amp = p + strlen(p);
    if ((eq == NULL) || (eq > amp))
      return false;

    int         keyLen = eq - p;
    const char* val    = eq + 1;
    int         valLen = amp - val;

    if ((keyLen == 4) && (strncmp(p, "type", 4) == 0))
    {
      if ((typeFromName(val, valLen, &pP->type, &pP->regs) == false) ||
          (((pP->table == MbCoil) || (pP->table == MbDiscrete)) != (pP->type == MbBool)))
        return false;                                  // a bit is a bool, a register is not
    }
    else if ((keyLen == 5) && (strncmp(p, "order", 5) == 0))
    {
      if ((valLen != 4) || ((strncmp(val, "ABCD", 4) != 0) && (strncmp(val, "CDAB", 4) != 0) &&
                            (strncmp(val, "BADC", 4) != 0) && (strncmp(val, "DCBA", 4) != 0)))
        return false;
      memcpy(pP->order, val, 4);
      pP->order[4] = 0;
    }
    else if ((keyLen == 5) && (strncmp(p, "scale", 5) == 0))     pP->scale    = strtod(val, NULL);
    else if ((keyLen == 6) && (strncmp(p, "offset", 6) == 0))    pP->offset   = strtod(val, NULL);
    else if ((keyLen == 4) && (strncmp(p, "unit", 4) == 0))      pP->unit     = (int) strtol(val, NULL, 10);
    else if ((keyLen == 4) && (strncmp(p, "poll", 4) == 0))      pP->pollMs   = (int) strtol(val, NULL, 10);
    else if ((keyLen == 8) && (strncmp(p, "deadband", 8) == 0))  pP->deadband = strtod(val, NULL);
    else
      return false;                                    // an unknown key is a mistake, not a default

    p = (*amp == '&') ? amp + 1 : amp;
  }

  if ((pP->scale == 0) || (pP->unit < 0) || (pP->unit > 255) || (pP->pollMs < 0) || (pP->address + pP->regs > 65536))
    return false;

  pP->endpoint = strdup(endpoint);
  pP->status   = NULL;
  return true;
}



// -----------------------------------------------------------------------------
//
// wordsToBytes - the registers of a value, in the byte order ABCD (A = most significant) says
//
// A Modbus register is big-endian on the wire (A B). Multi-register values come in four orders:
// ABCD (big-endian words, big-endian bytes), CDAB (word-swapped - the common "Modicon" order), BADC
// (byte-swapped), DCBA (little-endian). For 4-register float64 the pattern repeats per 32 bits.
//
static void wordsToBytes(const uint8_t* wire, int regs, const char* order, uint8_t* out)
{
  for (int half = 0; half < regs; half += 2)
  {
    const uint8_t* w = &wire[half * 2];
    uint8_t*       o = &out[half * 2];

    if (regs == 1)
    {
      o[0] = w[0];
      o[1] = w[1];
      return;
    }

    if      (strcmp(order, "ABCD") == 0) { o[0] = w[0]; o[1] = w[1]; o[2] = w[2]; o[3] = w[3]; }
    else if (strcmp(order, "CDAB") == 0) { o[0] = w[2]; o[1] = w[3]; o[2] = w[0]; o[3] = w[1]; }
    else if (strcmp(order, "BADC") == 0) { o[0] = w[1]; o[1] = w[0]; o[2] = w[3]; o[3] = w[2]; }
    else                                 { o[0] = w[3]; o[1] = w[2]; o[2] = w[1]; o[3] = w[0]; }
  }

  //
  // float64 in CDAB etc: the two 32-bit halves were each reordered above; which half comes first is
  // the same question once more - for CDAB and DCBA the second half is the most significant.
  //
  if ((regs == 4) && ((strcmp(order, "CDAB") == 0) || (strcmp(order, "DCBA") == 0)))
  {
    uint8_t tmp[4];
    memcpy(tmp, out, 4);
    memcpy(out, out + 4, 4);
    memcpy(out + 4, tmp, 4);
  }
}



// -----------------------------------------------------------------------------
//
// bytesToWords - the inverse of wordsToBytes, for a write
//
static void bytesToWords(const uint8_t* value, int regs, const char* order, uint8_t* wire)
{
  uint8_t v[8];
  memcpy(v, value, regs * 2);

  if ((regs == 4) && ((strcmp(order, "CDAB") == 0) || (strcmp(order, "DCBA") == 0)))
  {
    uint8_t tmp[4];
    memcpy(tmp, v, 4);
    memcpy(v, v + 4, 4);
    memcpy(v + 4, tmp, 4);
  }

  for (int half = 0; half < regs; half += 2)
  {
    const uint8_t* o = &v[half * 2];
    uint8_t*       w = &wire[half * 2];

    if (regs == 1)
    {
      w[0] = o[0];
      w[1] = o[1];
      return;
    }

    // Each of the four orders is its own inverse
    if      (strcmp(order, "ABCD") == 0) { w[0] = o[0]; w[1] = o[1]; w[2] = o[2]; w[3] = o[3]; }
    else if (strcmp(order, "CDAB") == 0) { w[0] = o[2]; w[1] = o[3]; w[2] = o[0]; w[3] = o[1]; }
    else if (strcmp(order, "BADC") == 0) { w[0] = o[1]; w[1] = o[0]; w[2] = o[3]; w[3] = o[2]; }
    else                                 { w[0] = o[3]; w[1] = o[2]; w[2] = o[1]; w[3] = o[0]; }
  }
}



// -----------------------------------------------------------------------------
//
// decode - the raw bytes of a value (most significant first) into a number
//
static double decode(const uint8_t* b, ModbusType type)
{
  switch (type)
  {
  case MbBool:    return b[0] ? 1 : 0;
  case MbInt16:   return (int16_t)  ((b[0] << 8) | b[1]);
  case MbUint16:  return (uint16_t) ((b[0] << 8) | b[1]);
  case MbInt32:   return (int32_t)  (((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | b[3]);
  case MbUint32:  return (uint32_t) (((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | b[3]);
  case MbFloat32:
  {
    uint32_t u = ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | b[3];
    float    f;
    memcpy(&f, &u, 4);
    return f;
  }
  case MbFloat64:
  {
    uint64_t u = 0;
    for (int ix = 0; ix < 8; ix++)
      u = (u << 8) | b[ix];
    double d;
    memcpy(&d, &u, 8);
    return d;
  }
  }

  return 0;
}



// -----------------------------------------------------------------------------
//
// encode - a number into the raw bytes of a value (most significant first); false if it does not fit
//
static bool encode(double v, ModbusType type, uint8_t* b)
{
  switch (type)
  {
  case MbBool:
    b[0] = (v != 0) ? 1 : 0;
    return true;

  case MbInt16:
  case MbUint16:
  {
    if ((type == MbInt16) ? ((v < -32768) || (v > 32767)) : ((v < 0) || (v > 65535)))
      return false;
    uint16_t u = (type == MbInt16) ? (uint16_t) (int16_t) v : (uint16_t) v;
    b[0] = u >> 8;
    b[1] = u & 0xFF;
    return true;
  }

  case MbInt32:
  case MbUint32:
  case MbFloat32:
  {
    uint32_t u;
    if (type == MbFloat32)
    {
      float f = (float) v;
      memcpy(&u, &f, 4);
    }
    else
    {
      if ((type == MbInt32) ? ((v < -2147483648.0) || (v > 2147483647.0)) : ((v < 0) || (v > 4294967295.0)))
        return false;
      u = (type == MbInt32) ? (uint32_t) (int32_t) v : (uint32_t) v;
    }
    b[0] = u >> 24; b[1] = (u >> 16) & 0xFF; b[2] = (u >> 8) & 0xFF; b[3] = u & 0xFF;
    return true;
  }

  case MbFloat64:
  {
    uint64_t u;
    memcpy(&u, &v, 8);
    for (int ix = 7; ix >= 0; ix--)
    {
      b[ix] = u & 0xFF;
      u   >>= 8;
    }
    return true;
  }
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// connectServer - (re)connect, with send and receive timeouts; false if the server is not there
//
static bool connectServer(void)
{
  if (sock >= 0)
    return true;

  char             portString[16];
  struct addrinfo  hints;
  struct addrinfo* resP = NULL;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(portString, sizeof(portString), "%d", port);

  if (getaddrinfo(host, portString, &hints, &resP) != 0)
    return false;

  for (struct addrinfo* aP = resP; aP != NULL; aP = aP->ai_next)
  {
    int fd = socket(aP->ai_family, aP->ai_socktype, aP->ai_protocol);
    if (fd < 0)
      continue;

    struct timeval tv = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    int            one = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (connect(fd, aP->ai_addr, aP->ai_addrlen) == 0)
    {
      sock = fd;
      break;
    }
    close(fd);
  }

  freeaddrinfo(resP);
  return sock >= 0;
}



// -----------------------------------------------------------------------------
//
// disconnect -
//
static void disconnect(void)
{
  if (sock >= 0)
    close(sock);
  sock = -1;
}



// -----------------------------------------------------------------------------
//
// recvAll - exactly n bytes, or false (timeout, closed)
//
static bool recvAll(uint8_t* buf, int n)
{
  int got = 0;

  while (got < n)
  {
    int r = recv(sock, buf + got, n - got, 0);
    if (r <= 0)
      return false;
    got += r;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// transact - one request, one response: the status ("ok", "timeout", "illegalAddress", ...)
//
// pdu in, the response PDU (function code onwards) out. On anything but a well-formed response the
// connection is dropped - a Modbus TCP stream that lost its framing cannot be trusted again - and
// the next request reconnects.
//
static const char* transact(int unit, const uint8_t* pdu, int pduLen, uint8_t* resp, int* respLenP)
{
  static const char* exceptionName[] =
  {
    "exception0", "illegalFunction", "illegalAddress", "illegalValue", "deviceFailure",
    "acknowledge", "deviceBusy", "exception7", "memoryParityError", "exception9",
    "gatewayPathUnavailable", "gatewayTargetFailed"
  };

  if (connectServer() == false)
    return "unreachable";

  uint8_t  frame[7 + 260];
  uint16_t tid = ++transaction;

  frame[0] = tid >> 8;
  frame[1] = tid & 0xFF;
  frame[2] = 0;                                       // protocol id: Modbus
  frame[3] = 0;
  frame[4] = (pduLen + 1) >> 8;                       // length: unit id + PDU
  frame[5] = (pduLen + 1) & 0xFF;
  frame[6] = (uint8_t) unit;
  memcpy(&frame[7], pdu, pduLen);

  if (send(sock, frame, 7 + pduLen, MSG_NOSIGNAL) != 7 + pduLen)
  {
    disconnect();
    return "unreachable";
  }

  uint8_t header[7];
  if (recvAll(header, 7) == false)
  {
    disconnect();
    return "timeout";
  }

  int len = (header[4] << 8) | header[5];
  if ((((header[0] << 8) | header[1]) != tid) || (len < 2) || (len > 254))
  {
    disconnect();
    return "badResponse";
  }

  if (recvAll(resp, len - 1) == false)
  {
    disconnect();
    return "timeout";
  }

  *respLenP = len - 1;

  if (resp[0] == (pdu[0] | 0x80))
    return (resp[1] < sizeof(exceptionName) / sizeof(exceptionName[0])) ? exceptionName[resp[1]] : "exception";

  if (resp[0] != pdu[0])
  {
    disconnect();
    return "badResponse";
  }

  return "ok";
}



// -----------------------------------------------------------------------------
//
// pointRead - poll one point: "ok" and the value as JSON, or the reason it could not be read
//
static const char* pointRead(ModbusPoint* pP, double* valueP, char* json, int jsonSize)
{
  static const uint8_t readFc[] = { 1, 2, 3, 4 };    // by ModbusTable
  uint8_t              pdu[5];
  uint8_t              resp[260];
  int                  respLen = 0;
  int                  count   = ((pP->table == MbCoil) || (pP->table == MbDiscrete)) ? 1 : pP->regs;

  pdu[0] = readFc[pP->table];
  pdu[1] = pP->address >> 8;
  pdu[2] = pP->address & 0xFF;
  pdu[3] = count >> 8;
  pdu[4] = count & 0xFF;

  const char* status = transact(pP->unit, pdu, 5, resp, &respLen);
  if (strcmp(status, "ok") != 0)
    return status;

  uint8_t bytes[8];
  if ((pP->table == MbCoil) || (pP->table == MbDiscrete))
  {
    if ((respLen < 3) || (resp[1] < 1))
      return "badResponse";
    bytes[0] = resp[2] & 1;
  }
  else
  {
    if ((respLen < 2 + count * 2) || (resp[1] != count * 2))
      return "badResponse";
    wordsToBytes(&resp[2], count, pP->order, bytes);
  }

  double raw = decode(bytes, pP->type);

  if (pP->type == MbBool)
  {
    *valueP = raw;
    snprintf(json, jsonSize, "%s", (raw != 0) ? "true" : "false");
  }
  else
  {
    *valueP = raw * pP->scale + pP->offset;
    snprintf(json, jsonSize, "%.15g", *valueP);
  }

  return "ok";
}



// -----------------------------------------------------------------------------
//
// Upcall - what report() decided to tell the broker, made AFTER the lock is released
//
// The broker's sampleIn does the NGSI-LD work - an attribute write, notifications - and may call back
// into the plugin (a publish) on this same thread; doing that with the mutex held would deadlock.
//
typedef struct Upcall
{
  bool  send;
  char  endpoint[512];
  char  json[64];
  char  meta[96];                                     // empty: sampleIn, else sampleMetaIn
} Upcall;



// -----------------------------------------------------------------------------
//
// report - decide what a poll (value or failure) has to tell the broker, if anything; under the lock
//
// A value goes in when it differs from the last one reported by more than the deadband. A failure
// is said once, when the status changes - as meta (modbusStatus) on the last known value, which is
// still the best thing anybody has. Back to "ok", the value that came back says so in the same way.
//
static void report(ModbusPoint* pP, const char* status, double value, const char* json, Upcall* upP)
{
  bool ok            = (strcmp(status, "ok") == 0);
  bool statusChanged = (pP->status == NULL) ? (ok == false) : (strcmp(pP->status, status) != 0);
  bool metaPossible  = (brokerP->abiVersion >= 6) && (brokerP->sampleMetaIn != NULL);

  upP->send    = false;
  upP->meta[0] = 0;
  snprintf(upP->endpoint, sizeof(upP->endpoint), "%s", pP->endpoint);

  if (ok == true)
  {
    bool changed = (pP->reported == false) || (fabs(value - pP->last) > pP->deadband) ||
                   ((pP->deadband == 0) && (value != pP->last));

    if ((changed == false) && (statusChanged == false))
      return;

    if (changed == true)
    {
      pP->last     = value;
      pP->reported = true;
      snprintf(pP->lastJson, sizeof(pP->lastJson), "%s", json);
    }

    upP->send = true;
    snprintf(upP->json, sizeof(upP->json), "%s", pP->lastJson);
    if ((statusChanged == true) && (metaPossible == true))
      snprintf(upP->meta, sizeof(upP->meta), "{\"modbusStatus\":\"ok\"}");

    pP->status = "ok";
    return;
  }

  if (statusChanged == false)
    return;

  pP->status = status;
  COR_W("modbus: %s: %s", pP->endpoint, status);

  if ((pP->reported == true) && (metaPossible == true))
  {
    upP->send = true;
    snprintf(upP->json, sizeof(upP->json), "%s", pP->lastJson);
    snprintf(upP->meta, sizeof(upP->meta), "{\"modbusStatus\":\"%s\"}", status);
  }
}



// -----------------------------------------------------------------------------
//
// upcall - what report() decided, now that the lock is released
//
static void upcall(const Upcall* upP)
{
  if (upP->send == false)
    return;

  if (upP->meta[0] != 0)
    brokerP->sampleMetaIn("modbus", upP->endpoint, upP->json, upP->meta, nowNs());
  else
    brokerP->sampleIn("modbus", upP->endpoint, upP->json, nowNs());
}



// -----------------------------------------------------------------------------
//
// ioLoop - the Bridge's one thread: queued writes first, then whatever polls are due
//
static void* ioLoop(void* vP)
{
  (void) vP;

  while (running == true)
  {
    //
    // Writes - the client is waiting for its value to reach the device, a poll can wait
    //
    while (true)
    {
      pthread_mutex_lock(&mtx);
      ModbusWrite* wP = writeHead;
      if (wP != NULL)
      {
        writeHead = wP->next;
        if (writeHead == NULL)
          writeTail = NULL;
      }
      pthread_mutex_unlock(&mtx);

      if (wP == NULL)
        break;

      uint8_t     resp[260];
      int         respLen = 0;
      const char* status  = transact(wP->unit, wP->pdu, wP->pduLen, resp, &respLen);

      if (strcmp(status, "ok") != 0)
        COR_W("modbus: write to %s: %s", wP->endpoint, status);

      free(wP->endpoint);
      free(wP);
    }

    //
    // Polls - the points that are due, one request each (v1; batching contiguous registers of one
    // unit into one read is the next step - see doc/modbus-bridge.md § 2.2)
    //
    int64_t now      = nowMs();
    int64_t nextDue  = now + 50;

    for (int ix = 0; ix < MODBUS_POINTS_MAX; ix++)
    {
      pthread_mutex_lock(&mtx);
      bool due = points[ix].inUse && (points[ix].pollMs > 0) && (points[ix].dueMs <= now);
      ModbusPoint point;
      if (due)
        point = points[ix];
      pthread_mutex_unlock(&mtx);

      if (due == false)
        continue;

      double      value = 0;
      char        json[64];
      const char* status = pointRead(&point, &value, json, sizeof(json));

      Upcall up = { .send = false };

      pthread_mutex_lock(&mtx);
      if (points[ix].inUse && (points[ix].generation == point.generation))   // not deleted (or replaced) meanwhile
      {
        report(&points[ix], status, value, json, &up);
        points[ix].dueMs = nowMs() + points[ix].pollMs;
        if (points[ix].dueMs < nextDue)
          nextDue = points[ix].dueMs;
      }
      pthread_mutex_unlock(&mtx);

      upcall(&up);
    }

    int64_t sleepMs = nextDue - nowMs();
    if (sleepMs > 50)
      sleepMs = 50;                                    // writes are picked up within 50 ms
    if (sleepMs > 0)
    {
      struct timespec ts = { 0, sleepMs * 1000000 };
      nanosleep(&ts, NULL);
    }
  }

  disconnect();
  return NULL;
}



// -----------------------------------------------------------------------------
//
// configRead - the plugin's part of the Bridge configuration ("server")
//
static void configRead(const char* configFile)
{
  if (configFile == NULL)
    return;

  FILE* fP = fopen(configFile, "r");
  if (fP == NULL)
    return;

  static char text[64 * 1024];
  size_t      n = fread(text, 1, sizeof(text) - 1, fP);
  fclose(fP);
  text[n] = 0;

  CorAlloc ka;
  CorJson  cj;
  char     buf[16 * 1024];

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "modbus");
  corJsonCreate(&cj, &ka);

  CorNode* rootP   = corJsonParse(&cj, text);
  CorNode* bridgeP = (rootP != NULL) ? corTreeLookup(rootP, "modbus") : NULL;
  CorNode* serverP = (bridgeP != NULL) ? corTreeLookup(bridgeP, "server") : NULL;

  if (serverP != NULL)
  {
    CorNode* nP;

    if (((nP = corTreeLookup(serverP, "host"))      != NULL) && (nP->type == CorString)) snprintf(host, sizeof(host), "%s", nP->value.s);
    if (((nP = corTreeLookup(serverP, "port"))      != NULL) && (nP->type == CorInt))    port        = (int) nP->value.i;
    if (((nP = corTreeLookup(serverP, "unit"))      != NULL) && (nP->type == CorInt))    defaultUnit = (int) nP->value.i;
    if (((nP = corTreeLookup(serverP, "pollMs"))    != NULL) && (nP->type == CorInt))    defaultPoll = (int) nP->value.i;
    if (((nP = corTreeLookup(serverP, "timeoutMs")) != NULL) && (nP->type == CorInt))    timeoutMs   = (int) nP->value.i;
  }

  corAllocBufferReset(&ka, false);
}



// -----------------------------------------------------------------------------
//
// modbusInit -
//
static int modbusInit(const char* configFile, const BridgeBroker* _brokerP)
{
  brokerP = _brokerP;
  configRead(configFile);

  running = true;
  if (pthread_create(&ioThread, NULL, ioLoop, NULL) != 0)
  {
    running = false;
    COR_E("modbus: cannot start the I/O thread");
    return BRIDGE_ERR;
  }

  COR_V("modbus: server %s:%d, unit %d, poll %d ms, timeout %d ms", host, port, defaultUnit, defaultPoll, timeoutMs);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// modbusClose -
//
static void modbusClose(void)
{
  if (running == true)
  {
    running = false;
    pthread_join(ioThread, NULL);
  }

  pthread_mutex_lock(&mtx);
  for (int ix = 0; ix < MODBUS_POINTS_MAX; ix++)
  {
    if (points[ix].inUse)
      free(points[ix].endpoint);
    points[ix].inUse = false;
  }
  while (writeHead != NULL)
  {
    ModbusWrite* wP = writeHead;
    writeHead = wP->next;
    free(wP->endpoint);
    free(wP);
  }
  writeTail = NULL;
  pthread_mutex_unlock(&mtx);
}



// -----------------------------------------------------------------------------
//
// modbusChannelAdd -
//
static int modbusChannelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction)
{
  if (kind != BridgeChannelTopic)
    return BRIDGE_UNSUPPORTED;                          // a read-on-demand Service is v2 (doc § 3.5)

  ModbusPoint point;
  if (pointParse(endpoint, &point) == false)
  {
    COR_E("modbus: '%s' is not a Modbus endpoint (table/address?type=...&order=...&scale=...)", endpoint);
    return BRIDGE_BAD_INPUT;
  }

  if (direction == BridgeDirectionOut)
    point.pollMs = 0;

  pthread_mutex_lock(&mtx);
  int free_ = -1;
  for (int ix = 0; ix < MODBUS_POINTS_MAX; ix++)
  {
    if (points[ix].inUse && (strcmp(points[ix].endpoint, endpoint) == 0))
    {
      pthread_mutex_unlock(&mtx);
      free(point.endpoint);
      return BRIDGE_OK;                                // already carried
    }
    if ((free_ == -1) && (points[ix].inUse == false))
      free_ = ix;
  }

  if (free_ == -1)
  {
    pthread_mutex_unlock(&mtx);
    free(point.endpoint);
    COR_E("modbus: more than %d endpoints", MODBUS_POINTS_MAX);
    return BRIDGE_ERR;
  }

  point.inUse      = true;
  point.dueMs      = nowMs();
  point.generation = ++generations;
  points[free_] = point;
  pthread_mutex_unlock(&mtx);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// modbusChannelDel -
//
static int modbusChannelDel(const char* endpoint)
{
  pthread_mutex_lock(&mtx);
  for (int ix = 0; ix < MODBUS_POINTS_MAX; ix++)
  {
    if (points[ix].inUse && (strcmp(points[ix].endpoint, endpoint) == 0))
    {
      free(points[ix].endpoint);
      points[ix].inUse = false;
      pthread_mutex_unlock(&mtx);
      return BRIDGE_OK;
    }
  }
  pthread_mutex_unlock(&mtx);
  return BRIDGE_NOT_FOUND;
}



// -----------------------------------------------------------------------------
//
// modbusPublish - an attribute was written: queue the write to the device
//
// The JSON is the attribute's value: true/false for a coil, a number for a holding register (the
// inverse of the read: (value - offset) / scale, then the type and the word order). Anything else -
// or a read-only table - is BRIDGE_BAD_INPUT, which the broker answers at once.
//
static int modbusPublish(const char* endpoint, const char* json)
{
  pthread_mutex_lock(&mtx);
  ModbusPoint point;
  bool        found = false;
  for (int ix = 0; ix < MODBUS_POINTS_MAX; ix++)
  {
    if (points[ix].inUse && (strcmp(points[ix].endpoint, endpoint) == 0))
    {
      point = points[ix];
      found = true;
      break;
    }
  }
  pthread_mutex_unlock(&mtx);

  if (found == false)
    return BRIDGE_NOT_FOUND;

  if ((point.table == MbDiscrete) || (point.table == MbInput))
    return BRIDGE_BAD_INPUT;                           // read-only tables

  while ((*json == ' ') || (*json == '\t') || (*json == '\n') || (*json == '\r'))
    ++json;

  double v;
  if (strncmp(json, "true", 4) == 0)
    v = 1;
  else if (strncmp(json, "false", 5) == 0)
    v = 0;
  else
  {
    char* end;
    v = strtod(json, &end);
    if (end == json)
      return BRIDGE_BAD_INPUT;
  }

  ModbusWrite* wP = (ModbusWrite*) calloc(1, sizeof(ModbusWrite));
  if (wP == NULL)
    return BRIDGE_ERR;

  wP->unit = point.unit;

  if (point.table == MbCoil)
  {
    wP->pdu[0] = 5;                                    // write single coil: FF00 = on, 0000 = off
    wP->pdu[1] = point.address >> 8;
    wP->pdu[2] = point.address & 0xFF;
    wP->pdu[3] = (v != 0) ? 0xFF : 0x00;
    wP->pdu[4] = 0x00;
    wP->pduLen = 5;
  }
  else
  {
    uint8_t bytes[8];
    if (encode((point.type == MbBool) ? v : (v - point.offset) / point.scale, point.type, bytes) == false)
    {
      free(wP);
      return BRIDGE_BAD_INPUT;
    }

    if (point.regs == 1)
    {
      wP->pdu[0] = 6;                                  // write single register
      wP->pdu[1] = point.address >> 8;
      wP->pdu[2] = point.address & 0xFF;
      bytesToWords(bytes, 1, point.order, &wP->pdu[3]);
      wP->pduLen = 5;
    }
    else
    {
      wP->pdu[0] = 16;                                 // write multiple registers
      wP->pdu[1] = point.address >> 8;
      wP->pdu[2] = point.address & 0xFF;
      wP->pdu[3] = 0;
      wP->pdu[4] = point.regs;
      wP->pdu[5] = point.regs * 2;
      bytesToWords(bytes, point.regs, point.order, &wP->pdu[6]);
      wP->pduLen = 6 + point.regs * 2;
    }
  }

  wP->endpoint = strdup(endpoint);

  pthread_mutex_lock(&mtx);
  if (writeTail != NULL)
    writeTail->next = wP;
  else
    writeHead = wP;
  writeTail = wP;
  pthread_mutex_unlock(&mtx);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// modbusVersionInfo -
//
static const char* modbusVersionInfo(void)
{
  return "modbus 0.1.0 (Modbus TCP, built in - no library)";
}



// -----------------------------------------------------------------------------
//
// bridgeRegister - the one symbol the broker looks for
//
void bridgeRegister(BridgeDriver* driverP)
{
  driverP->alias       = "modbus";
  driverP->version     = "0.1.0";
  driverP->args        = NULL;
  driverP->init        = modbusInit;
  driverP->close       = modbusClose;
  driverP->channelAdd  = modbusChannelAdd;
  driverP->channelDel  = modbusChannelDel;
  driverP->publish     = modbusPublish;
  driverP->versionInfo = modbusVersionInfo;
  driverP->abiVersion  = BRIDGE_ABI_VERSION;
}
