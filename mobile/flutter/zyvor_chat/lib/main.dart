import 'package:flutter/material.dart';
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
  bool _busy = false;

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
      setState(() => _msgs = msgs);
    } catch (e) {
      setState(() => _status = 'node: $e');
    }
  }

  Future<void> _send() async {
    final to = _toCtrl.text.trim();
    final text = _msgCtrl.text.trim();
    if (to.isEmpty || text.isEmpty || _chat == null) return;
    setState(() => _status = 'mengirim...');
    try {
      final txid = await _chat!.sendMessage(to, text);
      setState(() {
        _status = 'terkirim: ${txid.substring(0, 12)}...';
        _msgCtrl.clear();
      });
      _refresh();
    } catch (e) {
      setState(() => _status = 'gagal: $e');
    }
  }

  Future<void> _register() async {
    if (_chat == null) return;
    try {
      await _chat!.register();
      setState(() => _status = 'register ✓ (tunggu blok, lalu bisa terima E2E)');
    } catch (e) {
      setState(() => _status = 'register gagal: $e');
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
    return Scaffold(
      appBar: AppBar(
        title: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          const Text('Zyvor Chat', style: TextStyle(fontSize: 16)),
          Text(_wallet!.address,
              style: const TextStyle(fontSize: 10, color: Colors.white60)),
        ]),
        actions: [
          IconButton(icon: const Icon(Icons.app_registration), tooltip: 'Register pubkey', onPressed: _register),
          IconButton(icon: const Icon(Icons.refresh), onPressed: _refresh),
        ],
      ),
      body: Column(children: [
        if (_status.isNotEmpty)
          Padding(padding: const EdgeInsets.all(6), child: Text(_status, style: const TextStyle(fontSize: 11, color: Colors.amber))),
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
