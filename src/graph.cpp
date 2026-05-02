#include "bgp_sim.h"
#include <stack>
#include <queue>
#include <cassert>
#include <functional>

//  Parses a CAIDA AS-relationship file and builds the AS graph.
//  Returns false and prints an error if:
//  the file cannot be opened
//  a provider/customer cycle is detected

bool ASGraph::loadCAIDA(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "Error: cannot open CAIDA file: " << path << "\n";
        return false;
    }

    std::string line;
    while (std::getline(f, line)) {
        // Skip blank lines and comment lines
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ss(line);
        std::string tok;
        std::vector<std::string> parts;
        while (std::getline(ss, tok, '|')) parts.push_back(tok);
        if (parts.size() < 3) continue;

        uint32_t as1 = std::stoul(parts[0]);
        uint32_t as2 = std::stoul(parts[1]);
        int      rel = std::stoi(parts[2]);

        // Make sure both ASes exist in the graph before adding edges
        getOrCreate(as1);
        getOrCreate(as2);

        if (rel == -1) {
            // Provider->customer: as1 provides transit to as2
            ases[as1].customers.push_back(as2);
            ases[as2].providers.push_back(as1);
        } else if (rel == 0) {
            // Peer<->peer: traffic flows freely in both directions
            ases[as1].peers.push_back(as2);
            ases[as2].peers.push_back(as1);
        }
        // Other rel value is ignored
    }

    if (hasCycle()) {
        std::cerr << "\n";
        std::cerr << "=======================================================\n";
        std::cerr << "  ERROR: Cycle detected in provider/customer graph.\n";
        std::cerr << "  The AS graph must be a DAG (no circular provider-\n";
        std::cerr << "  customer relationships). Peer cycles are OK.\n";
        std::cerr << "  Check your CAIDA input file and fix the cycle above.\n";
        std::cerr << "=======================================================\n\n";
        return false;
    }

    // Assign propagation ranks now that the graph is fully built
    computeRanks();
    return true;
}

//  ASGraph::hasCycle

//  Detects cycles in the provider->customer directed graph using  a standard DFS with three-color marking:

//    0 = unvisited
//    1 = currently on the DFS stack (in progress)
//    2 = fully processed (done)


//  A back-edge (reaching a node still on the stack) means a cycle.

//  When a cycle is found, the two ASNs forming the back-edge are printed to help the user diagnose the problem.

bool ASGraph::hasCycle() const {
    std::unordered_map<uint32_t, int> color;
    for (auto& [asn, _] : ases) {
	    color[asn] = 0;
    }

    std::function<bool(uint32_t)> dfs = [&](uint32_t u) -> bool {
        color[u] = 1;  // mark as in-progress
        for (uint32_t customer : ases.at(u).customers) {
            if (color[customer] == 1) {
                // Back-edge: customer is still on the DFS stack -> cycle
                std::cerr << "  Cycle detected: ASN " << u
                          << " -> ASN " << customer
                          << " creates a provider/customer loop.\n";
                return true;
            }
            if (color[customer] == 0 && dfs(customer)) return true;
        }
        color[u] = 2;  // fully explored
        return false;
    };

    for (auto& [asn, _] : ases) {
        if (color[asn] == 0 && dfs(asn)) return true;
    }
    return false;
}

//  ASGraph::computeRanks

//  Assigns a propagation rank to every AS using BFS from leaves.
//  Flattens the DAG into layers for ordered propagation.

//  This ensures that during upward propagation (rank 0 -> max),  an AS only processes after all its customers have already forwarded their announcements. Isolated ASes get rank 0.

void ASGraph::computeRanks() {
    for (auto& [asn, as] : ases) as.prop_rank = -1;

    std::queue<uint32_t> q;

    // All leaf ASes (no customers) start at rank 0
    for (auto& [asn, as] : ases) {
        if (as.customers.empty()) {
            as.prop_rank = 0;
            q.push(asn);
        }
    }

    int max_rank = 0;
    while (!q.empty()) {
        uint32_t cur = q.front(); q.pop();
        int next_rank = ases[cur].prop_rank + 1;
        for (uint32_t prov : ases[cur].providers) {
            if (ases[prov].prop_rank < next_rank) {
                ases[prov].prop_rank = next_rank;
                max_rank = std::max(max_rank, next_rank);
                q.push(prov);
            }
        }
    }

    // Any AS still unranked is isolated (only has peer relationships)
    for (auto& [asn, as] : ases) {
        if (as.prop_rank == -1) as.prop_rank = 0;
    }

    // Build ranks[]: a vector of ASN lists grouped by rank, used for efficient rank-ordered iteration during propagation.
    ranks.assign(max_rank + 1, {});
    for (auto& [asn, as] : ases) {
        int r = std::min(as.prop_rank, max_rank);
        ranks[r].push_back(asn);
    }
}

//  Places an announcement directly into an AS's local RIB, bypassing the receive queue. Used to inject origin announcements before propagation begins.

void ASGraph::seed(uint32_t asn, const Announcement& ann) {
    auto it = ases.find(asn);
    if (it == ases.end()) {
        std::cerr << "Warning: cannot seed unknown ASN " << asn
                  << " (not in CAIDA file). Skipping.\n";
        return;
    }
    it->second.policy->local_rib[ann.prefix] = ann;
}

//  Copies all RIB entries of sender_asn into each neighbour's  receive queue with the appropriate relationship tag.

void ASGraph::sendTo(uint32_t sender_asn,
                     const std::vector<uint32_t>& neighbours,
                     Relationship rel) {
    auto& sender = ases.at(sender_asn);
    for (auto& [prefix, ann] : sender.policy->local_rib) {
        Announcement copy = ann;
        copy.recv_relationship = rel;    // from the receiver's perspective used for tiebraking
        copy.next_hop_asn = sender_asn; 
	for (uint32_t nb : neighbours) {
            auto nb_it = ases.find(nb);
            if (nb_it != ases.end()) {
                nb_it->second.policy->receive(copy);
            }
        }
    }
}
//  Phase 1 of BGP propagation: customers advertise to providers. Iterates ranks from 0 (leaves) up to max_rank (tier-1s).

void ASGraph::propagateUp() {
    for (auto& rank_vec : ranks) {
        // Process queued announcements at this rank first
        for (uint32_t asn : rank_vec) {
            ases[asn].policy->processQueue(asn);
        }
        // Forward best announcements upward to providers
        for (uint32_t asn : rank_vec) {
            sendTo(asn, ases[asn].providers, Relationship::CUSTOMER);
        }
    }
}

//  Phase 2 of BGP propagation: peers exchange announcements. Valley-free routing requires peer announcements travel exactly one hop. This is enforced by a strict two-phase approach:

void ASGraph::propagateAcross() {
    // Phase A: every AS sends to all peers simultaneously
    for (auto& [asn, as] : ases) {
        sendTo(asn, as.peers, Relationship::PEER);
    }
    // Phase B: every AS processes what it received from peers
    for (auto& [asn, as] : ases) {
        as.policy->processQueue(asn);
    }
}

//  Phase 3 of BGP propagation: providers push routes to customers. Mirror of propagateUp(), iterating from max_rank down to 0.

void ASGraph::propagateDown() {
    for (int r = (int)ranks.size() - 1; r >= 0; --r) {
        // Send announcements downward to all customers
        for (uint32_t asn : ranks[r]) {
            sendTo(asn, ases[asn].customers, Relationship::PROVIDER);
        }
        // The rank below processes what it just received
        if (r > 0) {
            for (uint32_t asn : ranks[r - 1]) {
                ases[asn].policy->processQueue(asn);
            }
        } else {
            // Rank 0 is the bottom; it processes its own queue
            for (uint32_t asn : ranks[0]) {
                ases[asn].policy->processQueue(asn);
            }
        }
    }
}

//  Runs the complete BGP propagation sequence:
//  Up    — customers advertise best routes to providers
//  Across — peers exchange routes (one hop only)
//  Down  — providers push best routes to customers
//
void ASGraph::propagate() {
    propagateUp();
    propagateAcross();
    propagateDown();
}

//  Dumps the final RIB state to a CSV file with columns: asn, prefix, as_pat as_path is hyphen-separated, origin-first:

void ASGraph::writeCSV(const std::string& path) const {
    std::ofstream f(path);
    if (!f) {
        std::cerr << "Error: cannot open output file: " << path << "\n";
        return;
    }
    f << "asn,prefix,as_path\n";
    for (auto& [asn, as] : ases) {
        for (auto& [prefix, ann] : as.policy->local_rib) {
            f << asn << ',' << prefix << ',' << ann.asPathStr() << '\n';
        }
    }
}

