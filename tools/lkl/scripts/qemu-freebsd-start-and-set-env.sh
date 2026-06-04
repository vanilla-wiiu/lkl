#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

qemu-system-x86_64 -enable-kvm  -m 512 -smp 4 \
  -drive file=FreeBSD-14.4-RELEASE-amd64-BASIC-CLOUDINIT-ufs.qcow2,format=qcow2 \
  -cdrom cloud-freebsd.img \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 -device e1000,netdev=net0 \
  -display none -serial mon:telnet::5555,server,nowait -daemonize

SSH_OPTS="-q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
export MYHOST=lkl@localhost
export MYSSH="sshpass -p lkl ssh $SSH_OPTS -p 2222 $MYHOST"
export MYSCP="sshpass -p lkl scp -O $SSH_OPTS -P 2222"
