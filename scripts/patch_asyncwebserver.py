"""
Limits for ESPAsyncWebServer requests before the login check

The library reads request headers before any handler or login check runs and
keeps no limit: endless header lines, many headers or many idle half-sent
requests exhaust the heap (and a failed allocation of a header ends in
std::bad_alloc - an abort). The middleware below hands the compiler patched
copies of WebRequest.cpp (ESPAsyncWebServer) and AsyncTCP.cpp (AsyncTCP); the
downloaded libraries stay untouched:

- a header line is at most MAX_LINE bytes, at most MAX_HEADERS headers and
  MAX_HEADER_BYTES in total are kept per request (further ones are dropped);
  the Cookie header (the login session) is always kept - Chromium sends it
  last, after 16+ other headers
- at most MAX_PARAMS query/form parameters are kept per request (each one
  is a heap node: a 4 kB body of '&' made ~4000 of them)
- multipart loops over the boundary use size_t (a uint8_t index never ended
  for boundaries over 255 characters)
- a new request is refused while MAX_ACTIVE_REQUESTS are already being
  received (counted from their first byte, so idle browser preconnects do
  not count; idle connections close after the library's own 3 s rx timeout)
- a request (other than a logged-in upload) is closed after
  MAX_REQUEST_LIFETIME_MS in total, also while its response is being sent -
  a client acking a few bytes at a time cannot hold a slot for good
- multipart field values and part headers are capped at MAX_PART_BUFFER
- a request still being received is dropped when the 8-bit heap runs low
  (MIN_FREE_8BIT / MIN_BLOCK_8BIT) - the bound that holds whatever the other
  limits let through - or after RECEIVE_DEADLINE_MS (a slow trickle resets
  the rx timeout); an authorized firmware upload is exempt from both (its
  data streams to flash), an oversized body without login is dropped at once
  (OversizeRequestHandler marks it)
- at most MAX_LARGE_REQUESTS requests that have received more than
  LARGE_REQUEST_BYTES may be in their receive phase at once (all of them are
  before the login check); the next one to cross the size is dropped. Six
  held requests of ~8 kB (attacks.py "6 big held") took the Pro's 8-bit heap
  from ~43 kB to 396 B. A request crossing the size is also dropped unless
  the 8-bit heap holds MIN_FREE_8BIT + LARGE_REQUEST_PEAK for it and for
  each large one already admitted
- AsyncTCP.cpp, AsyncServer::_accept: a new connection is reset at once while
  the 8-bit heap is below ACCEPT_MIN_FREE_8BIT. Its `new (std::nothrow)
  AsyncClient` aborted the Pro at an exhausted heap (operator new found no
  memory for the bad_alloc either: std::terminate)
- AsyncTCP.cpp, _tcp_recv (lwIP thread): while the 8-bit heap is below
  RECV_MIN_FREE_8BIT, incoming data is refused with ERR_MEM. The refused
  segment is already ACKed: lwIP parks it in its RAM as the pcb's
  refused_data and hands it over again every 250 ms; later segments of that
  connection are dropped unacknowledged and retransmitted by the sender
  until the parked one is taken (nothing is lost).
  Deferred data does not refresh the client's rx timestamp, so the
  library's 3 s rx timeout would reset a logged-in upload while the heap
  stays short: the web layer gives a logged-in upload a 30 s rx timeout
  when its headers are parsed (OversizeRequestHandler in
  src/Web/WebServer.cpp). Its data is still deferred - letting an upload
  through while six held requests ran took the Pro's 8-bit heap to
  2,352 B (8,072 B without the upload).
  The AsyncTCP task runs below the lwIP and Ethernet tasks, so a burst of
  connections otherwise queued every receive window (5.7 kB each) before a
  single byte reached the request gates: six held requests sent at once
  took the Pro's 8-bit heap to 684 B (Ethernet frames dropped), sent 0.5 s
  apart to 4,260 B
- AsyncTCP.cpp, the end of AsyncServer::_accept (client not allocated, no
  callback): returns ERR_ABRT after its tcp_abort(pcb) instead of ERR_OK
  (lwIP would go on using the freed pcb)
- ESPAsyncWebServer WebServer.cpp, the accept callback (async task): the
  8-bit heap is checked again (ACCEPT_MIN_FREE_8BIT, the lwIP-thread check
  can be stale by then) and the request is allocated with nothrow - a Pro
  rebooting into a running request flood aborted in its throwing new; a
  refused client goes to exploraDiscardClient (patched AsyncTCP.cpp): it
  closes the client, waits out an lwIP callback running for it, removes
  its queued events and deletes it. abort() + delete left an error event
  queued for the deleted client, and so did a peer FIN / RST that arrived
  before the accept callback ran (close() is a no-op without a pcb)
- AsyncTCP.cpp, event queue: clearing a connection rotated the whole
  FreeRTOS queue while the lwIP thread appended to it, so two receive
  events of another connection could swap (a firmware upload under a
  request flood arrived with two 1,436 B segments swapped - right length,
  image hash failed). A mutex covers every queue send and the consumer's
  rotation / poll coalescing; senders never wait for space while holding it.
  Removing a connection's events (_remove_events_with_arg, replaced) frees
  the pbuf chain of a dropped receive event (the library leaked it), and is
  done inline when the async task itself clears a connection
  (_tcp_clear_events): the library queued a CLEAR event to the front from
  the only consumer - a self-deadlock on a full queue, events left behind
  when its malloc failed, and a window in which a new client at the freed
  address lost its events. An event of a detached client (arg NULL) frees
  its pbuf too
- AsyncClient's constructor initialises _rx_ack_len (close() passes it to
  tcp_recved)
- AsyncTCP.cpp, AsyncClient::_close (replaced): the callbacks are detached
  and the pcb closed in one section under the lwIP core lock; the library
  unlocked in between, and an RST arriving then freed the pcb silently
  (error callback already detached) before the close freed it again
  (CORRUPT HEAP under a connect-and-RST flood at a short heap)
- AsyncTCP.cpp, AsyncClient::_tcp_error: the pcb is already freed when
  lwIP calls it; the library wrote tcp_arg / callbacks into it and read
  its state (writes into freed heap; under a connect-and-RST flood the
  lwIP thread then looped in tcp_output). It now only forgets the pcb
- AsyncTCP.cpp, a client's tcpip calls (output, write, recved, close,
  abort) run only while the client's _pcb is still the pcb they captured
  (read under the lwIP core lock). The library checked a "closed slot"
  that a new connection may take again between lwIP freeing the pcb and
  the call: under a connect-and-RST flood the calls hit freed or reused
  pcbs (lwIP asserts, CORRUPT HEAP, LoadProhibited)
- AsyncTCP.cpp, event queue: the lwIP thread never waits for space in the
  full queue (64 events). It waited holding the lwIP core lock while the
  async task, the only consumer, waited for that lock in a tcpip call
  (close, ack, write): a deadlock and a task watchdog reset. Clients that
  connect and FIN / RST at once (16 in parallel, ~240 per second) did it
  within seconds, also on the library as released. An event that does not
  fit is appended to an overflow list in order behind the queue (under the
  queue mutex; counted, /diag/heap "web" queueOverflow) and the consumer
  moves it into the queue as it frees space; a poll event is still
  dropped. An ACCEPT event that does not fit refuses the connection: the
  library then leaked the client (and left an error event queued for it),
  it is now detached and freed. Only a front send (ACCEPT, CONNECTED,
  CLEAR) from a task without the core lock still waits for space; an
  append never waits. Received data is refused (ERR_MEM: lwIP keeps it as
  the pcb's refused data and retries; it delays a later FIN behind it)
  once 16 events overflowed, so the list stays bounded by events lwIP
  does not repeat (FIN, ERROR, SENT ...; refusing already at a nearly full
  queue coincided with task watchdog resets under a flood on the bench);
  /diag/heap "web" queueOverflowHigh = longest list. FIN and ERROR take
  a reserve packet (2 x CONFIG_LWIP_MAX_ACTIVE_TCP) when malloc fails - a
  lost one left the client and its request allocated for good; events
  lost anyway count as eventLost

The heap figures are MALLOC_CAP_8BIT (heap_caps_*): ESP.getFreeHeap() also
counts the Pro's ~33 kB of 32-bit-only IRAM that malloc never gets, so the
former 30 kB internal limit never fired there. Thresholds (8-bit heap):
- MIN_FREE_8BIT 10 kB: the base reserve of src/Hardware/HeapGuard.h (8 kB,
  Ethernet / WiFi RX, lwIP, the loop task) plus a request's next segment and
  buffer growth (~2 kB); 5.7 kB free was the measured failure point. Five
  parallel pages on the Pro went down to 14.1 kB, so legitimate load is not
  dropped
- MIN_BLOCK_8BIT 5 kB: the largest receive buffer, a form body String of up
  to MAX_FORM_BODY, plus its heap header and slack
- ACCEPT_MIN_FREE_8BIT 8 kB: the base reserve; below it a new connection
  would take memory the network stacks need for the ones already open
- RECV_MIN_FREE_8BIT 16 kB: measured on the Pro, attacks.py three times in
  a row: low 4,208 B with 12 kB, 6,684 B with 16 kB (data already queued
  and being parsed still takes ~9 kB below the gate). Legitimate requests
  arrive at an idle heap; a response gets only ACKs, which are not
  deferred; a keep-alive request that arrives while the heap is below
  16 kB (5 parallel pages on the Pro: low 14.1 kB) waits for the next
  250 ms retry
- LARGE_REQUEST_BYTES 3 kB / MAX_LARGE_REQUESTS 2: a browser request with
  its cookie is ~1-2 kB
- LARGE_REQUEST_PEAK 16 kB: measured on the Pro (a617a46) - one held ~8 kB
  request (3.8 kB of headers in two 1.9 kB lines, 3.9 kB of form body) took
  the 8-bit heap from 43.8 kB to a low of 27.1 kB, two of them to 15.1 kB:
  the receive window in pbufs, the line buffer and its substring copies
  while a header is parsed, the body String growing. The Pro (43.8 kB idle)
  so admits one at a time (a second needs 42 kB), the Lite two
  A logged-in upload is exempt from the segment after the one that ends
  its headers. _onData gets one pbuf (at most one MSS, 1,460 B) per call,
  so an upload counts towards the size only with headers that do not end
  within its first 3 kB - like any request with such headers it is then
  dropped while the large-request limits are reached (an attack holding
  the large slots, or a short heap); browser requests stay under 3 kB

A build that links without both patched copies (the library moved, a
middleware no longer matches) fails after linking (check_patched).
A patch anchor that no longer matches stops the build (UserError).

Counters (std::atomic, defined in src/Web/WebServer.cpp, shown in
/diag/heap "web"): exploraAcceptRefused, exploraRecvDeferred (deliveries
refused, a deferred segment counts again on each retry),
exploraReceiveDropped, exploraQueueOverflow.

Patched places (each anchor must occur exactly once, or the build stops):
WebRequest.cpp - _temp growth of a header line, a complete header line,
_addHeader (header count / bytes), _addParam x2 and the multipart field /
file params (MAX_PARAMS), the multipart value / header buffers, the
includes (request tracking table, EXPLORA_REJECT), the destructor (untrack),
_onPoll (close rejected / expired), _onData (receive gate); the three
multipart `uint8_t i` loops. WebServer.cpp (ESPAsyncWebServer) - the
includes, the accept callback. AsyncTCP.cpp - the includes, _tcp_recv,
AsyncServer::_accept (heap check at the start, ERR_ABRT at the end), the
event queue (_init_async_event_queue, _send_async_event,
_prepend_async_event, the poll coalescing, _remove_events_with_arg as a
whole, the NULL-arg branch of _handle_async_event, _tcp_clear_events,
exploraDiscardClient after the tcpip_priv.h include), the AsyncClient
constructor, AsyncClient::_close as a whole, AsyncClient::_tcp_error, the
accept event failure in AsyncServer::_accept, the tcpip call wrappers (the
client in tcp_api_call_t, _explora_pcb_open, macros at the call sites).
The library sources are never modified: each build patches a fresh copy
into $BUILD_DIR, so running it again is a no-op when nothing changed.
"""

import json
import os

from SCons.Errors import UserError

Import("env")  # noqa: F821

ASYNCTCP_VERSION = "3.3.8"  # see patch_asynctcp
MAX_LINE = 2048  # a Cookie header for the gateway's host can be long
MAX_HEADERS = 16
MAX_HEADER_BYTES = 4096
MAX_PARAMS = 32
MAX_ACTIVE_REQUESTS = 6
MIN_FREE_8BIT = 10000
MIN_BLOCK_8BIT = 5120
ACCEPT_MIN_FREE_8BIT = 8000
RECV_MIN_FREE_8BIT = 16000
LARGE_REQUEST_BYTES = 3072
MAX_LARGE_REQUESTS = 2
LARGE_REQUEST_PEAK = 16000
RECEIVE_DEADLINE_MS = 10000
MAX_FORM_BODY = 4096  # must match MAX_REQUEST_BODY in src/Web/WebServer.cpp
MAX_REQUEST_LIFETIME_MS = 30000  # any request but a logged-in upload
MAX_PART_BUFFER = 2048  # multipart field values and part headers

PATCHES = [
    # A line without newline: refuse to grow past MAX_LINE
    (
        "        if (!_temp.reserve(_temp.length() + len)) {",
        "        if (_temp.length() + len > %d) {\n          EXPLORA_REJECT();\n        }\n"
        "        if (!_temp.reserve(_temp.length() + len)) {" % MAX_LINE,
    ),
    # A complete line that is too long
    (
        "        str[i] = 0;  // Terminate the string at the end of the line.\n        _temp.concat(str);\n",
        "        str[i] = 0;  // Terminate the string at the end of the line.\n        _temp.concat(str);\n"
        "        if (_temp.length() > %d) {\n          EXPLORA_REJECT();\n        }\n"
        % MAX_LINE,
    ),
    # Extra headers are dropped once the count or byte budget is used up
    (
        "    _headers.emplace_back(name, value);",
        "    size_t headerBytes = name.length() + value.length();\n"
        "    for (const auto &h : _headers) {\n"
        "      headerBytes += h.name().length() + h.value().length();\n"
        "    }\n"
        "    bool firstCookie = name.equalsIgnoreCase(\"Cookie\") && !hasHeader(\"Cookie\");\n"
        "    if (firstCookie || (_headers.size() < %d && headerBytes <= %d)) {\n"
        "      _headers.emplace_back(name, value);\n    }"
        % (MAX_HEADERS, MAX_HEADER_BYTES),
    ),
    # Parameters: query string, form body, multipart fields and files
    (
        "    if (name.length()) {\n      _params.emplace_back(name, value);\n    }",
        "    if (name.length() && _params.size() < %d) {\n      _params.emplace_back(name, value);\n    }" % MAX_PARAMS,
    ),
    (
        "    if (name.length()) {\n      _params.emplace_back(name, urlDecode(value), true);\n    }",
        "    if (name.length() && _params.size() < %d) {\n      _params.emplace_back(name, urlDecode(value), true);\n    }"
        % MAX_PARAMS,
    ),
    (
        "      if (!_itemIsFile) {\n        _params.emplace_back(_itemName, _itemValue, true);",
        "      if (!_itemIsFile) {\n        if (_params.size() < %d) _params.emplace_back(_itemName, _itemValue, true);" % MAX_PARAMS,
    ),
    (
        "          _params.emplace_back(_itemName, _itemFilename, true, true, _itemSize);",
        "          if (_params.size() < %d) _params.emplace_back(_itemName, _itemFilename, true, true, _itemSize);"
        % MAX_PARAMS,
    ),
    # Multipart buffers (also of a logged-in upload, exempt from the heap gate)
    (
        "    else                          \\\n      _itemValue += (char)(b);    \\\n",
        "    else if (_itemValue.length() < %d) \\\n      _itemValue += (char)(b);    \\\n" % MAX_PART_BUFFER,
    ),
    (
        "      _temp += (char)data;\n",
        "      if (_temp.length() < %d) _temp += (char)data;\n" % MAX_PART_BUFFER,
    ),
    # Count open requests
    (
        '#include <cstring>\n',
        '#include <cstring>\n#include <atomic>\n#include <esp_heap_caps.h>\n\n'
        '// Defined in src/Web/WebServer.cpp (attributes are keyed by pointer)\n'
        'extern const char EXPLORA_REJECT_ATTR[];\n'
        'extern std::atomic<uint32_t> exploraReceiveDropped;\n\n'
        '// Rejected requests: drop their buffers now, close the connection from\n'
        '// _onPoll (an abort() inside a data callback frees the client while\n'
        '// AsyncClient::_recv is still running on it)\n'
        '#define EXPLORA_REJECT() do { _parseState = PARSE_REQ_FAIL; _temp = String(); _headers.clear(); '
        '_params.clear(); return; } while (0)\n\n'
        '// Requests that have received data (all of this runs in the AsyncTCP task)\n'
        'struct ActiveRequest {\n  AsyncWebServerRequest *request;\n  uint32_t since;\n  size_t received;\n};\n'
        'static ActiveRequest s_activeRequests[%d];\n'
        'static ActiveRequest *trackRequest(AsyncWebServerRequest *r) {\n'
        '  for (auto &slot : s_activeRequests) {\n'
        '    if (slot.request == r) return &slot;\n'
        '  }\n'
        '  for (auto &slot : s_activeRequests) {\n'
        '    if (!slot.request) { slot.request = r; slot.since = millis(); slot.received = 0; return &slot; }\n'
        '  }\n'
        '  return nullptr;\n'
        '}\n'
        'static ActiveRequest *findRequest(AsyncWebServerRequest *r) {\n'
        '  for (auto &slot : s_activeRequests) {\n'
        '    if (slot.request == r) return &slot;\n'
        '  }\n'
        '  return nullptr;\n'
        '}\n'
        'static void untrackRequest(AsyncWebServerRequest *r) {\n'
        '  for (auto &slot : s_activeRequests) {\n'
        '    if (slot.request == r) slot.request = nullptr;\n'
        '  }\n'
        '}\n' % MAX_ACTIVE_REQUESTS,
    ),
    (
        "_itemIsFile(false), _tempObject(NULL) {\n",
        "_itemIsFile(false), _tempObject(NULL) {\n",
    ),
    (
        "AsyncWebServerRequest::~AsyncWebServerRequest() {\n",
        "AsyncWebServerRequest::~AsyncWebServerRequest() {\n  untrackRequest(this);\n",
    ),
    # Rejected requests are closed here (safe: AsyncClient::_poll does not
    # touch the client after its poll callback)
    (
        "void AsyncWebServerRequest::_onPoll() {\n",
        "void AsyncWebServerRequest::_onPoll() {\n"
        "  ActiveRequest *active = findRequest(this);\n"
        "  bool expired = active && _contentLength <= %d && millis() - active->since > %d;\n"
        "  if ((_parseState == PARSE_REQ_FAIL || expired) && _client != NULL) {\n    _client->abort();\n    return;\n  }\n"
        % (MAX_FORM_BODY, MAX_REQUEST_LIFETIME_MS),
    ),
    # Receive gate: a new request above the slot limit, a marked one, and
    # one still being received (not a logged-in upload) when the 8-bit heap
    # is short, its deadline passed or it is a third large one
    (
        "void AsyncWebServerRequest::_onData(void *buf, size_t len) {\n",
        "void AsyncWebServerRequest::_onData(void *buf, size_t len) {\n"
        "  ActiveRequest *active = trackRequest(this);\n"
        "  bool upload = _parseState >= PARSE_REQ_BODY && _contentLength > %d; // only logged-in uploads get this far\n"
        "  if (_parseState == PARSE_REQ_FAIL) {\n    return; // rejected, waiting for _onPoll to close it\n  }\n"
        "  if (!active || hasAttribute(EXPLORA_REJECT_ATTR)) {\n    EXPLORA_REJECT();\n  }\n"
        "  if (_parseState < PARSE_REQ_END && !upload) {\n"
        "    active->received += len;\n"
        "    unsigned large = 0;\n"
        "    if (active->received > %d) {\n"
        "      for (const auto &slot : s_activeRequests) {\n"
        "        const AsyncWebServerRequest *r = slot.request;\n"
        "        if (r && &slot != active && slot.received > %d && r->_parseState < PARSE_REQ_END &&\n"
        "            !(r->_parseState >= PARSE_REQ_BODY && r->_contentLength > %d)) {\n"
        "          large++;\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "    bool crossing = active->received > %d && active->received - len <= %d;\n"
        "    if (millis() - active->since > %d || large >= %d ||\n"
        "        (crossing && heap_caps_get_free_size(MALLOC_CAP_8BIT) < %d + %d * (large + 1)) ||\n"
        "        heap_caps_get_free_size(MALLOC_CAP_8BIT) < %d ||\n"
        "        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < %d) {\n"
        "      exploraReceiveDropped.fetch_add(1, std::memory_order_relaxed);\n"
        "      EXPLORA_REJECT();\n"
        "    }\n"
        "  }\n"
        % (MAX_FORM_BODY, LARGE_REQUEST_BYTES, LARGE_REQUEST_BYTES, MAX_FORM_BODY, LARGE_REQUEST_BYTES,
           LARGE_REQUEST_BYTES, RECEIVE_DEADLINE_MS, MAX_LARGE_REQUESTS, MIN_FREE_8BIT, LARGE_REQUEST_PEAK,
           MIN_FREE_8BIT, MIN_BLOCK_8BIT),
    ),
]


# Applied to every occurrence (three multipart loops)
REPLACE_ALL = [
    ("      uint8_t i;\n      for (i = 0; i < ", "      size_t i;\n      for (i = 0; i < ", 3),
]

# AsyncTCP 3.3.8 _remove_events_with_arg, replaced as a whole (TCP_PATCHES)
REMOVE_EVENTS_ORIGINAL = (
    "static bool _remove_events_with_arg(void *arg) {\n"
    "  if (!_async_queue) {\n"
    "    return false;\n"
    "  }\n"
    "\n"
    "  lwip_tcp_event_packet_t *first_packet = NULL;\n"
    "  lwip_tcp_event_packet_t *packet = NULL;\n"
    "\n"
    "  // figure out which is the first non-matching packet so we can keep the order\n"
    "  while (!first_packet) {\n"
    "    if (xQueueReceive(_async_queue, &first_packet, 0) != pdPASS) {\n"
    "      return false;\n"
    "    }\n"
    "    // discard packet if matching\n"
    "    if ((uintptr_t)first_packet->arg == (uintptr_t)arg) {\n"
    "      free(first_packet);\n"
    "      first_packet = NULL;\n"
    "    } else if (xQueueSend(_async_queue, &first_packet, 0) != pdPASS) {\n"
    "      // try to return first packet to the back of the queue\n"
    "      // we can't wait here if queue is full, because this call has been done from the only consumer task of this queue\n"
    "      // otherwise it would deadlock, we have to discard the event\n"
    "      free(first_packet);\n"
    "      first_packet = NULL;\n"
    "      return false;\n"
    "    }\n"
    "  }\n"
    "\n"
    "  while (xQueuePeek(_async_queue, &packet, 0) == pdPASS && packet != first_packet) {\n"
    "    if (xQueueReceive(_async_queue, &packet, 0) != pdPASS) {\n"
    "      return false;\n"
    "    }\n"
    "    if ((uintptr_t)packet->arg == (uintptr_t)arg) {\n"
    "      // remove matching event\n"
    "      free(packet);\n"
    "      packet = NULL;\n"
    "      // otherwise try to requeue it\n"
    "    } else if (xQueueSend(_async_queue, &packet, 0) != pdPASS) {\n"
    "      // we can't wait here if queue is full, because this call has been done from the only consumer task of this queue\n"
    "      // otherwise it would deadlock, we have to discard the event\n"
    "      free(packet);\n"
    "      packet = NULL;\n"
    "      return false;\n"
    "    }\n"
    "  }\n"
    "  return true;\n"
    "}\n"
)

# AsyncTCP 3.3.8 AsyncClient::_close, replaced as a whole (TCP_PATCHES)
CLOSE_ORIGINAL = (
    "int8_t AsyncClient::_close() {\n"
    "  // ets_printf(\"X: 0x%08x\\n\", (uint32_t)this);\n"
    "  int8_t err = ERR_OK;\n"
    "  if (_pcb) {\n"
    "    TCP_MUTEX_LOCK();\n"
    "    tcp_arg(_pcb, NULL);\n"
    "    tcp_sent(_pcb, NULL);\n"
    "    tcp_recv(_pcb, NULL);\n"
    "    tcp_err(_pcb, NULL);\n"
    "    tcp_poll(_pcb, NULL, 0);\n"
    "    TCP_MUTEX_UNLOCK();\n"
    "    _tcp_clear_events(this);\n"
    "    err = _tcp_close(_pcb, _closed_slot);\n"
    "    if (err != ERR_OK) {\n"
    "      err = abort();\n"
    "    }\n"
    "    _free_closed_slot();\n"
    "    _pcb = NULL;\n"
    "    if (_discard_cb) {\n"
    "      _discard_cb(_discard_cb_arg, this);\n"
    "    }\n"
    "  }\n"
    "  return err;\n"
    "}\n"
)

# AsyncTCP.cpp: refuse a connection at accept while the 8-bit heap is short
# (runs on the lwIP thread; aborting the pcb there requires ERR_ABRT)
TCP_PATCHES = [
    (
        '#include "AsyncTCP.h"\n',
        '#include "AsyncTCP.h"\n#include <atomic>\n#include <esp_heap_caps.h>\n\n'
        '// Defined in src/Web/WebServer.cpp\n'
        'extern std::atomic<uint32_t> exploraAcceptRefused;\n'
        'extern std::atomic<uint32_t> exploraRecvDeferred;\n'
        'extern std::atomic<uint32_t> exploraQueueOverflow;\n'
        'extern std::atomic<uint32_t> exploraQueueOverflowHigh;\n'
        'extern std::atomic<uint32_t> exploraEventLost;\n',
    ),
    # (A pcb closed while it holds refused data is reset, not closed with a
    # FIN: lwIP's tcp_close sends RST when received data was not taken.)
    # Incoming data while the 8-bit heap is short stays in lwIP: ERR_MEM
    # makes lwIP park this (already ACKed) segment as the pcb's refused data
    # (retried from its 250 ms timer and on the next segment) and drop
    # further segments of that connection unacknowledged until it is taken -
    # so a connection holds at most one more segment instead of its whole
    # receive window in the AsyncTCP queue. Nothing is lost, the sender
    # retransmits
    (
        "static int8_t _tcp_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *pb, int8_t err) {\n",
        "static int8_t _tcp_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *pb, int8_t err) {\n"
        "  if (pb && (heap_caps_get_free_size(MALLOC_CAP_8BIT) < %d || _explora_queue_congested())) {\n"
        "    exploraRecvDeferred.fetch_add(1, std::memory_order_relaxed);\n"
        "    return ERR_MEM;\n"
        "  }\n"
        "  if (!pb) {\n"
        "    // FIN: lwIP does not retry a refused one - a reserve packet when\n"
        "    // malloc fails (a lost FIN leaked the client and its request).\n"
        "    // Without a client (detached) there is nobody to tell\n"
        "    if (!arg) {\n"
        "      return ERR_OK;\n"
        "    }\n"
        "    lwip_tcp_event_packet_t *e = _explora_event_alloc();\n"
        "    if (!e) {\n"
        "      exploraEventLost.fetch_add(1, std::memory_order_relaxed);\n"
        "      return ERR_MEM;\n"
        "    }\n"
        "    e->event = LWIP_TCP_FIN;\n"
        "    e->arg = arg;\n"
        "    e->fin.pcb = pcb;\n"
        "    e->fin.err = err;\n"
        "    AsyncClient::_s_lwip_fin(e->arg, e->fin.pcb, e->fin.err);\n"
        "    if (!_send_async_event(&e)) {\n"
        "      _explora_event_release(e);\n"
        "      return ERR_TIMEOUT;\n"
        "    }\n"
        "    return ERR_OK;\n"
        "  }\n" % RECV_MIN_FREE_8BIT,
    ),
    (
        "  if (_connect_cb) {\n    AsyncClient *c = new (std::nothrow) AsyncClient(pcb);\n",
        "  if (_connect_cb && heap_caps_get_free_size(MALLOC_CAP_8BIT) < %d) {\n"
        "    exploraAcceptRefused.fetch_add(1, std::memory_order_relaxed);\n"
        "    tcp_abort(pcb);\n"
        "    return ERR_ABRT;\n"
        "  }\n"
        "  if (_connect_cb) {\n    AsyncClient *c = new (std::nothrow) AsyncClient(pcb);\n" % ACCEPT_MIN_FREE_8BIT,
    ),
    # Event queue order (AsyncTCP 3.3.8): _remove_events_with_arg (run in
    # the async task when a connection is cleared) rotates the whole FreeRTOS
    # queue - pops every event and sends the ones it keeps to the back again
    # - while the lwIP thread keeps appending. An event appended mid-rotation
    # lands before the older events still to be rotated, so two receive
    # events of one connection could swap: a firmware upload under a request
    # flood got two 1,436 B segments in swapped order (right length, image
    # hash failed; bench 2026-10-23, both segments found swapped in flash).
    # The same holds for the poll coalescing in _get_async_event (peek, then
    # receive, while the lwIP thread may push to the front). A mutex now
    # covers every queue send and both consumer-side manipulations; a sender
    # never waits for queue space while holding it (it retries every tick),
    # so the consumer cannot deadlock on it
    (
        "static inline bool _init_async_event_queue() {\n",
        "static SemaphoreHandle_t _explora_queue_mutex = NULL;\n"
        "struct ExploraQueueLock {\n"
        "  ExploraQueueLock() {\n    xSemaphoreTake(_explora_queue_mutex, portMAX_DELAY);\n  }\n"
        "  ~ExploraQueueLock() {\n    xSemaphoreGive(_explora_queue_mutex);\n  }\n"
        "};\n"
        "// Reserve packets for the events lwIP never repeats when one is lost\n"
        "// (FIN, ERROR): taken when malloc fails. A lost one left the client\n"
        "// and its request allocated for good (a heavy page slot with it).\n"
        "// At most one such event per client is ever queued\n"
        "#define EXPLORA_EVENT_RESERVE (2 * CONFIG_LWIP_MAX_ACTIVE_TCP)\n"
        "static lwip_tcp_event_packet_t _explora_reserve[EXPLORA_EVENT_RESERVE];\n"
        "static std::atomic<bool> _explora_reserve_used[EXPLORA_EVENT_RESERVE];\n"
        "static lwip_tcp_event_packet_t *_explora_event_alloc() {\n"
        "  lwip_tcp_event_packet_t *e = (lwip_tcp_event_packet_t *)malloc(sizeof(lwip_tcp_event_packet_t));\n"
        "  for (int i = 0; !e && i < EXPLORA_EVENT_RESERVE; i++) {\n"
        "    bool used = false;\n"
        "    if (_explora_reserve_used[i].compare_exchange_strong(used, true)) {\n"
        "      e = &_explora_reserve[i];\n"
        "    }\n"
        "  }\n"
        "  return e;\n"
        "}\n"
        "static void _explora_event_release(lwip_tcp_event_packet_t *e) {\n"
        "  if (e >= _explora_reserve && e < _explora_reserve + EXPLORA_EVENT_RESERVE) {\n"
        "    _explora_reserve_used[e - _explora_reserve] = false;\n"
        "  } else {\n"
        "    free(e);\n"
        "  }\n"
        "}\n"
        "static inline bool _init_async_event_queue() {\n"
        "  if (!_explora_queue_mutex) {\n"
        "    _explora_queue_mutex = xSemaphoreCreateMutex();\n"
        "    if (!_explora_queue_mutex) {\n      return false;\n    }\n"
        "  }\n",
    ),
    (
        "static inline bool _send_async_event(lwip_tcp_event_packet_t **e, TickType_t wait = portMAX_DELAY) {\n"
        "  return _async_queue && xQueueSend(_async_queue, e, wait) == pdPASS;\n}\n",
        "// Events that did not fit the full queue, in order behind it (see the\n"
        "// script): appended under the queue mutex, moved into the queue by the\n"
        "// consumer as it frees space\n"
        "static lwip_tcp_event_packet_t *_explora_overflow_head = NULL;\n"
        "static lwip_tcp_event_packet_t *_explora_overflow_tail = NULL;\n"
        "static uint32_t _explora_overflow_len = 0;\n\n"
        "// Received data waits in lwIP (ERR_MEM, its refused data) once 16\n"
        "// events overflowed: the list is for events lwIP does not repeat (FIN,\n"
        "// ERROR, SENT ...), not for a receive window per connection\n"
        "static inline bool _explora_queue_congested() {\n"
        "  return _explora_overflow_len >= 16;\n"
        "}\n\n"
        "static inline void _explora_refill_locked() {\n"
        "  while (_explora_overflow_head && xQueueSend(_async_queue, &_explora_overflow_head, 0) == pdPASS) {\n"
        "    _explora_overflow_len--;\n"
        "    _explora_overflow_head = (lwip_tcp_event_packet_t *)_explora_overflow_head->explora_next;\n"
        "    if (!_explora_overflow_head) {\n      _explora_overflow_tail = NULL;\n    }\n"
        "  }\n"
        "}\n\n"
        "static inline bool _explora_queue_put(lwip_tcp_event_packet_t **e, TickType_t wait, bool front) {\n"
        "  if (!_async_queue || !_explora_queue_mutex) {\n    return false;\n  }\n"
        "  // The lwIP thread (or a task holding the lwIP core lock) never waits:\n"
        "  // the async task may be waiting for that lock\n"
        "  bool core = sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER);\n"
        "  TickType_t start = xTaskGetTickCount();\n"
        "  while (true) {\n"
        "    {\n"
        "      ExploraQueueLock lock;\n"
        "      if (front) {\n"
        "        if (xQueueSendToFront(_async_queue, e, 0) == pdPASS) {\n          return true;\n        }\n"
        "      } else if (!_explora_overflow_head && xQueueSend(_async_queue, e, 0) == pdPASS) {\n"
        "        return true;\n"
        "      } else if (wait != 0) {\n"
        "        (*e)->explora_next = NULL;\n"
        "        if (_explora_overflow_tail) {\n"
        "          _explora_overflow_tail->explora_next = *e;\n"
        "        } else {\n"
        "          _explora_overflow_head = *e;\n"
        "        }\n"
        "        _explora_overflow_tail = *e;\n"
        "        exploraQueueOverflow.fetch_add(1, std::memory_order_relaxed);\n"
        "        if (++_explora_overflow_len > exploraQueueOverflowHigh.load(std::memory_order_relaxed)) {\n"
        "          exploraQueueOverflowHigh.store(_explora_overflow_len, std::memory_order_relaxed);\n"
        "        }\n"
        "        return true;\n"
        "      }\n"
        "    }\n"
        "    if (core || wait == 0 || (wait != portMAX_DELAY && xTaskGetTickCount() - start >= wait)) {\n      return false;\n    }\n"
        "    vTaskDelay(1);\n"
        "  }\n"
        "}\n\n"
        "static inline bool _send_async_event(lwip_tcp_event_packet_t **e, TickType_t wait = portMAX_DELAY) {\n"
        "  return _explora_queue_put(e, wait, false);\n}\n",
    ),
    (
        "static inline bool _prepend_async_event(lwip_tcp_event_packet_t **e, TickType_t wait = portMAX_DELAY) {\n"
        "  return _async_queue && xQueueSendToFront(_async_queue, e, wait) == pdPASS;\n}\n",
        "static inline bool _prepend_async_event(lwip_tcp_event_packet_t **e, TickType_t wait = portMAX_DELAY) {\n"
        "  return _explora_queue_put(e, wait, true);\n}\n",
    ),
    (
        "    lwip_tcp_event_packet_t *next_pkt = NULL;\n",
        "    lwip_tcp_event_packet_t *next_pkt = NULL;\n"
        "    ExploraQueueLock coalesceLock; // released at the end of this iteration\n",
    ),
    # Overflow list link (an event is malloc'ed with sizeof the packet)
    (
        "typedef struct {\n  lwip_tcp_event_t event;\n  void *arg;\n",
        "typedef struct {\n  lwip_tcp_event_t event;\n  void *arg;\n  void *explora_next;\n",
    ),
    # The consumer moves overflowed events into the queue before it waits
    # (it is the only one freeing space: receive, coalescing, removal)
    (
        "static inline bool _get_async_event(lwip_tcp_event_packet_t **e) {\n  while (true) {\n",
        "static inline bool _get_async_event(lwip_tcp_event_packet_t **e) {\n  while (true) {\n"
        "    if (_async_queue && _explora_queue_mutex) {\n"
        "      ExploraQueueLock lock;\n"
        "      _explora_refill_locked();\n"
        "    }\n",
    ),
    # Removing a connection's events (replaces the library's function): runs
    # under the queue mutex, so no sender appends meanwhile, every kept event
    # fits back and one pass over the queued count keeps their order - no
    # allocation, no waiting. A dropped receive event owns its pbuf chain
    # (_tcp_recv returned ERR_OK for it): freed with it, the library only
    # freed the event (pbuf leak on every connection closed with data still
    # queued). The other event types own nothing (FIN / ERROR: the pcb is
    # already gone or closed; ACCEPT is never queued for a client)
    (
        REMOVE_EVENTS_ORIGINAL,
        "static void _explora_free_event(lwip_tcp_event_packet_t *e) {\n"
        "  if (e->event == LWIP_TCP_RECV && e->recv.pb) {\n"
        "    pbuf_free(e->recv.pb);\n"
        "  }\n"
        "  _explora_event_release(e);\n"
        "}\n\n"
        "static bool _remove_events_with_arg_locked(void *arg) {\n"
        "  if (!_async_queue) {\n    return false;\n  }\n"
        "  UBaseType_t queued = uxQueueMessagesWaiting(_async_queue);\n"
        "  lwip_tcp_event_packet_t *packet = NULL;\n"
        "  for (UBaseType_t i = 0; i < queued; i++) {\n"
        "    if (xQueueReceive(_async_queue, &packet, 0) != pdPASS) {\n      return false;\n    }\n"
        "    if (packet->arg == arg) {\n"
        "      _explora_free_event(packet);\n"
        "    } else if (xQueueSend(_async_queue, &packet, 0) != pdPASS) {\n"
        "      _explora_free_event(packet); // cannot happen under the mutex\n"
        "      return false;\n"
        "    }\n"
        "  }\n"
        "  lwip_tcp_event_packet_t *prev = NULL;\n"
        "  for (lwip_tcp_event_packet_t *o = _explora_overflow_head; o;) {\n"
        "    lwip_tcp_event_packet_t *next = (lwip_tcp_event_packet_t *)o->explora_next;\n"
        "    if (o->arg == arg) {\n"
        "      if (prev) {\n        prev->explora_next = next;\n      } else {\n        _explora_overflow_head = next;\n      }\n"
        "      if (_explora_overflow_tail == o) {\n        _explora_overflow_tail = prev;\n      }\n"
        "      _explora_overflow_len--;\n"
        "      _explora_free_event(o);\n"
        "    } else {\n"
        "      prev = o;\n"
        "    }\n"
        "    o = next;\n"
        "  }\n"
        "  return true;\n"
        "}\n\n"
        "static bool _remove_events_with_arg(void *arg) {\n"
        "  ExploraQueueLock lock;\n"
        "  return _remove_events_with_arg_locked(arg);\n"
        "}\n",
    ),
    # An event whose client was detached (arg NULL) is dropped by the
    # library: a receive event still owns its pbuf
    (
        "  if (e->arg == NULL) {\n    // do nothing when arg is NULL\n",
        "  if (e->arg == NULL) {\n"
        "    if (e->event == LWIP_TCP_RECV && e->recv.pb) {\n      pbuf_free(e->recv.pb);\n    }\n"
        "    // do nothing when arg is NULL\n",
    ),
    # Clearing a connection's events from the async task itself (every
    # AsyncClient::_close / _fin of the web server): removed right here.
    # The library queued a CLEAR event to the front instead - from the only
    # consumer, so on a full queue the task waited for itself forever, a
    # failed malloc left the events queued, and a client deleted before the
    # CLEAR ran could be replaced by a new one at the same address whose
    # events the CLEAR then removed
    (
        "static int8_t _tcp_clear_events(void *arg) {\n",
        "static int8_t _tcp_clear_events(void *arg) {\n"
        "  if (_async_service_task_handle && xTaskGetCurrentTaskHandle() == _async_service_task_handle) {\n"
        "    ExploraQueueLock lock;\n"
        "    _remove_events_with_arg_locked(arg);\n"
        "    return ERR_OK;\n"
        "  }\n",
    ),
    # Discarding a client the web server refuses in its accept callback
    # (async task, see SERVER_PATCHES). The peer may have sent FIN / RST
    # before the ACCEPT event was handled: the lwIP thread then already
    # dropped the pcb (_s_lwip_fin / _tcp_error) and queued a FIN / ERROR
    # event for this client - close() does nothing without a pcb, and
    # deleting the client left that event to be handled on freed memory.
    # The no-op tcpip call waits for an lwIP callback that may be running
    # for this client right now (it queues its event before it returns);
    # after it the pcb is detached or gone, nothing queues for the client
    # any more, and its queued events are removed before the delete
    (
        "#include \"lwip/priv/tcpip_priv.h\"\n",
        "#include \"lwip/priv/tcpip_priv.h\"\n\n"
        "static err_t _explora_barrier_api(struct tcpip_api_call_data *) {\n  return ERR_OK;\n}\n\n"
        "void exploraDiscardClient(AsyncClient *c) {\n"
        "  c->close(true);\n"
        "  struct tcpip_api_call_data call;\n"
        "  tcpip_api_call(_explora_barrier_api, &call);\n"
        "  {\n"
        "    ExploraQueueLock lock;\n"
        "    _remove_events_with_arg_locked(c);\n"
        "  }\n"
        "  delete c;\n"
        "}\n",
    ),
    # _rx_ack_len is not initialised by the constructor; close() hands it to
    # tcp_recved() (a refused client never received anything)
    (
        "  _closed_slot = INVALID_CLOSED_SLOT;\n  if (_pcb) {\n    _rx_last_packet = millis();\n",
        "  _closed_slot = INVALID_CLOSED_SLOT;\n  _rx_ack_len = 0;\n  if (_pcb) {\n    _rx_last_packet = millis();\n",
    ),
    # The error event (lwIP calls the error callback once, after it freed
    # the pcb) takes a reserve packet when malloc fails; a lost one left the
    # client and its request allocated for good
    (
        "  lwip_tcp_event_packet_t *e = (lwip_tcp_event_packet_t *)malloc(sizeof(lwip_tcp_event_packet_t));\n"
        "  if (!e) {\n"
        "    log_e(\"Failed to allocate event packet\");\n"
        "    return;\n"
        "  }\n"
        "  e->event = LWIP_TCP_ERROR;",
        "  if (!arg) {\n"
        "    return; // a detached client (after _lwip_fin): nobody to tell, no reserve packet used\n"
        "  }\n"
        "  lwip_tcp_event_packet_t *e = _explora_event_alloc();\n"
        "  if (!e) {\n"
        "    exploraEventLost.fetch_add(1, std::memory_order_relaxed);\n"
        "    return;\n"
        "  }\n"
        "  e->event = LWIP_TCP_ERROR;",
    ),
    (
        "  if (!_send_async_event(&e)) {\n    ::free((void *)(e));\n  }",
        "  if (!_send_async_event(&e)) {\n    exploraEventLost.fetch_add(1, std::memory_order_relaxed);\n"
        "    _explora_event_release(e);\n  }",
    ),
    # The library's FIN branch of _tcp_recv: FIN is handled before it now
    (
        "  } else {\n    // ets_printf(\"+F: 0x%08x\\n\", pcb);\n    e->event = LWIP_TCP_FIN;\n    e->fin.pcb = pcb;\n"
        "    e->fin.err = err;\n    // close the PCB in LwIP thread\n"
        "    AsyncClient::_s_lwip_fin(e->arg, e->fin.pcb, e->fin.err);\n  }\n",
        "  }\n",
    ),
    (
        "  e->sent.len = len;\n  if (!_send_async_event(&e)) {\n    free((void *)(e));\n    return ERR_TIMEOUT;\n  }",
        "  e->sent.len = len;\n  if (!_send_async_event(&e)) {\n"
        "    exploraEventLost.fetch_add(1, std::memory_order_relaxed);\n"
        "    free((void *)(e));\n    return ERR_TIMEOUT;\n  }",
    ),
    # A sent event lwIP does not repeat either (counted when lost)
    (
        "    return ERR_MEM;\n  }\n  e->event = LWIP_TCP_SENT;",
        "    exploraEventLost.fetch_add(1, std::memory_order_relaxed);\n    return ERR_MEM;\n  }\n  e->event = LWIP_TCP_SENT;",
    ),
    (
        "  free((void *)(e));\n}\n\nstatic void _async_service_task",
        "  _explora_event_release(e);\n}\n\nstatic void _async_service_task",
    ),
    # The ACCEPT event did not fit: it goes to the front of the queue, and a
    # front send from the lwIP thread does not wait and does not overflow
    # (see _explora_queue_put), so it fails on a full queue. The library aborted
    # the pcb with the client still attached (its error callback queued an
    # event for it) and leaked the client. Detached and freed here - no
    # event was queued for it yet - and counted as refused
    (
        "      c->_pcb = nullptr;\n      tcp_abort(pcb);\n      log_e(\"_accept failed: couldn't accept client\");\n      return ERR_ABRT;\n",
        "      tcp_arg(pcb, NULL);\n      tcp_recv(pcb, NULL);\n      tcp_sent(pcb, NULL);\n      tcp_err(pcb, NULL);\n"
        "      tcp_poll(pcb, NULL, 0);\n      c->_pcb = nullptr;\n      delete c;\n"
        "      exploraAcceptRefused.fetch_add(1, std::memory_order_relaxed);\n"
        "      tcp_abort(pcb);\n      return ERR_ABRT;\n",
    ),
    # AsyncClient::_close (replaced): detaching the callbacks and closing the
    # pcb happen in one section under the lwIP core lock. The library
    # released the lock in between: an RST arriving then made lwIP free the
    # pcb without telling anyone (the error callback was just detached), and
    # the close freed it again (bench: "CORRUPT HEAP" in tcp_close from the
    # web server's refusal under a connect-and-RST flood). _pcb is read and
    # cleared under the same lock as the lwIP thread's _tcp_error clears it
    (
        CLOSE_ORIGINAL,
        "#if !CONFIG_LWIP_TCPIP_CORE_LOCKING\n"
        "#error \"AsyncClient::_close (patch_asyncwebserver.py) needs CONFIG_LWIP_TCPIP_CORE_LOCKING\"\n"
        "#endif\n"
        "int8_t AsyncClient::_close() {\n"
        "  int8_t err = ERR_OK;\n"
        "  // Unlocked again only if taken here: _close also runs on the lwIP\n"
        "  // thread (constructor without a closed slot), which holds the lock\n"
        "  bool locked = !sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER);\n"
        "  if (locked) {\n    LOCK_TCPIP_CORE();\n  }\n"
        "  tcp_pcb *pcb = _pcb;\n"
        "  if (pcb) {\n"
        "    tcp_arg(pcb, NULL);\n"
        "    tcp_sent(pcb, NULL);\n"
        "    tcp_recv(pcb, NULL);\n"
        "    tcp_err(pcb, NULL);\n"
        "    tcp_poll(pcb, NULL, 0);\n"
        "    err = tcp_close(pcb);\n"
        "    if (err != ERR_OK) {\n"
        "      tcp_abort(pcb);\n"
        "      err = ERR_ABRT;\n"
        "    }\n"
        "    _pcb = NULL;\n"
        "  }\n"
        "  if (locked) {\n    UNLOCK_TCPIP_CORE();\n  }\n"
        "  if (pcb) {\n"
        "    _tcp_clear_events(this);\n"
        "    _free_closed_slot();\n"
        "    if (_discard_cb) {\n"
        "      _discard_cb(_discard_cb_arg, this);\n"
        "    }\n"
        "  }\n"
        "  return err;\n"
        "}\n",
    ),
    # lwIP calls the error callback after it freed the pcb (tcp_err
    # contract). The library still wrote tcp_arg / the callbacks into it and
    # read its state: a write into freed heap that another task may already
    # own again. Under a connect-and-RST flood at a short heap the lwIP
    # thread then looped in tcp_output (task watchdog on IDLE0)
    (
        "  if (client && client->_pcb) {\n"
        "    tcp_arg(client->_pcb, NULL);\n"
        "    if (client->_pcb->state == LISTEN) {\n"
        "      tcp_sent(client->_pcb, NULL);\n"
        "      tcp_recv(client->_pcb, NULL);\n"
        "      tcp_err(client->_pcb, NULL);\n"
        "      tcp_poll(client->_pcb, NULL, 0);\n"
        "    }\n"
        "    client->_pcb = nullptr;\n",
        "  if (client && client->_pcb) {\n"
        "    client->_pcb = nullptr; // already freed by lwIP, not touched\n",
    ),
    # tcpip calls of a client (output, write, recved, close, abort) checked
    # that the pcb they captured is still open by its "closed slot" - a slot
    # lwIP's error callback frees and a new connection may take again
    # before the call runs (16 slots, hundreds of connections a second in a
    # flood): the call then went to a freed or reused pcb (bench: lwIP
    # asserts in tcp_update_rcv_ann_wnd, CORRUPT HEAP, LoadProhibited under
    # a connect-and-RST flood). The calls now carry the client and run only
    # while the client's _pcb is still the captured pcb - read under the
    # lwIP core lock, under which _tcp_error / _lwip_fin / _close clear it.
    # The call sites pass the client through macros defined after the
    # wrappers (AsyncServer's own calls pass none and keep the slot check)
    (
        "  tcp_pcb *pcb;\n  int8_t closed_slot;\n",
        "  tcp_pcb *pcb;\n  AsyncClient *client;\n  int8_t closed_slot;\n",
    ),
    (
        "} tcp_api_call_t;\n",
        "} tcp_api_call_t;\n\n"
        "static bool _explora_pcb_open(tcp_api_call_t *msg) {\n"
        "  if (msg->client) {\n    return msg->client->pcb() == msg->pcb;\n  }\n"
        "  return msg->closed_slot == INVALID_CLOSED_SLOT || !_closed_slots[msg->closed_slot];\n"
        "}\n",
    ),
    (
        "static esp_err_t _tcp_output(tcp_pcb *pcb, int8_t closed_slot) {\n",
        "static esp_err_t _tcp_output(tcp_pcb *pcb, int8_t closed_slot, AsyncClient *client = NULL) {\n",
    ),
    (
        "static esp_err_t _tcp_write(tcp_pcb *pcb, int8_t closed_slot, const char *data, size_t size, uint8_t apiflags) {\n",
        "static esp_err_t _tcp_write(tcp_pcb *pcb, int8_t closed_slot, const char *data, size_t size, uint8_t apiflags, AsyncClient *client = NULL) {\n",
    ),
    (
        "static esp_err_t _tcp_recved(tcp_pcb *pcb, int8_t closed_slot, size_t len) {\n",
        "static esp_err_t _tcp_recved(tcp_pcb *pcb, int8_t closed_slot, size_t len, AsyncClient *client = NULL) {\n",
    ),
    (
        "static esp_err_t _tcp_close(tcp_pcb *pcb, int8_t closed_slot) {\n",
        "static esp_err_t _tcp_close(tcp_pcb *pcb, int8_t closed_slot, AsyncClient *client = NULL) {\n",
    ),
    (
        "static esp_err_t _tcp_abort(tcp_pcb *pcb, int8_t closed_slot) {\n",
        "static esp_err_t _tcp_abort(tcp_pcb *pcb, int8_t closed_slot, AsyncClient *client = NULL) {\n",
    ),
    (
        "static esp_err_t _tcp_connect(tcp_pcb *pcb, int8_t closed_slot, ip_addr_t *addr, uint16_t port, tcp_connected_fn cb) {\n",
        "static esp_err_t _tcp_connect(tcp_pcb *pcb, int8_t closed_slot, ip_addr_t *addr, uint16_t port, tcp_connected_fn cb, AsyncClient *client = NULL) {\n",
    ),
    (
        "/*\n  Async TCP Client\n */\n",
        "static inline AsyncClient *_explora_client(AsyncClient *c) {\n  return c;\n}\n"
        "static inline AsyncClient *_explora_client(AsyncServer *) {\n  return NULL;\n}\n"
        "#define _tcp_output(p, s) _tcp_output(p, s, _explora_client(this))\n"
        "#define _tcp_write(p, s, d, n, f) _tcp_write(p, s, d, n, f, _explora_client(this))\n"
        "#define _tcp_recved(p, s, l) _tcp_recved(p, s, l, _explora_client(this))\n"
        "#define _tcp_close(p, s) _tcp_close(p, s, _explora_client(this))\n"
        "#define _tcp_abort(p, s) _tcp_abort(p, s, _explora_client(this))\n\n"
        "/*\n  Async TCP Client\n */\n",
    ),
    # The library's own failure tail (no client allocated / no callback)
    # aborts the pcb but returned ERR_OK: lwIP then keeps using the freed
    # pcb. After tcp_abort() inside the accept callback it must get ERR_ABRT
    (
        "  tcp_abort(pcb);\n  return ERR_OK;\n}\n\nint8_t AsyncServer::_accepted(",
        "  tcp_abort(pcb);\n  return ERR_ABRT;\n}\n\nint8_t AsyncServer::_accepted(",
    ),
]


# AsyncTCP.cpp, applied to every occurrence (the five checked tcpip calls,
# the six wrappers setting up a call)
TCP_REPLACE_ALL = [
    ("  if (msg->closed_slot == INVALID_CLOSED_SLOT || !_closed_slots[msg->closed_slot]) {\n",
     "  if (_explora_pcb_open(msg)) {\n", 5),
    ("  msg.closed_slot = closed_slot;\n", "  msg.closed_slot = closed_slot;\n  msg.client = client;\n", 6),
]


# ESPAsyncWebServer WebServer.cpp: the accept callback (async task) creates
# the request with a throwing new. The heap check at accept (lwIP thread)
# can be stale by the time the event is handled: the Pro rebooting into a
# running request flood aborted there (std::terminate, no memory for the
# bad_alloc either; bench 2026-10-23). Checked again here, and allocated
# with nothrow. A refused client is closed with close(), not the library's
# abort(): abort() left an lwIP error event queued for the client deleted
# right after it (two Guru Meditations when the refusal path got busy)
SERVER_PATCHES = [
    (
        '#include "WebHandlerImpl.h"\n',
        '#include "WebHandlerImpl.h"\n#include <atomic>\n#include <new>\n#include <esp_heap_caps.h>\n\n'
        '// Defined in src/Web/WebServer.cpp\n'
        'extern std::atomic<uint32_t> exploraAcceptRefused;\n'
        '// Patched AsyncTCP.cpp (scripts/patch_asyncwebserver.py)\n'
        'void exploraDiscardClient(AsyncClient *c);\n',
    ),
    (
        "      AsyncWebServerRequest *r = new AsyncWebServerRequest((AsyncWebServer *)s, c);\n"
        "      if (r == NULL) {\n"
        "        c->abort();\n"
        "        delete c;\n"
        "      }\n",
        "      bool lowHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT) < %d;\n"
        "      AsyncWebServerRequest *r = lowHeap ? NULL : new (std::nothrow) AsyncWebServerRequest((AsyncWebServer *)s, c);\n"
        "      if (r == NULL) {\n"
        "        exploraAcceptRefused.fetch_add(1, std::memory_order_relaxed);\n"
        "        // Not abort() + delete: abort() made lwIP queue an error event\n"
        "        // for this client, handled after the delete (use-after-free);\n"
        "        // a FIN / RST that arrived before this callback did the same\n"
        "        exploraDiscardClient(c);\n"
        "      }\n"
        % ACCEPT_MIN_FREE_8BIT,
    ),
]


# Libraries whose patched copy entered this build (see check_patched)
APPLIED = set()


def patched_object(env, node, library, patches, replace_all):
    APPLIED.add(library)
    path = node.srcnode().get_abspath()
    with open(path) as f:
        source = f.read()
    for old, new in patches:
        if source.count(old) != 1:
            raise UserError("patch_asyncwebserver.py: %s changed, patch does not apply: %r" % (library, old[:60]))
        source = source.replace(old, new)
    for old, new, count in replace_all:
        if source.count(old) != count:
            raise UserError("patch_asyncwebserver.py: %s changed, patch does not apply: %r" % (library, old[:60]))
        source = source.replace(old, new)
    out_dir = os.path.join(env.subst("$BUILD_DIR"), "patched_" + library.lower())
    os.makedirs(out_dir, exist_ok=True)
    name = os.path.splitext(os.path.basename(path))[0]
    out = os.path.join(out_dir, name + ".cpp")
    current = open(out).read() if os.path.exists(out) else None
    if current != source:
        with open(out, "w") as f:
            f.write(source)
    # Quoted includes of the original directory must still resolve
    return env.Object(
        os.path.join(out_dir, name + ".o"),
        out,
        CPPPATH=env.get("CPPPATH", []) + [os.path.dirname(path)],
    )


def patch_webrequest(env, node):
    path = node.srcnode().get_abspath()
    if os.path.basename(path) != "WebRequest.cpp" or "ESPAsyncWebServer" not in path:
        return node
    return patched_object(env, node, "AsyncWebServer", PATCHES, REPLACE_ALL)


def patch_webserver(env, node):
    path = node.srcnode().get_abspath()
    if os.path.basename(path) != "WebServer.cpp" or "ESPAsyncWebServer" not in path:
        return node
    return patched_object(env, node, "AsyncWebServer-server", SERVER_PATCHES, [])


def patch_asynctcp(env, node):
    path = node.srcnode().get_abspath()
    if os.path.basename(path) != "AsyncTCP.cpp" or "AsyncTCP" not in os.path.dirname(path):
        return node
    # The patches are written against this version. platformio.ini pins it;
    # an older checkout's library folder may still hold a newer AsyncTCP
    # (once installed as ESPAsyncWebServer's dependency) for the library
    # finder to pick
    manifest = os.path.join(os.path.dirname(os.path.dirname(path)), "library.json")
    version = None
    if os.path.exists(manifest):
        with open(manifest) as f:
            version = json.load(f).get("version")
    if version != ASYNCTCP_VERSION:
        raise UserError("patch_asyncwebserver.py: the build picked AsyncTCP %s from %s, the patches need %s - "
                        "remove the other AsyncTCP folder from .pio/libdeps" % (version, path, ASYNCTCP_VERSION))
    return patched_object(env, node, "AsyncTCP", TCP_PATCHES, TCP_REPLACE_ALL)


# Loud failure when a library was linked unpatched (a middleware pattern or
# path test no longer matches): the limits above would silently be missing.
# A failing post action fails the build; the .elf gets no signature, so the
# next build links (and checks) again
def check_patched(target, source, env):
    missing = {"AsyncWebServer", "AsyncWebServer-server", "AsyncTCP"} - APPLIED
    if missing:
        print("patch_asyncwebserver.py: ERROR - linked without the patched copy of: %s "
              "(a build middleware no longer matches the library path)" % ", ".join(sorted(missing)))
        return 1
    return 0


env.AddBuildMiddleware(patch_webrequest, "*WebRequest.cpp")  # noqa: F821
env.AddBuildMiddleware(patch_webserver, "*WebServer.cpp")  # noqa: F821
env.AddBuildMiddleware(patch_asynctcp, "*AsyncTCP.cpp")  # noqa: F821
env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check_patched)  # noqa: F821
