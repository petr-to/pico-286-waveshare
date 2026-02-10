#!/bin/bash

set -e

VM_KBS="1024"

cd ./build
rm -Rf *
cmake .. -DZERO2=ON -DENABLE_I2S_SOUND=ON
make -j${NPROC}

