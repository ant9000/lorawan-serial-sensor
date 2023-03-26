#!/bin/bash

cd $(realpath $(dirname $0))
config=
if ! [ -z "$1" ]; then
  if ! [ -f boards.config ]; then
    echo "File boards.config is missing."
    echo "Copy boards.config.sample to boards.config and edit as needed"
    exit 1
  fi
  board=$(egrep "^${1}=" boards.config)
  if [ -z "$board" ]; then
    echo "Board $1 not found."
    exit 1
  fi
  config=$(echo $board|cut -f2- -d=)
fi
env $config make -j flash
