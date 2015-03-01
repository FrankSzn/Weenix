#!/bin/bash
find . -name '*.c' | xargs ~/ycm_temp/llvm_root_dir/bin/clang-format -i
find . -name '*.h' | xargs ~/ycm_temp/llvm_root_dir/bin/clang-format -i
ctags --langmap=c:.c.h --c-kinds=+p --extra=+f --fields=+l -R .
