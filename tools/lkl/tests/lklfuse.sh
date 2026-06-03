#!/usr/bin/env bash

script_dir=$(cd $(dirname ${BASH_SOURCE:-$0}); pwd)

source $script_dir/test.sh
source $script_dir/fs.sh

cleanup()
{
    umount_cmd=$(lkl_test_cmd 'which fusermount3 || which fusermount')
    if [ -n "$umount_cmd" ]; then
        lkl_test_cmd $umount_cmd -u $dir
    else
        lkl_test_cmd umount $dir
    fi
    cleanfsimg
    lkl_test_cmd rmdir $dir
}

# $1 - filesystem type
lklfuse_mount()
{
    dir=$(lkl_test_cmd mktemp -d mnt-XXXX)
    export_vars dir
    lkl_test_exec ${script_dir}/../lklfuse $file $dir -o type=$1,lock=$file
}

lklfuse_basic()
{
    set -e

    lkl_test_cmd touch $dir/a
    lkl_test_cmd test -e $dir/a
    lkl_test_cmd rm $dir/a
    lkl_test_cmd mkdir $dir/a
    lkl_test_cmd test -d $dir/a
    lkl_test_cmd rmdir $dir/a
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
    local ret=$TEST_FAILURE unused_mnt=$(lkl_test_cmd mktemp -d)

    set +e
    # assume lklfuse already running with same lock file, causing lock conflict
    if [ -n "$BSD_WDIR" ]; then
        lkl_test_cmd $BSD_WDIR/lklfuse -f $file $unused_mnt -o type=$1,lock=$file
    else
        lkl_test_exec ${script_dir}/../lklfuse -f $file $unused_mnt -o type=$1,lock=$file
    fi
    [ $? -eq 2 ] && ret=$TEST_SUCCESS
    lkl_test_cmd rmdir "$unused_mnt"
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

if ! QUIET=1 lkl_test_cmd test -e /dev/fuse; then
    lkl_test_plan 0 "lklfuse.sh $fstype"
    echo "/dev/fuse not available"
    exit 0
fi

if [ -z $(which mkfs.$fstype) ]; then
    lkl_test_plan 0 "lklfuse.sh $fstype"
    echo "mkfs.$fstype not available"
    exit 0
fi

trap cleanup EXIT

lkl_test_plan 5 "lklfuse $fstype"

lkl_test_run 1 prepfsimg $fstype
lkl_test_run 2 lklfuse_mount $fstype
lkl_test_run 3 lklfuse_basic
# stress-ng returns 2 with no apparent failures so skip it for now
#lkl_test_run 4 lklfuse_stressng $fstype
lkl_test_run 4 lklfuse_lock_conflict $fstype
trap : EXIT
lkl_test_run 5 cleanup
