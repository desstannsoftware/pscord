#!/bin/sh
# Builds the PC harnesses for the Discord core (macOS, Homebrew OpenSSL).
set -e
cd "$(dirname "$0")/.."
ossl=$(brew --prefix openssl@3)
out=build/mac
mkdir -p "$out"
flags="-std=c++20 -O1 -g -Wall -Wextra -Werror -fno-exceptions -Isrc -I$ossl/include"
cc -O2 -c src/third_party/yyjson/yyjson.c -o "$out/yyjson.o"
cc -O2 -c src/third_party/qrcodegen/qrcodegen.c -o "$out/qrcodegen.o"
core="src/dc/net/dns.cpp src/dc/net/tls.cpp src/dc/net/http.cpp src/dc/net/http_server.cpp src/dc/net/websocket.cpp src/dc/util/base64.cpp src/dc/client.cpp"
clang++ $flags $core tests-mac/dccli.cpp "$out/yyjson.o" "$out/qrcodegen.o" -L"$ossl/lib" -lssl -lcrypto -o "$out/dccli" 2>&1 | grep -v "built for newer" || true
clang++ $flags src/dc/net/dns.cpp src/dc/net/tls.cpp tests-mac/tls_smoke.cpp -L"$ossl/lib" -lssl -lcrypto -o "$out/tls_smoke" 2>&1 | grep -v "built for newer" || true
clang++ $flags $core tests-mac/weblogin.cpp "$out/yyjson.o" "$out/qrcodegen.o" -L"$ossl/lib" -lssl -lcrypto -o "$out/weblogin" 2>&1 | grep -v "built for newer" || true
ls "$out/dccli" "$out/tls_smoke" "$out/weblogin"
