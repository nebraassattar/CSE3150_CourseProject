#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <optional>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>

//  Relationship enum

enum class Relationship : uint8_t {
    ORIGIN   = 0,   // originated here – highest priority
    CUSTOMER = 1,
    PEER     = 2,
    PROVIDER = 3
};

inline int relPriority(Relationship r) {
    // Lower int = higher priority
    switch (r) {
        case Relationship::ORIGIN:   return 0;
        case Relationship::CUSTOMER: return 1;
        case Relationship::PEER:     return 2;
        case Relationship::PROVIDER: return 3;
    }
    return 9;
}

inline std::string relStr(Relationship r) {
    switch (r) {
        case Relationship::ORIGIN:   return "origin";
        case Relationship::CUSTOMER: return "customer";
        case Relationship::PEER:     return "peer";
        case Relationship::PROVIDER: return "provider";
    }
    return "unknown";
}


//  Announcement

struct Announcement {
    std::string      prefix;
    std::vector<uint32_t> as_path;   // front = origin, back = most-recently-prepended
    uint32_t         next_hop_asn = 0;
    Relationship     recv_relationship = Relationship::ORIGIN;
    bool             rov_invalid = false;

    // Comparison: lower score = more preferred
    // Returns true if *this* is better than other
    bool betterThan(const Announcement& other) const {
        int myPri  = relPriority(recv_relationship);
        int otPri  = relPriority(other.recv_relationship);
        if (myPri != otPri) return myPri < otPri;
        if (as_path.size() != other.as_path.size())
            return as_path.size() < other.as_path.size();
        return next_hop_asn < other.next_hop_asn;
    }

    std::string asPathStr() const {
        std::string s;
        for (size_t i = 0; i < as_path.size(); ++i) {
            if (i) s += '-';
            s += std::to_string(as_path[i]);
        }
        return s;
    }
};


//  Policy (abstract base)

class Policy {
public:
    // local_rib: prefix -> best announcement
    std::unordered_map<std::string, Announcement> local_rib;
    // recv_queue: prefix -> list of received announcements
    std::unordered_map<std::string, std::vector<Announcement>> recv_queue;

    virtual ~Policy() = default;

    // Returns true if ann was accepted & stored
    virtual bool receive(const Announcement& ann) {
        recv_queue[ann.prefix].push_back(ann);
        return true;
    }

    // Process recv_queue into local_rib; return whether rib changed
    virtual bool processQueue(uint32_t my_asn) {
        bool changed = false;
        for (auto& [prefix, anns] : recv_queue) {
            for (auto& ann : anns) {
                if (!shouldAccept(ann)) continue;
                auto it = local_rib.find(prefix);
                if (it == local_rib.end() || ann.betterThan(it->second)) {
                    // Prepend my ASN
                    Announcement stored = ann;
                    stored.as_path.push_back(my_asn);
                    local_rib[prefix] = std::move(stored);
                    changed = true;
                }
            }
        }
        recv_queue.clear();
        return changed;
    }

    virtual bool shouldAccept(const Announcement&) { return true; }
};


//  BGP policy

class BGP : public Policy {};


//  ROV policy – drops rov_invalid announcements

class ROV : public BGP {
public:
    bool shouldAccept(const Announcement& ann) override {
        return !ann.rov_invalid;
    }
};


//  AS node

struct AS {
    uint32_t asn = 0;
    std::vector<uint32_t> providers;
    std::vector<uint32_t> customers;
    std::vector<uint32_t> peers;
    int prop_rank = -1;
    std::unique_ptr<Policy> policy;

    AS() : policy(std::make_unique<BGP>()) {}
    explicit AS(uint32_t a) : asn(a), policy(std::make_unique<BGP>()) {}

    void setROV() { policy = std::make_unique<ROV>(); }
};

//  AS Graph

class ASGraph {
public:
    std::unordered_map<uint32_t, AS> ases;
    std::vector<std::vector<uint32_t>> ranks; // ranks[0] = leaf ASes

    AS& getOrCreate(uint32_t asn) {
        auto it = ases.find(asn);
        if (it == ases.end()) {
            ases.emplace(asn, AS(asn));
        }
        return ases.at(asn);
    }

    // Returns true if loaded OK, false on cycle
    bool loadCAIDA(const std::string& path);

    // Detect provider/customer cycles (peer cycles are OK)
    // Uses DFS. Returns true if cycle found.
    bool hasCycle() const;

    // Compute propagation ranks
    void computeRanks();

    // Seed an announcement at a specific AS (stores in local_rib, bypasses queue)
    void seed(uint32_t asn, const Announcement& ann);

    // Full propagation: up -> across -> down
    void propagate();

    // Write CSV output
    void writeCSV(const std::string& path) const;

private:
    void propagateUp();
    void propagateAcross();
    void propagateDown();

    // Send all rib entries of `sender_asn` to a list of neighbours
    // Sets recv_relationship on copies
    void sendTo(uint32_t sender_asn,
                const std::vector<uint32_t>& neighbours,
                Relationship rel);
};
