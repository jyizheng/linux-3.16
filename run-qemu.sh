#!/bin/bash

HDA="disk.img"
INITRD="initrd.img"
MEM=`cat /proc/meminfo | head -n 1 | awk '{print $2}'`
MEM=`expr $MEM / 1024 / 4`

while [ "$1" != "" ];
do
	case $1 in
	-hda)
		shift
		HDA="$1"
		;;
	-gdb)
		GDB="-s"
		;;
	-serial)
		[ -f console.log ] && mv console.log console-last.log
		SERIAL_APPEND="console=\"ttyS0,115200\""
		SERIAL="-serial file:console.log"
		;;
	-selinux)
		SECURITY_APPEND="selinux=1 security=selinux"
		INITRD="initrd.img-selinux"
		;;
	-noselinux)
		SECURITY_APPEND="selinux=0"
		;;
	-apparmor)
		SECURITY_APPEND="apparmor=1 security=apparmor"
		;;
	esac
	shift
done

CONSOLE="console=tty1 highres=off $SERIAL_APPEND"
ROOT="root=/dev/hda rw --no-log"

set -x
 
qemu-system-x86_64 $GDB -m ${MEM}M -smp 4 -hda $HDA -kernel arch/x86/boot/bzImage \
	-initrd $INITRD \
	-append "$CONSOLE $ROOT $SECURITY_APPEND" \
	-curses -snapshot $SERIAL
