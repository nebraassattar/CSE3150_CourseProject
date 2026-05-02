# BGP Simulator — CSE 3150 Course Project

This is a BGP simulator written in C++17. It loads a real-world AS topology from CAIDA, seeds BGP announcements, propagates them through the graph using standard BGP policies (including ROV), and dumps the resulting RIBs to a CSV.

---

## Building and Running

```bash
make              # builds ./bgp_sim
make test         # runs the test suite
make clean        # removes binaries
```

```bash
./bgp_sim <caida_file> <announcements.csv> <rov_asns.csv> <output.csv>
```

- `caida_file` — CAIDA AS-relationship file (pipe-delimited, downloaded from caida.org)
- `announcements.csv` — the announcements to seed, with columns `asn,prefix,rov_invalid`
- `rov_asns.csv` — one ASN per line, each of which deploys ROV
- `output.csv` — where the final RIBs get written (`asn,prefix,as_path`)

To verify your output against the expected results:
```bash
./compare_output.sh ribs.csv output.csv
```

If a provider/customer cycle is detected in the CAIDA file, the program prints which ASNs form the cycle and exits with code 2. Peer cycles are fine and expected.

---

## How it works

### The AS Graph

The internet at the AS level is a directed acyclic graph. ASes connect to each other in two ways: provider-customer relationships (the customer pays the provider for transit) and peer-peer relationships (traffic flows freely between them). We load this topology from a CAIDA relationship file, which encodes each edge as `as1|as2|rel` where `rel=-1` means as1 is the provider and `rel=0` means they're peers.

Each AS node tracks its providers, customers, and peers as adjacency lists. The graph is stored as an `unordered_map<uint32_t, AS>` so lookups are O(1) — with 100k nodes and 500k edges this makes a real difference compared to a sorted map.

### Cycle Detection

Before doing anything else, we run a DFS over the provider→customer edges to check for cycles. A back-edge in the DFS means a cycle exists. If one is found, we print the ASNs involved and exit — the graph must be a DAG for propagation to work correctly. Peer cycles are not checked because they're normal and expected.

### Propagation Ranks

To propagate announcements in the right order, we flatten the DAG into layers. ASes with no customers get rank 0 (these are the stub/edge ASes at the bottom of the internet hierarchy). Their providers get rank 1, and so on up. This is a BFS from the leaves. The result is a `vector<vector<uint32_t>>` where `ranks[i]` contains all ASes at rank i.

### BGP Announcements

Each announcement carries a prefix (e.g. `1.2.0.0/16`), an AS-path (the list of ASNs it has traveled through), a next-hop ASN, and the relationship it was received from. ASes store their best announcement per prefix in a local RIB (a hashmap from prefix to announcement).

When an AS receives multiple announcements for the same prefix, it picks the best one using standard BGP preference:
1. Best received relationship — customer-learned routes beat peer-learned routes, which beat provider-learned routes
2. Shortest AS-path
3. Lowest next-hop ASN as a tiebreaker

### Propagation

Propagation happens in three phases, which reflects how BGP actually works on the internet:

**Up:** Starting from rank 0, each AS sends its announcements to its providers. Providers process their receive queues and forward further up. By the time we reach the top, every tier-1 AS has seen every announcement that could possibly reach it from below.

**Across:** Every AS sends to its peers simultaneously, then every AS processes what it received. The send-then-process order is critical — if we interleaved them, an announcement could hop A→B→C across two peer links in a single pass, which would violate valley-free routing. The two-phase approach prevents that.

**Down:** Starting from the top rank, each AS sends its announcements down to customers. Customers process their queues and forward further down.

### ROV

Some ASes deploy Route Origin Validation (ROV), which drops announcements marked `rov_invalid`. This is implemented using a `Policy` base class with two subclasses: `BGP` (accepts everything) and `ROV` (drops invalid announcements). Each AS holds a `unique_ptr<Policy>`, so the propagation code never needs to know which policy an AS uses — it just calls `processQueue()` and the right behavior happens.

### AS-Path Storage

The AS-path is stored origin-first in a vector (e.g. `[777, 3, 4]` means 777 originated, then 3 received it, then 4). Each AS appends its own ASN with `push_back` when it stores an announcement. This avoids the O(n) cost of inserting at the front on every hop, and prints naturally in the expected format.

---

## File Structure

```
bgp_sim/
├── include/bgp_sim.h      — all types: Announcement, Policy, BGP, ROV, AS, ASGraph
├── src/graph.cpp          — graph loading, cycle detection, ranking, propagation, output
├── src/main.cpp           — entry point, reads input CSVs, drives the simulation
├── tests/test_bgp.cpp     — unit and system tests, no external dependencies
├── Makefile
└── README.md
```

---

## Tests

`make test` runs 20 tests covering:

- Announcement tiebreaking (relationship > path length > next-hop ASN)
- A simple two-AS graph to verify basic propagation
- The bgpsimulator.com example graph, including conflict resolution at AS 4
- ROV filtering (ROV ASes drop invalid announcements, non-ROV ASes don't)
- Cycle detection (program correctly rejects a cyclic provider/customer graph)
- The peer one-hop rule (announcements can't travel two peer links in one propagation)

