package build

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
)

const (
	// KernelSectors / RamdiskSectors must match boot/stage1.S and the
	// kernel layout.  The kernel image on disk only holds .text/.rodata/
	// .data; .bss is zeroed by kernel/entry.S.
	KernelSectors = 180 // 90 KiB, loaded at 0x10000
	BootSectors   = 1

	// The littlefs partition starts right after the kernel image.  This
	// LBA is what the kernel's ATA driver mounts; keep it in sync with
	// LFS_PARTITION_LBA in kernel/ramdisk.h.
	LFSPartitionLBA = BootSectors + KernelSectors // = 181

	// Sanity bound: the kernel (image + .bss, including the heap arena) must
	// end below 0x400000.  There is no frame allocator any more, so this is
	// just a guard against .bss growing into the region reserved for boards
	// that may map RAM differently.
	KernelMaxEnd = 0x400000
)

// Target describes which architecture/board to build for.
type Target struct {
	Name    string // "x86", "rv32" or "esp32c3"
	Arch    string // arch subdirectory
	Board   string // board subdirectory
	Triple  string // clang target triple
	March   string // -march value ("" = whatever the compiler defaults to)
	Machine string // zig -mcpu value ("" = default)

	// How the filesystem reaches the kernel:
	//   disk      PC: written into the boot disk after the kernel
	//   embedded  rv32: linked into the kernel image as a RAM disk
	//   flash     esp32c3: burned to SPI flash, read through the data window
	FSBacking string // "disk", "embedded" or "flash"

	// RamdiskSectors is the number of 512-byte sectors in the littlefs
	// image.  It must equal the board's PLAT_LFS_BLOCKS * 8 (the kernel
	// mounts PLAT_LFS_BLOCKS blocks of FS_BLOCK_SIZE each).  Per-target so
	// the C3 can grow its 4 MiB flash partition past the other boards'
	// 300 KiB.
	RamdiskSectors int

	// HeapBytes is the size of the kernel heap arena (kernel/heap.c), passed
	// in as TOS_HEAP_BYTES.  0 keeps heap.c's default of 32 KiB, which is
	// what every target now uses: the C3 briefly asked for 64 KiB to soak up
	// the ~50 KiB that was free between the end of .bss and the first task
	// window, but that put the kernel at 175.8 KiB of its 176 KiB budget -
	// 240 bytes of headroom - while the measured peak never went past
	// 11.6 KiB (writing 3 MiB, littlefs working set included).  The option
	// stays for a board that genuinely needs a bigger arena; the C3 is not
	// one, and its 32 KiB of headroom is worth more than an idle arena.
	HeapBytes int

	// XipFlashBase is where the kernel's code lives in flash when the board
	// executes it in place instead of copying it into RAM.  0 means "copy
	// everything to RAM", which is what every board but the ESP32-C3 does.
	// The linker puts .text/.rodata at the IROM window base and the packer
	// writes those bytes at this offset; the boot stub maps the window to it.
	XipFlashBase uint32

	// MaxProcs is the size of the process table (kernel/process.c), passed
	// in as TOS_MAX_PROCS.  0 means "use the default of 16".  It exists
	// because a proc_t is 792 bytes: a board with 12 task windows gains
	// 1.6 KiB by not sizing the table for tasks it can never run.
	MaxProcs int

	// CPUs is how many harts this target is built for.  0 and 1 mean the
	// same thing - a uniprocessor, with no TOS_MAX_CPUS define at all, so
	// none of the SMP code (second trap vector, second kernel stack, per-CPU
	// run queue) is even assembled.  2 or more defines TOS_MAX_CPUS, which
	// start.S, kernel/cpu.c and kernel/process.c all read, and asks QEMU to
	// start that many harts.
	CPUs int
}

var Targets = map[string]Target{
	// The kernel heap has to hold one decompressed program image at a
	// time: exec() unpacks the program into a kernel buffer before it
	// copies it into the task window.  The default 32 KiB is enough for
	// the small tools, but not for the Python interpreter, whose image is
	// tens of kilobytes.  A board that can spare the room says so here.
	"x86":  {Name: "x86", Arch: "x86", Board: "pc", Triple: "x86-freestanding", FSBacking: "disk", RamdiskSectors: 600, HeapBytes: 256 * 1024},
	"rv32": {Name: "rv32", Arch: "rv32", Board: "qemu-virt", Triple: "riscv32-unknown-elf", March: "rv32imc", FSBacking: "embedded", RamdiskSectors: 600, HeapBytes: 256 * 1024},
	// The shipped littlefs image is small (600 sectors = 300 KiB, same as
	// every other target): the kernel mounts it with block_count=0 (take
	// the size from the image superblock) and calls lfs_fs_grow to stretch
	// it over the real partition, PLAT_LFS_BLOCKS = 896 blocks = 3.5 MiB.
	// That is what keeps the flashed artifact at 300 KiB while the
	// filesystem still gets the whole 4 MiB part.
	// The heap has to hold one decompressed program image while it is
	// being copied into the task window, so it has to be bigger than the
	// biggest program: the Python interpreter is 31 KiB raw.  With XIP the
	// kernel window is down to 128 KiB and the task area is up to 15
	// windows, so the 56 KiB is affordable; checkLayout() fails the build
	// if it ever stops fitting.
	//
	// 14 process slots: the board's task area gives 12 x 16 KiB windows, so
	// a 16-entry table (792 bytes each) carried two slots that could never
	// run.  Twelve live tasks plus two slots for children that exited and
	// are waiting to be reaped is the real ceiling.  XipFlashBase is what
	// selects board/esp32c3/kernel-xip.ld; set it to 0 for the all-RAM
	// layout.
	"esp32c3": {Name: "esp32c3", Arch: "rv32", Board: "esp32c3", Triple: "riscv32-unknown-elf", March: "rv32imc", FSBacking: "flash", RamdiskSectors: 600, HeapBytes: 56 * 1024, MaxProcs: 14, XipFlashBase: 0x40000},
	"arm":     {Name: "arm", Arch: "arm", Board: "qemu-mps2", Triple: "thumbv7m-none-eabi", Machine: "cortex-m3", FSBacking: "embedded", RamdiskSectors: 600, HeapBytes: 128 * 1024},
	"rp2040":  {Name: "rp2040", Arch: "arm", Board: "rp2040", Triple: "armv6m-none-eabi", Machine: "cortex-m0plus", FSBacking: "flash", RamdiskSectors: 600},
// RP2350: ARMv8-M Mainline (Cortex-M33, Secure state).  Its PMSAv8 MPU is
// not the ARMv7-M one, so the port builds with TOS_ARM_MPU_V8 and
// arch/arm/platform.c programs PRBAR/PRLAR instead of RBAR/RASR.
// The M33 user programs are bigger than the RV32 ones: bin/sh is a
// 19,884-byte ELF that compresses to 15,632 bytes.  exec() holds the
// compressed buffer (ramdisk_find) AND the decompression buffer at the
// same time, so the 32 KiB default arena fails right at the first shell;
// the biggest program (py) needs ~49 KiB.  60 KiB (was 64) still leaves
// that with an 11 KiB margin and keeps .bss below PLAT_KERNEL_MAX
// (0x20020000) - the panel's DMA row buffer (3.8 KiB in lcd_st7789.c)
// had to come out of the same 128 KiB budget.
	"rp2350": {Name: "rp2350", Arch: "arm", Board: "rp2350", Triple: "thumbv8m.main-none-eabi", Machine: "cortex-m33", FSBacking: "flash", RamdiskSectors: 600, HeapBytes: 60 * 1024},

	// rp2350smp: the same board with both Cortex-M33 cores.  Core 1 is not
	// running at reset (the boot ROM leaves it in a wait-for-FIFO loop), so
	// the board launches it from cpu_init() - that is what makes this a
	// separate target rather than a runtime switch.  The per-core kernel
	// stack drops to 16 KiB (KSTACK_BYTES in kernel/cpu.h is per arch), so
	// two cores still reserve the 32 KiB the single-core image did and the
	// 60 KiB exec arena is untouched.
	"rp2350smp": {Name: "rp2350smp", Arch: "arm", Board: "rp2350", Triple: "thumbv8m.main-none-eabi", Machine: "cortex-m33", FSBacking: "flash", RamdiskSectors: 600, CPUs: 2, HeapBytes: 60 * 1024},

	// The SMP build: the same qemu-virt board, two harts, and the only
	// target that adds the A extension.  Atomic memory operations are what
	// the spin lock is made of (arch/rv32/platform.c), and the plain rv32
	// target keeps rv32imc so the ESP32-C3's ISA stays the reference.
	"rv32smp": {Name: "rv32smp", Arch: "rv32", Board: "qemu-virt", Triple: "riscv32-unknown-elf", March: "rv32imac", FSBacking: "embedded", RamdiskSectors: 600, CPUs: 2, HeapBytes: 256 * 1024},

	// rv32nc = rv32im, no C extension: the same machine as rv32 without
	// compressed instructions.  Exists so the cost/benefit of RVC can be
	// measured per release (see docs/SIZE-NOTES.md if present); the ELF
	// links identically, only the encoding differs.
	"rv32nc": {Name: "rv32nc", Arch: "rv32", Board: "qemu-virt", Triple: "riscv32-unknown-elf", March: "rv32im", FSBacking: "embedded", RamdiskSectors: 600, HeapBytes: 256 * 1024},
}

// Builder compiles and links everything and writes build/tinyos.img.
type Builder struct {
	Root     string
	BuildDir string
	TC       Toolchain
	QEMU     string
	Verbose  bool
	Target   Target

	mu sync.Mutex
}

func FindRoot() (string, error) {
	dir, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "kernel", "kernel.ld")); err == nil {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", fmt.Errorf("cannot find the TinyOS source tree (run from inside the repository)")
		}
		dir = parent
	}
}

func New(root string) *Builder {
	return &Builder{
		Root:     root,
		BuildDir: filepath.Join(root, "build"),
	}
}

func (b *Builder) logf(format string, args ...interface{}) {
	b.mu.Lock()
	defer b.mu.Unlock()
	fmt.Printf(format, args...)
}

func (b *Builder) runTool(name string, args []string, label string) error {
	cmd := exec.Command(name, args...)
	cmd.Dir = b.Root
	var out strings.Builder
	cmd.Stdout = &out
	cmd.Stderr = &out
	if b.Verbose {
		b.logf("  %s %s\n", label, strings.Join(args, " "))
	}
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("%s failed: %v\n%s", label, err, out.String())
	}
	return nil
}

func (b *Builder) ccFlags(includes []string, extra ...string) []string {
	opt := os.Getenv("TOS_OPT")
	if opt == "" {
		opt = "-O2"
	}

	// Architecture specific flags.  Everything else is shared so both ports
	// see the same C dialect, optimiser and warning set.
	var archFlags []string
	if b.Target.Arch == "rv32" {
		// ilp32 = soft float, matching every RV32 MCU we target and keeping
		// the kernel free of compiler_rt's hard-float helpers.
		archFlags = []string{"-mabi=ilp32", "-mcmodel=medany"}
		if b.Target.March != "" {
			archFlags = append(archFlags, "-march="+b.Target.March)
		}
		if b.Target.Machine != "" {
			archFlags = append(archFlags, "-mcpu="+b.Target.Machine)
		}
	} else if b.Target.Arch == "arm" {
		// Cortex-M is Thumb only; soft-float keeps the kernel free of FPU
		// state save code and matches the reference M3 core.
		archFlags = []string{"-mthumb", "-mfloat-abi=soft"}
		if b.Target.Machine != "" {
			archFlags = append(archFlags, "-mcpu="+b.Target.Machine)
		}
		// RP2040 is a Cortex-M0+ (ARMv6-M): no MPU, no BASEPRI, no
		// ldrex/strex, no CFSR/MMFAR/BFAR.  arch/arm isolates the M0+
		// specific bits behind this macro so the M3 build is unaffected.
		if b.Target.Board == "rp2040" {
			archFlags = append(archFlags, "-DTOS_ARM_M0P")
		}
		// Cortex-M33 has an MPU, but it is ARMv8-M PMSAv8 (PRBAR/PRLAR plus
		// MAIR), not the ARMv7-M one: same region model on top, different
		// registers underneath, so arch/arm/platform.c compiles its other
		// writer.  Fault reporting (CFSR/MMFAR/BFAR) stays on either way -
		// only TOS_ARM_M0P suppresses that.
		if b.Target.Board == "rp2350" {
			archFlags = append(archFlags, "-DTOS_ARM_MPU_V8")
			// Bring-up aid: a 16-entry ring of the last syscall numbers,
			// dumped by arm_handle_fault().  Only the new M33 port pays for
			// it; remove once the shell is stable on real hardware.
			archFlags = append(archFlags, "-DTOS_SYSCALL_TRACE=1")
		}
	} else {
		archFlags = []string{
			"-march=i686",
			"-mno-mmx",
			"-mno-sse",
			"-mno-sse2",
			"-mno-80387",
		}
	}

	// Kernel heap arena size: .bss on every board, so the board decides how
	// much of its SRAM the allocator may claim.
	if b.Target.HeapBytes > 0 {
		archFlags = append(archFlags,
			fmt.Sprintf("-DTOS_HEAP_BYTES=%d", b.Target.HeapBytes))
	}

	// XIP: the boot stub needs the flash offset to map the IROM window to.
	if b.Target.XipFlashBase != 0 {
		archFlags = append(archFlags,
			fmt.Sprintf("-DTOS_XIP_FLASH_BASE=0x%x", b.Target.XipFlashBase))
	}

	// Process table size (kernel/process.c).  Left undefined on boards that
	// can run 16 or more tasks, so they keep tty-independent defaults.
	if b.Target.MaxProcs > 0 {
		archFlags = append(archFlags,
			fmt.Sprintf("-DTOS_MAX_PROCS=%d", b.Target.MaxProcs))
	}

	// How many harts.  Left undefined for a uniprocessor so that every
	// `#if TOS_MAX_CPUS > 1` in the tree (start.S, arch/rv32/platform.c)
	// simply is not compiled - the single-core ports never see SMP code at
	// all, and the Makefile-free build has exactly one place to say "this
	// target is multiprocessor".
	if b.Target.CPUs > 1 {
		archFlags = append(archFlags,
			fmt.Sprintf("-DTOS_MAX_CPUS=%d", b.Target.CPUs))
	}

	flags := append([]string{}, archFlags...)
	flags = append(flags,
		"-ffreestanding",
		"-fno-builtin",
		"-fno-stack-protector",
		"-fno-pic",
		"-fno-pie",
		"-fno-asynchronous-unwind-tables",
		"-fno-unwind-tables",
		// one section per function/data item so the linker can drop the
		// parts of lib.c a program never calls (each user binary statically
		// links the whole tiny libc otherwise)
		"-ffunction-sections",
		"-fdata-sections",
		"-nostdinc",
		// Size used to be the driver, but clang's size levels -Oz / -Os
		// produce RV32 code that QEMU's TCG re-translates on every loop
		// iteration (each instruction becomes its own TB): the whole kernel
		// then runs ~100x slower and the smoke test never sees a shell
		// prompt.  -O2 is fast under TCG and stays small (measured with
		// clang 23.1.4 / lld 23.1.4, rv32 kernel, Oct 2026):
		//
		//                     -Oz      -O1      -O2
		//   rv32 .text     27,040   36,086   31,654 bytes
		//   QEMU boot       slow      fast     fast
		//
		// Use TOS_OPT=-Oz when building for real hardware (it is fine there)
		// or -Og when debugging.
		opt,
		"-Wall",
		"-Wextra",
		"-Wno-unused-parameter",
		"-Wno-unused-function",
	)
	for _, inc := range includes {
		flags = append(flags, "-I", inc)
	}
	flags = append(flags, extra...)
	return flags
}

func (b *Builder) compile(src, obj string, includes []string, extra ...string) error {
	args := append([]string{}, b.TC.CC[1:]...)
	args = append(args, b.ccFlags(includes)...)
	args = append(args, extra...)
	args = append(args, "-c", src, "-o", obj)
	if err := b.runTool(b.TC.CC[0], args, "CC"); err != nil {
		return err
	}
	if b.Verbose {
		b.logf("  ok  %s\n", relName(b.Root, src))
	}
	return nil
}

// linkViaDriver reports whether the link tool is a compiler driver (clang,
// gcc, zig cc) that needs -Wl, to forward flags to the real linker, rather
// than a bare linker that takes them directly.
func linkViaDriver(ld []string) bool {
	base := strings.ToLower(filepath.Base(ld[0]))
	base = strings.TrimSuffix(base, ".exe")
	return base == "zig" || base == "gcc" || base == "cc" ||
		strings.HasPrefix(base, "clang") || strings.HasSuffix(base, "-gcc")
}

func (b *Builder) link(objs []string, script, out string, extra ...string) error {
	var args []string
	var tool string

	// ld.lld takes -m and the -T/--gc-sections style flags directly; a
	// compiler driver needs them wrapped in -Wl, and would add its own
	// runtime libraries.  zig is driven as "zig ld.lld" (a driver, but one
	// that then accepts raw linker flags).
	driver := false
	switch {
	case b.TC.UsesZig:
		tool = b.TC.CC[0]
		args = []string{"ld.lld", "-m", "elf32lriscv"}
	case !linkViaDriver(b.TC.LD):
		// a bare ld.lld takes linker flags directly
		tool = b.TC.LD[0]
		args = append([]string{}, b.TC.LD[1:]...) // "-m", "elf32lriscv"
	default:
		tool = b.TC.LD[0]
		args = append([]string{}, b.TC.LD[1:]...)
		driver = true
	}

	w := func(flag string) string {
		if driver {
			return "-Wl," + flag
		}
		return flag
	}

	args = append(args, "-T", script, "-nostdlib", w("-static"))
	// Drop unreferenced sections.  Everything is compiled with
	// -ffunction-sections/-fdata-sections, so this prunes e.g. the
	// littlefs entry points the kernel never calls (lfs_rename & co),
	// exactly like the user program links already do.
	args = append(args, w("--gc-sections"))
	args = append(args, extra...)
	// no symbol table in the final image: it is only needed for debugging,
	// and `strip` would not touch the loadable segments anyway
	if os.Getenv("TOS_KEEP_SYMS") == "" {
		args = append(args, w("-s"))
	}
	args = append(args, "-o", out)
	args = append(args, objs...)
	return b.runTool(tool, args, "LD")
}

// ldLldPath finds the ld.lld bundled with a zig installation.
func ldLldPath(zig string) string {
	// <zig dir>/zig.exe sits next to ld.lld on Windows zig distributions;
	// otherwise fall back to searching PATH.
	dir := filepath.Dir(zig)
	for _, name := range []string{"ld.lld.exe", "ld.lld"} {
		p := filepath.Join(dir, name)
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			return p
		}
	}
	if p := findTool("ld.lld"); p != "" {
		return p
	}
	return zig // last resort: let zig report the error
}

// linkPIE links a user program as a position independent executable (ET_DYN).
// The kernel relocates it to the board's APP_BASE at load time, so the same
// binary works on every board regardless of where its RAM sits.
func (b *Builder) linkPIE(objs []string, script, out string) error {
	var tool string
	var args []string
	driver := false

	switch {
	case b.TC.UsesZig:
		// zig cc parses -Wl arguments itself and rejects -pie, while a
		// plain clang driver only forwards -pie through the -Wl form.
		tool = b.TC.LD[0]
		args = append([]string{}, b.TC.LD[1:]...)
		args = append(args, "-pie")
	case linkViaDriver(b.TC.LD):
		tool = b.TC.LD[0]
		args = append([]string{}, b.TC.LD[1:]...)
		driver = true
	default:
		tool = b.TC.LD[0]
		args = append([]string{}, b.TC.LD[1:]...)
	}

	w := func(flag string) string {
		if driver {
			return "-Wl," + flag
		}
		return flag
	}

	args = append(args, "-T", script, "-nostdlib", w("-pie"))
	args = append(args,
		w("--gc-sections"),
		w("-z"), w("max-page-size=16"),
		w("-z"), w("now"),
		w("-s"),
		"-o", out,
	)
	args = append(args, objs...)
	return b.runTool(tool, args, "LD")
}

func globFiles(dir, pattern string) ([]string, error) {
	m, err := filepath.Glob(filepath.Join(dir, pattern))
	if err != nil {
		return nil, err
	}
	sort.Strings(m)
	return m, nil
}

type task struct {
	label string
	fn    func() error
}

func (b *Builder) runTasks(tasks []task) error {
	sem := make(chan struct{}, max(1, runtime.NumCPU()))
	var wg sync.WaitGroup
	errs := make([]error, len(tasks))

	for i := range tasks {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			sem <- struct{}{}
			defer func() { <-sem }()
			errs[i] = tasks[i].fn()
		}(i)
	}
	wg.Wait()

	for _, err := range errs {
		if err != nil {
			return err
		}
	}
	return nil
}

func (b *Builder) Build() error {
	var err error
	if b.Target.Name == "" {
		b.Target = Targets["x86"]
	}
	b.TC, err = DetectToolchain(b.Target)
	if err != nil {
		return err
	}
	if err := b.checkLayout(); err != nil {
		return err
	}

	for _, d := range []string{
		b.BuildDir,
		filepath.Join(b.BuildDir, "kernel"),
		filepath.Join(b.BuildDir, "boot"),
		filepath.Join(b.BuildDir, "user"),
		filepath.Join(b.BuildDir, "user", "bin"),
	} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return err
		}
	}

	// The filesystem image has to exist before the kernel is linked on
	// targets that embed it (rv32), so user programs and mklfs come first.
	// The esp32c3 target also builds the image - it gets burned to flash
	// next to the kernel - it just is not linked into the kernel.
	if b.Target.Name != "x86" {
		bins, err := b.buildUser()
		if err != nil {
			return err
		}
		if err := b.buildMklfs(); err != nil {
			return err
		}
		if b.Target.FSBacking != "disk" {
			if err := b.buildFSImage(bins); err != nil {
				return err
			}
		}
		if err := b.buildKernel(); err != nil {
			return err
		}
		b.logf("      %s\n", filepath.Join("build", "kernel-"+b.Target.Name+".elf"))
		return nil
	}

	if err := b.buildBoot(); err != nil {
		return err
	}
	if err := b.buildKernel(); err != nil {
		return err
	}
	bins, err := b.buildUser()
	if err != nil {
		return err
	}
	if err := b.buildMklfs(); err != nil {
		return err
	}
	if err := b.buildFSImage(bins); err != nil {
		return err
	}
	if err := b.buildDiskImage(); err != nil {
		return err
	}
	return nil
}

func (b *Builder) buildBoot() error {
	b.logf("boot sector\n")
	src := filepath.Join(b.Root, "boot", "stage1.S")
	obj := filepath.Join(b.BuildDir, "boot", "stage1.o")
	elfPath := filepath.Join(b.BuildDir, "boot", "stage1.elf")

	if err := b.compile(src, obj, []string{filepath.Join(b.Root, "include")}); err != nil {
		return err
	}
	if err := b.link([]string{obj}, filepath.Join(b.Root, "boot", "stage1.ld"), elfPath); err != nil {
		return err
	}

	flat, base, err := ELFToFlat(elfPath)
	if err != nil {
		return err
	}
	// The linker may leave the first segment page-aligned (e.g. 0x7000) and
	// pad the file so that the code still ends up at 0x7C00.
	if base != 0x7C00 {
		if base > 0x7C00 || 0x7C00-base > uint64(len(flat)) {
			return fmt.Errorf("boot sector linked at 0x%x, expected 0x7C00", base)
		}
		flat = flat[0x7C00-base:]
	}
	if len(flat) > 512 {
		return fmt.Errorf("boot sector is %d bytes, does not fit in 512", len(flat))
	}
	flat = padTo(flat, 512)
	flat = flat[:512]
	flat[510] = 0x55
	flat[511] = 0xAA

	if err := os.WriteFile(filepath.Join(b.BuildDir, "boot", "stage1.bin"), flat, 0o644); err != nil {
		return err
	}
	b.logf("      %d bytes\n", len(flat))
	return nil
}

func (b *Builder) buildKernel() error {
	b.logf("kernel\n")
	kdir := filepath.Join(b.Root, "kernel")
	lfsDir := filepath.Join(b.Root, "third_party", "littlefs")
	// arch/<arch> is on the include path so platform.h can pull the port's
	// register frame (arch/<arch>/regs.h).
	incs := []string{
		filepath.Join(b.Root, "include"),
		kdir,
		lfsDir,
		filepath.Join(b.Root, "arch", b.Target.Arch),
	}

	// The RP2350 console is a TinyUSB CDC-ACM device (board/rp2350/usb_cdc.c).
	// third_party/tinyusb holds a trimmed upstream tree - device stack, CDC
	// class, RP2040 controller port - and port_rp2/ is the pico-sdk facade
	// that lets those upstream files compile unchanged.  The board directory
	// joins the include path so tusb_config.h is found by name.
	tusbDir := filepath.Join(b.Root, "third_party", "tinyusb")
	useTinyUSB := b.Target.Board == "rp2350"
	if useTinyUSB {
		incs = append(incs,
			filepath.Join(b.Root, "board", b.Target.Board),
			filepath.Join(tusbDir, "src"),
			filepath.Join(tusbDir, "port_rp2", "include"),
		)
	}

	// Sources come from three places: the arch-independent core, the
	// architecture port, and the board support package.  Which arch and
	// board directory is used depends on the build target.
	var srcs []string
	dirs := []string{
		kdir,
		filepath.Join(b.Root, "arch", b.Target.Arch),
		filepath.Join(b.Root, "board", b.Target.Board),
	}
	for _, dir := range dirs {
		cs, err := globFiles(dir, "*.c")
		if err != nil {
			return err
		}
		ss, err := globFiles(dir, "*.S")
		if err != nil {
			return err
		}
		srcs = append(srcs, ss...)
		srcs = append(srcs, cs...)
	}

	// Vendored TinyUSB, RP2350 only: the seven translation units that make up
	// a CDC-ACM device.  Everything else in upstream's tree (host stack, the
	// other device classes, other chips' ports) is not checked in.
	if useTinyUSB {
		for _, rel := range []string{
			"src/tusb.c",
			"src/common/tusb_fifo.c",
			"src/device/usbd.c",
			"src/device/usbd_control.c",
			"src/class/cdc/cdc_device.c",
			"src/portable/raspberrypi/rp2040/dcd_rp2040.c",
			"src/portable/raspberrypi/rp2040/rp2040_usb.c",
		} {
			srcs = append(srcs, filepath.Join(tusbDir, filepath.FromSlash(rel)))
		}
	}

	var objs []string
	var tasks []task
	for _, src := range srcs {
		// Object names carry the directory (rv32_platform.o,
		// qemu-virt_platform.o): several source trees legitimately have
		// same-named files (arch and every board have a platform.c), and
		// a bare basename would make them overwrite each other.
		rel, err := filepath.Rel(b.Root, src)
		if err != nil {
			rel = filepath.Base(src)
		}
		name := filepath.Base(filepath.Dir(rel)) + "_" +
			strings.TrimSuffix(filepath.Base(src), filepath.Ext(src)) + ".o"
		obj := filepath.Join(b.BuildDir, "kernel", name)
		objs = append(objs, obj)
		srcCopy, objCopy := src, obj
		tasks = append(tasks, task{label: name, fn: func() error {
			b.logf("      CC  kernel/%s\n", filepath.Base(srcCopy))
			return b.compile(srcCopy, objCopy, incs)
		}})
	}

	/* vendored littlefs, with the kernel config force-included */
	lfsCfg := filepath.Join(kdir, "lfs_config.h")
	for _, src := range []string{"lfs.c", "lfs_util.c"} {
		full := filepath.Join(lfsDir, src)
		obj := filepath.Join(b.BuildDir, "kernel", "lfs_"+strings.TrimSuffix(src, ".c")+".o")
		objs = append(objs, obj)
		srcCopy, objCopy := src, obj
		tasks = append(tasks, task{label: src, fn: func() error {
			b.logf("      CC  third_party/littlefs/%s\n", srcCopy)
			return b.compile(full, objCopy, incs,
				"-include", lfsCfg)
		}})
	}

	// Targets without a disk carry the filesystem inside the kernel image:
	// a generated assembly file .incbin's build/ramdisk.bin into a writable
	// section, which plat_block_* then serves as a RAM disk.  The esp32c3
	// target reads the partition from flash instead (board/esp32c3).
	if b.Target.FSBacking == "embedded" {
		blob, err := b.writeRamdiskBlob()
		if err != nil {
			return err
		}
		obj := filepath.Join(b.BuildDir, "kernel", "ramdisk_blob.o")
		objs = append(objs, obj)
		tasks = append(tasks, task{label: "ramdisk_blob", fn: func() error {
			b.logf("      CC  kernel/ramdisk_blob.S\n")
			return b.compile(blob, obj, nil)
		}})
	}

	if err := b.runTasks(tasks); err != nil {
		return err
	}

	// RV32 has no boot sector and no flat binary: QEMU and the board
	// bootloader both load the ELF directly.  A board may supply its own
	// linker script (different RAM base); the arch default is the fallback.
	if b.Target.Name != "x86" {
		// A board may supply its own linker script (different RAM base);
		// the arch default is the fallback.
		elfPath := filepath.Join(b.BuildDir, "kernel-"+b.Target.Name+".elf")
		script := filepath.Join(b.Root, "arch", b.Target.Arch, "kernel.ld")
		boardScript := filepath.Join(b.Root, "board", b.Target.Board, "kernel.ld")
		if _, err := os.Stat(boardScript); err == nil {
			script = boardScript
		}
		// A board that executes code in place needs a different layout: the
		// code half links at the icache window instead of at the RAM base.
		// Keeping it in a separate script means turning XIP off is one
		// target field, not an edit to the board's normal layout.
		if b.Target.XipFlashBase != 0 {
			xip := filepath.Join(b.Root, "board", b.Target.Board, "kernel-xip.ld")
			if _, err := os.Stat(xip); err != nil {
				return fmt.Errorf("target %s sets XipFlashBase but %s is missing",
					b.Target.Name, xip)
			}
			script = xip
		}
		if err := b.link(objs, script, elfPath); err != nil {
			return err
		}
		if end, err := ELFEndAddress(elfPath); err == nil {
			// With XIP the highest segment is the code half in the flash
			// window; the number that matters for headroom is the RAM one.
			if b.Target.XipFlashBase != 0 {
				if ram, err2 := ELFEndBelow(elfPath, espXipVaddrLow); err2 == nil {
					b.logf("      kernel RAM ends at 0x%x, code at 0x%x (flash 0x%x)\n",
						ram, end, b.Target.XipFlashBase)
				}
			} else {
				b.logf("      kernel ends at 0x%x\n", end)
			}
		}

		// Flash-backed targets need a board-specific image format.
		if b.Target.FSBacking == "flash" {
			switch b.Target.Board {
			case "esp32c3":
				img, segs, err := ESPImageFromELF(elfPath, true, b.Target.XipFlashBase)
				if err != nil {
					return err
				}
				out := filepath.Join(b.BuildDir, "esp32c3.bin")
				if err := os.WriteFile(out, img, 0o644); err != nil {
					return err
				}
				b.logf("      %s (%d bytes, %d segment(s))\n", relName(b.Root, out), len(img), len(segs))
				b.logf("      burn: esptool write-flash 0x0 %s | fs: write-flash 0x80000 %s\n",
					relName(b.Root, out), relName(b.Root, b.fsImage()))
			case "rp2040":
				if err := b.emitRP2040UF2(elfPath); err != nil {
					return err
				}
			case "rp2350":
				if err := b.emitRP2350UF2(elfPath); err != nil {
					return err
				}
			}
		}
		return nil
	}

	elfPath := filepath.Join(b.BuildDir, "kernel.elf")
	if err := b.link(objs, filepath.Join(kdir, "kernel.ld"), elfPath); err != nil {
		return err
	}

	flat, base, err := ELFToFlat(elfPath)
	if err != nil {
		return err
	}
	if base != 0x10000 {
		return fmt.Errorf("kernel linked at 0x%x, expected 0x10000", base)
	}
	maxBytes := KernelSectors * 512
	if len(flat) > maxBytes {
		return fmt.Errorf("kernel image is %d bytes, the boot loader only loads %d (raise KERNEL_SECTORS in boot/stage1.S and KernelSectors here, then move the littlefs LBA)",
			len(flat), maxBytes)
	}
	// .bss is not part of the image, but it still occupies RAM at the end of
	// the kernel, so it has to be counted against the 0x400000 bound too.
	end, err := ELFEndAddress(elfPath)
	if err != nil {
		return err
	}
	if end > KernelMaxEnd {
		return fmt.Errorf("kernel ends at 0x%x (.bss included), which exceeds the 0x%x bound",
			end, KernelMaxEnd)
	}
	b.logf("      image %d / %d bytes, footprint ends at 0x%x (limit 0x%x, %d KiB headroom)\n",
		len(flat), maxBytes, end, KernelMaxEnd, (KernelMaxEnd-end)/1024)
	return os.WriteFile(filepath.Join(b.BuildDir, "kernel.bin"), flat, 0o644)
}

// fsImage is the littlefs image this target boots from.
//
// The name carries the target because every target packs its own user
// programs into it, and a single shared name made the last build win:
// build rv32 then rp2040 and build/ramdisk.bin holds ARM programs, so
// flashing an esp32c3 kernel beside it gives a board that lists 14 files and
// then rejects every one of them ("not a valid ELF32 program") with nothing
// on screen to say the filesystem is somebody else's.
func (b *Builder) fsImage() string {
	name := "ramdisk.bin"
	if b.Target.Name != "" {
		name = "ramdisk-" + b.Target.Name + ".bin"
	}
	return filepath.Join(b.BuildDir, name)
}

// writeRamdiskBlob emits the tiny assembly stub that links the littlefs
// image into the kernel as a writable buffer.  It is regenerated on every
// build because the image contents change with the user programs.
func (b *Builder) writeRamdiskBlob() (string, error) {
	img := relName(b.Root, b.fsImage())
	path := filepath.Join(b.BuildDir, "kernel", "ramdisk_blob.S")
	src := "/* generated by 'tos build': embeds the littlefs image in the kernel */\n" +
		"    .section .data.ramdisk, \"aw\"\n" +
		"    .balign 16\n" +
		"    .globl __ramdisk_start\n" +
		"__ramdisk_start:\n" +
		"    .incbin \"" + img + "\"\n" +
		"    .globl __ramdisk_end\n" +
		"__ramdisk_end:\n" +
		"    .balign 16\n"
	if err := os.WriteFile(path, []byte(src), 0o644); err != nil {
		return "", err
	}
	return path, nil
}

// buildMklfs compiles the host-side littlefs image builder.  It links the
// vendored littlefs against the host libc so the on-disk format is exactly
// the one the kernel mounts.  We deliberately do not pass the project's
// freestanding include/ shims here: this is a normal host program.
func (b *Builder) buildMklfs() error {
	b.logf("mklfs (host tool)\n")
	lfsDir := filepath.Join(b.Root, "third_party", "littlefs")
	args := []string{
		"-Os",
		"-I", filepath.Join(b.Root, "tools"),
		"-I", lfsDir,
		filepath.Join(b.Root, "tools", "mklfs.c"),
		filepath.Join(b.Root, "tools", "lz4.c"),
		filepath.Join(lfsDir, "lfs.c"),
		filepath.Join(lfsDir, "lfs_util.c"),
		"-o", filepath.Join(b.BuildDir, "mklfs.exe"),
	}
	// This runs on the build host, so it must use each compiler's native
	// mode: "zig cc" keeps the host target, while a plain clang driver has
	// to drop the freestanding cross flags ("--target=... -m32").
	tool := b.TC.CC[0]
	if b.TC.UsesZig {
		args = append([]string{"cc"}, args...)
	}
	if err := b.runTool(tool, args, "MKFS"); err != nil {
		return err
	}
	return nil
}

func (b *Builder) buildUser() ([]string, error) {
	b.logf("user programs\n")
	udir := filepath.Join(b.Root, "user")
	// arch/<arch> supplies usys.h, the syscall instruction + register ABI.
	incs := []string{
		udir,
		filepath.Join(b.Root, "include"),
		filepath.Join(b.Root, "kernel"),
		filepath.Join(b.Root, "arch", b.Target.Arch),
	}

	// User programs are position independent: the kernel relocates them to
	// the board's APP_BASE, so one binary runs on QEMU and on real hardware.
	// -fPIC (not just -fPIE) is needed because the linker rejects GOT-free
	// absolute references against local symbols in a shared-style link.
	userPIEFlags := []string{"-fPIC"}

	// crt0 is per architecture, not per board: an esp32c3 user binary is
	// bit-identical to an rv32 one (same ISA, same syscall ABI).
	// aeabi.c supplies the EABI division helpers the Cortex-M0+ needs (it
	// has no hardware divide); on the M3, rv32 and i386 nothing references
	// it and --gc-sections drops it.
	common := []string{"crt0_" + b.Target.Arch + ".S", "lib.c", "syscall.c", "aeabi.c"}
	var commonObjs []string
	var tasks []task
	for _, name := range common {
		src := filepath.Join(udir, name)
		obj := filepath.Join(b.BuildDir, "user", strings.TrimSuffix(name, filepath.Ext(name))+".o")
		commonObjs = append(commonObjs, obj)
		srcCopy, objCopy := src, obj
		flags := userPIEFlags
		if strings.HasSuffix(name, ".S") {
			flags = nil // assembler: no -fPIE, it just emits relative code
		}
		tasks = append(tasks, task{fn: func() error {
			return b.compile(srcCopy, objCopy, incs, flags...)
		}})
	}

	entries, err := os.ReadDir(udir)
	if err != nil {
		return nil, err
	}
	// A program is either one .c file (user/hello.c -> bin/hello) or a
	// directory of .c files (user/py/*.c -> bin/py).  The directory form
	// exists because an interpreter does not fit in one file, and splitting
	// it into user/py_lex.c and user/py_parse.c would otherwise make two
	// programs out of one.  A directory wins over a same-named file.
	var progs []string
	var dirs []string
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() {
			dirs = append(dirs, name)
			continue
		}
		if !strings.HasSuffix(name, ".c") {
			continue
		}
		if name == "lib.c" || name == "syscall.c" || name == "aeabi.c" {
			continue
		}
		progs = append(progs, strings.TrimSuffix(name, ".c"))
	}
	sort.Strings(progs)
	sort.Strings(dirs)

	for _, prog := range progs {
		src := filepath.Join(udir, prog+".c")
		obj := filepath.Join(b.BuildDir, "user", prog+".o")
		srcCopy, objCopy := src, obj
		tasks = append(tasks, task{fn: func() error {
			return b.compile(srcCopy, objCopy, incs, userPIEFlags...)
		}})
	}

	// One object per source in the directory.  The object path carries the
	// directory name so two programs cannot overwrite each other's objects
	// (the same bug that once made esp32c3 and rv32 share one .o).
	dirObjs := map[string][]string{}
	for _, dir := range dirs {
		srcs, err := filepath.Glob(filepath.Join(udir, dir, "*.c"))
		if err != nil {
			return nil, err
		}
		if len(srcs) == 0 {
			continue // not a program directory; nothing to link
		}
		sort.Strings(srcs)
		outDir := filepath.Join(b.BuildDir, "user", dir)
		// The objects of a directory program go in a directory of their
		// own, so create it before the compile tasks run.
		if err := os.MkdirAll(outDir, 0o755); err != nil {
			return nil, err
		}

		// A program may ask for its own optimisation level by putting one
		// in an "opt" file next to its sources.  The flag goes after the
		// default in the command line, and the last -O wins.
		//
		// The Python interpreter asks for -Oz: its image is tens of
		// kilobytes and has to fit in a task window on a microcontroller
		// whose whole user area is 192 KiB.  The kernel stays at -O2,
		// because -Oz on RV32 is what makes QEMU's TCG re-translate every
		// loop (see ccFlags) - that is a property of the emulator, not of
		// the code, and the interpreter is not what runs the kernel.
		flags := userPIEFlags
		if raw, err := os.ReadFile(filepath.Join(udir, dir, "opt")); err == nil {
			if s := strings.TrimSpace(string(raw)); s != "" {
				flags = append(append([]string{}, flags...), s)
				b.logf("      OPT %s %s\n", dir, s)
			}
		}

		for _, src := range srcs {
			base := strings.TrimSuffix(filepath.Base(src), ".c")
			obj := filepath.Join(outDir, base+".o")
			dirObjs[dir] = append(dirObjs[dir], obj)
			srcCopy, objCopy, flagsCopy := src, obj, flags
			tasks = append(tasks, task{fn: func() error {
				return b.compile(srcCopy, objCopy, incs, flagsCopy...)
			}})
		}
	}

	if err := b.runTasks(tasks); err != nil {
		return nil, err
	}

	var bins []string
	for _, prog := range progs {
		obj := filepath.Join(b.BuildDir, "user", prog+".o")
		out := filepath.Join(b.BuildDir, "user", "bin", prog)
		objs := append(append([]string{}, commonObjs...), obj)
		// -pie makes the image ET_DYN so the kernel can relocate it to any
		// APP_BASE (this is what lets one binary run on QEMU and on the
		// real MCU); --gc-sections drops unused libc; -s strips symbols.
		if err := b.linkPIE(objs, filepath.Join(udir, "user.ld"), out); err != nil {
			return nil, err
		}
		b.logf("      LD  bin/%s\n", prog)
		bins = append(bins, prog)
	}
	for _, dir := range dirs {
		objs := dirObjs[dir]
		if len(objs) == 0 {
			continue
		}
		out := filepath.Join(b.BuildDir, "user", "bin", dir)
		all := append(append([]string{}, commonObjs...), objs...)
		if err := b.linkPIE(all, filepath.Join(udir, "user.ld"), out); err != nil {
			return nil, err
		}
		b.logf("      LD  bin/%s\n", dir)
		bins = append(bins, dir)
	}
	return bins, nil
}

func (b *Builder) buildFSImage(bins []string) error {
	b.logf("littlefs image\n")

	var files []RamFile
	for _, prog := range bins {
		data, err := os.ReadFile(filepath.Join(b.BuildDir, "user", "bin", prog))
		if err != nil {
			return err
		}
		files = append(files, RamFile{Name: "bin/" + prog, Data: data})
	}

	extra, err := collectFiles(b.Root)
	if err != nil {
		return err
	}
	files = append(files, extra...)

	/* stage the file contents on disk so mklfs (a host program) can read
	   them and pack them into the littlefs image. */
	stage := filepath.Join(b.BuildDir, "fs-stage")
	if err := os.RemoveAll(stage); err != nil {
		return err
	}
	var names []string
	for _, f := range files {
		dst := filepath.Join(stage, filepath.FromSlash(f.Name))
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return err
		}
		if err := os.WriteFile(dst, f.Data, 0o644); err != nil {
			return err
		}
		names = append(names, f.Name)
	}

	ramdiskPath := b.fsImage()
	/* Start from a fresh image: mklfs adds to whatever is already there, so
	   a file the guest wrote in a previous run would survive into the next
	   build and quietly change what the image contains. */
	if err := os.Remove(ramdiskPath); err != nil && !os.IsNotExist(err) {
		return err
	}
	args := []string{
		ramdiskPath,
		fmt.Sprintf("%d", b.Target.RamdiskSectors*512),
		stage,
	}
	args = append(args, names...)

	if err := b.runTool(filepath.Join(b.BuildDir, "mklfs.exe"), args, "MKFS"); err != nil {
		return err
	}

	ramdisk, err := os.ReadFile(ramdiskPath)
	if err != nil {
		return err
	}
	b.logf("      %s (%d KiB, %d files)\n",
		relName(b.Root, ramdiskPath), len(ramdisk)/1024, len(files))
	return nil
}

// buildDiskImage stitches boot sector + kernel + filesystem into the raw
// disk image the PC target boots from.
func (b *Builder) buildDiskImage() error {
	b.logf("disk image\n")

	ramdisk, err := os.ReadFile(b.fsImage())
	if err != nil {
		return err
	}
	boot, err := os.ReadFile(filepath.Join(b.BuildDir, "boot", "stage1.bin"))
	if err != nil {
		return err
	}
	kernel, err := os.ReadFile(filepath.Join(b.BuildDir, "kernel.bin"))
	if err != nil {
		return err
	}

	img := make([]byte, 0, (BootSectors+KernelSectors+b.Target.RamdiskSectors)*512)
	img = append(img, boot[:512]...)
	img = append(img, padTo(kernel, KernelSectors*512)...)
	img = append(img, padTo(ramdisk, b.Target.RamdiskSectors*512)...)

	imgPath := filepath.Join(b.BuildDir, "tinyos.img")
	if err := os.WriteFile(imgPath, img, 0o644); err != nil {
		return err
	}
	b.logf("      %s (%d KiB)\n", relName(b.Root, imgPath), len(img)/1024)
	return nil
}

func padTo(b []byte, n int) []byte {
	if len(b) >= n {
		return b[:n]
	}
	out := make([]byte, n)
	copy(out, b)
	return out
}
