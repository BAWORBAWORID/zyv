# Zyvor Mobile (Flutter) — libzyvcore + FFI

Aplikasi chat on-chain Zyvor untuk Android/iOS. Privkey **tetap di HP**;
server/node hanya relay tx yang sudah ditandatangani.

## Struktur

```
mobile/
├── zyvcore.h          # C API library (FFI contract)
├── zyvcore.cpp        # implementasi (wrap keys.cpp/transaction.cpp/blockchain.cpp)
├── libzyvcore.so      # build host (testing) — Android build di bawah
└── flutter/
    ├── lib/
    │   ├── zyvcore.dart      # dart:ffi bindings
    │   ├── api.dart          # HTTP client ke node RPC
    │   └── chat_service.dart # logika chat (encrypt→buildTx→sendRawTx, inbox→decrypt)
    └── README.md
```

## Build library native

### Host (tes cepat, Linux)
```bash
cd mobile
g++ -std=c++17 -shared -fPIC -o libzyvcore.so zyvcore.cpp \
    ../src/keys.cpp ../src/transaction.cpp ../src/sha256.cpp \
    ../src/blockchain.cpp ../src/block.cpp -I../src -lcrypto
```

### Android (NDK)
```bash
# satu command per ABI (arm64-v8a, armeabi-v7a, x86_64)
$NDK/toolchain.../aarch64-linux-android24-clang++ -std=c++17 -shared -fPIC \
    -o libzyvcore.so zyvcore.cpp ../src/{keys,transaction,sha256,blockchain,block}.cpp \
    -I../src -lcrypto
# hasil taruh di:
#   android/app/src/main/jniLibs/arm64-v8a/libzyvcore.so  (dst per ABI)
```
OpenSSL untuk Android: build statis per ABI (android-openssl) atau pakai
conan/vcpkg; link `-lcrypto` statis supaya APK mandiri.

### iOS
Kompilasi source yang sama sebagai framework statis (Xcode "C++ Interop"):
tambahkan keys.cpp, transaction.cpp, sha256.cpp, blockchain.cpp, block.cpp,
zyvcore.cpp ke target + link libcrypto (BoringSSL/OpenSSL.xcframework).
Flutter iOS memanggil via `DynamicLibrary.process()` (lihat zyvcore.dart).

## Flutter app (langkah membuat project UI)

```bash
flutter create zyvor_chat --org my.zyvor
cd zyvor_chat
# salin lib/zyvcore.dart, lib/api.dart, lib/chat_service.dart ke lib/
# pubspec.yaml: dependencies: http: ^1.2.0
```

## Alur chat (privkey tidak pernah keluar dari HP)

Kirim:
1. `api.pubkey(to)` — ambil pubkey penerima
2. `ZyvCore.encrypt(pubTo, pesan)` — ECIES di perangkat
3. `api.address(me)` — UTXO
4. `ZyvCore.buildTx(...)` — bangun + sign di perangkat
5. `api.sendRawTx(serialized)` — broadcast (node relay)

Terima:
- polling `api.messages(me)` tiap 3–5 detik
- pesan `1|...` → `ZyvCore.decrypt(privkey)` lokal
- status `pending` → `confirmed` (blok) = centang 2

## Keamanan

- Simpan privkey di **flutter_secure_storage** (Keychain/Keystore), JANGAN
  shared_preferences.
- Node URL bisa diganti user; idealnya HTTPS (nginx+TLS di depan node) atau
  konek langsung ke node milik sendiri.
- Node tidak pernah melihat privkey maupun plaintext pesan terenkripsi.
