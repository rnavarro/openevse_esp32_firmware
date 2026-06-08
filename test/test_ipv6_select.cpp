// Host unit test for the inbound-advertisement IPv6 selector (src/ipv6_select.h).
//
// Compile and run on the host (no device, no PlatformIO needed):
//   c++ -std=c++11 -I src -Wall -Wextra -o /tmp/test_ipv6_select test/test_ipv6_select.cpp && /tmp/test_ipv6_select
//
// Why this exists: the live network tests (RA injection, real-network switch)
// exercised scope classification, state classification, and the renumber path,
// but never observed the selector ranking TWO DISTINCT GUAs against each other
// (preferred-GUA vs deprecated-GUA) — that case was rig-blocked. This closes
// that gap deterministically. It calls the SAME ipv6SelectAdvertisedSlot() the
// firmware calls, so a future change to the production formula breaks this test
// rather than leaving a stale parallel copy passing.

#include "ipv6_select.h"

#include <cassert>
#include <cstdio>

static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1; \
    } \
  } while (0)

// Eligible-slot constructors (valid, not any, not link-local).
static Ipv6SlotInfo gua(bool preferred)  { return Ipv6SlotInfo{true, false, false, true,  preferred}; }
static Ipv6SlotInfo ula(bool preferred)  { return Ipv6SlotInfo{true, false, false, false, preferred}; }
static Ipv6SlotInfo linklocal()          { return Ipv6SlotInfo{true, false, true,  false, true};  }
static Ipv6SlotInfo invalid()            { return Ipv6SlotInfo{false, false, false, true,  true};  } // INVALID/TENTATIVE GUA
static Ipv6SlotInfo unspecified()        { return Ipv6SlotInfo{true,  true,  false, false, false}; } // ::

int main()
{
  // --- The actual gap: two distinct GUAs, preferred must beat deprecated,
  //     in BOTH array orders. This is what proves slot/acquisition order does
  //     NOT decide the winner (lwIP assigns slots by acquisition order, which
  //     we don't control). preferred-GUA(3) vs deprecated-GUA(2): not a tie.
  {
    // preferred GUA in the lower slot
    Ipv6SlotInfo a[] = { linklocal(), gua(true), gua(false) };
    CHECK(ipv6SelectAdvertisedSlot(a, 3) == 1);
  }
  {
    // preferred GUA in the HIGHER slot — the renumber shape: slot 0 holds the
    // dying (deprecated) old prefix, the new preferred prefix is above it.
    Ipv6SlotInfo a[] = { gua(false), gua(true) };
    CHECK(ipv6SelectAdvertisedSlot(a, 2) == 1);
  }
  {
    // Three slots, deprecated GUA below preferred GUA below link-local mix.
    Ipv6SlotInfo a[] = { linklocal(), gua(false), gua(true) };
    CHECK(ipv6SelectAdvertisedSlot(a, 3) == 2);
  }

  // --- Full 4-way total order:
  //     preferred-GUA(3) > deprecated-GUA(2) > preferred-ULA(1) > deprecated-ULA(0)
  CHECK(ipv6AdvertiseScore(true,  true)  == 3);
  CHECK(ipv6AdvertiseScore(true,  false) == 2);
  CHECK(ipv6AdvertiseScore(false, true)  == 1);
  CHECK(ipv6AdvertiseScore(false, false) == 0);
  {
    // All four present in ascending-score slot order — top score (slot 3) wins.
    Ipv6SlotInfo a[] = { ula(false), ula(true), gua(false), gua(true) };
    CHECK(ipv6SelectAdvertisedSlot(a, 4) == 3);
  }
  {
    // Same four in DESCENDING-score slot order — top score (slot 0) wins.
    Ipv6SlotInfo a[] = { gua(true), gua(false), ula(true), ula(false) };
    CHECK(ipv6SelectAdvertisedSlot(a, 4) == 0);
  }
  {
    // No GUA available — preferred ULA beats deprecated ULA.
    Ipv6SlotInfo a[] = { ula(false), ula(true) };
    CHECK(ipv6SelectAdvertisedSlot(a, 2) == 1);
  }

  // --- Tie: equal scores => incumbent (lowest index) wins (strict '>').
  {
    Ipv6SlotInfo a[] = { gua(true), gua(true) };
    CHECK(ipv6SelectAdvertisedSlot(a, 2) == 0);
  }
  {
    Ipv6SlotInfo a[] = { ula(false), ula(false), ula(false) };
    CHECK(ipv6SelectAdvertisedSlot(a, 3) == 0);
  }

  // --- Skip predicates: invalid/tentative, unspecified, link-local are ignored.
  {
    // A preferred GUA that is INVALID/TENTATIVE must be skipped in favour of a
    // valid deprecated GUA below it.
    Ipv6SlotInfo a[] = { invalid(), gua(false) };
    CHECK(ipv6SelectAdvertisedSlot(a, 2) == 1);
  }
  {
    // Link-local and unspecified are never advertised even when "preferred".
    Ipv6SlotInfo a[] = { linklocal(), unspecified(), ula(false) };
    CHECK(ipv6SelectAdvertisedSlot(a, 3) == 2);
  }

  // --- No eligible slot => -1.
  {
    Ipv6SlotInfo a[] = { linklocal(), invalid(), unspecified() };
    CHECK(ipv6SelectAdvertisedSlot(a, 3) == -1);
  }
  CHECK(ipv6SelectAdvertisedSlot(nullptr, 0) == -1);

  std::printf("ok - %d checks passed\n", g_checks);
  return 0;
}
