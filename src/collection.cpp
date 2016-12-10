#include "collection.h"

#include <iostream>
#include <numeric>
#include <chrono>
#include <topster.h>
#include <intersection.h>
#include <match_score.h>
#include <string_utils.h>
#include <art.h>
#include "art.h"
#include "json.hpp"

Collection::Collection(std::string state_dir_path): seq_id(0) {
    store = new Store(state_dir_path);
    art_tree_init(&t);
}

Collection::~Collection() {
    delete store;
    art_tree_destroy(&t);
}

uint32_t Collection::next_seq_id() {
    return ++seq_id;
}

std::string Collection::add(std::string json_str) {
    nlohmann::json document = nlohmann::json::parse(json_str);

    uint32_t seq_id = next_seq_id();
    std::string seq_id_str = std::to_string(seq_id);

    if(document.count("id") == 0) {
        document["id"] = seq_id_str;
    }

    store->insert(get_seq_id_key(seq_id), document.dump());
    store->insert(get_id_key(document["id"]), seq_id_str);

    std::vector<std::string> tokens;
    StringUtils::tokenize(document["title"], tokens, " ", true);

    std::unordered_map<std::string, std::vector<uint32_t>> token_to_offsets;
    for(uint32_t i=0; i<tokens.size(); i++) {
        auto token = tokens[i];
        std::transform(token.begin(), token.end(), token.begin(), ::tolower);
        token_to_offsets[token].push_back(i);
    }

    for(auto & kv: token_to_offsets) {
        art_document art_doc;
        art_doc.id = seq_id;
        art_doc.score = document["points"];
        art_doc.offsets_len = (uint32_t) kv.second.size();
        art_doc.offsets = new uint32_t[kv.second.size()];

        uint32_t num_hits = 0;

        const unsigned char *key = (const unsigned char *) kv.first.c_str();
        int key_len = (int) kv.first.length() + 1;  // for the terminating \0 char

        art_leaf* leaf = (art_leaf *) art_search(&t, key, key_len);
        if(leaf != NULL) {
            num_hits = leaf->values->ids.getLength();
        }

        num_hits += 1;

        for(auto i=0; i<kv.second.size(); i++) {
            art_doc.offsets[i] = kv.second[i];
        }

        art_insert(&t, key, key_len, &art_doc, num_hits);
        delete art_doc.offsets;
    }

    doc_scores[seq_id] = document["points"];

    return document["id"];
}

void Collection::search_candidates(std::vector<std::vector<art_leaf*>> & token_leaves,
                                   std::vector<nlohmann::json> & results, size_t & total_results,
                                   const size_t & max_results) {
    const size_t combination_limit = 10;
    auto product = []( long long a, std::vector<art_leaf*>& b ) { return a*b.size(); };
    long long int N = std::accumulate(token_leaves.begin(), token_leaves.end(), 1LL, product);

    // For deduplication. If 2 query suggestions give a document as result, ensure that only one is returned
    spp::sparse_hash_set<uint64_t> dedup_seq_ids;

    for(long long n=0; n<N && n<combination_limit; ++n) {
        // every element in `query_suggestion` contains a token and its associated hits
        std::vector<art_leaf *> query_suggestion = next_suggestion(token_leaves, n);

        /*std:: cout << "\nSuggestion: ";
        for(auto suggestion_leaf: query_suggestion) {
            std:: cout << suggestion_leaf->key << " ";
        }
        std::cout << std::endl;*/

        // initialize results with the starting element (for further intersection)
        uint32_t* result_ids = query_suggestion[0]->values->ids.uncompress();
        size_t result_size = query_suggestion[0]->values->ids.getLength();

        if(result_size == 0) continue;

        // intersect the document ids for each token to find docs that contain all the tokens (stored in `result_ids`)
        for(auto i=1; i < query_suggestion.size(); i++) {
            uint32_t* out = new uint32_t[result_size];
            uint32_t* curr = query_suggestion[i]->values->ids.uncompress();
            result_size = Intersection::scalar(result_ids, result_size, curr, query_suggestion[i]->values->ids.getLength(), out);
            delete[] result_ids;
            delete[] curr;
            result_ids = out;
        }

        // go through each matching document id and calculate match score
        Topster<100> topster;
        score_results(topster, query_suggestion, result_ids, result_size);
        delete[] result_ids;
        topster.sort();

        for (uint32_t i = 0; i < topster.size && total_results < max_results; i++) {
            uint64_t seq_id = topster.getKeyAt(i);
            if(dedup_seq_ids.count(seq_id) == 0) {
                std::string value;
                store->get(get_seq_id_key((uint32_t) seq_id), value);
                nlohmann::json document = nlohmann::json::parse(value);
                results.push_back(document);
                dedup_seq_ids.emplace(seq_id);
                total_results++;
            }
        }

        if(total_results >= max_results) break;
    }
}

/*
   1. Split the query into tokens
   2. Outer loop will generate bounded cartesian product with costs for each token
   3. Inner loop will iterate on each token with associated cost
   4. Cartesian product of the results of the token searches will be used to form search phrases
      (cartesian product adapted from: http://stackoverflow.com/a/31169617/131050)
   4. Intersect the lists to find docs that match each phrase
   5. Sort the docs based on some ranking criteria
*/
std::vector<nlohmann::json> Collection::search(std::string query, const int num_typos, const size_t num_results,
                                               const token_ordering token_order, const bool prefix) {
    auto begin = std::chrono::high_resolution_clock::now();

    std::vector<std::string> tokens;
    StringUtils::tokenize(query, tokens, " ", true);

    const int max_cost = (num_typos < 0 || num_typos > 2) ? 2 : num_typos;
    const size_t max_results = std::min(num_results, (size_t) Collection::MAX_RESULTS);

    size_t total_results = 0;
    std::vector<nlohmann::json> results;

    // To prevent us from doing ART search repeatedly as we iterate through possible corrections
    spp::sparse_hash_map<std::string, std::vector<art_leaf*>> token_cache;

    // Used to drop the least occurring token(s) for partial searches
    spp::sparse_hash_map<std::string, uint32_t> token_to_count;

    std::vector<std::vector<int>> token_to_costs;
    std::vector<int> all_costs;

    for(int cost = 0; cost <= max_cost; cost++) {
        all_costs.push_back(cost);
    }

    for(size_t token_index = 0; token_index < tokens.size(); token_index++) {
        token_to_costs.push_back(all_costs);
        std::transform(tokens[token_index].begin(), tokens[token_index].end(), tokens[token_index].begin(), ::tolower);
    }

    std::vector<std::vector<art_leaf*>> token_leaves;
    const size_t combination_limit = 10;
    auto product = []( long long a, std::vector<int>& b ) { return a*b.size(); };
    long long n = 0;
    long long int N = std::accumulate(token_to_costs.begin(), token_to_costs.end(), 1LL, product);

    while(n < N && n < combination_limit) {
        // Outerloop generates combinations of [cost to max_cost] for each token
        // For e.g. for a 3-token query: [0, 0, 0], [0, 0, 1], [0, 1, 1] etc.
        std::vector<uint32_t> costs(token_to_costs.size());
        ldiv_t q { n, 0 };
        for(long long i = (token_to_costs.size() - 1); 0 <= i ; --i ) {
            q = ldiv(q.quot, token_to_costs[i].size());
            costs[i] = token_to_costs[i][q.rem];
        }

        token_leaves.clear();
        size_t token_index = 0;
        bool retry_with_larger_cost = false;

        while(token_index < tokens.size()) {
            // For each token, look up the generated cost for this iteration and search using that cost
            std::string token = tokens[token_index];
            const std::string token_cost_hash = token + std::to_string(costs[token_index]);

            std::vector<art_leaf*> leaves;

            if(token_cache.count(token_cost_hash) != 0) {
                leaves = token_cache[token_cost_hash];
            } else {
                int token_len = prefix ? (int) token.length() : (int) token.length() + 1;
                art_fuzzy_search(&t, (const unsigned char *) token.c_str(), token_len,
                                 costs[token_index], 3, token_order, prefix, leaves);
                if(!leaves.empty()) {
                    token_cache.emplace(token_cost_hash, leaves);
                }
            }

            if(!leaves.empty()) {
                log_leaves(costs[token_index], token, leaves);
                token_leaves.push_back(leaves);
                token_to_count[token] = leaves.at(0)->values->ids.getLength();
            } else {
                // No result at `cost = costs[token_index]` => remove cost for token and re-do combinations
                auto it = std::find(token_to_costs[token_index].begin(), token_to_costs[token_index].end(), costs[token_index]);
                if(it != token_to_costs[token_index].end()) {
                    token_to_costs[token_index].erase(it);

                    // no more costs left for this token, clean up
                    if(token_to_costs[token_index].empty()) {
                        token_to_costs.erase(token_to_costs.begin()+token_index);
                        tokens.erase(tokens.begin()+token_index);
                        token_index--;
                    }
                }

                n = -1;
                N = std::accumulate(token_to_costs.begin(), token_to_costs.end(), 1LL, product);

                // Unless we're already at max_cost for this token, don't look at remaining tokens since we would
                // see them again in a future iteration when we retry with a larger cost
                if(costs[token_index] != max_cost) {
                    retry_with_larger_cost = true;
                    break;
                }
            }

            token_index++;
        }

        if(token_leaves.size() != 0 && !retry_with_larger_cost) {
            // If a) all tokens were found, or b) Some were skipped because they don't exist within max_cost,
            // go ahead and search for candidates with what we have so far
            search_candidates(token_leaves, results, total_results, max_results);

            if (total_results > 0) {
                // Unless there are results, we continue outerloop (looking at tokens with greater cost)
                break;
            }
        }

        n++;
    }

    if(results.size() == 0 && token_to_count.size() != 0) {
        // Drop certain token with least hits and try searching again
        std::string truncated_query;

        std::vector<std::pair<std::string, uint32_t>> token_count_pairs;
        for (auto itr = token_to_count.begin(); itr != token_to_count.end(); ++itr) {
            token_count_pairs.push_back(*itr);
        }

        std::sort(token_count_pairs.begin(), token_count_pairs.end(), [=]
                  (const std::pair<std::string, uint32_t>& a, const std::pair<std::string, uint32_t>& b) {
                      return a.second > b.second;
                  }
        );

        for(uint32_t i = 0; i < token_count_pairs.size()-1; i++) {
            if(token_to_count.count(tokens[i]) != 0) {
                truncated_query += " " + token_count_pairs.at(i).first;
            }
        }

        return search(truncated_query, num_typos, num_results);
    }

    long long int timeMillis = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - begin).count();
    std::cout << "Time taken for result calc: " << timeMillis << "us" << std::endl;
    store->print_memory_usage();
    return results;
}

void Collection::log_leaves(const int cost, const std::string &token, const std::vector<art_leaf *> &leaves) const {
    printf("Token: %s, cost: %d, candidates: \n", token.c_str(), cost);
    for(auto i=0; i < leaves.size(); i++) {
        printf("%.*s, ", leaves[i]->key_len, leaves[i]->key);
        printf("frequency: %d, max_score: %d\n", leaves[i]->values->ids.getLength(), leaves[i]->max_score);
        /*for(auto j=0; j<leaves[i]->values->ids.getLength(); j++) {
            printf("id: %d\n", leaves[i]->values->ids.at(j));
        }*/
    }
}

void Collection::score_results(Topster<100> &topster, const std::vector<art_leaf *> &query_suggestion,
                                const uint32_t *result_ids, const size_t result_size) const {
    for(auto i=0; i<result_size; i++) {
        uint32_t doc_id = result_ids[i];
        std::vector<std::vector<uint16_t>> token_positions;

        MatchScore mscore;

        if(query_suggestion.size() == 1) {
            mscore = MatchScore{1, 1};
        } else {
            // for each token in the query, find the positions that it appears in this document
            for (art_leaf *token_leaf : query_suggestion) {
                std::vector<uint16_t> positions;
                uint32_t doc_index = token_leaf->values->ids.indexOf(doc_id);
                uint32_t start_offset = token_leaf->values->offset_index.at(doc_index);
                uint32_t end_offset = (doc_index == token_leaf->values->ids.getLength() - 1) ?
                                      token_leaf->values->offsets.getLength() :
                                      token_leaf->values->offset_index.at(doc_index+1);

                while(start_offset < end_offset) {
                    positions.push_back((uint16_t) token_leaf->values->offsets.at(start_offset));
                    start_offset++;
                }

                token_positions.push_back(positions);
            }

            mscore = MatchScore::match_score(doc_id, token_positions);
        }

        const uint64_t final_score = ((uint64_t)(mscore.words_present * 32 + (MAX_SEARCH_TOKENS - mscore.distance)) * UINT32_MAX) +
                                     doc_scores.at(doc_id);

        /*
          std::cout << "final_score: " << final_score << ", doc_id: " << doc_id << std::endl;
          uint32_t doc_score = doc_scores.at(doc_id);
          std::cout << "result_ids[i]: " << result_ids[i] << " - mscore.distance: "
                  << (int) mscore.distance << " - mscore.words_present: " << (int) mscore.words_present
                  << " - doc_scores[doc_id]: " << (int) doc_scores.at(doc_id) << "  - final_score: "
                  << final_score << std::endl;
        */

        topster.add(doc_id, final_score);
    }
}

inline std::vector<art_leaf *> Collection::next_suggestion(
        const std::vector<std::vector<art_leaf *>> &token_leaves,
        long long int n) {
    std::vector<art_leaf*> query_suggestion(token_leaves.size());

    // generate the next combination from `token_leaves` and store it in `query_suggestion`
    ldiv_t q { n, 0 };
    for(long long i=token_leaves.size()-1 ; 0<=i ; --i ) {
        q = ldiv(q.quot, token_leaves[i].size());
        query_suggestion[i] = token_leaves[i][q.rem];
    }

    // sort ascending based on matched documents for each token for faster intersection
    sort(query_suggestion.begin(), query_suggestion.end(), [](const art_leaf* left, const art_leaf* right) {
        return left->values->ids.getLength() < right->values->ids.getLength();
    });

    return query_suggestion;
}

void _remove_and_shift_offset_index(forarray &offset_index, const uint32_t* indices_sorted, const uint32_t indices_length) {
    uint32_t *curr_array = offset_index.uncompress();
    uint32_t *new_array = new uint32_t[offset_index.getLength()];

    new_array[0] = 0;
    uint32_t new_index = 0;
    uint32_t curr_index = 0;
    uint32_t indices_counter = 0;
    uint32_t shift_value = 0;

    while(curr_index < offset_index.getLength()) {
        if(indices_counter < indices_length && curr_index >= indices_sorted[indices_counter]) {
            // skip copying
            if(curr_index == indices_sorted[indices_counter]) {
                curr_index++;
                const uint32_t diff = curr_index == offset_index.getLength() ?
                                0 : (offset_index.at(curr_index) - offset_index.at(curr_index-1));

                shift_value += diff;
            }
            indices_counter++;
        } else {
            new_array[new_index++] = curr_array[curr_index++] - shift_value;
        }
    }

    offset_index.load_sorted(new_array, new_index);

    delete[] curr_array;
    delete[] new_array;
}

void Collection::remove(std::string id) {
    std::string seq_id_str;
    store->get(get_id_key(id), seq_id_str);

    uint32_t seq_id = (uint32_t) std::stoi(seq_id_str);

    std::string parsed_document;
    store->get(get_seq_id_key(seq_id), parsed_document);

    nlohmann::json document = nlohmann::json::parse(parsed_document);

    std::vector<std::string> tokens;
    StringUtils::tokenize(document["title"], tokens, " ", true);

    for(auto token: tokens) {
        std::transform(token.begin(), token.end(), token.begin(), ::tolower);

        const unsigned char *key = (const unsigned char *) token.c_str();
        int key_len = (int) (token.length() + 1);

        art_leaf* leaf = (art_leaf *) art_search(&t, key, key_len);
        if(leaf != NULL) {
            uint32_t seq_id_values[1] = {seq_id};

            uint32_t doc_index = leaf->values->ids.indexOf(seq_id);

            /*
            auto len = leaf->values->offset_index.getLength();
            for(auto i=0; i<len; i++) {
                std::cout << "i: " << i << ", val: " << leaf->values->offset_index.at(i) << std::endl;
            }
            std::cout << "----" << std::endl;
            */
            uint32_t start_offset = leaf->values->offset_index.at(doc_index);
            uint32_t end_offset = (doc_index == leaf->values->ids.getLength() - 1) ?
                                  leaf->values->offsets.getLength() :
                                  leaf->values->offset_index.at(doc_index+1);

            uint32_t doc_indices[1] = {doc_index};
            _remove_and_shift_offset_index(leaf->values->offset_index, doc_indices, 1);

            leaf->values->offsets.remove_index_unsorted(start_offset, end_offset);
            leaf->values->ids.remove_values_sorted(seq_id_values, 1);

            /*len = leaf->values->offset_index.getLength();
            for(auto i=0; i<len; i++) {
                std::cout << "i: " << i << ", val: " << leaf->values->offset_index.at(i) << std::endl;
            }
            std::cout << "----" << std::endl;*/

            if(leaf->values->ids.getLength() == 0) {
                art_delete(&t, key, key_len);
            }
        }
    }

    store->remove(get_id_key(id));
    store->remove(get_seq_id_key(seq_id));
}

std::string Collection::get_seq_id_key(uint32_t seq_id) {
    return SEQ_ID_PREFIX+std::to_string(seq_id);
}

std::string Collection::get_id_key(std::string id) {
    return ID_PREFIX+id;
}