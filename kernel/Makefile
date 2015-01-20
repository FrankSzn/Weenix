CFLAGS    := -ffreestanding
LDFLAGS   := -m elf_i386 -z nodefaultlib
# XXX should have --omagic?

include ../Global.mk

CFLAGS    += -D__KERNEL__

###

HEAD      := $(wildcard include/*/*.h include/*/*/*.h)
SRCDIR    := main boot util drivers/disk drivers/tty drivers mm proc fs/ramfs fs/s5fs fs vm api test test/kshell entry test/vfstest
SRC       := $(foreach dr, $(SRCDIR), $(wildcard $(dr)/*.[cS]))
OBJS      := $(addsuffix .o,$(basename $(SRC)))
SCRIPTS   := $(foreach dr, $(SRCDIR), $(wildcard $(dr)/*.gdb $(dr)/*.py))

# List of prebuilt modules that do not include the source
PREBUILT  :=

SYMBOLS   := weenix.dbg
BSYMBOLS  := symbols.dbg
KERNEL    := kernel.bin
IMAGE     := weenix.img
ISO_IMAGE := weenix.iso
GDBCOMM   := gdb-commands

.PHONY: all cscope clean

%.a:
	touch $@

all: $(SYMBOLS) $(BSYMBOLS) $(ISO_IMAGE) $(GDBCOMM)

$(KERNEL): $(OBJS) $(PREBUILT)
	@ echo "  Linking for \"kernel/$@\"..."
	@ # entry.o included from link.ld. boot/boot.S must be the first file so that the multiboot header is close enough to the front.
	@ $(LD) $(LDFLAGS) -T link.ld boot/boot.o $(filter-out boot/boot.o entry/entry.o,$^) -o $@

$(BSYMBOLS): $(SYMBOLS)
	@ echo "  Generating kernel symbols list..."
	@ readelf -Ws $(SYMBOLS) | grep -Ev 'SECTION|UND|FILE|Num:|Symbol|^$$' | awk '{printf "0x%s %s\n", $$2, $$8}' > $@

$(SYMBOLS): $(OBJS) $(PREBUILT)
	@ echo "  Generating a image for debugging..."
	@ # TODO This is bad and fragile. We really should be modifying $(KERNEL) to make it's paddr's match the vaddr's in its program headers.
	@ # entry.o included from link.ld. boot/boot.S must be the first file so that the multiboot header is close enough to the front.
	@ $(LD) $(LDFLAGS) -T debug.ld boot/boot.o $(filter-out boot/boot.o entry/entry.o,$^) -o $@

$(ISO_IMAGE): $(KERNEL)
	@ echo "  Creating \"kernel/$@\" from floppy disk image..."
	@ mkdir -p .iso/boot/grub
	@ ln -f $< .iso/boot/$< || cp -f $< .iso/boot/$<
	@ echo "default=0" > .iso/boot/grub/grub.cfg
	@ echo "timeout=0" > .iso/boot/grub/grub.cfg
	@ echo "menuentry \"$@\" {" >> .iso/boot/grub/grub.cfg
	@ echo " echo \"Booting $@ from /boot/$<\" " >> .iso/boot/grub/grub.cfg
	@ echo " echo \"Welcome To Weenix!\" " >> .iso/boot/grub/grub.cfg
	@ echo " multiboot /boot/$< " >> .iso/boot/grub/grub.cfg
	@ echo " boot " >> .iso/boot/grub/grub.cfg
	@ echo "}" >> .iso/boot/grub/grub.cfg
	@ $(MKRESCUE) -o $@ ./.iso

$(GDBCOMM): $(SCRIPTS)
	@ echo "  Creating gdb command list..."
	@ $(foreach script, $(SCRIPTS), echo $(abspath $(script)) >> $(dir $(script))$(shell basename $(dir $(script))).gdbcomm; )
	@ cat */*.gdbcomm > $@

%.o: %.c
	@ echo "  Compiling \"kernel/$<\"..."
	@ $(CC) -c $(CFLAGS) $< -o $@

%.o: %.S
	@ echo "  Compiling \"kernel/$<\"..."
	@ $(CC) -c $(ASFLAGS) $(CFLAGS) $< -o $@

cscope: $(HEAD) $(SRC)
	@ echo "  Updating cscope symbol cross-reference..."
	@ echo $(HEAD) $(SRC) > cscope.files
	@ $(CSCOPE) -k -b -q -v > /dev/null

FILTER=`echo "DRIVERS $(DRIVERS)\nVFS $(VFS)\nS5FS $(S5FS)\nVM $(VM)" | grep 1 | cut -f1 -d" " | tr "\n" "|"`PROCS
nyi:
	@ echo "  Not yet implemented:"
	@ echo
	@ find . -name \*.c -printf "%P\n" \
| xargs grep -Hn "NOT_YET_IMPLEMENTED" \
| sed -e 's/^\(.*:.*\):.*\"\(.*\): \(.*\)\".*/\2 \1 \3/' \
| grep -E "^($(FILTER))" \
| awk '{printf("%25s %30s() %8s\n", $$2, $$3, $$1)}'

clean:
	@ rm -f $(OBJS) $(SYMBOLS) $(BSYMBOLS) $(KERNEL) $(IMAGE) $(ISO_IMAGE) $(GDBCOMM) */*.gdbcomm cscope*.out cscope.files
	@ rm -rf .iso
