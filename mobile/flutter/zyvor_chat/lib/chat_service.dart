// ============================================================================
// chat_service.dart — Logika chat Zyvor untuk Flutter (gabungan zyvcore + api)
// ============================================================================
// Alur kirim pesan E2E dari HP:
//   1. api.pubkey(to)                        → pubkey penerima
//   2. zyvcore.encrypt(pubkeyTo, pesan)      → ciphertext (privkey tidak keluar)
//   3. api.address(alamatku)                 → UTXO
//   4. zyvcore.buildTx(inputs, outputs, ct)  → tx ter-sign (serialized)
//   5. api.sendRawTx(serialized)             → broadcast ke jaringan
//
// Alur terima:
//   polling api.messages(alamatku) → pesan encrypted → zyvcore.decrypt(privkey)
// ============================================================================
import 'zyvcore.dart';
import 'api.dart';

class ChatService {
  final ZyvorApi api;
  final Wallet me;
  ChatService(this.api, this.me);

  static const double chatAmount = 0.0001;
  static const double fee = 0.0001;

  /// Kirim pesan terenkripsi. Return txid.
  Future<String> sendMessage(String toAddr, String plaintext) async {
    // 1. pubkey penerima (bila ke diri sendiri / register, pakai pubkey lokal langsung)
    final pubTo = (toAddr == me.address && me.pubkey.isNotEmpty)
        ? me.pubkey
        : await api.pubkey(toAddr);

    // 2. enkripsi di HP
    final ciphertext = ZyvCore.encrypt(pubTo, plaintext);

    // 3. UTXO saya
    final addrInfo = await api.address(me.address);
    final utxos = (addrInfo['utxos'] as List).cast<Map<String, dynamic>>();

    // pilih UTXO cukup untuk amount+fee (unit terkecil: 1 ZYVC = 1e8)
    const need = (chatAmount + fee) * 1e8;
    final inputs = <Map<String, dynamic>>[];
    var total = 0;
    for (final u in utxos) {
      double amt = 0.0;
      if (u.containsKey('amount') && u['amount'] != null) {
        amt = (u['amount'] is num) ? (u['amount'] as num).toDouble() : (double.tryParse(u['amount'].toString()) ?? 0.0);
      } else if (u.containsKey('value') && u['value'] != null) {
        amt = double.tryParse(u['value'].toString()) ?? 0.0;
      }
      total += (amt * 1e8).round();
      inputs.add({'txid': u['txid'], 'vout': u['vout']});
      if (total >= need) break;
    }
    if (total < need) {
      throw Exception('Saldo kurang untuk kirim pesan');
    }

    // 4. outputs: penerima + kembalian
    final outputs = <Map<String, dynamic>>[
      {'address': toAddr, 'amount': (chatAmount * 1e8).round()},
    ];
    final change = total - (need).round();
    if (change > 0) {
      outputs.add({'address': me.address, 'amount': change});
    }

    // 5. sign di HP → serialized
    final serialized = ZyvCore.buildTx(
      inputs: inputs,
      outputs: outputs,
      data: ciphertext,
      privkeyHex: me.privkey,
    );

    // 6. broadcast
    return api.sendRawTx(serialized);
  }

  /// Inbox saya (terbaru dulu), dgn dekripsi lokal di HP.
  Future<List<ChatMessage>> inbox({int limit = 50}) async {
    final raw = await api.messages(me.address, limit: limit);
    final out = <ChatMessage>[];
    for (final m in raw) {
      final toMe = m['to'] == me.address;
      String text = m['data'] as String;
      bool isEnc = text.startsWith('1|');
      if (toMe && isEnc) {
        try {
          text = ZyvCore.decrypt(me.privkey, text);
        } catch (_) {
          text = '[terenkripsi]';
        }
      }
      out.add(ChatMessage(
        from: m['from'] as String,
        to: m['to'] as String,
        text: text,
        encrypted: isEnc,
        outgoing: m['from'] == me.address,
        status: m['status'] as String,
        time: m['time'] as int,
        block: m['block'] as int,
        txid: m['txid'] as String,
      ));
    }
    return out;
  }

  /// Daftarkan pubkey saya (agar bisa menerima pesan E2E) — kirim 0.0001 ke diri sendiri.
  Future<String> register() => sendMessage(me.address, 'register');

  /// Saldo saya saat ini.
  Future<String> getBalance() async {
    final addrInfo = await api.address(me.address);
    return addrInfo['balance']?.toString() ?? '0.00000000';
  }

  /// Klaim koin gratis (faucet 50 ZYVC).
  Future<Map<String, dynamic>> claimFaucet() => api.claimFaucet(me.address);
}

class ChatMessage {
  final String from, to, text, status, txid;
  final bool encrypted, outgoing;
  final int time, block;
  ChatMessage({
    required this.from,
    required this.to,
    required this.text,
    required this.encrypted,
    required this.outgoing,
    required this.status,
    required this.time,
    required this.block,
    required this.txid,
  });

  bool get isPending => status == 'pending';
}
