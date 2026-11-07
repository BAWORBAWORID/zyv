#include "miner.h"
#include "blockchain.h"
#include "block.h"
#include "mempool.h"
#include "sha256.h"
#include "params.h"
#include <chrono>
#include <curl/curl.h>
#include <iostream>
#include <sstream>
#include <thread>

namespace zyv {

static bool curlInitOnce() {
    static bool ok = [] { return curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK; }();
    return ok;
}

// ---------------------------------------------------------------------------
// util HTTP (libcurl)
// ---------------------------------------------------------------------------
static size_t curlWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static std::string httpPost(const std::string& url, const std::string& body) {
    CURL* c = curl_easy_init();
    if (!c) return "";
    std::string out;
    struct curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_perform(c);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return out;
}

static std::string httpGet(const std::string& url) {
    CURL* c = curl_easy_init();
    if (!c) return "";
    std::string out;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
    return out;
}

// JSON field extractor sederhana: "key":"value" atau "key":123
static std::string jsonField(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\":";
    size_t p = json.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < json.size() && json[p] == ' ') p++;
    if (p >= json.size()) return "";
    if (json[p] == '"') {
        size_t e = json.find('"', p + 1);
        if (e == std::string::npos) return "";
        return json.substr(p + 1, e - p - 1);
    }
    size_t e = p;
    while (e < json.size() && (isdigit((unsigned char)json[e]) || json[e] == '.' ||
                               json[e] == '-'))
        e++;
    return json.substr(p, e - p);
}

// ---------------------------------------------------------------------------
// SOLO mining
// ---------------------------------------------------------------------------
void MinerDaemon::runSolo(Blockchain& bc, Mempool& mp, const std::string& minerAddress,
                          int pollIntervalSec, bool alwaysMine, std::atomic<bool>& stopFlag) {
    curlInitOnce();
    std::cout << std::unitbuf; // log real-time
    std::cout << "[miner] solo mining aktif - reward ke " << minerAddress << "\n";
    std::cout << "[miner] poll mempool tiap " << pollIntervalSec << "s, Ctrl+C untuk berhenti\n";
    while (!stopFlag.load()) {
        size_t pending = mp.all().size();
        if (alwaysMine || pending > 0) {
            auto start = std::chrono::steady_clock::now();
            const Block& b = bc.mineBlock(mp.all(), minerAddress);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            std::cout << "[miner] blok #" << b.index << " nonce=" << b.nonce
                      << " txs=" << b.transactions.size() << " (" << ms << " ms)"
                      << " reward=" << b.transactions[0].outputs[0].amount << "\n";
            std::vector<std::string> included;
            for (size_t t = 1; t < b.transactions.size(); t++)
                included.push_back(b.transactions[t].id);
            mp.remove(included);
        } else {
            std::cout << "[miner] mempool kosong - menunggu...\n";
        }
        for (int i = 0; i < pollIntervalSec * 10 && !stopFlag.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "[miner] berhenti\n";
}

// ---------------------------------------------------------------------------
// POOL worker (stratum-style over HTTP)
// ---------------------------------------------------------------------------
PoolJob MinerDaemon::poolFetchWork(const std::string& poolUrl) {
    PoolJob j;
    std::string raw = httpGet(poolUrl + "/api/pool/work");
    j.prevHash = jsonField(raw, "prevHash");
    j.height = std::stoull(jsonField(raw, "height"));
    j.difficulty = std::stoul(jsonField(raw, "difficulty"));
    j.shareDifficulty = std::stoul(jsonField(raw, "shareDifficulty"));
    j.poolAddress = jsonField(raw, "poolAddress");
    j.reward = std::stoull(jsonField(raw, "reward"));
    return j;
}

std::string MinerDaemon::poolSubmit(const std::string& poolUrl, const std::string& body) {
    return httpPost(poolUrl + "/api/pool/submit", body);
}

void MinerDaemon::runPoolWorker(const std::string& poolUrl, const std::string& workerName,
                                const std::string& payoutAddress, std::atomic<bool>& stopFlag) {
    curlInitOnce();
    std::cout << std::unitbuf; // log real-time
    std::cout << "[worker] stratum worker '" << workerName << "' -> pool " << poolUrl << "\n";
    std::cout << "[worker] payout alamat: " << payoutAddress << "\n";

    // registrasi alamat payout
    std::string reg = "{\"worker\":\"" + workerName + "\",\"address\":\"" + payoutAddress + "\"}";
    poolSubmit(poolUrl, reg);

    uint64_t shares = 0;
    uint64_t nonceBase = 1;
    while (!stopFlag.load()) {
        PoolJob job;
        try {
            job = poolFetchWork(poolUrl);
        } catch (...) {
            std::cout << "[worker] pool tidak merespons - retry 10s\n";
            for (int i = 0; i < 100 && !stopFlag.load(); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (job.prevHash.empty()) {
            std::cout << "[worker] job kosong - tunggu 3s\n";
            for (int i = 0; i < 30 && !stopFlag.load(); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Hash share: sha256(sha256(prevHash|worker|nonce)) sampai < shareDifficulty.
        // Bila memenuhi network difficulty -> submit full=true (blok kandidat).
        uint64_t nonce = nonceBase;
        bool foundBlock = false;
        while (!stopFlag.load()) {
            std::string preimage = job.prevHash + "|" + workerName + "|" + std::to_string(nonce);
            std::string h = sha256(sha256(preimage));
            if (meetsDifficulty(h, job.shareDifficulty)) {
                shares++;
                std::cout << "[worker] share (#" << shares << ") nonce=" << nonce << "\n";
                bool full = meetsDifficulty(h, job.difficulty);
                std::string body = "{\"worker\":\"" + workerName +
                                   "\",\"nonce\":" + std::to_string(nonce) +
                                   ",\"hash\":\"" + h + "\",\"height\":" +
                                   std::to_string(job.height) + ",\"full\":" +
                                   (full ? "true" : "false") + "}";
                std::string resp = poolSubmit(poolUrl, body);
                std::cout << "[worker] pool: " << resp << "\n";
                if (resp.find("BLOCK_FOUND") != std::string::npos) foundBlock = true;
                if (resp.find("duplikat") == std::string::npos) {
                    nonceBase = nonce + 1;
                    break;
                }
            }
            nonce++;
        }
        if (foundBlock)
            std::cout << "[worker] BLOK DITEMUKAN! pool akan PPLNS payout\n";
        for (int i = 0; i < 20 && !stopFlag.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "[worker] berhenti - total share: " << shares << "\n";
}

} // namespace zyv
