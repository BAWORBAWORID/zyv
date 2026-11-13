import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'dart:async';
import 'zyvcore.dart';
import 'api.dart';
import 'chat_service.dart';

void main() => runApp(const ZyvorChatApp());

const defaultNode = 'http://bore.pub:8934';

class ZyvorChatApp extends StatelessWidget {
  const ZyvorChatApp({super.key});
  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'Zyvor Chat',
        theme: ThemeData.dark(useMaterial3: true),
        home: const HomePage(),
      );
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});
  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final _storage = const FlutterSecureStorage();
  Wallet? _wallet;
  ChatService? _chat;
  String _node = defaultNode;
  final _toCtrl = TextEditingController();
  final _msgCtrl = TextEditingController();
  List<ChatMessage> _msgs = [];
  Timer? _poll;
  String _status = '';
  String _balance = '0.00000000';
  bool _busy = false;
  bool _claimingFaucet = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void dispose() {
    _poll?.cancel();
    _toCtrl.dispose();
    _msgCtrl.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    final addr = await _storage.read(key: 'address');
    final priv = await _storage.read(key: 'privkey');
    final pub = await _storage.read(key: 'pubkey');
    final node = await _storage.read(key: 'node');
    if (node != null) _node = node;
    if (addr != null && priv != null && pub != null) {
      _wallet = Wallet(address: addr, privkey: priv, pubkey: pub);
      _startChat();
    }
    setState(() {});
  }

  Future<void> _createWallet() async {
    if (_busy) return;
    _busy = true;
    setState(() => _status = 'membuat wallet...');
    try {
      final w = ZyvCore.keygen();
      await _storage.write(key: 'address', value: w.address);
      await _storage.write(key: 'privkey', value: w.privkey);
      await _storage.write(key: 'pubkey', value: w.pubkey);
      setState(() => _wallet = w);
      _startChat();
    } catch (e) {
      setState(() => _status = 'gagal buat wallet: $e');
    } finally {
      _busy = false;
    }
  }

  void _startChat() {
    _chat = ChatService(ZyvorApi(_node), _wallet!);
    _poll?.cancel();
    _poll = Timer.periodic(const Duration(seconds: 4), (_) => _refresh());
    _refresh();
  }

  Future<void> _refresh() async {
    if (_chat == null) return;
    try {
      final msgs = await _chat!.inbox();
      final bal = await _chat!.getBalance();
      setState(() {
        _msgs = msgs;
        _balance = bal;
      });
    } catch (e) {
      setState(() => _status = 'node: $e');
    }
  }

  Future<void> _claimFaucet() async {
    if (_chat == null || _claimingFaucet) return;
    setState(() {
      _claimingFaucet = true;
      _status = 'Menambang koin faucet (+50 ZYVC) untuk wallet Anda...';
    });
    try {
      await _chat!.claimFaucet();
      setState(() => _status = 'Faucet berhasil! +50 ZYVC masuk.');
      await _refresh();
    } catch (e) {
      setState(() => _status = 'Faucet gagal: $e');
    } finally {
      setState(() => _claimingFaucet = false);
    }
  }

  Future<void> _send() async {
    final to = _toCtrl.text.trim();
    final text = _msgCtrl.text.trim();
    if (to.isEmpty || text.isEmpty || _chat == null || _busy) return;
    final balVal = double.tryParse(_balance) ?? 0.0;
    if (balVal < 0.0002) {
      setState(() => _status = 'Saldo kurang! Silakan klik "Klaim Koin" terlebih dahulu.');
      return;
    }
    _busy = true;
    setState(() => _status = 'mengirim...');
    try {
      final txid = await _chat!.sendMessage(to, text);
      setState(() {
        _status = 'terkirim: ${txid.substring(0, 12)}...';
        _msgCtrl.clear();
      });
      await _refresh();
    } catch (e) {
      if (e.toString().contains('Saldo kurang')) {
        setState(() => _status = 'Saldo kurang untuk kirim pesan. Silakan klaim koin.');
      } else {
        setState(() => _status = 'gagal: $e');
      }
    } finally {
      setState(() => _busy = false);
    }
  }

  Future<void> _register() async {
    if (_chat == null || _busy) return;
    final balVal = double.tryParse(_balance) ?? 0.0;
    if (balVal < 0.0002) {
      setState(() => _status = 'Saldo 0 ZYVC! Silakan klik "Klaim Koin" terlebih dahulu.');
      return;
    }
    _busy = true;
    setState(() => _status = 'Mendaftarkan pubkey ke blockchain...');
    try {
      await _chat!.register();
      setState(() => _status = 'register ✓ (tunggu blok, lalu bisa terima E2E)');
      await _refresh();
    } catch (e) {
      if (e.toString().contains('Saldo kurang')) {
        setState(() => _status = 'Saldo kurang untuk register (butuh 0.0002 ZYVC). Silakan klik "Klaim Koin".');
      } else {
        setState(() => _status = 'register gagal: $e');
      }
    } finally {
      setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    if (_wallet == null) {
      return Scaffold(
        body: Center(
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 24),
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                const Icon(Icons.shield_outlined, size: 72, color: Colors.blueAccent),
                const SizedBox(height: 16),
                const Text('Zyvor Chat',
                    style: TextStyle(fontSize: 28, fontWeight: FontWeight.bold)),
                const SizedBox(height: 8),
                const Text(
                  'Chat E2E on-chain • Privkey tersimpan aman di HP kamu',
                  textAlign: TextAlign.center,
                  style: TextStyle(color: Colors.white70),
                ),
                const SizedBox(height: 32),
                SizedBox(
                  width: 220,
                  height: 48,
                  child: FilledButton.icon(
                    icon: _busy
                        ? const SizedBox(
                            width: 18,
                            height: 18,
                            child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white),
                          )
                        : const Icon(Icons.key),
                    label: Text(_busy ? 'Membuat Wallet...' : 'Buat Wallet'),
                    onPressed: _busy ? null : _createWallet,
                  ),
                ),
                if (_status.isNotEmpty) ...[
                  const SizedBox(height: 16),
                  Container(
                    padding: const EdgeInsets.all(10),
                    decoration: BoxDecoration(
                      color: _status.contains('gagal') ? Colors.red.withValues(alpha: 0.2) : Colors.amber.withValues(alpha: 0.2),
                      borderRadius: BorderRadius.circular(8),
                    ),
                    child: Text(
                      _status,
                      textAlign: TextAlign.center,
                      style: TextStyle(
                        fontSize: 12,
                        color: _status.contains('gagal') ? Colors.redAccent : Colors.amberAccent,
                      ),
                    ),
                  ),
                ],
              ],
            ),
          ),
        ),
      );
    }

    final incoming = _msgs.where((m) => !m.outgoing).toList();
    final balVal = double.tryParse(_balance) ?? 0.0;
    final isZeroBalance = balVal < 0.0002;

    return Scaffold(
      appBar: AppBar(
        title: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            const Text('Zyvor Chat', style: TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
            const SizedBox(width: 8),
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
              decoration: BoxDecoration(
                color: isZeroBalance ? Colors.orange.withValues(alpha: 0.25) : Colors.green.withValues(alpha: 0.25),
                borderRadius: BorderRadius.circular(4),
              ),
              child: Text(
                '$_balance ZYVC',
                style: TextStyle(
                  fontSize: 11,
                  fontWeight: FontWeight.bold,
                  color: isZeroBalance ? Colors.amberAccent : Colors.greenAccent,
                ),
              ),
            ),
          ]),
          InkWell(
            onTap: () {
              Clipboard.setData(ClipboardData(text: _wallet!.address));
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(
                  content: Text('Alamat wallet disalin ke clipboard!'),
                  duration: Duration(seconds: 2),
                ),
              );
            },
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Text(
                  _wallet!.address.length > 22
                      ? '${_wallet!.address.substring(0, 10)}...${_wallet!.address.substring(_wallet!.address.length - 8)}'
                      : _wallet!.address,
                  style: const TextStyle(fontSize: 10, color: Colors.white70),
                ),
                const SizedBox(width: 4),
                const Icon(Icons.copy, size: 10, color: Colors.white54),
              ],
            ),
          ),
        ]),
        actions: [
          if (_claimingFaucet)
            const Padding(
              padding: EdgeInsets.symmetric(horizontal: 14.0),
              child: SizedBox(
                width: 18,
                height: 18,
                child: CircularProgressIndicator(strokeWidth: 2, color: Colors.amberAccent),
              ),
            )
          else
            IconButton(
              icon: const Icon(Icons.paid_outlined, color: Colors.amberAccent),
              tooltip: 'Klaim Koin Faucet (50 ZYVC)',
              onPressed: _claimFaucet,
            ),
          IconButton(
            icon: const Icon(Icons.app_registration),
            tooltip: 'Register pubkey (0.0002 ZYVC)',
            onPressed: _busy ? null : _register,
          ),
          IconButton(icon: const Icon(Icons.refresh), tooltip: 'Refresh', onPressed: _refresh),
        ],
      ),
      body: Column(children: [
        if (isZeroBalance)
          Container(
            margin: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
            decoration: BoxDecoration(
              color: Colors.amber.withValues(alpha: 0.15),
              border: Border.all(color: Colors.amber.withValues(alpha: 0.4)),
              borderRadius: BorderRadius.circular(8),
            ),
            child: Row(
              children: [
                const Icon(Icons.info_outline, color: Colors.amberAccent, size: 18),
                const SizedBox(width: 8),
                const Expanded(
                  child: Text(
                    'Saldo 0 ZYVC. Klaim koin untuk Register & Chat.',
                    style: TextStyle(fontSize: 11, color: Colors.white),
                  ),
                ),
                TextButton(
                  onPressed: _claimingFaucet ? null : _claimFaucet,
                  child: _claimingFaucet
                      ? const SizedBox(
                          width: 14,
                          height: 14,
                          child: CircularProgressIndicator(strokeWidth: 2, color: Colors.amberAccent),
                        )
                      : const Text(
                          'Klaim Koin',
                          style: TextStyle(color: Colors.amberAccent, fontWeight: FontWeight.bold, fontSize: 12),
                        ),
                ),
              ],
            ),
          ),
        if (_status.isNotEmpty)
          Padding(
            padding: const EdgeInsets.all(6),
            child: Text(_status, style: const TextStyle(fontSize: 11, color: Colors.amber)),
          ),
        Expanded(
          child: incoming.isEmpty
              ? const Center(child: Text('Belum ada pesan.\nKirim ke alamat di bawah', textAlign: TextAlign.center))
              : ListView.builder(
                  reverse: true,
                  padding: const EdgeInsets.all(8),
                  itemCount: incoming.length,
                  itemBuilder: (_, i) {
                    final m = incoming[incoming.length - 1 - i];
                    return Align(
                      alignment: Alignment.centerLeft,
                      child: Container(
                        margin: const EdgeInsets.symmetric(vertical: 4),
                        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                        constraints: const BoxConstraints(maxWidth: 280),
                        decoration: BoxDecoration(
                          color: Colors.blueGrey[800],
                          borderRadius: BorderRadius.circular(12),
                        ),
                        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                          Text(m.from, style: const TextStyle(fontSize: 9, color: Colors.white54)),
                          Text(m.text, style: const TextStyle(fontSize: 14)),
                          Row(mainAxisSize: MainAxisSize.min, children: [
                            Icon(m.isPending ? Icons.schedule : Icons.done_all,
                                size: 12, color: m.isPending ? Colors.grey : Colors.lightGreen),
                            const SizedBox(width: 4),
                            Text(m.isPending ? 'pending' : 'blok ${m.block}',
                                style: const TextStyle(fontSize: 9, color: Colors.white38)),
                            if (m.encrypted) const Padding(
                              padding: EdgeInsets.only(left: 4),
                              child: Icon(Icons.lock, size: 11, color: Colors.greenAccent),
                            ),
                          ]),
                        ]),
                      ),
                    );
                  },
                ),
        ),
        SafeArea(
          child: Padding(
            padding: const EdgeInsets.all(8),
            child: Column(children: [
              TextField(
                controller: _toCtrl,
                decoration: const InputDecoration(
                    hintText: 'Alamat tujuan (C...)', border: OutlineInputBorder(), isDense: true),
                style: const TextStyle(fontSize: 13),
              ),
              const SizedBox(height: 6),
              Row(children: [
                Expanded(
                  child: TextField(
                    controller: _msgCtrl,
                    decoration: const InputDecoration(
                        hintText: 'Pesan...', border: OutlineInputBorder(), isDense: true),
                    style: const TextStyle(fontSize: 14),
                    onSubmitted: (_) => _send(),
                  ),
                ),
                IconButton.filled(icon: const Icon(Icons.send), onPressed: _send),
              ]),
            ]),
          ),
        ),
      ]),
    );
  }
}
