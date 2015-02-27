#!/bin/bash
#find . -name '*.c' | xargs clang-format-3.5 -i
ctags --langmap=c:.c.h --c-kinds=+p --extra=+f --fields=+l -R .
