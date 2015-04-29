!/usr/bin/env bash
objdump --headers --section=".text" $1
# add-symbol-file user/sbin/init.exec 0x08048094
