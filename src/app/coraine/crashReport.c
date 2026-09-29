//
// FILE            crashReport.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// What a crash report is for: to REPRODUCE the crash. A stack says where it died; what makes it
// reproducible is what the broker was doing - the request (with its payload) and the
// configuration. So that is what this writes, to the log and to stderr (the old handler wrote a
// stack to stderr alone, and only for SIGSEGV).
//
// Everything below runs in a signal handler, after something has gone badly wrong - possibly a
// corrupted heap. So: no malloc, no stdio, nothing that takes a lock (not corLog's mutex either: the
// crashing thread may hold it). writev(2), strlen,
// backtrace_symbols_fd (it writes, it does not allocate), and integers formatted by hand.
//
#include <execinfo.h>                                // backtrace, backtrace_symbols_fd
#include <signal.h>                                  // sigaction, raise, SIG*
#include <stdbool.h>                                 // bool
#include <string.h>                                  // strlen, memset
#include <sys/uio.h>                                 // writev, struct iovec

#include "corLog/corLogGlobals.h"                    // corLogFd
#include "corRest/CorRestState.h"                    // corRestP, CorRestState

#include "coraineVersion.h"                          // CORAINE_VERSION
#include "crashReport.h"                             // Own interface



// -----------------------------------------------------------------------------
//
// The command line, kept from main (argv outlives every crash)
//
static int    crashArgC  = 0;
static char** crashArgV  = NULL;

#define PAYLOAD_MAX  (64 * 1024)                     // more is truncated, and says so



// -----------------------------------------------------------------------------
//
// out - gathered into an iovec, written with ONE writev per line (flush) - the way corLog writes a
// trace line: a line of the report is not interleaved with the trace lines other threads are
// writing meanwhile.
//
// Where: the log, and stderr. But a broker logging to stdout (--foreground) writes the report to
// stdout ONLY - a container runtime, journald and the test harness all merge stdout and stderr,
// and two copies, woven together line by line, are what that capture then held.
//
// The pieces must live until the flush: strings do (argv, the request state, literals); numbers
// are formatted into numberV, one slot per number, reused after each flush.
//
#define IOV_MAX_ITEMS  64
#define NUMBERS_MAX    8

static struct iovec iov[IOV_MAX_ITEMS];
static int          iovN = 0;
static char         numberV[NUMBERS_MAX][24];
static int          numberN = 0;

static bool logToFile(void)
{
  return corLogFd > 2;                         // a log file: that, and stderr too
}

static int reportFd(void)
{
  return ((corLogFd == 1) || (corLogFd == 2)) ? corLogFd : 2;   // logging to stdout/stderr: there only
}

static void flush(void)
{
  if (iovN == 0)
    return;

  ssize_t ignored = writev(reportFd(), iov, iovN);
  if (logToFile())
    ignored = writev(corLogFd, iov, iovN);
  (void) ignored;

  iovN    = 0;
  numberN = 0;
}

static void outN(const char* s, int len)
{
  if ((s == NULL) || (len <= 0))
    return;

  if (iovN == IOV_MAX_ITEMS)
    flush();

  iov[iovN].iov_base = (void*) s;
  iov[iovN].iov_len  = len;
  ++iovN;
}

static void out(const char* s)
{
  if (s != NULL)
    outN(s, strlen(s));
}

static void outInt(long n)
{
  if (numberN == NUMBERS_MAX)
    flush();

  char* buf = numberV[numberN++];
  char* end = &buf[sizeof(numberV[0])];
  char* p   = end;
  bool  neg = (n < 0);
  unsigned long u = neg ? (unsigned long) -n : (unsigned long) n;

  do
  {
    *--p = '0' + (u % 10);
    u   /= 10;
  } while (u != 0);

  if (neg)
    *--p = '-';

  outN(p, end - p);
}



// -----------------------------------------------------------------------------
//
// credential - a header whose value stays out of a report that may be shared (case-insensitive)
//
static bool credential(const char* name)
{
  static const char* credentialV[] = { "authorization", "proxy-authorization", "cookie", "x-auth-token", "fiware-token", NULL };

  if (name == NULL)
    return false;

  for (int i = 0; credentialV[i] != NULL; i++)
  {
    const char* a = name;
    const char* b = credentialV[i];

    while ((*a != 0) && (*b != 0) && ((*a | 0x20) == *b))
    {
      ++a;
      ++b;
    }

    if ((*a == 0) && (*b == 0))
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// signalName -
//
static const char* signalName(int sigNo)
{
  switch (sigNo)
  {
  case SIGSEGV: return "SIGSEGV";
  case SIGBUS:  return "SIGBUS";
  case SIGFPE:  return "SIGFPE";
  case SIGILL:  return "SIGILL";
  case SIGABRT: return "SIGABRT";
  default:      return "signal";
  }
}



// -----------------------------------------------------------------------------
//
// pathOut - the URL path as it was SENT, repaired in the output (the buffer is not touched)
//
// The service lookup matches the path IN PLACE: it ends each wildcard's value with a NUL written
// over the separator that follows it - always the '/' before the next part (.../entities/{id}/attrs
// leaves ".../entities/{id}"). urlPathLen is the length before that, so a '/' for every NUL up to
// it gives back the path. Not yet measured (0): up to the first NUL.
//
static void pathOut(const char* path, int len)
{
  if (len <= 0)
  {
    out(path);
    return;
  }

  int start = 0;

  for (int i = 0; i < len; i++)
  {
    if (path[i] == 0)
    {
      outN(&path[start], i - start);
      out("/");
      start = i + 1;
    }
  }

  outN(&path[start], len - start);
}



// -----------------------------------------------------------------------------
//
// queryOut - the query string, as far as it can be had
//
// The query is parsed IN PLACE: split on '&' and '=' (NULs written over both) and percent-decoded
// where it sits. Once parsed, the parameters are rendered from the parsed list - every one of them,
// decoded: exact in meaning, not in encoding. Not yet parsed (no parameters, the raw string still
// there): the raw string.
//
static void queryOut(CorRestState* rP)
{
  if ((rP->in.uriParamCount > 0) && (rP->in.uriParamV != NULL))
  {
    for (int i = 0; i < rP->in.uriParamCount; i++)
    {
      out((i == 0) ? "?" : "&");
      out(rP->in.uriParamV[i].key);
      if (rP->in.uriParamV[i].value != NULL)
      {
        out("=");
        out(rP->in.uriParamV[i].value);
      }
    }
  }
  else if ((rP->in.urlParams != NULL) && (rP->in.urlParams[0] != 0))
  {
    out("?");
    out(rP->in.urlParams);
  }
}



// -----------------------------------------------------------------------------
//
// payloadOut - the payload as it was SENT, repaired in the output (the buffer is not touched)
//
// Its end is payloadSize - the bytes received, counted before any parsing - not a terminating NUL.
//
// The JSON parser works IN PLACE, and the only thing it writes is a NUL over the closing '"' of
// every string and member name. So a '"' for every NUL gives back the exact payload - also when
// the crash was inside the parser (what is not yet parsed is untouched).
//
// Except a string with escape sequences: unescaping shifts it left, so its NUL lands early, and
// between the NUL and its (intact) closing '"' there are stale bytes. JSON puts one of : , } ] or
// whitespace right after a closing '"' - so when anything else follows the NUL, the stale bytes are
// skipped up to the first '"' that IS followed by one of those. The string then shows unescaped
// (a '"' or a newline in it, raw) - its content right, its spelling not.
//
static bool afterString(char c)
{
  return (c == ':') || (c == ',') || (c == '}') || (c == ']') || (c == ' ') || (c == '\t') || (c == '\r') || (c == '\n');
}

static void payloadOut(const char* payload, int size)
{
  int start = 0;
  int i     = 0;

  while (i < size)
  {
    if (payload[i] != 0)
    {
      ++i;
      continue;
    }

    outN(&payload[start], i - start);
    out("\"");
    ++i;

    if ((i < size) && (afterString(payload[i]) == false))   // stale bytes of an unescaped string
    {
      int q = i;
      while ((q < size) && !((payload[q] == '"') && ((q + 1 == size) || afterString(payload[q + 1]))))
        ++q;

      if (q < size)      // the original closing '"' - found: resume after it
        i = q + 1;
    }

    start = i;
  }

  outN(&payload[start], size - start);
}



// -----------------------------------------------------------------------------
//
// requestReport - the request the crashing thread was handling, if it was handling one
//
// corRestP is the thread's own pointer to the request state, read directly: the corRest macro
// would BIND a fallback state, and a thread that is not in a request has nothing to report.
//
static void requestReport(void)
{
  CorRestState* rP = corRestP;

  if ((rP == NULL) || (rP->in.verbString == NULL) || (rP->in.urlPath == NULL))
  {
    out("request:      (none - the crash was not in a request)\n");
    flush();
    return;
  }

  out("request:      ");
  out(rP->in.verbString);
  out(" ");
  pathOut(rP->in.urlPath, rP->in.urlPathLen);
  queryOut(rP);
  out("\n");
  flush();

  for (int i = 0; (i < rP->in.httpHeaderCount) && (rP->in.httpHeaderV != NULL); i++)
  {
    const char* key = rP->in.httpHeaderV[i].key;

    out("  header:     ");
    out(key);
    out(": ");
    out(credential(key) ? "(left out)" : rP->in.httpHeaderV[i].value);
    out("\n");
    flush();
  }

  if ((rP->in.payload == NULL) || (rP->in.payloadSize <= 0))
  {
    out("payload:      (none)\n");
    flush();
    return;
  }

  int n = (rP->in.payloadSize > PAYLOAD_MAX) ? PAYLOAD_MAX : rP->in.payloadSize;

  out("payload:      ");
  outInt(rP->in.payloadSize);
  out(" bytes");
  if (n < rP->in.payloadSize)
  {
    out(", the first ");
    outInt(n);
  }
  out(":\n");
  payloadOut(rP->in.payload, n);
  out("\n");
  flush();
}



// -----------------------------------------------------------------------------
//
// onCrash -
//
static void onCrash(int sigNo)
{
  //
  // A crash INSIDE the report (the request state itself is garbage, say): the handler was reset
  // on entry (SA_RESETHAND), so that second signal kills the broker at once - never a loop.
  //
  out("\n=== coraine crashed: ");
  out(signalName(sigNo));
  out(" (");
  outInt(sigNo);
  out(") ===\n");
  flush();

  out("version:      " CORAINE_VERSION "\n");
  flush();

  out("command line:");
  for (int i = 0; i < crashArgC; i++)
  {
    out(" ");
    out(crashArgV[i]);
  }
  out("\n");
  flush();

  requestReport();

  void* frames[128];
  int   frameN = backtrace(frames, 128);

  out("stack:\n");
  flush();
  backtrace_symbols_fd(frames, frameN, reportFd());
  if (logToFile())
    backtrace_symbols_fd(frames, frameN, corLogFd);

  out("=== end of crash report ===\n");
  flush();

  //
  // The signal's own death: the exit status says which signal, and a core is dumped where the
  // system dumps cores. (The old handler exited 139 whatever the signal.)
  //
  raise(sigNo);
}



// -----------------------------------------------------------------------------
//
// crashReportInstall -
//
void crashReportInstall(int argC, char** argV)
{
  crashArgC = argC;
  crashArgV = argV;

  //
  // backtrace() loads libgcc the first time it runs - which allocates. Not in a crash: now.
  //
  void* frames[4];
  (void) backtrace(frames, 4);

  //
  // An alternate stack for the main thread: a stack overflow has no stack left to run the
  // handler on. (Other threads have their default stacks - a stack overflow there dies
  // unreported, as it did.)
  //
  static char altStack[64 * 1024];
  stack_t     ss;

  memset(&ss, 0, sizeof(ss));
  ss.ss_sp    = altStack;
  ss.ss_size  = sizeof(altStack);
  sigaltstack(&ss, NULL);

  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = onCrash;
  sa.sa_flags   = SA_RESETHAND | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);

  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS,  &sa, NULL);
  sigaction(SIGFPE,  &sa, NULL);
  sigaction(SIGILL,  &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}
