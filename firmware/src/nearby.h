// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 B0z0dcl0wn and the SignalSweep contributors

// The Nearby LED mode's pure pieces: the blink schedule each category pixel
// runs (and the per-cycle latch that keeps a run of blinks whole), which
// tracked devices count and under what label, and the app's same-box grouping
// that decides how many rows -- so how many blinks -- each category holds.
// No ESP headers, so firmware/test/run.sh compiles and tests them on the host
// with plain g++.
#ifndef NEARBY_H
#define NEARBY_H

#include <stddef.h>
#include <stdint.h>
#include <vector>

// The bar under Nearby, in strip pixels. Pixel 0 carries the heartbeat, the
// same pixel as the idle heartbeat in every other mode; pixels 0 ..
// NEARBY_FLASH_PIXELS-1 flash on a new alert; the categories fill the other
// end, AlertCategory 0 on the last pixel (nearbyCategoryPixel).
// hardware_manager.cpp static_asserts these against AlertCategory and
// NEOPIXEL_COUNT where the render code uses them.
#define NEARBY_CATEGORIES   5
#define NEARBY_PIXELS       8
#define NEARBY_STATUS_PIXEL 0
#define NEARBY_FLASH_PIXELS (NEARBY_PIXELS - NEARBY_CATEGORIES)
static_assert(NEARBY_FLASH_PIXELS > 0, "Nearby needs a pixel beside the categories");

// Strip pixel for an AlertCategory.
static inline int nearbyCategoryPixel(int category) {
    return NEARBY_PIXELS - 1 - category;
}

// One blink slot is 250 ms lit + 250 ms dark. A pixel showing `count` devices
// runs a cycle of (count + 1) slots: `count` blinks, then one dark slot as the
// gap that separates one count from the next.
#define NEARBY_SLOT_MS 500
#define NEARBY_ON_MS   250

// Is a pixel showing `count` devices lit at time `t` (ms) into its cycle?
// Zero is always dark. On the free-running clock the phase shifts whenever
// `count` changes, which is why the render code goes through NearbyLatch.
static inline bool nearbyBlinkOn(uint32_t count, uint32_t t) {
    if (count == 0) return false;
    uint64_t slot = (t / NEARBY_SLOT_MS) % ((uint64_t)count + 1);
    if (slot == count) return false;   // the gap slot
    return (t % NEARBY_SLOT_MS) < NEARBY_ON_MS;
}

// One pixel's cycle state. The count is latched when a cycle starts and held
// until its gap slot ends, so a count that changes mid-cycle never truncates
// or merges a run of blinks: the new value shows from the next cycle. A pixel
// at rest (latched 0) starts on the first slot boundary of the free-running
// clock after its count rises, not mid-slot.
struct NearbyLatch {
    uint32_t count;       // latched for the running cycle; 0 = at rest
    uint32_t cycleStart;  // ms; while at rest, the boundary it may start on
    bool waiting;         // at rest with a nonzero live count, cycleStart set
};

// Advance `L` to time `now` against the live count, and say whether the pixel
// is lit. Signed compares, so millis() rollover does not stall a pixel.
static inline bool nearbyLatchOn(NearbyLatch& L, uint32_t live, uint32_t now) {
    if (L.count == 0) {
        if (live == 0) { L.waiting = false; return false; }
        if (!L.waiting) {
            uint32_t into = now % NEARBY_SLOT_MS;
            L.cycleStart = into == 0 ? now : now + (NEARBY_SLOT_MS - into);
            L.waiting = true;
        }
        if ((int32_t)(now - L.cycleStart) < 0) return false;
        L.count = live;
        L.waiting = false;
    }
    uint32_t len = (L.count + 1) * (uint32_t)NEARBY_SLOT_MS;
    uint32_t elapsed = now - L.cycleStart;
    if (elapsed >= len) {
        // The cycle (and its gap) ended: latch the live count for the next
        // one. Whole cycles, so a frame skipped by the hunt meter or a flash
        // lands on a boundary rather than mid-blink.
        L.cycleStart += len * (elapsed / len);
        elapsed = now - L.cycleStart;
        L.count = live;
        if (live == 0) return false;
        len = (live + 1) * (uint32_t)NEARBY_SLOT_MS;
        if (elapsed >= len) {   // only after skipped frames: realign the same way
            L.cycleStart += len * (elapsed / len);
            elapsed = now - L.cycleStart;
        }
    }
    return nearbyBlinkOn(L.count, elapsed);
}

// Which label a tracked device is counted under, or NULL when the app would
// not count it: not listed (the push's own predicate, scanAll or confidence at
// least listMin), or listed but with no category -- no type and no matched
// rule, the app's "no match" row. The label is the type, else the matched
// rule, as the app's categoryOf(m.type || m.rule). There is deliberately no
// ignore or mute check: the app still lists ignored and muted devices.
static inline const char* nearbyLabel(bool scanAll, int confidence, int listMin,
                                      const char* type, const char* rule) {
    if (!(scanAll || confidence >= listMin)) return NULL;
    if (type && type[0]) return type;
    if (rule && rule[0]) return rule;
    return NULL;
}

// The app's unitLead() rank: a real vendor category (a type) beats a weak hint
// (a matched rule only) beats no match.
static inline uint8_t nearbyRank(const char* type, const char* rule) {
    if (type && type[0]) return 2;
    if (rule && rule[0]) return 1;
    return 0;
}

// Does a tracked device go into the grouping at all? It must be listed (the
// push's predicate, scanAll or confidence at least listMin). With the filter
// on, the app also drops "no match" rows (no type, no rule) before grouping
// (liveRows), so they must not bridge two boxes here; with the filter off the
// app keeps them and they do.
static inline bool nearbyInclude(bool scanAll, int confidence, int listMin,
                                 const char* type, const char* rule) {
    if (!(scanAll || confidence >= listMin)) return false;
    if (!scanAll && nearbyRank(type, rule) == 0) return false;
    return true;
}

// One listed device. `mac` is "AA:BB:CC:DD:EE:FF" (':' or '-' separators,
// either case); anything else -- a sentinel row such as "Tag flood: Tile" --
// is its own unit. `ch` is the last Wi-Fi channel heard, 0 = unknown.
// `label` is nearbyLabel()'s answer (NULL = uncategorized) and `rank` is
// nearbyRank()'s; an uncategorized device still joins groups, as in the app.
struct NearbyRadio {
    const char* mac;
    bool wifi;      // protocol == "WiFi" exactly, as the app's groupSiblings()
    uint8_t ch;
    const char* label;
    uint8_t rank;
};

// Mirrors SIBLING_SPAN in app/public/app.js (selftest pins the two equal).
#define NEARBY_SIBLING_SPAN 4

static inline int nearbyHexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Exactly six two-digit hex octets separated by ':' or '-'. The app's split
// accepts mixed separators, so this does too.
static inline bool nearbyParseMac(const char* s, uint8_t out[6]) {
    if (!s) return false;
    for (int i = 0; i < 6; i++) {
        int hi = nearbyHexNibble(s[0]);
        int lo = hi < 0 ? -1 : nearbyHexNibble(s[1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi * 16 + lo);
        s += 2;
        if (i < 5) {
            if (*s != ':' && *s != '-') return false;
            s++;
        }
    }
    return *s == '\0';
}

// The app's groupSiblings(), as a union-find root per device. Two Wi-Fi
// radios are one box, chained through a group, when either:
//  * first 5 octets are equal and the last octets are within
//    NEARBY_SIBLING_SPAN (a dual-band box's 2.4 and 5 GHz radios), or
//  * last 5 octets are equal, the first octets differ, at least one has the
//    locally-administered bit (0x02), and the channels match when both are
//    known (virtual networks on one radio).
// The app chains the first rule through adjacent pairs after sorting by the
// last octet; unioning every pair within the span gives the same groups.
// Pairwise is O(n^2): n is the listed devices (at most WATCHERS_MAX_TARGETS,
// 400). Indices are 16-bit to keep the 1 Hz heap cost small on the C5, whose
// internal RAM is the tight budget (CHANGELOG 0.10.1); callers pass at most
// NEARBY_MAX_RADIOS.
#define NEARBY_MAX_RADIOS 0xFFFF
typedef uint16_t NearbyIdx;
static inline std::vector<NearbyIdx> nearbyGroupRoots(const NearbyRadio* radios, size_t n) {
    if (n > NEARBY_MAX_RADIOS) n = NEARBY_MAX_RADIOS;
    std::vector<NearbyIdx> parent(n);
    std::vector<uint8_t> octets(n * 6);
    std::vector<bool> grouped(n, false);
    for (size_t i = 0; i < n; i++) {
        parent[i] = (NearbyIdx)i;
        grouped[i] = radios[i].wifi && nearbyParseMac(radios[i].mac, &octets[i * 6]);
    }
    auto find = [&parent](NearbyIdx x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    for (size_t i = 0; i < n; i++) {
        if (!grouped[i]) continue;
        const uint8_t* a = &octets[i * 6];
        for (size_t j = i + 1; j < n; j++) {
            if (!grouped[j]) continue;
            const uint8_t* b = &octets[j * 6];
            bool sameHead = a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == b[4];
            bool sameTail = a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == b[4] && a[5] == b[5];
            bool join = false;
            if (sameHead) {
                int d = (int)a[5] - (int)b[5];
                if (d < 0) d = -d;
                join = d <= NEARBY_SIBLING_SPAN;
            }
            if (!join && sameTail && a[0] != b[0] && ((a[0] | b[0]) & 0x02)) {
                uint8_t ca = radios[i].ch, cb = radios[j].ch;
                join = !(ca > 0 && cb > 0 && ca != cb);
            }
            if (join) parent[find((NearbyIdx)i)] = find((NearbyIdx)j);
        }
    }
    for (size_t i = 0; i < n; i++) parent[i] = find((NearbyIdx)i);
    return parent;
}

// How many rows the app makes of these devices, whatever their category.
static inline size_t nearbyGroupCount(const NearbyRadio* radios, size_t n) {
    std::vector<NearbyIdx> root = nearbyGroupRoots(radios, n);
    size_t units = 0;
    for (size_t i = 0; i < root.size(); i++)
        if (root[i] == i) units++;
    return units;
}

// Per-category row counts, as the app's Everything list colors its rows.
// `radios` is every listed device, categorized or not, in one call: the app
// groups the whole list, across categories and through uncategorized radios.
// Each unit counts once, under its lead's category: the highest-rank member,
// ties to the first in input order (the app's unitLead()). A rank-0 lead is a
// "no match" row and is not counted. `categoryOf` maps a label to its
// AlertCategory index (0 .. NEARBY_CATEGORIES-1). Saturates at 65535.
static inline void nearbyCount(const NearbyRadio* radios, size_t n,
                               int (*categoryOf)(const char* label),
                               uint16_t counts[NEARBY_CATEGORIES]) {
    for (int c = 0; c < NEARBY_CATEGORIES; c++) counts[c] = 0;
    std::vector<NearbyIdx> root = nearbyGroupRoots(radios, n);
    n = root.size();
    // lead[r] is the leading member of the unit rooted at r, or NONE. NONE never collides with a real index, since
    // n <= NEARBY_MAX_RADIOS keeps every index below 0xFFFF.
    const NearbyIdx NONE = (NearbyIdx)0xFFFF;
    std::vector<NearbyIdx> lead(n, NONE);
    for (size_t i = 0; i < n; i++) {
        NearbyIdx r = root[i];
        if (lead[r] == NONE || radios[i].rank > radios[lead[r]].rank) lead[r] = (NearbyIdx)i;
    }
    for (size_t r = 0; r < n; r++) {
        if (lead[r] == NONE) continue;
        const NearbyRadio& m = radios[lead[r]];
        if (m.rank == 0 || !m.label) continue;
        int c = categoryOf(m.label);
        if (c < 0 || c >= NEARBY_CATEGORIES) continue;
        if (counts[c] < 0xFFFF) counts[c]++;
    }
}

#endif // NEARBY_H
