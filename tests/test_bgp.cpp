#include "bgp_sim.h"
#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>

//  Mini helpers
static int tests_run = 0, tests_passed = 0;

#define CHECK(cond, msg) do { \
    ++tests_run; \
    if (cond) { ++tests_passed; std::cout << "  PASS: " << msg << "\n"; } \
    else { std::cout << "  FAIL: " << msg << "\n"; } \
} while(0)

//  Test 1: Announcement comparison

void testAnnouncementComparison() {
    std::cout << "\n=== Announcement Comparison ===\n";

    Announcement a, b;
    a.prefix = "1.2.0.0/16"; b.prefix = "1.2.0.0/16";

    // Customer beats Provider
    a.recv_relationship = Relationship::CUSTOMER;
    b.recv_relationship = Relationship::PROVIDER;
    a.as_path = {1,2}; b.as_path = {3};
    a.next_hop_asn = 5; b.next_hop_asn = 1;
    CHECK(a.betterThan(b), "Customer beats Provider (even with longer path)");
    CHECK(!b.betterThan(a), "Provider does not beat Customer");

    // Same rel, shorter path wins
    a.recv_relationship = Relationship::PEER;
    b.recv_relationship = Relationship::PEER;
    a.as_path = {1};
    b.as_path = {1, 2};
    CHECK(a.betterThan(b), "Shorter path wins same relationship");

    // Same rel, same path, lower next_hop wins
    a.as_path = {1}; b.as_path = {2};
    a.next_hop_asn = 5; b.next_hop_asn = 10;
    CHECK(a.betterThan(b), "Lower next_hop wins tiebreak");
}

//  Test 2: Simple linear graph  777 -> 3 -> customer 777 announces 1.2.0.0/16, should reach all ASes

void testSimpleLinearGraph() {
    std::cout << "\n=== Simple Linear Graph: 777 -> 3 ===\n";
    // Create CAIDA-style input
    // 3 is customer of 777 (777|3|-1)
    std::string caida = "/tmp/test_caida.txt";
    {
        std::ofstream f(caida);
        f << "# comment\n";
        f << "777|3|-1|i\n";  // 777 is provider of 3
    }

    ASGraph g;
    bool ok = g.loadCAIDA(caida);
    CHECK(ok, "Loaded simple graph without cycle");
    CHECK(g.ases.size() == 2, "Graph has 2 ASes");
    CHECK(g.ases.count(777) && g.ases.count(3), "ASNs 777 and 3 exist");

    // 777 has customer 3; 3 has provider 777
    CHECK(g.ases[777].customers.size() == 1 && g.ases[777].customers[0] == 3,
          "777's customer is 3");
    CHECK(g.ases[3].providers.size() == 1 && g.ases[3].providers[0] == 777,
          "3's provider is 777");

    // Seed announcement at 777
    Announcement ann;
    ann.prefix = "1.2.0.0/16";
    ann.as_path = {777};
    ann.next_hop_asn = 777;
    ann.recv_relationship = Relationship::ORIGIN;
    ann.rov_invalid = false;
    g.seed(777, ann);

    g.propagate();

    // Both ASes should have the prefix
    CHECK(g.ases[777].policy->local_rib.count("1.2.0.0/16"),
          "777 has 1.2.0.0/16 in its RIB");
    CHECK(g.ases[3].policy->local_rib.count("1.2.0.0/16"),
          "3 has 1.2.0.0/16 in its RIB");

    // AS 3's path should be [3, 777]  (3 prepends itself)
    auto& ann3 = g.ases[3].policy->local_rib.at("1.2.0.0/16");
    CHECK(ann3.as_path.size() == 2 && ann3.as_path[0] == 777 && ann3.as_path[1] == 3,
          "AS 3 path is [777, 3] (stored as origin first)");
    std::cout << "  AS 3 path string: " << ann3.asPathStr() << "\n";
}

//  Test 3: bgpsimulator.com example
//  AS 4 is provider of 666 and 3; AS 4 is peer with 777
//  777 announces 1.2.0.0/16, 666 also announces 1.2.0.0/16

void testExampleGraph() {
    std::cout << "\n=== bgpsimulator.com Example Graph ===\n";
    // 4 is provider of 666 and 3
    // 4 and 777 are peers
    std::string caida = "/tmp/test_caida2.txt";
    {
        std::ofstream f(caida);
        f << "4|666|-1|i\n";   // 4 provider of 666
        f << "4|3|-1|i\n";     // 4 provider of 3
        f << "4|777|0|i\n";    // 4 and 777 are peers
    }

    ASGraph g;
    CHECK(g.loadCAIDA(caida), "Loaded example graph");

    // 777 announces 1.2.0.0/16
    {
        Announcement ann;
        ann.prefix = "1.2.0.0/16";
        ann.as_path = {777};
        ann.next_hop_asn = 777;
        ann.recv_relationship = Relationship::ORIGIN;
        ann.rov_invalid = false;
        g.seed(777, ann);
    }
    // 666 hijacks 1.2.0.0/16
    {
        Announcement ann;
        ann.prefix = "1.2.0.0/16";
        ann.as_path = {666};
        ann.next_hop_asn = 666;
        ann.recv_relationship = Relationship::ORIGIN;
        ann.rov_invalid = true;  // this is a hijack
        g.seed(666, ann);
    }

    g.propagate();

    // At AS 4: receives from customer 666 and from peer 777
    // 666 has shorter path (len 1) and is a customer -> 666 wins
    auto& rib4 = g.ases[4].policy->local_rib;
    if (rib4.count("1.2.0.0/16")) {
        auto& a = rib4.at("1.2.0.0/16");
        std::cout << "  AS 4 chose path: " << a.asPathStr()
                  << " (rel=" << relStr(a.recv_relationship) << ")\n";
        CHECK(a.as_path[0] == 666, "AS 4 chooses 666's announcement (customer, shorter)");
    } else {
        CHECK(false, "AS 4 has 1.2.0.0/16");
    }
}

//  Test 4: ROV drops rov_invalid

void testROV() {
    std::cout << "\n=== ROV Filtering ===\n";
    std::string caida = "/tmp/test_caida3.txt";
    {
        std::ofstream f(caida);
        f << "4|3|-1|i\n";   // 4 is provider of 3
        f << "4|666|-1|i\n"; // 4 is provider of 666
    }

    ASGraph g;
    g.loadCAIDA(caida);

    // 3 is an ROV AS
    g.ases[3].setROV();

    // 666 sends rov_invalid announcement
    {
        Announcement ann;
        ann.prefix = "1.2.0.0/16";
        ann.as_path = {666};
        ann.next_hop_asn = 666;
        ann.recv_relationship = Relationship::ORIGIN;
        ann.rov_invalid = true;
        g.seed(666, ann);
    }

    g.propagate();

    // AS 3 (ROV) should NOT have the prefix
    CHECK(!g.ases[3].policy->local_rib.count("1.2.0.0/16"),
          "ROV AS 3 rejects rov_invalid announcement");
    // AS 4 (non-ROV) should have it
    CHECK(g.ases[4].policy->local_rib.count("1.2.0.0/16"),
          "Non-ROV AS 4 accepts announcement from customer 666");
}

//  Test 5: Cycle detection

void testCycleDetection() {
    std::cout << "\n=== Cycle Detection ===\n";
    std::string caida = "/tmp/test_caida_cycle.txt";
    {
        std::ofstream f(caida);
        // 1->2->3->1 provider/customer cycle
        f << "1|2|-1|i\n";
        f << "2|3|-1|i\n";
        f << "3|1|-1|i\n";
    }

    ASGraph g;
    bool ok = g.loadCAIDA(caida);
    CHECK(!ok, "Cycle detected in provider/customer graph");
}


//  Test 6: Peer announcements only travel one hop

void testPeerOneHop() {
    std::cout << "\n=== Peer routing: one hop only ===\n";
    // A - B (peers), B - C (peers)
    // A announces prefix, should reach B but NOT C via peers
    std::string caida = "/tmp/test_peer.txt";
    {
        std::ofstream f(caida);
        f << "100|200|0|i\n"; // 100 and 200 are peers
        f << "200|300|0|i\n"; // 200 and 300 are peers
    }

    ASGraph g;
    g.loadCAIDA(caida);

    Announcement ann;
    ann.prefix = "5.5.0.0/16";
    ann.as_path = {100};
    ann.next_hop_asn = 100;
    ann.recv_relationship = Relationship::ORIGIN;
    g.seed(100, ann);

    g.propagate();

    CHECK(g.ases[100].policy->local_rib.count("5.5.0.0/16"), "AS 100 has prefix");
    CHECK(g.ases[200].policy->local_rib.count("5.5.0.0/16"), "AS 200 gets prefix from peer 100");
    // 300 should NOT get it (would require two peer hops)
    CHECK(!g.ases[300].policy->local_rib.count("5.5.0.0/16"),
          "AS 300 does NOT get prefix (no two peer hops)");
}

int main() {
    std::cout << "BGP Simulator Test Suite\n";
    std::cout << "========================\n";

    testAnnouncementComparison();
    testSimpleLinearGraph();
    testExampleGraph();
    testROV();
    testCycleDetection();
    testPeerOneHop();

    std::cout << "\n========================\n";
    std::cout << "Results: " << tests_passed << "/" << tests_run << " passed\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
