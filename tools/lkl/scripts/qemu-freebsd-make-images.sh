#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

if ! [ -e FreeBSD-14.4-RELEASE-amd64-BASIC-CLOUDINIT-ufs.qcow2.xz ]; then
  wget -q https://download.freebsd.org/releases/VM-IMAGES/14.4-RELEASE/amd64/Latest/FreeBSD-14.4-RELEASE-amd64-BASIC-CLOUDINIT-ufs.qcow2.xz
fi
unxz -k -f FreeBSD-14.4-RELEASE-amd64-BASIC-CLOUDINIT-ufs.qcow2.xz

cat > cloud-freebsd.yml <<EOF
#cloud-config
ssh_pwauth: true

packages:
  - sudo
  - fusefs-libs3

users:
  - name: lkl
    groups:
      - wheel
      - dialer
    shell: /bin/sh
    lock_passwd: false
    sudo: ALL=(ALL) NOPASSWD:ALL

chpasswd:
  list: |
    lkl:lkl
  expire: false

write_files:
  - path: /boot/loader.conf
    append: true
    content: |
      fusefs_load="YES"

  - path: /etc/sysctl.conf
    append: true
    content: |
      vfs.usermount=1

  - path: /etc/rc.conf
    append: true
    content: |
      firstboot_freebsd_update_enable="NO"
      firstboot_pkgs_enable="NO"
      sysrc devfs_system_ruleset="localrules"

  - path: /etc/devfs.rules
    content: |
      [localrules=10]
      add path 'tap*' mode 0660 group dialer
      add path 'tun*' mode 0660 group dialer

runcmd:
  - kldload fusefs
  - sysctl vfs.usermount=1
EOF
cloud-localds cloud-freebsd.img cloud-freebsd.yml
rm cloud-freebsd.yml
