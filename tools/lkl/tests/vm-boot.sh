#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

script_dir=$(cd $(dirname ${BASH_SOURCE:-$0}); pwd)
source $script_dir/test.sh

function vm_boot()
{
    if [ -z "$LKL_QEMU_TEST" ]; then
        return
    fi

    for i in `seq 300`; do
        if $MYSSH exit 2> /dev/null; then
            break
        fi
        sleep 1
    done
}

lkl_test_plan 1 "vm-boot"
lkl_test_run 1 vm_boot
