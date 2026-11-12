import 'dart:ffi';
import 'dart:io' show Platform;
import 'dart:convert';
import 'package:ffi/ffi.dart';

/// Flutter FFI bindings untuk libzyvcore (core Zyvor C++).
///
/// Library dinamis:
///  - Android : libzyvcore.so (jniLibs per ABI)
///  - iOS     : di-link statis via podspec / framework
///  - Desktop : libzyvcore.so / .dylib / .dll di PATH atau sebelah executable
class ZyvCore {
  static DynamicLibrary? _lib;
  static DynamicLibrary get lib => _lib ??= _open();

  static DynamicLibrary _open() {
    if (Platform.isAndroid) return DynamicLibrary.open('libzyvcore.so');
    if (Platform.isIOS) return DynamicLibrary.process(); // statis link
    if (Platform.isMacOS) return DynamicLibrary.open('libzyvcore.dylib');
    if (Platform.isWindows) return DynamicLibrary.open('zyvcore.dll');
    return DynamicLibrary.open('libzyvcore.so'); // linux
  }

  // ---- native signatures -------------------------------------------------
  static final Pointer<NativeFunction<Pointer<Utf8> Function()>> _version =
      lib.lookup('zyv_version').cast();
  static final Pointer<NativeFunction<Pointer<Utf8> Function()>> _keygen =
      lib.lookup('zyv_keygen').cast();
  static final Pointer<NativeFunction<Pointer<Utf8> Function(Pointer<Utf8>)>>
      _addrFromPub = lib.lookup('zyv_address_from_pubkey').cast();
  static final Pointer<
          NativeFunction<Pointer<Utf8> Function(Pointer<Utf8>, Pointer<Utf8>)>>
      _sign = lib.lookup('zyv_sign').cast();
  static final Pointer<
          NativeFunction<Pointer<Utf8> Function(Pointer<Utf8>, Pointer<Utf8>)>>
      _encrypt = lib.lookup('zyv_encrypt').cast();
  static final Pointer<
          NativeFunction<Pointer<Utf8> Function(Pointer<Utf8>, Pointer<Utf8>)>>
      _decrypt = lib.lookup('zyv_decrypt').cast();
  static final Pointer<
          NativeFunction<
              Pointer<Utf8> Function(
                  Pointer<Utf8>,
                  Pointer<Utf8>,
                  Pointer<Utf8>,
                  Pointer<Utf8>)>> _buildTx =
      lib.lookup('zyv_build_tx').cast();
  static final Pointer<NativeFunction<Void Function(Pointer<Utf8>)>> _free =
      lib.lookup('zyv_free').cast();

  // ---- helpers -----------------------------------------------------------
  static String _call(Pointer<NativeFunction<Pointer<Utf8> Function()>> fn) {
    final p = fn.asFunction<Pointer<Utf8> Function()>()();
    final s = p.toDartString();
    _free.asFunction<void Function(Pointer<Utf8>)>()(p);
    return s;
  }

  static String _call1(
      Pointer<NativeFunction<Pointer<Utf8> Function(Pointer<Utf8>)>> fn,
      String a) {
    final pa = a.toNativeUtf8();
    final p = fn.asFunction<Pointer<Utf8> Function(Pointer<Utf8>)>()(pa);
    final s = p.toDartString();
    _free.asFunction<void Function(Pointer<Utf8>)>()(p);
    calloc.free(pa);
    return s;
  }

  static String _call2(
      Pointer<
              NativeFunction<
                  Pointer<Utf8> Function(Pointer<Utf8>, Pointer<Utf8>)>>
          fn,
      String a,
      String b) {
    final pa = a.toNativeUtf8();
    final pb = b.toNativeUtf8();
    final p = fn.asFunction<Pointer<Utf8> Function(Pointer<Utf8>, Pointer<Utf8>)>()(pa, pb);
    final s = p.toDartString();
    _free.asFunction<void Function(Pointer<Utf8>)>()(p);
    calloc.free(pa);
    calloc.free(pb);
    return s;
  }

  static void _throwIfError(String s) {
    if (s.startsWith('ERROR:')) throw ZyvCoreException(s.substring(7));
  }

  // ---- public API --------------------------------------------------------
  static String version() => _call(_version);

  /// Generate wallet baru. Throws [ZyvCoreException] bila gagal.
  static Wallet keygen() {
    final s = _call(_keygen);
    _throwIfError(s);
    final m = jsonDecode(s) as Map<String, dynamic>;
    return Wallet(
        address: m['address'] as String,
        privkey: m['privkey'] as String,
        pubkey: m['pubkey'] as String);
  }

  static String addressFromPubkey(String pubkeyHex) {
    final s = _call1(_addrFromPub, pubkeyHex);
    _throwIfError(s);
    return s;
  }

  static String sign(String privkeyHex, String message) {
    final s = _call2(_sign, privkeyHex, message);
    _throwIfError(s);
    return s;
  }

  /// Enkripsi E2E (ECIES) — format on-chain identik dgn node.
  static String encrypt(String recipientPubkeyHex, String plaintext) {
    final s = _call2(_encrypt, recipientPubkeyHex, plaintext);
    _throwIfError(s);
    return s;
  }

  /// Dekripsi memo on-chain dgn privkey penerima.
  static String decrypt(String privkeyHex, String encryptedMemo) {
    final s = _call2(_decrypt, privkeyHex, encryptedMemo);
    _throwIfError(s);
    return s;
  }

  /// Bangun & sign tx → serialized (kirim via POST /api/sendrawtx).
  /// [inputs]  : [{"txid":"..","vout":0}, ...]
  /// [outputs] : [{"address":"..","amount":10000}, ...] (unit terkecil)
  /// [data]    : memo on-chain (kosong/NULL utk transfer polos)
  static String buildTx({
    required List<Map<String, dynamic>> inputs,
    required List<Map<String, dynamic>> outputs,
    String data = '',
    required String privkeyHex,
  }) {
    final inJson = jsonEncode(inputs);
    final outJson = jsonEncode(outputs);
    final pIn = inJson.toNativeUtf8();
    final pOut = outJson.toNativeUtf8();
    final pData = data.toNativeUtf8();
    final pPriv = privkeyHex.toNativeUtf8();
    final p = _buildTx
        .asFunction<
            Pointer<Utf8> Function(Pointer<Utf8>, Pointer<Utf8>,
                Pointer<Utf8>, Pointer<Utf8>)>()(pIn, pOut, pData, pPriv);
    final s = p.toDartString();
    _free.asFunction<void Function(Pointer<Utf8>)>()(p);
    calloc.free(pIn);
    calloc.free(pOut);
    calloc.free(pData);
    calloc.free(pPriv);
    _throwIfError(s);
    return s;
  }
}

class ZyvCoreException implements Exception {
  final String message;
  ZyvCoreException(this.message);
  @override
  String toString() => 'ZyvCoreException: $message';
}

class Wallet {
  final String address;
  final String privkey;
  final String pubkey;
  Wallet({required this.address, required this.privkey, required this.pubkey});

  Map<String, dynamic> toJson() =>
      {'address': address, 'privkey': privkey, 'pubkey': pubkey};
}

// helper free mem dari toNativeUtf8 (calloc)
void callocFree(dynamic p) {
  // ignore: invalid_use_of_internal_member
  calloc.free(p as Pointer<NativeType>);
}
