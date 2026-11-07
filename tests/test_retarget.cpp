#include "../src/blockchain.h"
#include <cassert>
#include <iostream>
using namespace zyv;

int main() {
    Blockchain bc;
    bc.createGenesis();
    uint32_t d0 = bc.difficulty();  // 4
    // isi hingga >2016 blok dgn mineBlock (cepat, timestamp real konsisten)
    for (int i = 0; i < 2017; i++) bc.mineBlock({}, "RETARGETTEST");
    uint32_t d1 = bc.difficulty();  // retarget alami sudah terpicu di blok ke-2016

    // uji unit: adjustDifficulty utk scenario sintetis
    // mulai ulang dari salinan segar:
    Blockchain bc2;
    bc2.createGenesis();
    for (int i = 0; i < 2017; i++) bc2.mineBlock({}, "RT");

    // hitung timestamp hipotetis: kalau 2016 blok terakhir ditempuh dalam 1/8 target:
    uint64_t lastTs = 0;
    // akses chain? blockchain.h punya chain()? cek publik: bc2.chain() dipakai p2p GETBLOCKS → ya publik.
    uint64_t tsNow = bc2.chain().back().timestamp;
    uint64_t startTs = bc2.chain()[bc2.chain().size()-1-2016].timestamp;
    uint64_t target = 2016ull * 600ull;
    // jalur cepat: pretend actual = target/8 → tapi adjustDifficulty pakai newTimestamp - start.
    uint64_t fastTs = startTs + target/8;
    // catatan: adjustDifficulty hanya berjalan jika (size-1)%2016==0; size=2018 (genesis+2017) → 2017%2016 != 0. 
    // Karena itu mine satu lagi supaya size-1=2018... 2018%2016=2 → tidak. Perlu size-1 = 2016 → size 2017.
    // Ulangi: genesis + 2016 blok → size=2017 → (size-1)=2016 %2016==0 ✓
    Blockchain bc3;
    bc3.createGenesis();
    for (int i = 0; i < 2016; i++) bc3.mineBlock({}, "RT");
    uint32_t d3 = bc3.difficulty();  // retarget natural: blok nyata cepat → +2
    std::cout << "natural retarget (jaringan cepat): " << d0 << " → " << d3 << "\n";
    assert(d3 == d0 + 2);  // cepat → naik
    std::cout << "RETARGET ARAH OK: cepat → naik (clamp maks +2)\n";
    return 0;
}
