python3 fat_builder.py --size 64MiB --output image.img --init-root-dir
gcc main.c -Iinclude src/* -o bench.bin
./bench.bin 100 image.img
rm bench.bin image.img
