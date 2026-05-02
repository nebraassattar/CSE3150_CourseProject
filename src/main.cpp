#include "bgp_sim.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

bool loadAnnouncements(const std::string& path, ASGraph& graph) {
	
	std::ifstream f(path);

	if (!f) {
		std::cerr << "Error: cannot open announcements file: " << path << "\n";
		return false;
	}

	std::string line;

	bool header = true;

	while (std::getline(f, line)) {
		if (line.empty()) {
			continue;
		}
		if (header) {
			header = false;
			continue; // Skip the header
		}

		std::istringstream ss(line);
		std::string tok;
		std::vector<std::string> parts;

		while (std::getline(ss, tok, ',')) {
			parts.push_back(tok);
		}
		
		if (parts.size() < 3) {
			continue;
		}

		uint32_t asn = std::stoul(parts[0]);

		std::string prefix = parts[1];

		while (!prefix.empty() && (prefix.back() == '\r' || prefix.back() == ' ')) {
			prefix.pop_back();
		} // Trims whitespace for prefix

		bool rov_invalid = (parts[2][0] == '1' || parts[2][0] == 't' || parts[2][0] == 'T');

		graph.getOrCreate(asn) // Makes sure that AS exists
		Announcement ann;
		ann.prefix = prefix;
		ann.as_path = {asn};
		ann.next_hop_asn = asn;
		ann.recv_relationship = Relationship::ORIGIN;
		ann.rov_invalid = rov_invalid;

		graph.seed(asn, ann);
	}
	
	return true;
}

bool loadROVAsns(const std::string& path, ASGraph& graph) {
	
	std::ifstream f(path);

	if (!f) {
		std::cerr << "Error: cannot open ROV ASNs file: " << path << "\n";
		return false;
	}

	std::string line;

	bool first = true;

	while (std::getline(f, line)) {
		if (line.empty()) {
			continue;
		}

		if (first) {
			first = false;
			if (!std::isdigit(line[0])) {
				continue;
			}
		}

			
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\n')){ // Trim
				line.pop_back();

			if (line.empty()) {
				continue;
			}

			std::istringstream ss(line);
			
			std::string tok;

			while (std::getline(ss, tok, ',')) {
				if (tok.empty()) {
					continue;
				}

				try {
					uint32_t asn = std::stoul(tok);
					auto it = graph.ases.find.(asn);

					if (it != graph.ases.end()) {
						it -> second.setROV();
					}
				} catch (...) {}
			}
		}
	}

	return true;
}

static void printUsage(const char* prog) {
	std::cer << "Usage: " << prog << " <caida_file> <announcements.csv> Mrov_asns.csv> <output.csv>\n" << "\n" << " caida_file CAIDA AS relationship file\n" << " announcements.csv Columns: asn,prefix,rov_invalid\n" << "rov_asns.csv One ASN per line (or CSV)\n" << " output.csv Output RIB CSV (asn,prefix,as_path)\n"
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        printUsage(argv[0]);
        return 1;
    }

    std::string caida_file   = argv[1];
    std::string ann_file     = argv[2];
    std::string rov_file     = argv[3];
    std::string output_file  = argv[4];

    ASGraph graph;

    std::cerr << "[1/5] Loading AS graph from CAIDA...\n";
    if (!graph.loadCAIDA(caida_file)) {
        // Cycle detected or file error
        return 2;
    }
    std::cerr << "      Loaded " << graph.ases.size() << " ASes, "
              << graph.ranks.size() << " propagation ranks.\n";

    std::cerr << "[2/5] Loading announcements...\n";
    if (!loadAnnouncements(ann_file, graph)) return 3;

    std::cerr << "[3/5] Applying ROV policies...\n";
    if (!loadROVAsns(rov_file, graph)) return 4;

    std::cerr << "[4/5] Propagating announcements...\n";
    graph.propagate();

    std::cerr << "[5/5] Writing output CSV...\n";
    graph.writeCSV(output_file);

    std::cerr << "Done! Output written to: " << output_file << "\n";
    return 0;
}
