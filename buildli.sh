#!/bin/bash

set -e

cd ./build
rm -Rf *
cmake .. -DPICO_PLATFORM=host -DENABLE_I2S_SOUND=ON -DCMAKE_BUILD_TYPE=Debug
make -j${NPROC}

