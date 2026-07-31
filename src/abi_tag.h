// SYNTHUX ACADEMY /////////////////////////////////////////
// SPOTYKACH ///////////////////////////////////////////////
#pragma once

// ABI tagging for build flags that change TYPE LAYOUT rather than behaviour.
//
// SPK_TERMINAL adds a member to CoreUI (_input_frozen) and three virtuals to IEngine; TERM_USBDIAG
// adds another CoreUI member. Objects compiled with and without them are therefore not link-compatible
// - every member after the added one sits at a different offset, and every vtable slot after the added
// virtuals shifts. Nothing in a normal C++ build detects that: the link succeeds and the firmware
// misbehaves in ways that look like a hardware fault. On 2026-07-31 exactly that cost a full hardware
// debugging session - a frozen, garbled panel with a working command channel, chased through the USB
// stack before it turned out to be a stale object file (see docs/dev/terminal-impl.md).
//
// Wrapping the affected types in an INLINE NAMESPACE whose name encodes the flags turns a mismatch
// into a LINK ERROR: a stale object defines spotykach::abi_t0d0::CoreUI::render_leds() while a fresh
// one calls spotykach::abi_t1d0::CoreUI::render_leds(), and the linker names the offending symbol.
//
// Inline namespaces are transparent - `spotykach::CoreUI` and `spotykach::IEngine` still name the
// types, template/overload resolution is unaffected - and they cost nothing at runtime: this is purely
// a change to mangled names.
//
// The Makefile's build-flag stamps already force a full rebuild when these flags change, which closes
// the build-system route to a mismatch. This closes the general one: a hand-compiled object, a stale
// prebuilt library, an out-of-tree build, or any future flag plumbing that forgets a stamp.
//
// To add a flag here: extend the tag, and make sure EVERY declaration of an affected type - including
// forward declarations elsewhere (see memory/storage.h) - is wrapped, or the two will not agree.

#if SPK_TERMINAL
#  if TERM_USBDIAG
#    define SPK_ABI_NS abi_t1d1
#  else
#    define SPK_ABI_NS abi_t1d0
#  endif
#else
#  define SPK_ABI_NS abi_t0d0
#endif

#define SPK_ABI_BEGIN inline namespace SPK_ABI_NS {
#define SPK_ABI_END   }   // SPK_ABI_NS
