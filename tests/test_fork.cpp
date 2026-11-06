
#include "blockchain.h"
#include "block.h"
#include <iostream>
using namespace zyv;
int main() {
    // Node A: genesis + 2 blok sendiri
    Blockchain a;
    a.createGenesis();
    auto b1 = a.mineBlock({}, "ADDRX");
    auto b2 = a.mineBlock({}, "ADDRX");
    std::cout << "A chain height: " << a.height() << " work: " << a.totalWork() << "\n";

    // Node B fork: mulai dari rantai [genesis, b1] lalu tambah 2 blok BERBEDA (timestamp beda)
    Blockchain bb;
    bb.createGenesis();
    // replikasi genesis & b1 identik dgn A: deserialize rantai A parsial
    std::string prefix;
    {
        Blockchain t;
        t.deserialize(a.serialize());
        (void)t;
    }
    // simpler: copy A lalu ganti blok 2 dgn versi timestamp beda -> kita bangun manual
    // Ambil genesis & b1 dari A
    Blockchain c;
    c.createGenesis();
    // c.chain_ private; gunakan serialize trick: buat Blockchain dgn chain A minus blok terakhir
    // API publik tidak expose; maka test pakai pendekatan lain: mine b2' dari state yang sama tak bisa.
    // Alternatif: gunakan 2 genesis berbeda? Harus sama utk fork.
    std::cout << "(butuh akses prefix — uji via full API bawah)\n";

    // Uji reorg via addBlock: node A menerima blok fork yang dibangun dari b1
    // Buat blok fork manual: index 2, prevHash = b1.hash, timestamp beda (b2.timestamp + 1)
    Block f;
    f.index = b2.index;
    f.timestamp = b2.timestamp + 1; // beda -> hash beda
    f.prevHash = b1.hash;
    f.difficulty = 4;
    Transaction cb;
    cb.outputs.push_back({INITIAL_REWARD_ZYVOR, "FORKMINER"});
    cb.timestamp = f.timestamp;
    cb.id = computeTxId(cb);
    f.transactions.push_back(cb);
    f.merkleRoot = merkleRoot({cb.id});
    f.nonce = 0;
    f.hash = blockHash(f);
    while (!meetsDifficulty(f.hash, f.difficulty)) { ++f.nonce; f.hash = blockHash(f); }

    // A sudah punya b2 (bukan f). Buat node A2 = A tapi tanpa b2? Tidak bisa via API publik.
    // Jadi: tambahkan f -> harus masuk orphan/kasus fork (b.prevHash == b1.hash ada di rantai).
    bool added = a.addBlock(f);
    std::cout << "addBlock(fork block) accepted: " << added << " (harus false: work sama)\n";
    std::cout << "A height after fork attempt: " << a.height() << "\n";

    // Sekarang buat fork dengan 2 blok (work lebih besar):
    // f2 extends f
    Block f2;
    f2.index = f.index + 1;
    f2.timestamp = f.timestamp + 1;
    f2.prevHash = f.hash;
    f2.difficulty = 4;
    Transaction cb2;
    cb2.outputs.push_back({INITIAL_REWARD_ZYVOR, "FORKMINER"});
    cb2.timestamp = f2.timestamp;
    cb2.id = computeTxId(cb2);
    f2.transactions.push_back(cb2);
    f2.merkleRoot = merkleRoot({cb2.id});
    f2.nonce = 0;
    f2.hash = blockHash(f2);
    while (!meetsDifficulty(f2.hash, f2.difficulty)) { ++f2.nonce; f2.hash = blockHash(f2); }

    bool added2 = a.addBlock(f2);  // f2 orphan (prev = f.hash, belum di chain)
    std::cout << "addBlock(f2) first (orphan): " << added2 << "\n";
    bool added3 = a.addBlock(f);   // f: fork dari blok tengah; harus menarik f2 dari pool
    std::cout << "addBlock(f) with f2 in pool: " << added3 << "\n";
    std::cout << "A height final: " << a.height() << " (harus 4 jika reorg ke fork 2 blok)\n";
    std::cout << "tip address: " << a.tip().transactions[0].outputs[0].address << "\n";
    std::cout << "orphans: " << a.orphanCount() << "\n";
    bool ok = a.validateChain();
    std::cout << "chain valid: " << ok << "\n";

    // f3 extends f2 -> fork lebih panjang
    Block f3;
    f3.index = f2.index + 1;
    f3.timestamp = f2.timestamp + 1;
    f3.prevHash = f2.hash;
    f3.difficulty = 4;
    Transaction cb3;
    cb3.outputs.push_back({INITIAL_REWARD_ZYVOR, "FORKMINER"});
    cb3.timestamp = f3.timestamp;
    cb3.id = computeTxId(cb3);
    f3.transactions.push_back(cb3);
    f3.merkleRoot = merkleRoot({cb3.id});
    f3.nonce = 0;
    f3.hash = blockHash(f3);
    while (!meetsDifficulty(f3.hash, f3.difficulty)) { ++f3.nonce; f3.hash = blockHash(f3); }
    bool added4 = a.addBlock(f3);
    std::cout << "addBlock(f3): " << added4 << "\n";
    std::cout << "A height final: " << a.height() << " (harus 5 jika reorg ke fork 3 blok)\n";
    std::cout << "tip address: " << a.tip().transactions[0].outputs[0].address << " (harus FORKMINER)\n";
    std::cout << "chain valid: " << a.validateChain() << "\n";
    return a.tip().transactions[0].outputs[0].address == "FORKMINER" ? 0 : 1;

}
