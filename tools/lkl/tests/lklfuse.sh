#!/usr/bin/env bash

script_dir=$(cd $(dirname ${BASH_SOURCE:-$0}); pwd)

source $script_dir/test.sh

cleanup()
{
    set -e

    if type -P fusermount3 > /dev/null; then
        fusermount3 -u $dir
    else
        fusermount -u $dir
    fi
    rm $file
    rmdir $dir
}

# $1 - fstype
function prepfs()
{
    set -e

    dd if=/dev/zero of=$file bs=1048576 count=300

    yes | mkfs.$1 $file
}

# $1 - filesystem type
lklfuse_mount()
{
    ${script_dir}/../lklfuse $file $dir -o type=$1,lock=$lock_file
}

lklfuse_basic()
{
    set -e

    cd $dir
    touch a
    if ! [ -e ]; then exit 1; fi
    rm a
    mkdir a
    if ! [ -d ]; then exit 1; fi
    rmdir a
}

# $1 - filesystem type
lklfuse_stressng()
{
    set -e

    if [ -z $(which stress-ng) ]; then
        echo "missing stress-ng"
        return $TEST_SKIP
    fi

    cd $dir

    if [ "$1" = "vfat" ]; then
        exclude="chmod,filename,link,mknod,symlink,xattr"
    fi

    stress-ng --class filesystem --all 0 --timeout 10 \
	      --exclude fiemap,$exclude --fallocate-bytes 10m \
	      --sync-file-bytes 10m
}

# $1 - filesystem type
lklfuse_lock_conflict()
{
    local ret=$TEST_FAILURE unused_mnt=`mktemp -d`

    set +e
    # assume lklfuse already running with same lock file, causing lock conflict
    ${script_dir}/../lklfuse -f $file $unused_mnt -o type=$1,lock=$lock_file
    [ $? -eq 2 ] && ret=$TEST_SUCCESS
    rmdir "$unused_mnt"
    return $ret
}

if [ "$1" = "-t" ]; then
    shift
    fstype=$1
    shift
fi

if [ -z "$fstype" ]; then
    fstype="ext4"
fi

if [ "$LKL_HOST_CONFIG_FUSE" != "y" ]; then
    lkl_test_plan 0 "lklfuse.sh $fstype"
    echo "lklfuse not available"
    exit 0
fi

if ! [ -e /dev/fuse ]; then
    lkl_test_plan 0 "lklfuse.sh $fstype"
    echo "/dev/fuse not available"
    exit 0
fi

if [ -z $(which mkfs.$fstype) ]; then
    lkl_test_plan 0 "lklfuse.sh $fstype"
    echo "mkfs.$fstype not available"
    exit 0
fi

file=`mktemp`
dir=`mktemp -d`
lock_file="$file"

trap cleanup EXIT

lkl_test_plan 5 "lklfuse $fstype"

lkl_test_run 1 prepfs $fstype
lkl_test_run 2 lklfuse_mount $fstype
lkl_test_run 3 lklfuse_basic
# stress-ng returns 2 with no apparent failures so skip it for now
#lkl_test_run 4 lklfuse_stressng $fstype
lkl_test_run 4 lklfuse_lock_conflict $fstype
trap : EXIT
lkl_test_run 5 cleanup
