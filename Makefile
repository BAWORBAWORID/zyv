CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-deprecated-declarations
LDLIBS   := -lcrypto -pthread
SRC      := src/main.cpp src/sha256.cpp src/block.cpp src/blockchain.cpp \
            src/transaction.cpp src/keys.cpp src/p2p.cpp src/rpc.cpp
HDRS     := src/sha256.h src/params.h src/transaction.h src/block.h \
            src/blockchain.h src/keys.h src/utxo.h src/mempool.h
TARGET   := zyvor

.PHONY: all clean test run

all: $(TARGET)

$(TARGET): $(SRC) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDLIBS)

test: $(TARGET)
	@echo "SHA-256(\"abc\") = `./$(TARGET) hash abc`"
	@echo "(harusnya        = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad)"

run: $(TARGET)
	./$(TARGET) info

clean:
	rm -f $(TARGET) zyvor_chain.dat zyvor_mempool.dat zyvor_wallet.dat
