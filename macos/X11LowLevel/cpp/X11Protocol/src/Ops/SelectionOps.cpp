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
#include "Core/HostCommandQueue.hpp"
#include "Utils/ByteReader.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/WireLE.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" {
#include "SwiftX11Bridge.h"
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
void SelectionOps::handleSetSelectionOwner(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }

  const uint32_t owner     = br.readU32();
  const uint32_t selection = br.readU32();
  const uint32_t time      = br.readU32();

  br.skip(br.remaining());

  uint32_t prevOwner = 0;
  {
    std::lock_guard<std::mutex> lk(sSelMtx);
    auto it = sSelOwner.find(selection);
    if (it != sSelOwner.end()) {
      prevOwner = it->second;
      it->second = owner;
    } else {
      sSelOwner[selection] = owner;
    }
  }

  // If previous owner was different and non-zero, send SelectionClear (type 29)
  if (prevOwner != 0 && prevOwner != owner) {
    uint8_t ev[32] = {0};
    ev[0] = 29; // SelectionClear
    wire::wr16_le(ev + 2, ctx.transport().lastSeq());
    wire::wr32_le(ev + 4,  time);
    wire::wr32_le(ev + 8,  prevOwner);  // window
    wire::wr32_le(ev + 12, selection);  // atom
    (void)ctx.transport().sendEvent32(prevOwner, ev);
  }

  // Track macOS changeCount so ConvertSelection can detect external macOS changes
  {
    int64_t cc = x11_clipboard_get_change_count();
    std::lock_guard<std::mutex> lk(sSelMtx);
    sSelMacCC[selection] = cc;
  }

  // Queue a deferred clipboard capture via HostCommandQueue.
  // This will be processed on the NEXT poll iteration in drainHostCommands,
  // which activates the owning client in a separate cycle — avoiding the
  // fatal IO error that occurs when sending a SelectionRequest to a client
  // during its own request processing.
  if (owner != 0 && (selection == atom::kPRIMARY || selection == atom::kCLIPBOARD)) {
    auto* srv = x11_proto_bridge_get_server();
    if (srv) {
      x11::HostCmd hc{};
      hc.type = x11::HostCmdType::ClipboardCapture;
      hc.xid = owner;
      hc.keyCode = selection; // reuse keyCode field for selection atom
      srv->hostCmds().push(hc);
    }
  }

#ifndef NDEBUG
  fprintf(stderr, "[SelectionOps] SetSelectionOwner sel=%u owner=0x%08X prev=0x%08X\n",
          (unsigned)selection, (unsigned)owner, (unsigned)prevOwner);
#endif
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
  // If target == TARGETS, return list of supported target atoms
  if (target == atom::kTARGETS) {
    // Write TARGETS list as CARD32 array (format=32)
    const uint32_t targets[] = {
      atom::kTARGETS,
      atom::kUTF8_STRING,
      atom::kSTRING,
      atom::kTEXT,
      atom::kTIMESTAMP,
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
    fprintf(stderr, "[CLIPBOARD] Served TARGETS (%u atoms) to 0x%08X\n",
            nTargets, (unsigned)requestor);
#endif
    return true;
  }

  // If target == TIMESTAMP, return CurrentTime (0)
  if (target == atom::kTIMESTAMP) {
    uint8_t ts[4] = {0};
    wire::wr32_le(ts, 0); // CurrentTime
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
    // Read macOS clipboard
    char buf[65536];
    uint32_t len = x11_clipboard_get_text(buf, sizeof(buf));

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

    // Write text to requestor's property
    PropertyTable::instance().setReplace(
      requestor, property,
      target, // type = same as requested target
      8,      // format = 8 (byte string)
      reinterpret_cast<const uint8_t*>(buf), len
    );

    // Send SelectionNotify with property set
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
    fprintf(stderr, "[CLIPBOARD] Served %u bytes from macOS pasteboard to 0x%08X\n",
            len, (unsigned)requestor);
#endif
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

  // Per ICCCM: if property is None, use target as property
  if (property == 0) property = target;

  uint32_t owner = 0;
  {
    std::lock_guard<std::mutex> lk(sSelMtx);
    auto it = sSelOwner.find(selection);
    if (it != sSelOwner.end()) owner = it->second;
  }

#ifndef NDEBUG
  fprintf(stderr, "[SelectionOps] ConvertSelection sel=%u target=%u req=0x%08X owner=0x%08X\n",
          (unsigned)selection, (unsigned)target, (unsigned)requestor, (unsigned)owner);
#endif

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
        fprintf(stderr, "[CLIPBOARD] macOS clipboard newer (cc %lld > %lld) — serving from macOS\n",
                (long long)currentCC, (long long)storedCC);
#endif
        serveMacOSClipboard(ctx, requestor, selection, target, property, time);
        return;
      }
    }

    // X11 client owns this selection and has newer content — forward SelectionRequest
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
    if ((selAtom == atom::kCLIPBOARD || selAtom == atom::kPRIMARY) &&
        propAtom != 0 && resolvedDest != 0)
    {
      // The selection owner just set a property on the requestor.
      // Read it and push to macOS clipboard.
      PropertyTable::Prop p{};
      if (PropertyTable::instance().get(resolvedDest, propAtom, p) &&
          p.format == 8 && !p.data.empty())
      {
        x11_clipboard_set_text(reinterpret_cast<const char*>(p.data.data()),
                               (uint32_t)p.data.size());

        // Update stored changeCount so ConvertSelection knows this push was ours
        // (prevents interpreting our own push as an external macOS change).
        int64_t cc = x11_clipboard_get_change_count();
        {
          std::lock_guard<std::mutex> lk(sSelMtx);
          sSelMacCC[selAtom] = cc;
        }

#ifndef NDEBUG
        fprintf(stderr, "[CLIPBOARD] Captured %zu bytes from X11 (sel=%u) -> macOS (cc=%lld)\n",
                p.data.size(), (unsigned)selAtom, (long long)cc);
#endif
      }
    }
  }

#ifdef X11_TRACE_VERBOSE
  ctx.tracef("[SelectionOps] SendEvent dest=0x%08X resolved=0x%08X type=%u\n",
             (unsigned)destination, (unsigned)resolvedDest, (unsigned)(event[0] & 0x7Fu));
#endif
}


} // namespace x11
