#!/bin/bash

SAVE_PATH=~/Documents
PROJECT_DIR=$(basename $PWD)
TARGET_NAME=$PROJECT_DIR

pushd ../ > /dev/null
rm $PROJECT_DIR/test/{.cache,build} -rf
tar -cvzf $SAVE_PATH/$TARGET_NAME\_$(date "+%Y%m%d_%H%M%S").tar.gz $PROJECT_DIR
popd > /dev/null
