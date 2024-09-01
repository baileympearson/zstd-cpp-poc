

rm -rf deps
mkdir -p deps/zstd

curl -L "https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz" | tar  -zxf - -C deps/zstd --strip-components 1
cd deps/zstd/
make
