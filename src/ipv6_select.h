#ifndef _IPV6_SELECT_H
#define _IPV6_SELECT_H

/*
 * Pure, dependency-free ranking for INBOUND IPv6 address advertisement
 * (mDNS AAAA record + /status display). This is NOT outbound source address
 * selection — lwIP runs RFC 6724 per-socket for that and is untouched here.
 *
 * The question this answers is "which of our addresses do we tell the world to
 * reach us on". Scope dominates (RFC 6724 Rule 2) and preferred-vs-deprecated
 * is the intra-scope tiebreak (RFC 6724 Rule 3), giving the total order:
 *
 *   preferred-GUA(3) > deprecated-GUA(2) > preferred-ULA(1) > deprecated-ULA(0)
 *
 * The intra-GUA step (preferred 3 vs deprecated 2) is what makes renumbering
 * work: during a prefix change the old GUA goes deprecated-but-valid (grace
 * window) while the new one is preferred, so we follow the new prefix instead
 * of staying pinned to the dying address.
 *
 * Kept free of lwIP types on purpose so it host-compiles under a plain g++
 * unit test (test/test_ipv6_select.cpp). The firmware call sites translate live
 * lwIP per-slot state into these POD fields using the real lwIP macros, so the
 * test and the device exercise the same selection operator — one source of
 * truth, not a parallel reimplementation.
 */

struct Ipv6SlotInfo {
  bool valid;        // ip6_addr_isvalid(state): false => INVALID/TENTATIVE, skip
  bool isAny;        // ip6_addr_isany(addr): unspecified address, skip
  bool isLinkLocal;  // ip6_addr_islinklocal(addr): never advertised, skip
  bool isGlobal;     // ip6_addr_isglobal(addr): GUA (true) vs ULA (false)
  bool isPreferred;  // ip6_addr_ispreferred(state): preferred (true) vs deprecated
};

// Rank a single eligible slot. Higher wins. See the total order above.
static inline int ipv6AdvertiseScore(bool isGlobal, bool isPreferred) {
  return (isGlobal ? 2 : 0) + (isPreferred ? 1 : 0);
}

// Return the index of the best slot to advertise, or -1 if none is eligible.
// Strict '>' so the incumbent (lowest index) wins on equal scores, keeping the
// advertised address stable when nothing strictly better exists.
static inline int ipv6SelectAdvertisedSlot(const Ipv6SlotInfo *slots, int n) {
  int best = -1, best_score = -1;
  for (int i = 0; i < n; i++) {
    if (!slots[i].valid) continue;             // INVALID / TENTATIVE
    if (slots[i].isAny || slots[i].isLinkLocal) continue;
    int score = ipv6AdvertiseScore(slots[i].isGlobal, slots[i].isPreferred);
    if (score > best_score) {
      best = i;
      best_score = score;
    }
  }
  return best;
}

#endif // _IPV6_SELECT_H
