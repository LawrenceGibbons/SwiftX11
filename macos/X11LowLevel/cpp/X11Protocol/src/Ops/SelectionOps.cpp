//
//  SelectionOps.cpp
//  X11LowLevel
//
//  Handles selection opcodes 22-25 (SetSelectionOwner, GetSelectionOwner,
//  ConvertSelection, SendEvent) with macOS clipboard bridge for CLIPBOARD.
//

#include "Ops/SelectionOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Core/XProtoServer.hpp"
#include "Core/PropertyTable.hpp"
#include "Core/ClipboardAtoms.hpp"
#include "Core/IncrTransfer.hpp"
#include "Core/HostCommandQueue.hpp"
#include "Utils/ByteReader.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/WireLE.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/X11ExtOpcodes.hpp"
#include "Core/timestamp.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" {
#include "SwiftX11Bridge.h"
#include "Utils/MachTime.hpp"
}

// Access the server's HostCommandQueue for deferred clipboard capture.
extern "C" x11::XProtoServer* x11_proto_bridge_get_server(void);

namespace x11 {

// ---------------------------------------------------------------------------
// Static selection table: atom -> owner window XID
// ---------------------------------------------------------------------------
static std::mutex sSelMtx;
static std::unordered_map<uint32_t /*atom*/, uint32_t /*owner_wid*/> sSelOwner;

// macOS changeCount at the time each selection was claimed by an X11 owner.
// Used to detect when the macOS clipboard has been updated externally (e.g.
// user Cmd+C in Safari) after an X11 app claimed the selection.
static std::unordered_map<uint32_t /*atom*/, int64_t> sSelMacCC;

// macOS changeCount AFTER our last proactive push of X11 content to macOS.
// Used to detect if the user copied something on macOS between our pushes.
// If currentCC > sSelPushedCC, the macOS clipboard has external content
// that we must NOT overwrite with stale X11 data.
static std::unordered_map<uint32_t /*atom*/, int64_t> sSelPushedCC;

// Timestamp of last SetSelectionOwner per selection atom (ICCCM compliance).
// Used to reject stale requests and return via TIMESTAMP target.
static std::unordered_map<uint32_t /*atom*/, uint32_t /*time*/> sSelTime;

// ---------------------------------------------------------------------------
// INCR receive state (server-as-requestor).  When the proactive capture's
// SelectionNotify carries a property of type INCR, the owner will deliver
// the actual data as chunks via ChangeProperty on the proxy requestor
// (root, XID 1), each acknowledged by deleting the property.  We accumulate
// chunks here; a zero-length chunk completes the transfer.
// xproto thread only — no mutex.
// ---------------------------------------------------------------------------
struct IncrRecvState {
  uint32_t selection = 0;   // CLIPBOARD or PRIMARY atom
  uint64_t announced = 0;   // owner's size estimate (advisory)
  std::vector<uint8_t> data;
};
static std::unordered_map<uint64_t /*(wid<<32)|prop*/, IncrRecvState> sIncrRecv;

static inline uint64_t incrKey(uint32_t wid, uint32_t prop) {
  return (uint64_t(wid) << 32) | uint64_t(prop);
}

// Accumulation cap — matches the PropertyTable safety valve (48MB).
static constexpr uint64_t kIncrRecvMax = (3ull << 24);

// Send PropertyNotify for the proxy requestor window on the CURRENT
// transport (the owner is the client being dispatched in both call paths).
// The owner waits for state=Deleted on the requestor to advance the
// transfer.
//
// MUST use sendAll, not sendEvent32: the proxy requestor is root (XID 1),
// which has no WindowView — sendEvent32 silently drops events for windows
// it can't look up, which starved the owner of acks and stalled every
// INCR transfer (v1.19.36.9 field failure).
static void incrNotifyOwner(XProtoContext& ctx, uint32_t wid, uint32_t prop,
                            bool deleted) {
  uint8_t ev[32] = {};
  ev[0] = 28; // PropertyNotify
  wire::wr16_le(ev + 2, ctx.transport().lastSeq());
  wire::wr32_le(ev + 4, wid);
  wire::wr32_le(ev + 8, prop);
  wire::wr32_le(ev + 12, x11_now_ms_monotonic());
  ev[16] = deleted ? 1 : 0;
  (void)ctx.transport().sendAll(ev, 32);
}


// ---------------------------------------------------------------------------
// FocusIn hook: claim PRIMARY + CLIPBOARD when macOS clipboard changed.
// Called from XProtoServerBridge Focus handler when a window gains focus.
// If the user Cmd+C'd on macOS while in another app, the macOS clipboard
// is newer than any X11 selection.  By claiming ownership to root (XID 1),
// the next ConvertSelection goes through the server and serves macOS content.
// Without this, Xlib short-circuits ConvertSelection when the requestor
// is also the owner (e.g., xterm owns PRIMARY from a previous select).
// ---------------------------------------------------------------------------
void SelectionOps::claimSelectionsIfMacOSChanged(XProtoContext& ctx) {
  int64_t currentCC = x11_clipboard_get_change_count();
  if (currentCC <= 0) return;

  std::lock_guard<std::mutex> lk(sSelMtx);

  const uint32_t sels[] = { atom::kPRIMARY, atom::kCLIPBOARD };
  for (uint32_t sel : sels) {
    auto ccIt = sSelMacCC.find(sel);
    int64_t lastKnownCC = (ccIt != sSelMacCC.end()) ? ccIt->second : -1;

    if (currentCC > lastKnownCC) {
      // macOS clipboard is newer — claim this selection to root proxy
      auto ownerIt = sSelOwner.find(sel);
      uint32_t prevOwner = (ownerIt != sSelOwner.end()) ? ownerIt->second : 0;

      if (prevOwner > 1) {
        // Send SelectionClear to the previous X11 owner
        uint8_t ev[32] = {0};
        ev[0] = 29; // SelectionClear
        wire::wr16_le(ev + 2, ctx.transport().lastSeq());
        wire::wr32_le(ev + 4, x11_now_ms_monotonic()); // ICCCM: never 0
        wire::wr32_le(ev + 8,  prevOwner);
        wire::wr32_le(ev + 12, sel);
        (void)ctx.transport().sendEvent32(prevOwner, ev);

        TS_DBG("[CLIPBOARD] Focus claim: sel=%u prev=0x%08X→root "
                "(macOS cc %lld > %lld)\n",
                (unsigned)sel, (unsigned)prevOwner,
                (long long)currentCC, (long long)lastKnownCC);
      }

      sSelOwner[sel] = 1; // root proxy
      sSelMacCC[sel] = currentCC;
      sSelPushedCC[sel] = currentCC;
    }
  }
}

// ---------------------------------------------------------------------------
// Clear selection ownership held by a disconnecting client's windows (§6.3)
// ---------------------------------------------------------------------------
void SelectionOps::clearOwnersOwnedBy(uint32_t clientBase, uint32_t clientMask) {
  const uint32_t hi = ~clientMask;
  std::lock_guard<std::mutex> lk(sSelMtx);
  for (auto it = sSelOwner.begin(); it != sSelOwner.end(); ) {
    const uint32_t owner = it->second;
    // Keep the root proxy (XID 1 = macOS-clipboard bridge); clear real
    // client windows in the disconnecting range so ConvertSelection falls
    // back to serving the macOS clipboard instead of a dead owner.
    if (owner > 1 && (owner & hi) == (clientBase & hi)) {
      it = sSelOwner.erase(it);
    } else {
      ++it;
    }
  }
}

// ---------------------------------------------------------------------------
// XFIXES SelectionNotify subscriptions (M4).
// Modeled on xorg xfixes/select.c.  Accessed only from the single xproto
// dispatch thread, so no lock is needed on the subscription vector.
// ---------------------------------------------------------------------------
namespace {
struct XFixesSelSub {
  int      fd;         // subscribing client's transport fd (event destination client)
  uint32_t window;     // window the client selected on (event destination window)
  uint32_t selection;  // selection atom being watched
  uint32_t mask;       // XFixes selection-event mask (SetSelectionOwner/... bits)
};
std::vector<XFixesSelSub> g_xfixesSubs;
constexpr uint32_t kXFixesSetSelectionOwnerNotifyMask = (1u << 0);
constexpr uint8_t  kXFixesSelectionNotifyEvent        = 0; // event number within XFIXES
} // namespace

void SelectionOps::xfixesSelectSelectionInput(int fd, uint32_t window,
                                              uint32_t selection, uint32_t mask) {
  for (auto it = g_xfixesSubs.begin(); it != g_xfixesSubs.end(); ++it) {
    if (it->fd == fd && it->window == window && it->selection == selection) {
      if (mask == 0) g_xfixesSubs.erase(it);  // eventMask 0 = unsubscribe
      else           it->mask = mask;
      return;
    }
  }
  if (mask != 0) g_xfixesSubs.push_back({fd, window, selection, mask});
}

void SelectionOps::xfixesClearSubscriptionsOwnedBy(int fd) {
  g_xfixesSubs.erase(
      std::remove_if(g_xfixesSubs.begin(), g_xfixesSubs.end(),
                     [fd](const XFixesSelSub& s) { return s.fd == fd; }),
      g_xfixesSubs.end());
}

void SelectionOps::xfixesNotifySetOwner(XProtoContext& ctx, uint32_t selection,
                                        uint32_t ownerWindow, uint32_t timestamp) {
  if (g_xfixesSubs.empty()) return;
  const uint8_t evType =
      (uint8_t)(x11::ext::kXFIXES_FirstEvent + kXFixesSelectionNotifyEvent);
  for (const auto& s : g_xfixesSubs) {
    if (s.selection != selection) continue;
    if (!(s.mask & kXFixesSetSelectionOwnerNotifyMask)) continue;
    uint8_t ev[32];
    std::memset(ev, 0, sizeof(ev));
    ev[0] = evType;                                    // XFixesSelectionNotify
    ev[1] = 0;                                          // subtype=SetSelectionOwnerNotify
    wire::wr16_le(ev + 2,  ctx.transport().lastSeq());  // sequenceNumber
    wire::wr32_le(ev + 4,  s.window);                   // window (client's selected window)
    wire::wr32_le(ev + 8,  ownerWindow);                // owner (new owner; 0 = None)
    wire::wr32_le(ev + 12, selection);                  // selection atom
    wire::wr32_le(ev + 16, timestamp);                  // timestamp
    wire::wr32_le(ev + 20, timestamp);                  // selectionTimestamp
    // pad2 (offset 24) and pad3 (offset 28) already zero.
    (void)ctx.transport().sendEvent32(s.window, ev);    // routes to owner_fd of s.window
  }
}

// ---------------------------------------------------------------------------
// Construction — register opcodes 22-25
// ---------------------------------------------------------------------------
SelectionOps::SelectionOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::SetSelectionOwner, &SelectionOps::onMajor, this); // 22
  reg.registerMajor(x11::opcode::GetSelectionOwner, &SelectionOps::onMajor, this); // 23
  reg.registerMajor(x11::opcode::ConvertSelection,  &SelectionOps::onMajor, this); // 24
  reg.registerMajor(x11::opcode::SendEvent,         &SelectionOps::onMajor, this); // 25
}

void SelectionOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<SelectionOps*>(user)->handle(ctx, dc);
}

void SelectionOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::SetSelectionOwner: handleSetSelectionOwner(ctx, dc.seq, dc.br); return;
    case x11::opcode::GetSelectionOwner: handleGetSelectionOwner(ctx, dc.seq, dc.br); return;
    case x11::opcode::ConvertSelection:  handleConvertSelection(ctx, dc.seq, dc.br); return;
    case x11::opcode::SendEvent:         handleSendEvent(ctx, dc.seq, dc.minor, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      return;
  }
}

// ---------------------------------------------------------------------------
// 22 SetSelectionOwner  (void — no reply)
// Body (12 bytes): CARD32 owner, CARD32 selection, CARD32 time
// ---------------------------------------------------------------------------
void SelectionOps::handleSetSelectionOwner(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }

  const uint32_t owner     = br.readU32();
  const uint32_t selection = br.readU32();
  const uint32_t time      = br.readU32();

  br.skip(br.remaining());

  // Validate owner window (0 = None means "no owner", root = 1 is our proxy)
  if (owner != 0 && owner != 1 && !ctx.windows().exists(owner)) {
    ctx.transport().sendErrorCore(x11::error::BadWindow, seq, owner, x11::opcode::SetSelectionOwner);
    return;
  }

#ifndef NDEBUG
  TS_DBG("[SEL] SetSelectionOwner sel=%u owner=0x%08X time=%u\n",
          (unsigned)selection, (unsigned)owner, (unsigned)time);
#endif

  uint32_t prevOwner = 0;
  {
    std::lock_guard<std::mutex> lk(sSelMtx);

    // ICCCM timestamp validation: if time is not CurrentTime (0) and is
    // earlier than the last SetSelectionOwner time, ignore the request.
    if (time != 0) {
      auto tIt = sSelTime.find(selection);
      if (tIt != sSelTime.end() && tIt->second != 0) {
        // Compare as unsigned (X11 timestamps wrap at 2^32)
        int32_t diff = (int32_t)(time - tIt->second);
        if (diff < 0) {
          TS_DBG("[SEL] SetSelectionOwner REJECTED: time %u < last %u\n",
                  (unsigned)time, (unsigned)tIt->second);
          return;
        }
      }
    }

    auto it = sSelOwner.find(selection);
    if (it != sSelOwner.end()) {
      prevOwner = it->second;
      it->second = owner;
    } else {
      sSelOwner[selection] = owner;
    }
    sSelTime[selection] = time;
  }

  // If previous owner was different and non-zero, send SelectionClear (type 29)
  // Skip root window (1) — our clipboard proxy, no client to receive the event.
  if (prevOwner > 1 && prevOwner != owner) {
    uint8_t ev[32] = {0};
    ev[0] = 29; // SelectionClear
    wire::wr16_le(ev + 2, ctx.transport().lastSeq());
    wire::wr32_le(ev + 4,  time ? time : x11_now_ms_monotonic()); // ICCCM: never 0
    wire::wr32_le(ev + 8,  prevOwner);  // window
    wire::wr32_le(ev + 12, selection);  // atom
    (void)ctx.transport().sendEvent32(prevOwner, ev);
  }

  // XFIXES (M4): notify every subscriber that this selection changed owner.
  // Fires on any SetSelectionOwner — claim (owner!=0) or clear (owner==0) —
  // matching xorg's SelectionSetOwner callback.  `owner` is the new owner.
  SelectionOps::xfixesNotifySetOwner(ctx, selection, owner,
                                     time ? time : (uint32_t)x11_now_ms_monotonic());

  // Track macOS changeCount so ConvertSelection can detect external macOS changes.
  // Also reset sSelPushedCC so the proactive capture push isn't blocked by
  // external macOS clipboard changes that happened before this ownership claim.
  // Without this reset, any macOS Cmd+C after our last push permanently blocks
  // all subsequent X11→macOS clipboard captures.
  {
    int64_t cc = x11_clipboard_get_change_count();
    std::lock_guard<std::mutex> lk(sSelMtx);
    sSelMacCC[selection] = cc;
    sSelPushedCC[selection] = cc;
  }

  // Proactive clipboard capture: send SelectionRequest from root (XID 1)
  // to the new owner asking for UTF8_STRING.  The owner responds with
  // ChangeProperty + SendEvent(SelectionNotify), which our handleSendEvent
  // intercepts and pushes to NSPasteboard.
  //
  // This mimics XQuartz's pasteboard proxy (window 0x00800001) which
  // automatically requests CLIPBOARD content on ownership change.
  //
  // Capture both CLIPBOARD (Vivado Edit→Copy) and PRIMARY (xterm select).
  // Only when the owner is a real client window (not None/root).
  if ((selection == atom::kCLIPBOARD || selection == atom::kPRIMARY) && owner > 1) {
    uint8_t ev[32] = {0};
    ev[0] = 30; // SelectionRequest
    wire::wr16_le(ev + 2, ctx.transport().lastSeq());
    wire::wr32_le(ev + 4,  time);                   // time
    wire::wr32_le(ev + 8,  owner);                   // owner
    wire::wr32_le(ev + 12, 1u);                        // requestor (root XID = our proxy)
    wire::wr32_le(ev + 16, selection);               // selection (CLIPBOARD)
    wire::wr32_le(ev + 20, atom::kUTF8_STRING);     // target
    wire::wr32_le(ev + 24, atom::kUTF8_STRING);     // property
    (void)ctx.transport().sendEvent32(owner, ev);

#ifndef NDEBUG
    TS_DBG("[CLIPBOARD] Sent proactive SelectionRequest(UTF8_STRING) to owner 0x%08X\n",
            (unsigned)owner);
#endif
  }

}

// ---------------------------------------------------------------------------
// 23 GetSelectionOwner  (reply)
// Body (4 bytes): CARD32 selection
// Reply: bytes 8..11 = owner window
// ---------------------------------------------------------------------------
void SelectionOps::handleGetSelectionOwner(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }

  const uint32_t selection = br.readU32();

  br.skip(br.remaining());

  uint32_t owner = 0;
  {
    std::lock_guard<std::mutex> lk(sSelMtx);
    auto it = sSelOwner.find(selection);
    if (it != sSelOwner.end()) owner = it->second;
  }

  ReplyWriter rw(ctx.transport());
  rw.sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    wire::wr32_le(rep.data() + 8, owner);
  });

#ifdef X11_TRACE_VERBOSE
  ctx.tracef("[SelectionOps] GetSelectionOwner sel=%u -> owner=0x%08X\n",
             (unsigned)selection, (unsigned)owner);
#endif
}

// ---------------------------------------------------------------------------
// Helper: serve CLIPBOARD from macOS pasteboard (no X11 owner)
// Writes property on requestor window and sends SelectionNotify.
// Returns true if handled, false if clipboard bridge unavailable.
// ---------------------------------------------------------------------------
static bool serveMacOSClipboard(XProtoContext& ctx,
                                uint32_t requestor,
                                uint32_t selection,
                                uint32_t target,
                                uint32_t property,
                                uint32_t time)
{
  // ICCCM forbids time=0 in SelectionNotify (§2.13): stamp server time
  // when the requestor passed CurrentTime.
  if (time == 0) time = x11_now_ms_monotonic();

  // If target == TARGETS, return list of supported target atoms
  if (target == atom::kTARGETS) {
    // Write TARGETS list as CARD32 array (format=32)
    const uint32_t targets[] = {
      atom::kTARGETS,
      atom::kUTF8_STRING,
      atom::kSTRING,
      atom::kTEXT,
      atom::kTIMESTAMP,
      atom::kINCR,
    };
    const uint32_t nTargets = sizeof(targets) / sizeof(targets[0]);

    // Encode as little-endian CARD32 array
    std::vector<uint8_t> payload(nTargets * 4);
    for (uint32_t i = 0; i < nTargets; i++) {
      wire::wr32_le(payload.data() + i * 4, targets[i]);
    }

    PropertyTable::instance().setReplace(
      requestor, property,
      atom::kATOM, // type = ATOM
      32,          // format = 32
      payload.data(), payload.size()
    );

    // Send SelectionNotify
    uint8_t ev[32] = {0};
    ev[0] = 31; // SelectionNotify
    wire::wr16_le(ev + 2, ctx.transport().lastSeq());
    wire::wr32_le(ev + 4,  time);
    wire::wr32_le(ev + 8,  requestor);
    wire::wr32_le(ev + 12, selection);
    wire::wr32_le(ev + 16, target);
    wire::wr32_le(ev + 20, property);
    (void)ctx.transport().sendEvent32(requestor, ev);

#ifndef NDEBUG
    TS_DBG("[CLIPBOARD] Served TARGETS (%u atoms) to 0x%08X\n",
            nTargets, (unsigned)requestor);
#endif
    return true;
  }

  // If target == TIMESTAMP, return the time the selection was last acquired
  if (target == atom::kTIMESTAMP) {
    uint32_t selTime = 0;
    {
      std::lock_guard<std::mutex> lk(sSelMtx);
      auto tIt = sSelTime.find(selection);
      if (tIt != sSelTime.end()) selTime = tIt->second;
    }
    uint8_t ts[4] = {0};
    wire::wr32_le(ts, selTime);
    PropertyTable::instance().setReplace(
      requestor, property,
      atom::kTIMESTAMP, 32, ts, 4
    );

    uint8_t ev[32] = {0};
    ev[0] = 31; // SelectionNotify
    wire::wr16_le(ev + 2, ctx.transport().lastSeq());
    wire::wr32_le(ev + 4,  time);
    wire::wr32_le(ev + 8,  requestor);
    wire::wr32_le(ev + 12, selection);
    wire::wr32_le(ev + 16, target);
    wire::wr32_le(ev + 20, property);
    (void)ctx.transport().sendEvent32(requestor, ev);
    return true;
  }

  // For UTF8_STRING, STRING, or TEXT — serve text from macOS pasteboard
  if (target == atom::kUTF8_STRING ||
      target == atom::kSTRING ||
      target == atom::kTEXT)
  {
    // Read macOS clipboard into dynamic buffer (up to 12MB)
    static constexpr uint32_t kMaxClipRead = 12u * 1024 * 1024;
    std::vector<char> clipBuf(kMaxClipRead);
    uint32_t len = x11_clipboard_get_text(clipBuf.data(), kMaxClipRead);

    TS_DBG("[CLIPBOARD] serveMacOS: read %u bytes for target=%u req=0x%08X\n",
            len, (unsigned)target, (unsigned)requestor);

    if (len == 0) {
      // Clipboard empty or no bridge — send SelectionNotify with property=None
      uint8_t ev[32] = {0};
      ev[0] = 31; // SelectionNotify
      wire::wr16_le(ev + 2, ctx.transport().lastSeq());
      wire::wr32_le(ev + 4,  time);
      wire::wr32_le(ev + 8,  requestor);
      wire::wr32_le(ev + 12, selection);
      wire::wr32_le(ev + 16, target);
      wire::wr32_le(ev + 20, 0); // property = None
      (void)ctx.transport().sendEvent32(requestor, ev);
      return true;
    }

    // Determine requestor's owner_fd for INCR cleanup on disconnect
    int reqFd = -1;
    {
      const WindowView* rv = ctx.window(requestor);
      if (rv) reqFd = rv->owner_fd;
    }

    if (len > IncrTransfer::kChunkSize) {
      // Large data: use INCR protocol (chunked transfer)
      std::vector<uint8_t> data(reinterpret_cast<const uint8_t*>(clipBuf.data()),
                                reinterpret_cast<const uint8_t*>(clipBuf.data()) + len);
      uint32_t totalSize = IncrTransfer::instance().startTransfer(
        requestor, property, target, reqFd, std::move(data));

      TS_DBG("[CLIPBOARD] INCR started: %u bytes to 0x%08X (chunks of %zu)\n",
              totalSize, (unsigned)requestor, IncrTransfer::kChunkSize);
    } else {
      // Small data: direct property write (existing fast path)
      PropertyTable::instance().setReplace(
        requestor, property,
        target, 8,
        reinterpret_cast<const uint8_t*>(clipBuf.data()), len
      );

#ifndef NDEBUG
      TS_DBG("[CLIPBOARD] Served %u bytes from macOS pasteboard to 0x%08X\n",
              len, (unsigned)requestor);
#endif
    }

    // Send SelectionNotify with property set (for both INCR and direct)
    uint8_t ev[32] = {0};
    ev[0] = 31; // SelectionNotify
    wire::wr16_le(ev + 2, ctx.transport().lastSeq());
    wire::wr32_le(ev + 4,  time);
    wire::wr32_le(ev + 8,  requestor);
    wire::wr32_le(ev + 12, selection);
    wire::wr32_le(ev + 16, target);
    wire::wr32_le(ev + 20, property);
    (void)ctx.transport().sendEvent32(requestor, ev);
    return true;
  }

  // Unknown target — send failure
  uint8_t ev[32] = {0};
  ev[0] = 31; // SelectionNotify
  wire::wr16_le(ev + 2, ctx.transport().lastSeq());
  wire::wr32_le(ev + 4,  time);
  wire::wr32_le(ev + 8,  requestor);
  wire::wr32_le(ev + 12, selection);
  wire::wr32_le(ev + 16, target);
  wire::wr32_le(ev + 20, 0); // property = None (conversion failed)
  (void)ctx.transport().sendEvent32(requestor, ev);
  return true;
}

// ---------------------------------------------------------------------------
// 24 ConvertSelection  (void)
// Body (20 bytes): CARD32 requestor, CARD32 selection, CARD32 target,
//                  CARD32 property, CARD32 time
// ---------------------------------------------------------------------------
void SelectionOps::handleConvertSelection(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t requestor = br.readU32();
  const uint32_t selection = br.readU32();
  const uint32_t target    = br.readU32();
  uint32_t property        = br.readU32();
  const uint32_t time      = br.readU32();

  br.skip(br.remaining());

#ifndef NDEBUG
  TS_DBG("[SEL] ConvertSelection sel=%u target=%u req=0x%08X prop=%u time=%u\n",
          (unsigned)selection, (unsigned)target, (unsigned)requestor, (unsigned)property, (unsigned)time);
#endif

  // Per ICCCM: if property is None, use target as property
  if (property == 0) property = target;

  uint32_t owner = 0;
  {
    std::lock_guard<std::mutex> lk(sSelMtx);
    auto it = sSelOwner.find(selection);
    if (it != sSelOwner.end()) owner = it->second;
  }

  if (owner != 0) {
    // Check if macOS clipboard has newer content than when the X11 owner was set.
    // This handles the case where user Cmd+C in Safari, then pastes in xterm.
    if (selection == atom::kPRIMARY || selection == atom::kCLIPBOARD) {
      int64_t storedCC = -1;
      {
        std::lock_guard<std::mutex> lk(sSelMtx);
        auto it = sSelMacCC.find(selection);
        if (it != sSelMacCC.end()) storedCC = it->second;
      }
      int64_t currentCC = x11_clipboard_get_change_count();
      if (currentCC > storedCC) {
#ifndef NDEBUG
        TS_DBG("[CLIPBOARD] macOS clipboard newer (cc %lld > %lld) — serving from macOS\n",
                (long long)currentCC, (long long)storedCC);
#endif
        serveMacOSClipboard(ctx, requestor, selection, target, property, time);
        return;
      }
#ifndef NDEBUG
      TS_DBG("[CLIPBOARD] macOS NOT newer (cc %lld <= %lld) — forwarding to X11 owner 0x%08X\n",
              (long long)currentCC, (long long)storedCC, (unsigned)owner);
#endif
    }

    // If owner is root window (1) — this is our proxy after ClipboardCapture.
    // Root has no client transport, so serve from macOS clipboard directly.
    if (owner == 1) {
#ifndef NDEBUG
      TS_DBG("[CLIPBOARD] owner=root (proxy) — serving from macOS\n");
#endif
      if (selection == atom::kPRIMARY || selection == atom::kCLIPBOARD) {
        serveMacOSClipboard(ctx, requestor, selection, target, property, time);
        return;
      }
    }

    // X11 client owns this selection.
    // If the owner is on the SAME connection as the requestor, don't send
    // SelectionRequest — the client's event thread may be blocked (Java
    // Swing menu tracking, text selection callback) and can't process it,
    // causing deadlock.  Instead, send SelectionNotify with property=None
    // (conversion refused).  The client handles this gracefully.
    {
      WindowView ownerWv{}, reqWv{};
      bool ownerOk = ctx.windows().snapshot(owner, ownerWv);
      bool reqOk   = ctx.windows().snapshot(requestor, reqWv);
      if (ownerOk && reqOk && ownerWv.owner_fd == reqWv.owner_fd) {
        // Same client — can't do round-trip, refuse the conversion
        TS_DBG("[CLIPBOARD] REFUSED: same-client deadlock prevention "
                "(owner=0x%08X req=0x%08X fd=%d)\n",
                (unsigned)owner, (unsigned)requestor, ownerWv.owner_fd);
        uint8_t nev[32] = {0};
        nev[0] = 31; // SelectionNotify
        wire::wr16_le(nev + 2, ctx.transport().lastSeq());
        wire::wr32_le(nev + 4,  time);
        wire::wr32_le(nev + 8,  requestor);
        wire::wr32_le(nev + 12, selection);
        wire::wr32_le(nev + 16, target);
        wire::wr32_le(nev + 20, 0); // property = None (can't self-convert)
        (void)ctx.transport().sendEvent32(requestor, nev);
        return;
      }
    }

    // Different client owns the selection — forward SelectionRequest
    TS_DBG("[CLIPBOARD] Forwarding SelectionRequest to X11 owner 0x%08X (fd=%d)\n",
            (unsigned)owner, ctx.transport().clientFd());
    uint8_t ev[32] = {0};
    ev[0] = 30; // SelectionRequest
    wire::wr16_le(ev + 2, ctx.transport().lastSeq());
    wire::wr32_le(ev + 4,  time);
    wire::wr32_le(ev + 8,  owner);
    wire::wr32_le(ev + 12, requestor);
    wire::wr32_le(ev + 16, selection);
    wire::wr32_le(ev + 20, target);
    wire::wr32_le(ev + 24, property);
    (void)ctx.transport().sendEvent32(owner, ev);
    return;
  }

  // No X11 owner — try macOS clipboard bridge for CLIPBOARD and PRIMARY
  if (selection == atom::kCLIPBOARD || selection == atom::kPRIMARY) {
    if (serveMacOSClipboard(ctx, requestor, selection, target, property, time)) {
      return;
    }
  }

  // No owner and no bridge — send SelectionNotify with property=None
  uint8_t ev[32] = {0};
  ev[0] = 31; // SelectionNotify
  wire::wr16_le(ev + 2, ctx.transport().lastSeq());
  wire::wr32_le(ev + 4,  time);
  wire::wr32_le(ev + 8,  requestor);
  wire::wr32_le(ev + 12, selection);
  wire::wr32_le(ev + 16, target);
  wire::wr32_le(ev + 20, 0); // property = None
  (void)ctx.transport().sendEvent32(requestor, ev);
}

// ---------------------------------------------------------------------------
// 25 SendEvent  (void)
// dc.minor = propagate byte
// Body (36 bytes): CARD32 destination, CARD32 eventMask, BYTE[32] event
// ---------------------------------------------------------------------------
void SelectionOps::handleSendEvent(XProtoContext& ctx, uint16_t /*seq*/, uint8_t propagate, ByteReader& br) {
  (void)propagate;

  if (br.remaining() < 36) { br.skip(br.remaining()); return; }

  const uint32_t destination = br.readU32();
  const uint32_t eventMask   = br.readU32();
  (void)eventMask;

  uint8_t event[32];
  for (int i = 0; i < 32; ++i) event[i] = br.readU8();

  br.skip(br.remaining());

  // Set bit 0x80 on event[0] (SendEvent flag per X11 spec)
  event[0] |= 0x80u;

  // Stamp server sequence number into forwarded event (bytes 2-3).
  // Clients construct events with seq=0; XCB on the receiving end validates
  // that the sequence number is non-zero and monotonically increasing.
  // A real X11 server always overwrites bytes 2-3 with the recipient's
  // last-processed sequence number.
  wire::wr16_le(event + 2, ctx.transport().lastSeq());

  // Resolve special destinations
  uint32_t resolvedDest = destination;
  if (destination == 0) {
    // PointerWindow
    resolvedDest = ctx.input().last_xid;
  } else if (destination == 1) {
    // InputFocus
    resolvedDest = ctx.input().focus_xid;
  }

  if (resolvedDest != 0) {
    (void)ctx.transport().sendEvent32(resolvedDest, event);
  }

  // If this is a SelectionNotify for PRIMARY or CLIPBOARD, capture data for macOS bridge
  const uint8_t evType = event[0] & 0x7Fu;
  if (evType == 31) { // SelectionNotify
    const uint32_t selAtom = wire::rd32_le(event + 12);
    const uint32_t propAtom = wire::rd32_le(event + 20);
    // Use the requestor from the event (bytes 8-11), NOT resolvedDest.
    // When our proactive capture uses root (XID 1) as requestor, Java sends
    // SendEvent(destination=1), which gets resolved as InputFocus.  But the
    // property was set on root (1), so we must read from the event's requestor.
    const uint32_t evRequestor = wire::rd32_le(event + 8);
    if ((selAtom == atom::kCLIPBOARD || selAtom == atom::kPRIMARY) &&
        propAtom == 0)
    {
#ifndef NDEBUG
      // Owner refused the conversion (property=None). Classic causes:
      // ICCCM timestamp rejection, unsupported target, or empty selection.
      TS_DBG("[CLIPBOARD] Capture FAILED: owner sent SelectionNotify property=None "
              "(sel=%u requestor=0x%08X) — conversion refused\n",
              (unsigned)selAtom, (unsigned)wire::rd32_le(event + 8));
#endif
    }
    if ((selAtom == atom::kCLIPBOARD || selAtom == atom::kPRIMARY) &&
        propAtom != 0 && evRequestor != 0)
    {
      // The selection owner just set a property on the requestor.
      // Read it and push to macOS clipboard.
      PropertyTable::Prop p{};
      const bool haveProp = PropertyTable::instance().get(evRequestor, propAtom, p);

      // Large transfers arrive as INCR: the property holds a CARD32 size
      // estimate (type=INCR, format=32).  Start a chunked receive — the
      // owner sends the data via ChangeProperty on the requestor, gated on
      // our PropertyNotify(Deleted) acks (see PropOps → incrOnChunk).
      if (haveProp && p.type == atom::kINCR && p.format == 32) {
        uint64_t announced = 0;
        if (p.data.size() >= 4) announced = wire::rd32_le(p.data.data());
        sIncrRecv[incrKey(evRequestor, propAtom)] =
            IncrRecvState{selAtom, announced, {}};
        PropertyTable::instance().erase(evRequestor, propAtom);
        incrNotifyOwner(ctx, evRequestor, propAtom, /*deleted*/true);
#ifndef NDEBUG
        TS_DBG("[CLIPBOARD] INCR receive START: sel=%u req=0x%08X prop=%u "
                "announced=%llu bytes\n",
                (unsigned)selAtom, (unsigned)evRequestor, (unsigned)propAtom,
                (unsigned long long)announced);
#endif
        return;
      }

#ifndef NDEBUG
      if (!haveProp || p.format != 8 || p.data.empty()) {
        TS_DBG("[CLIPBOARD] Capture FAILED: prop %s (sel=%u req=0x%08X prop=%u "
                "format=%u size=%zu type=%u)\n",
                !haveProp ? "missing" : (p.format != 8 ? "wrong format (INCR?)" : "empty"),
                (unsigned)selAtom, (unsigned)evRequestor, (unsigned)propAtom,
                (unsigned)p.format, p.data.size(), (unsigned)p.type);
      }
#endif
      if (haveProp &&
          p.format == 8 && !p.data.empty())
      {
        // Before pushing X11 content to macOS, check if the macOS clipboard
        // was updated externally (user Cmd+C) since our last push.  If so,
        // the macOS content is NEWER — don't overwrite it with stale X11 data.
        bool shouldPush = true;
        {
          std::lock_guard<std::mutex> lk(sSelMtx);
          auto it = sSelPushedCC.find(selAtom);
          if (it != sSelPushedCC.end()) {
            int64_t currentCC = x11_clipboard_get_change_count();
            if (currentCC > it->second) {
              // macOS clipboard was updated externally since our last push.
              // The user's macOS content is newer — skip the push.
              shouldPush = false;
#ifndef NDEBUG
              TS_DBG("[CLIPBOARD] Skipping push — macOS clipboard updated externally "
                      "(cc %lld > pushed %lld)\n",
                      (long long)currentCC, (long long)it->second);
#endif
            }
          }
        }

        if (shouldPush) {
          x11_clipboard_set_text(reinterpret_cast<const char*>(p.data.data()),
                                 (uint32_t)p.data.size());
          // Record the CC after our push so we can detect external changes
          {
            int64_t newCC = x11_clipboard_get_change_count();
            std::lock_guard<std::mutex> lk(sSelMtx);
            sSelPushedCC[selAtom] = newCC;
          }
        }

        // For CLIPBOARD only: take over ownership and send SelectionClear.
        // This forces Java to re-query via ConvertSelection on paste,
        // which lets our bridge serve newer macOS content.
        //
        // Do NOT do this for PRIMARY — PRIMARY represents the live text
        // selection.  Sending SelectionClear for PRIMARY tells Java it
        // lost the selection, causing the visual highlight to break and
        // Java to think nothing is selected.
        if (selAtom == atom::kCLIPBOARD) {
          uint32_t prevSelOwner = 0;
          {
            std::lock_guard<std::mutex> lk(sSelMtx);
            auto it = sSelOwner.find(selAtom);
            if (it != sSelOwner.end()) prevSelOwner = it->second;
            sSelOwner[selAtom] = 1; // root window
          }

          if (prevSelOwner > 1) {
            uint8_t clrEv[32] = {0};
            clrEv[0] = 29; // SelectionClear
            wire::wr16_le(clrEv + 2, ctx.transport().lastSeq());
            wire::wr32_le(clrEv + 4, x11_now_ms_monotonic()); // ICCCM: never 0
            wire::wr32_le(clrEv + 8,  prevSelOwner);  // window
            wire::wr32_le(clrEv + 12, selAtom);        // selection atom
            (void)ctx.transport().sendEvent32(prevSelOwner, clrEv);
          }
        }

#ifndef NDEBUG
        int64_t cc = x11_clipboard_get_change_count();
        TS_DBG("[CLIPBOARD] Proactive capture: %zu bytes, sel=%u, pushed=%s, cc=%lld, owner→root\n",
                p.data.size(), (unsigned)selAtom,
                shouldPush ? "yes" : "SKIPPED(macOS newer)",
                (long long)cc);
#endif
      }
    }
  }

#ifdef X11_TRACE_VERBOSE
  ctx.tracef("[SelectionOps] SendEvent dest=0x%08X resolved=0x%08X type=%u\n",
             (unsigned)destination, (unsigned)resolvedDest, (unsigned)(event[0] & 0x7Fu));
#endif
}

// ---------------------------------------------------------------------------
// INCR receive hooks (called from PropOps::handleChangeProperty)
// ---------------------------------------------------------------------------
bool SelectionOps::incrReceiveActive(uint32_t wid, uint32_t prop) {
  return sIncrRecv.find(incrKey(wid, prop)) != sIncrRecv.end();
}

void SelectionOps::incrOnChunk(XProtoContext& ctx, uint32_t wid, uint32_t prop,
                               uint32_t type, uint8_t format,
                               const uint8_t* data, uint64_t len) {
  auto it = sIncrRecv.find(incrKey(wid, prop));
  if (it == sIncrRecv.end()) return;

  // A fresh INCR announcement on this slot supersedes a stale transfer
  // (both PRIMARY and CLIPBOARD captures share the (root, UTF8_STRING)
  // slot).  Drop the stale state and leave the property stored so the
  // upcoming SelectionNotify starts a new receive — do NOT consume it as
  // a data chunk (v1.19.36.9 field failure: the CLIPBOARD announcement
  // was eaten as a 4-byte chunk of the abandoned PRIMARY transfer).
  if (type == atom::kINCR && format == 32) {
#ifndef NDEBUG
    TS_DBG("[CLIPBOARD] INCR receive SUPERSEDED: sel=%u dropped (%zu bytes "
            "accumulated) — new announcement on wid=0x%08X prop=%u\n",
            (unsigned)it->second.selection, it->second.data.size(),
            (unsigned)wid, (unsigned)prop);
#endif
    sIncrRecv.erase(it);
    return;
  }

  IncrRecvState& st = it->second;

  if (len > 0) {
    // Data chunk: accumulate (capped), consume the property, ack with
    // PropertyNotify(Deleted) so the owner sends the next chunk.
    if ((uint64_t)st.data.size() + len <= kIncrRecvMax) {
      st.data.insert(st.data.end(), data, data + len);
    }
#ifndef NDEBUG
    TS_DBG("[CLIPBOARD] INCR chunk: %llu bytes (total %zu / announced %llu)\n",
           (unsigned long long)len, st.data.size(),
           (unsigned long long)st.announced);
#endif
    PropertyTable::instance().erase(wid, prop);
    incrNotifyOwner(ctx, wid, prop, /*deleted*/true);
    return;
  }

  // Zero-length chunk: transfer complete.
  const uint32_t selAtom = st.selection;
  std::vector<uint8_t> text = std::move(st.data);
  sIncrRecv.erase(it);
  PropertyTable::instance().erase(wid, prop);
  incrNotifyOwner(ctx, wid, prop, /*deleted*/true);

  // Push to macOS unless the macOS clipboard changed externally since our
  // last push (same guard as the direct capture path).
  bool shouldPush = !text.empty();
  if (shouldPush) {
    std::lock_guard<std::mutex> lk(sSelMtx);
    auto ccIt = sSelPushedCC.find(selAtom);
    if (ccIt != sSelPushedCC.end()) {
      int64_t currentCC = x11_clipboard_get_change_count();
      if (currentCC > ccIt->second) shouldPush = false;
    }
  }
  if (shouldPush) {
    x11_clipboard_set_text(reinterpret_cast<const char*>(text.data()),
                           (uint32_t)text.size());
    int64_t newCC = x11_clipboard_get_change_count();
    std::lock_guard<std::mutex> lk(sSelMtx);
    sSelPushedCC[selAtom] = newCC;
  }

  // For CLIPBOARD only: take over ownership so the next paste re-queries
  // through the server (mirrors the direct capture path; see there for
  // why PRIMARY must keep its owner).
  if (selAtom == atom::kCLIPBOARD) {
    uint32_t prevSelOwner = 0;
    {
      std::lock_guard<std::mutex> lk(sSelMtx);
      auto oIt = sSelOwner.find(selAtom);
      if (oIt != sSelOwner.end()) prevSelOwner = oIt->second;
      sSelOwner[selAtom] = 1; // root proxy
    }
    if (prevSelOwner > 1) {
      uint8_t clrEv[32] = {0};
      clrEv[0] = 29; // SelectionClear
      wire::wr16_le(clrEv + 2, ctx.transport().lastSeq());
      wire::wr32_le(clrEv + 4, x11_now_ms_monotonic()); // ICCCM: never 0
      wire::wr32_le(clrEv + 8,  prevSelOwner);
      wire::wr32_le(clrEv + 12, selAtom);
      (void)ctx.transport().sendEvent32(prevSelOwner, clrEv);
    }
  }

#ifndef NDEBUG
  TS_DBG("[CLIPBOARD] INCR receive DONE: %zu bytes, sel=%u, pushed=%s, owner→root\n",
         text.size(), (unsigned)selAtom, shouldPush ? "yes" : "SKIPPED(macOS newer)");
#endif
}


} // namespace x11
