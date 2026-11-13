// ============================================================================
// api.dart — Klien API node Zyvor untuk aplikasi chat mobile (Flutter)
// ============================================================================
// Dependency: package:http (tambahkan di pubspec.yaml).
//
// Contoh penggunaan:
//   final api = ZyvorApi('http://119.28.71.87:8933');
//   final msgs = await api.messages(alamatku);
//   final txid = await api.sendRawTx(serializedTx);
// ============================================================================
import 'dart:convert';
import 'package:http/http.dart' as http;

class ZyvorApi {
  final String nodeUrl;
  ZyvorApi(this.nodeUrl);

  Map<String, dynamic> _parse(http.Response resp) {
    final m = jsonDecode(resp.body) as Map<String, dynamic>;
    if (m['status'] == 'error') throw ApiException(m['message'] as String);
    return m;
  }

  // ---------------------------------------------------------------- read
  /// Info node: height, peers, mempool, dll.
  Future<Map<String, dynamic>> info() async =>
      _parse(await http.get(Uri.parse('$nodeUrl/api/info')));

  /// Semua pesan untuk alamat (chain + mempool). status: pending|confirmed.
  /// dec=true → node dekripsi otomatis (hanya bila wallet alamat ada di node).
  Future<List<Map<String, dynamic>>> messages(String addr,
      {int limit = 50, bool dec = false}) async {
    final m = _parse(await http
        .get(Uri.parse('$nodeUrl/api/messages?addr=$addr&limit=$limit${dec ? '&dec=1' : ''}')));
    return (m['messages'] as List).cast<Map<String, dynamic>>();
  }

  /// Pubkey on-chain alamat (butuh alamat pernah kirim tx / register).
  Future<String> pubkey(String addr) async {
    final m = _parse(await http.get(Uri.parse('$nodeUrl/api/pubkey?addr=$addr')));
    return m['pubkey'] as String;
  }

  /// Saldo & UTXO alamat.
  Future<Map<String, dynamic>> address(String addr) async =>
      _parse(await http.get(Uri.parse('$nodeUrl/api/address/$addr')));

  /// Klaim koin dari faucet/miner node (untuk saldo awal registrasi & kirim pesan).
  Future<Map<String, dynamic>> claimFaucet(String addr) async =>
      _parse(await http.get(Uri.parse('$nodeUrl/api/mine?addr=$addr&n=1')));

  // ---------------------------------------------------------------- write
  /// Kirim tx yang SUDAH ditandatangani di HP (zyvcore.buildTx) → broadcast.
  /// Return txid. Privkey TIDAK pernah keluar dari HP.
  Future<String> sendRawTx(String serializedTx) async {
    final m = _parse(await http.post(
      Uri.parse('$nodeUrl/api/sendrawtx'),
      headers: {'Content-Type': 'text/plain'},
      body: serializedTx,
    ));
    return m['txid'] as String;
  }

  /// (opsional) kirim pesan pakai wallet node — untuk mode kiosk/faucet,
  /// bukan untuk chat privasi (privkey di server).
  Future<String> sendEncrypted(String from, String to, String plaintext,
      {double amount = 0.0001}) async {
    final data = Uri.encodeComponent(plaintext);
    final m = _parse(await http.post(Uri.parse(
        '$nodeUrl/api/send?from=$from&to=$to&amount=$amount&enc=1&data=$data')));
    return m['txid'] as String;
  }
}

class ApiException implements Exception {
  final String message;
  ApiException(this.message);
  @override
  String toString() => 'ApiException: $message';
}
