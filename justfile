make:
    mkdir -p ./build
    cd ./build/ ; cmake ..
    cd ./build/ ; make

make_debug:
    mkdir -p ./build
    cd ./build/ ; cmake -DCMAKE_BUILD_TYPE=Debug ..
    cd ./build/ ; make

clean:
    rm -r ./build/*j