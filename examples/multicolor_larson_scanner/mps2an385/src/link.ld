/* mps2-an385 memory map, confirmed via `qemu-system-arm -M mps2-an385
   -S -monitor stdio -serial none -display none` + `info mtree`: code
   region (mps.ssram1, 4MB) at 0x0, RAM (mps.ssram23, 4MB) at 0x20000000.

   This board file only sets the symbols picolibcpp.ld (shipped by the
   LLVM Embedded Toolchain for Arm, lib/clang-runtimes/arm-none-eabi/
   <variant>/lib/picolibcpp.ld) reads for its own MEMORY block - the same
   pattern the toolchain's own samples/ldscripts/microbit.ld uses.
   picolibcpp.ld owns every section below the vector table (.data/.bss/
   .tdata/.tbss/.except_ordered/.except_unordered/.got/.heap/.stack/...),
   including the EHABI unwind tables real exceptions need - see issue
   #123 Findings 7-8 for why building on picolibcpp.ld directly, rather
   than hand-rolling an equivalent linker script, is worth it: the bug
   that broke typed catch (a missing `-Wl,--target2=rel`, set in
   cmake/toolchain-mps2an385.cmake) is a link-time flag, not a section, so
   diffing a hand-rolled script's *shapes* against picolibcpp.ld's own
   never would have caught it - staying on picolibcpp.ld itself removes
   that whole class of risk going forward. */
__boot_flash = 0x00000000;
__boot_flash_size = 0x00001000; /* vector table + tiny reset stub */
__flash = 0x00001000;
__flash_size = 0x003ff000; /* remainder of the 4MB ROM region */
__ram = 0x20000000;
__ram_size = 0x00400000;
__stack_size = 0x00001000;

INCLUDE picolibcpp.ld
