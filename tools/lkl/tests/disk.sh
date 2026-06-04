#!/usr/bin/env bash

script_dir=$(cd $(dirname ${BASH_SOURCE:-$0}); pwd)

source $script_dir/test.sh
source $script_dir/fs.sh

if [ "$1" = "-t" ]; then
    shift
    fstype=$1
    shift
fi

if [ -z "$fstype" ]; then
    fstype="ext4"
fi

if [ -z $(which mkfs.$fstype) ]; then
    lkl_test_plan 0 "disk $fstype"
    echo "no mkfs.$fstype command"
    exit 0
fi

lkl_test_plan 1 "disk $fstype"
lkl_test_run 1 prepfsimg $fstype
lkl_test_exec $script_dir/disk -d $file -t $fstype $@
lkl_test_plan 1 "disk $fstype"
lkl_test_run 1 cleanfsimg

