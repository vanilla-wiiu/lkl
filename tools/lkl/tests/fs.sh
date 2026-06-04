#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

prepfsimg()
{
    set -e

    file=`mktemp disk-XXXX`

    size_mb=10
    if [ $1 == "xfs" ]; then
      size_mb=300
    elif [ $1 == "btrfs" ]; then
      size_mb=115
    fi

    dd if=/dev/zero of=$file bs=1048576 count=$size_mb

    yes | mkfs.$1 $file

    if ! [ -z $ANDROID_WDIR ]; then
        adb shell mkdir -p $ANDROID_WDIR
        adb push $file $ANDROID_WDIR
        rm $file
        file=$ANDROID_WDIR/$(basename $file)
    fi
    if ! [ -z $BSD_WDIR ]; then
        $MYSSH mkdir -p $BSD_WDIR
        $MYSCP $file $MYHOST:$BSD_WDIR
        rm $file
        file=$BSD_WDIR/$(basename $file)
    fi

    export_vars file
}

cleanfsimg()
{
    set -e

    if ! [ -z $ANDROID_WDIR ]; then
        adb shell rm -f $file
        adb shell rm -f $ANDROID_WDIR/disk
    elif ! [ -z $BSD_WDIR ]; then
        $MYSSH rm -f $file
        $MYSSH rm -f $BSD_WDIR/disk
    else
        rm -f $file
    fi
}
