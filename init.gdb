handle SIGSEGV nostop noprint nopass
break dbg_panic_halt
break hard_shutdown
break bootstrap
#set kernel memcheck on
continue
echo add-symbol-file user/usr/bin/hello.exec 0x08048208 \n
echo add-symbol-file user/sbin/init.exec 0x08048094 \n
echo add-symbol-file user/bin/sh.exec 0x08048094 \n
echo add-symbol-file user/usr/bin/vfstest.exec 0x08048094 \n
