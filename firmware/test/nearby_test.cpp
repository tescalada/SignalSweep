// Host-side tests for the Nearby LED mode (src/nearby.h): the per-pixel blink
// schedule and its per-cycle latch, which devices count under which label, and
// the whole-list grouping that must count what the app's Everything list shows.
#include "../src/nearby.h"
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>

// The firmware's alertCategoryFromName() keyword order (hardware_manager.cpp),
// case-insensitive like the original. Indices are AlertCategory: alpr 0,
// bodycam 1, drone 2, tracker 3, other 4.
static int testCategoryOf(const char* label) {
    char s[64];
    size_t i = 0;
    for (; label[i] && i < sizeof(s) - 1; i++) s[i] = (char)std::tolower((unsigned char)label[i]);
    s[i] = '\0';
    auto has = [&s](const char* k) { return std::strstr(s, k) != nullptr; };
    if (has("drone") || has("remote id") || has("uas")) return 2;
    if (has("track") || has("airtag") || has("tile") || has("tag") || has("beacon")) return 3;
    if (has("flock") || has("alpr") || has("plate") || has("surveil") ||
        has("soundthinking") || has("shotspotter")) return 0;
    if (has("body") || has("axon") || has("cam") || has("glasses")) return 1;
    return 4;
}

// A listed device for nearbyCount(), its label and rank from the same helpers
// the firmware uses. Listed with the filter off, so only type/rule decide.
static NearbyRadio radio(const char* mac, bool wifi, uint8_t ch, const char* type, const char* rule) {
    return NearbyRadio{mac, wifi, ch, nearbyLabel(true, 0, 60, type, rule), nearbyRank(type, rule)};
}

static void count(const NearbyRadio* r, size_t n, uint16_t out[NEARBY_CATEGORIES]) {
    nearbyCount(r, n, testCategoryOf, out);
}

static bool countsAre(const uint16_t got[NEARBY_CATEGORIES], uint16_t a, uint16_t b, uint16_t c,
                      uint16_t d, uint16_t e) {
    return got[0] == a && got[1] == b && got[2] == c && got[3] == d && got[4] == e;
}

// Blinks seen over one full cycle: count rising edges.
static int blinksInCycle(uint32_t count, uint32_t start) {
    int edges = 0;
    bool prev = false;
    for (uint32_t t = start; t < start + (count + 1) * NEARBY_SLOT_MS; t += 10) {
        bool on = nearbyBlinkOn(count, t);
        if (on && !prev) edges++;
        prev = on;
    }
    return edges;
}

// The heartbeat keeps pixel 0, the flash takes the pixels beside it, and the
// categories fill the other end: ALPR (AlertCategory 0) on pixel 7 down to
// other (4) on pixel 3, so no category shares a pixel with the flash.
static void test_layout() {
    assert(NEARBY_STATUS_PIXEL == 0);
    assert(NEARBY_FLASH_PIXELS == 3);
    assert(nearbyCategoryPixel(0) == 7);
    assert(nearbyCategoryPixel(NEARBY_CATEGORIES - 1) == 3);
    assert(nearbyCategoryPixel(NEARBY_CATEGORIES - 1) >= NEARBY_FLASH_PIXELS);
}

// One ALPR and two trackers, counts {1,0,0,2,0} -> px0 one blink + a gap slot, px3 two
// blinks + a gap slot, the rest dark.
static void test_blink_schedule() {
    const uint32_t counts[5] = {1, 0, 0, 2, 0};
    // px0: on 0-249, off 250-499, gap 500-999, repeat at 1000.
    assert(nearbyBlinkOn(counts[0], 0));
    assert(nearbyBlinkOn(counts[0], 249));
    assert(!nearbyBlinkOn(counts[0], 250));
    assert(!nearbyBlinkOn(counts[0], 500));
    assert(!nearbyBlinkOn(counts[0], 749));
    assert(!nearbyBlinkOn(counts[0], 999));
    assert(nearbyBlinkOn(counts[0], 1000));
    assert(blinksInCycle(counts[0], 0) == 1);
    // px3: on 0-249, on 500-749, gap 1000-1499, repeat at 1500.
    assert(nearbyBlinkOn(counts[3], 0));
    assert(!nearbyBlinkOn(counts[3], 300));
    assert(nearbyBlinkOn(counts[3], 500));
    assert(!nearbyBlinkOn(counts[3], 1000));
    assert(!nearbyBlinkOn(counts[3], 1200));
    assert(nearbyBlinkOn(counts[3], 1500));
    assert(blinksInCycle(counts[3], 0) == 2);
    assert(blinksInCycle(counts[3], 1500 * 7) == 2);
    // Empty categories never light.
    for (int px : {1, 2, 4})
        for (uint32_t t = 0; t < 5000; t += 10) assert(!nearbyBlinkOn(counts[px], t));
    // No cap: a large count still blinks that many times.
    assert(blinksInCycle(37, 0) == 37);
    assert(blinksInCycle(300, 0) == 300);
}

// Drive a latch every 10 ms from `from` to `to` with a live count that may
// change at `changeAt`; return the lengths of the blink runs seen, each run
// ended by a dark stretch of at least one whole slot (the gap).
static int runLatch(NearbyLatch& L, uint32_t from, uint32_t to, uint32_t before,
                    uint32_t after, uint32_t changeAt, int runs[], int maxRuns,
                    uint32_t* firstOn = nullptr) {
    int n = 0, cur = 0;
    bool prev = false;
    uint32_t darkSince = from;
    for (uint32_t t = from; t < to; t += 10) {
        bool on = nearbyLatchOn(L, t < changeAt ? before : after, t);
        if (on && firstOn && *firstOn == UINT32_MAX) *firstOn = t;
        if (on && !prev) {
            if (cur > 0 && t - darkSince >= NEARBY_SLOT_MS && n < maxRuns) { runs[n++] = cur; cur = 0; }
            cur++;
        }
        if (!on && prev) darkSince = t;
        prev = on;
    }
    if (cur > 0 && n < maxRuns) runs[n++] = cur;
    return n;
}

static void test_latch() {
    // A steady count runs the same schedule as nearbyBlinkOn from its start.
    {
        NearbyLatch L = {0, 0, false};
        for (uint32_t t = 0; t < 6000; t += 10)
            assert(nearbyLatchOn(L, 2, t) == nearbyBlinkOn(2, t));
    }
    // 3 -> 1 during the second blink: that cycle still shows 3, the next 1.
    {
        NearbyLatch L = {0, 0, false};
        int runs[8];
        int n = runLatch(L, 0, 2000 + 1000 * 3, 3, 1, 600, runs, 8);
        assert(n >= 3);
        assert(runs[0] == 3);
        assert(runs[1] == 1 && runs[2] == 1);
    }
    // 1 -> 3 during the first cycle's gap: no merged run of 4, no run of 2.
    {
        NearbyLatch L = {0, 0, false};
        int runs[8];
        int n = runLatch(L, 0, 1000 + 2000 * 2, 1, 3, 700, runs, 8);
        assert(n >= 3);
        assert(runs[0] == 1 && runs[1] == 3 && runs[2] == 3);
    }
    // 2 -> 0 mid-cycle: the cycle finishes its two blinks, then stays dark.
    {
        NearbyLatch L = {0, 0, false};
        int runs[8];
        int n = runLatch(L, 0, 6000, 2, 0, 300, runs, 8);
        assert(n == 1 && runs[0] == 2);
        assert(L.count == 0);
    }
    // Rising from 0 at 730 ms starts on the next slot boundary, 1000 ms.
    {
        NearbyLatch L = {0, 0, false};
        uint32_t firstOn = UINT32_MAX;
        int runs[8];
        runLatch(L, 0, 3000, 0, 1, 730, runs, 8, &firstOn);
        assert(firstOn == 1000);
    }
    // A skipped stretch (hunt meter, flash) lands back on a cycle boundary.
    {
        NearbyLatch L = {0, 0, false};
        assert(nearbyLatchOn(L, 1, 0));
        // 1 blink + gap = 1000 ms cycles; 10 230 ms is 230 ms into a cycle.
        assert(nearbyLatchOn(L, 1, 10230));
        assert(!nearbyLatchOn(L, 1, 10260));
        assert(!nearbyLatchOn(L, 1, 10800));
        assert(nearbyLatchOn(L, 1, 11000));
    }
    // millis() rollover: a pixel waiting on a boundary just past the wrap.
    {
        NearbyLatch L = {0, 0, false};
        uint32_t t0 = UINT32_MAX - 99;   // 4294967196 ms, 296 ms into a slot
        assert(!nearbyLatchOn(L, 1, t0));
        assert(L.waiting);
        bool lit = false;
        for (uint32_t k = 0; k < 2000; k += 10) lit = lit || nearbyLatchOn(L, 1, t0 + k);
        assert(lit);
    }
}

// A tracked device as the 1 Hz loop sees it, and that loop's include step
// (nearbyInclude) feeding nearbyCount, so the filter-on rules are covered.
struct Tracked { const char* mac; bool wifi; uint8_t ch; int confidence; const char* type; const char* rule; };
static void countTracked(const Tracked* t, size_t n, bool scanAll, int listMin,
                         uint16_t out[NEARBY_CATEGORIES]) {
    std::vector<NearbyRadio> listed;
    for (size_t i = 0; i < n; i++) {
        if (!nearbyInclude(scanAll, t[i].confidence, listMin, t[i].type, t[i].rule)) continue;
        listed.push_back({t[i].mac, t[i].wifi, t[i].ch,
                          nearbyLabel(scanAll, t[i].confidence, listMin, t[i].type, t[i].rule),
                          nearbyRank(t[i].type, t[i].rule)});
    }
    count(listed.data(), listed.size(), out);
}

// With the filter on, a radio between two listed Flock radios at :10 and :18
// bridges nothing when it is unlisted (confidence below listMin) or a "no
// match" row, since the app drops both before grouping: two rows, two blinks.
// With the filter off the same "no match" radio is listed and joins them.
static void test_include_filter_on() {
    const int listMin = 60;
    uint16_t c[NEARBY_CATEGORIES];
    Tracked unlisted[] = {
        {"AA:BB:CC:DD:EE:10", true, 6, 90, "Flock Safety", ""},
        {"AA:BB:CC:DD:EE:14", true, 0, listMin - 1, "Flock Safety", ""},
        {"AA:BB:CC:DD:EE:18", true, 36, 90, "Flock Safety", ""},
    };
    countTracked(unlisted, 3, false, listMin, c);
    assert(countsAre(c, 2, 0, 0, 0, 0));
    Tracked noMatch[] = {
        {"AA:BB:CC:DD:EE:10", true, 6, 90, "Flock Safety", ""},
        {"AA:BB:CC:DD:EE:14", true, 0, 90, "", ""},
        {"AA:BB:CC:DD:EE:18", true, 36, 90, "Flock Safety", ""},
    };
    countTracked(noMatch, 3, false, listMin, c);
    assert(countsAre(c, 2, 0, 0, 0, 0));
    countTracked(noMatch, 3, true, listMin, c);
    assert(countsAre(c, 1, 0, 0, 0, 0));
    // The predicate itself.
    assert(!nearbyInclude(false, listMin - 1, listMin, "Tracker", ""));
    assert(!nearbyInclude(false, listMin, listMin, "", ""));
    assert(nearbyInclude(false, listMin, listMin, "", "Lite-On IE"));
    assert(nearbyInclude(true, 0, listMin, "", ""));
}

// The label rules: which tracked devices are counted, and
// under what label.
static void test_counted_label() {
    for (int listMin : {1, 60, 85}) {
        // Unmatched device with the filter off: listed, but no type and no rule.
        assert(nearbyLabel(true, 0, listMin, "", "") == NULL);
        assert(nearbyLabel(true, 0, listMin, NULL, NULL) == NULL);
        // Below the listing threshold with the filter on: not listed at all.
        assert(nearbyLabel(false, listMin - 1, listMin, "Tracker", "AirTag") == NULL);
        // Listed: the type wins, else the matched rule (a weak hint).
        assert(std::strcmp(nearbyLabel(false, listMin, listMin, "Tracker", "AirTag"), "Tracker") == 0);
        assert(std::strcmp(nearbyLabel(false, listMin + 5, listMin, "", "Lite-On IE"), "Lite-On IE") == 0);
        // The filter off lists low-confidence matched devices too.
        assert(std::strcmp(nearbyLabel(true, 0, listMin, "Flock Safety", ""), "Flock Safety") == 0);
    }
    assert(nearbyRank("Tracker", "AirTag") == 2);
    assert(nearbyRank("", "Lite-On IE") == 1);
    assert(nearbyRank("", "") == 0);
    assert(nearbyRank(NULL, NULL) == 0);
}

static void test_parse_mac() {
    uint8_t o[6];
    assert(nearbyParseMac("AA:BB:CC:DD:EE:FF", o) && o[0] == 0xAA && o[5] == 0xFF);
    assert(nearbyParseMac("aa-bb-cc-dd-ee-0f", o) && o[0] == 0xAA && o[5] == 0x0F);
    // Mixed separators, as the app's split(/[:-]/) accepts.
    assert(nearbyParseMac("aa:BB-cc:DD-ee:01", o) && o[1] == 0xBB && o[5] == 0x01);
    // Malformed: short, long, one-digit octet, bad hex, bad separator, empty.
    assert(!nearbyParseMac("AA:BB:CC:DD:EE", o));
    assert(!nearbyParseMac("AA:BB:CC:DD:EE:FF:00", o));
    assert(!nearbyParseMac("AA:BB:CC:DD:EE:F", o));
    assert(!nearbyParseMac("AA:BB:CC:DD:EE:FG", o));
    assert(!nearbyParseMac("AA.BB.CC.DD.EE.FF", o));
    assert(!nearbyParseMac("AABBCCDDEEFF", o));
    assert(!nearbyParseMac("", o));
    assert(!nearbyParseMac(NULL, o));
    assert(!nearbyParseMac("Tag flood: Tile", o));
}

// Two trackers and one ALPR, end to end: counts {1,0,0,2,0}.
static void test_two_trackers_one_alpr() {
    NearbyRadio r[] = {
        radio("11:22:33:44:55:66", false, 0, "Tracker", "AirTag"),
        radio("21:22:33:44:55:66", false, 0, "Tracker", "Tile"),
        radio("02:AA:BB:CC:DD:01", true, 6, "Flock Safety", ""),
    };
    uint16_t c[NEARBY_CATEGORIES];
    count(r, 3, c);
    assert(countsAre(c, 1, 0, 0, 2, 0));
}

// A dual-band camera -- two Wi-Fi MACs, first 5 octets equal,
// last within 4 -- is one device.
static void test_dual_band_counts_once() {
    NearbyRadio r[] = {
        radio("AA:BB:CC:DD:EE:10", true, 6, "Flock Safety", ""),
        radio("aa-bb-cc-dd-ee-14", true, 36, "Flock Safety", ""),
    };
    assert(nearbyGroupCount(r, 2) == 1);
    uint16_t c[NEARBY_CATEGORIES];
    count(r, 2, c);
    assert(countsAre(c, 1, 0, 0, 0, 0));
    // Five apart is two boxes.
    NearbyRadio far[] = {
        radio("AA:BB:CC:DD:EE:10", true, 6, "Flock Safety", ""),
        radio("AA:BB:CC:DD:EE:15", true, 36, "Flock Safety", ""),
    };
    assert(nearbyGroupCount(far, 2) == 2);
    // Chained: 10, 14, 18 are one group although 10 and 18 are 8 apart.
    NearbyRadio chain[] = {
        radio("AA:BB:CC:DD:EE:10", true, 0, "Flock Safety", ""),
        radio("AA:BB:CC:DD:EE:18", true, 0, "Flock Safety", ""),
        radio("AA:BB:CC:DD:EE:14", true, 0, "Flock Safety", ""),
    };
    assert(nearbyGroupCount(chain, 3) == 1);
}

// An unmatched device with the filter off is listed but has no
// category, so it is not counted.
static void test_unmatched_not_counted() {
    NearbyRadio r[] = {
        radio("11:22:33:44:55:66", false, 0, "", ""),
        radio("AA:BB:CC:DD:EE:10", true, 1, "", ""),
    };
    uint16_t c[NEARBY_CATEGORIES];
    count(r, 2, c);
    assert(countsAre(c, 0, 0, 0, 0, 0));
}

// An ignored tracker. Ignoring changes alerting only; the board
// still lists it, so it reaches nearbyCount like any other and is counted.
static void test_ignored_tracker_counted() {
    NearbyRadio r[] = { radio("C0:FF:EE:00:00:01", false, 0, "Tracker", "AirTag") };
    uint16_t c[NEARBY_CATEGORIES];
    count(r, 1, c);
    assert(countsAre(c, 0, 0, 0, 1, 0));
}

static void test_only_wifi_groups() {
    // The same near-MACs over BLE are separate devices, as in the app.
    NearbyRadio ble[] = {
        radio("AA:BB:CC:DD:EE:10", false, 0, "Tracker", ""),
        radio("AA:BB:CC:DD:EE:11", false, 0, "Tracker", ""),
    };
    assert(nearbyGroupCount(ble, 2) == 2);
    // A sentinel row has no address and is its own unit.
    NearbyRadio sentinel[] = {
        radio("Tag flood: Tile", true, 0, "Tracker", ""),
        radio("AA:BB:CC:DD:EE:10", true, 0, "Tracker", ""),
    };
    assert(nearbyGroupCount(sentinel, 2) == 2);
    assert(nearbyGroupCount(sentinel, 0) == 0);
    uint16_t c[NEARBY_CATEGORIES];
    count(sentinel, 0, c);
    assert(countsAre(c, 0, 0, 0, 0, 0));
}

static void test_virtual_networks() {
    // Last 5 octets equal, first differs, one locally administered: one box.
    NearbyRadio v[] = {
        radio("84:BB:69:CC:78:41", true, 11, "Flock Safety", ""),
        radio("86:BB:69:CC:78:41", true, 11, "Flock Safety", ""),
    };
    assert(nearbyGroupCount(v, 2) == 1);
    // Unknown channel on one side still joins.
    NearbyRadio vu[] = {
        radio("84:BB:69:CC:78:41", true, 0, "Flock Safety", ""),
        radio("86:BB:69:CC:78:41", true, 11, "Flock Safety", ""),
    };
    assert(nearbyGroupCount(vu, 2) == 1);
    // Different known channels: two boxes.
    NearbyRadio vc[] = {
        radio("84:BB:69:CC:78:41", true, 1, "Flock Safety", ""),
        radio("86:BB:69:CC:78:41", true, 11, "Flock Safety", ""),
    };
    assert(nearbyGroupCount(vc, 2) == 2);
    // Neither locally administered: two boxes.
    NearbyRadio vg[] = {
        radio("84:BB:69:CC:78:41", true, 11, "Flock Safety", ""),
        radio("88:BB:69:CC:78:41", true, 11, "Flock Safety", ""),
    };
    assert(nearbyGroupCount(vg, 2) == 2);
    // Both rules chain through one group (the app's siblingRulesChain case).
    NearbyRadio both[] = {
        radio("84:BB:69:CC:78:41", true, 11, "Flock Safety", ""),
        radio("86:BB:69:CC:78:41", true, 11, "Flock Safety", ""),
        radio("84:BB:69:CC:78:44", true, 36, "Flock Safety", ""),
    };
    assert(nearbyGroupCount(both, 3) == 1);
}

// One box whose radios fall in different categories is one row in the app,
// colored by its lead, so it blinks once, under the lead's category.
static void test_cross_category_box() {
    // A typed ALPR radio and a rule-only sibling whose rule reads as a tracker:
    // the type outranks the weak hint, whatever the order.
    NearbyRadio r[] = {
        radio("AA:BB:CC:DD:EE:10", true, 6, "", "beacon rule"),
        radio("AA:BB:CC:DD:EE:12", true, 36, "Flock Safety", ""),
    };
    uint16_t c[NEARBY_CATEGORIES];
    count(r, 2, c);
    assert(countsAre(c, 1, 0, 0, 0, 0));
    // Two typed radios of different categories: equal rank, so the first in
    // input order leads (unitLead's tie rule).
    NearbyRadio tie[] = {
        radio("AA:BB:CC:DD:EE:10", true, 6, "Axon", ""),
        radio("AA:BB:CC:DD:EE:12", true, 36, "Flock Safety", ""),
    };
    count(tie, 2, c);
    assert(countsAre(c, 0, 1, 0, 0, 0));
    // A rule-only radio leads an otherwise unmatched box: weak beats none.
    NearbyRadio weak[] = {
        radio("AA:BB:CC:DD:EE:10", true, 6, "", ""),
        radio("AA:BB:CC:DD:EE:12", true, 36, "", "Lite-On IE"),
    };
    count(weak, 2, c);
    assert(countsAre(c, 0, 0, 0, 0, 1));
}

// Two same-category radios chained through an uncategorized one are one unit:
// the app groups the whole list, unmatched rows included with the filter off.
static void test_chain_through_uncategorized() {
    NearbyRadio r[] = {
        radio("AA:BB:CC:DD:EE:10", true, 6, "Flock Safety", ""),
        radio("AA:BB:CC:DD:EE:14", true, 0, "", ""),
        radio("AA:BB:CC:DD:EE:18", true, 36, "Flock Safety", ""),
    };
    uint16_t c[NEARBY_CATEGORIES];
    count(r, 3, c);
    assert(countsAre(c, 1, 0, 0, 0, 0));
    // Without the middle radio the two are 8 apart: two rows, two blinks.
    NearbyRadio apart[] = { r[0], r[2] };
    count(apart, 2, c);
    assert(countsAre(c, 2, 0, 0, 0, 0));
}

int main() {
    test_layout();
    test_counted_label();
    test_parse_mac();
    test_blink_schedule();
    test_latch();
    test_two_trackers_one_alpr();
    test_dual_band_counts_once();
    test_unmatched_not_counted();
    test_ignored_tracker_counted();
    test_only_wifi_groups();
    test_virtual_networks();
    test_cross_category_box();
    test_chain_through_uncategorized();
    test_include_filter_on();
    std::printf("nearby: all tests passed\n");
    return 0;
}
