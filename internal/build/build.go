package build

import (
	"bytes"
	"debug/elf"
	"encoding/binary"
	"fmt"
	"io"
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
	KernelSectors = 200 // 100 KiB, loaded at 0x10000
	BootSectors   = 1

	// The littlefs partition starts right after the kernel image.  This
	// LBA is what the kernel's ATA driver mounts; keep it in sync with
	// LFS_PARTITION_LBA in kernel/ramdisk.h.
	LFSPartitionLBA = BootSectors + KernelSectors // = 201

	// Sanity bound: the kernel (image + .bss, including the heap arena) must
	// end below 0x400000.  There is no frame allocator any more, so this is
	// just a guard against .bss growing into the region reserved for boards
	// that may map RAM differently.
	KernelMaxEnd = 0x400000
)

// Target describes which architecture/board to build for.
// Target names one buildable image (one board, one ISA, one CPU count).
//
// Derived fields are filled by ensureBoardConfig() from board.conf and the
// chip table; the literals in Targets only carry what is neither a chip fact
// nor a board fact: Arch, March (an image choice inside the ISA), CPUs and
// the build defaults (heap/procs/kstack).  A field that is both board truth
// and image policy belongs to configs, not here.
type Target struct {
	Name string // "atk-dnr2350am-smp", "rpi-pico2", "rv32"
	// Aliases are the old names this image answered to before it was named
	// after its board.  `-target` accepts them; the output keeps the
	// canonical name, so build/tinyos-atk-dnr2350am-smp.uf2 is the only artifact.
	Aliases []string
	Arch    string // arch subdirectory
	Board   string // board subdirectory
	Triple  string // clang target triple
	March   string // -march value ("" = whatever the compiler defaults to)
	Machine string // zig -mcpu value ("" = default)
	// RCCArch selects rcc's code generator: "rv32", "thumb1" or "thumb2".
	// Empty keeps the compiler's default (Thumb-2 on ARM, RV32 on riscv).
	// Per image, not per chip: an M33 can run Thumb-1 too, and several SoCs
	// share a core.
	RCCArch string

	// Opt is the kernel's C optimisation flag ("" = -O2).  Per image, not
	// per board: the same silicon may want -Oz where the image is short of
	// flash and -O2 where the hot loops decide throughput.  A config file's
	// [build] opt and the TOS_OPT environment variable still win, so an
	// experiment does not have to edit the target list.
	Opt string

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
	// in as TOS_HEAP_BYTES.  0 keeps heap.c's default of 32 KiB.  Since
	// exec() streams images straight into the task window the arena only
	// carries littlefs's working set, so the target list is a spread rather
	// than one number: 256 KiB where RAM is free (x86, rv32 - it costs
	// nothing and keeps a big write from fragmenting), 8 KiB on the C3,
	// where every 16 KiB of .bss is another task window.
	//
	// History worth keeping: the C3 once asked for 64 KiB to soak up the
	// ~50 KiB between the end of .bss and the first task window, which put
	// the kernel at 240 bytes of headroom while the measured peak never
	// passed 11.6 KiB.  That is the pattern to avoid - a buffer sized to
	// fill space rather than to hold data.
	HeapBytes int

	// XipFlashBase is where the kernel's code lives in flash when the board
	// executes it in place instead of copying it into RAM.  0 means "copy
	// everything to RAM", which is what every board but the ESP32-C3 does.
	// The packer writes the code at this offset and the linker places it at
	// 0x42000000 + (XipFlashBase - XipMapBase).
	XipFlashBase uint32
	// XipMapBase is the 64 KiB flash page the boot stub maps at 0x42000000.
	// It equals XipFlashBase unless the code starts inside the mapping page,
	// which is what lets the ESP32-C3 put code at 0x2000 instead of 0x10000.
	XipMapBase uint32

	// MaxProcs is the size of the process table (kernel/process.c), passed
	// in as TOS_MAX_PROCS.  0 means "use the default of 16".  It exists
	// because a proc_t is 792 bytes: a board with 12 task windows gains
	// 1.6 KiB by not sizing the table for tasks it can never run.
	MaxProcs int

	// KStack is the per-CPU kernel stack in bytes; 0 keeps the arch default
	// (16 KiB on ARM/RV32).  cpu.h and start.S both consume the same
	// TOS_KSTACK_BYTES define, so this cannot drift between them.
	KStack int

	// Window geometry, filled from board.conf by applyBoard().  Keeping it
	// out of the literals is what lets a new board touch only its conf and
	// layout.c.
	winGrain int
	winStack int
	winGuard int

	// CPUs is how many harts this target is built for.  0 and 1 mean the
	// same thing - a uniprocessor, with no TOS_MAX_CPUS define at all, so
	// none of the SMP code (second trap vector, second kernel stack, per-CPU
	// run queue) is even assembled.  2 or more defines TOS_MAX_CPUS, which
	// start.S, kernel/cpu.c and kernel/process.c all read, and asks QEMU to
	// start that many harts.
	CPUs int

	// Drivers lists the board's default driver directories, relative to the
	// tree root.  They are compiled even when no -config is given, so
	// `tos build -target <board>` keeps working; a config's [drivers] adds
	// to this list.
	Drivers []string

	// DefaultFeatures are turned on for this board even when no config
	// file mentions them.  A config's [features] entry can still set one
	// to false, which is how menuconfig trims a driver it defaults to on.
	DefaultFeatures []string
}

// Targets is loaded from targets/*.conf (targets.go).  Each file names a
// board, an ISA and the image knobs; nothing about a board lives here.

// WindowGeometry is filled by applyBoard() from board.conf; there is no
// board-name switch here, so a new board only edits its conf and layout.c.
// Callers must let ensureBoardConfig() run first (Build does).
func (t Target) WindowGeometry() (grain, stack, guard int) {
	return t.winGrain, t.winStack, t.winGuard
}

// windowNotes is what windowNote() finds in a linked user ELF.
type windowNotes struct {
	span, stack, heap uint32 // image span (page rounded), stack, heap
	window            uint32 // TOS_WINDOW bytes; 0 is TOS_ONE_GRAIN()
	hasWindow         bool   // a TOS_WINDOW note was present
}

// windowNote reads the TOS_STACK/TOS_HEAP/TOS_WINDOW notes out of a linked
// user ELF and computes the span mklfs will store: the PT_LOAD range rounded
// up to the page size.
func windowNote(data []byte) (windowNotes, bool) {
	var w windowNotes

	f, err := elf.NewFile(bytes.NewReader(data))
	if err != nil {
		return w, false
	}
	lo, hi := ^uint64(0), uint64(0)
	for _, p := range f.Progs {
		switch p.Type {
		case elf.PT_LOAD:
			if p.Memsz == 0 {
				continue
			}
			if p.Vaddr < lo {
				lo = p.Vaddr
			}
			if p.Vaddr+p.Memsz > hi {
				hi = p.Vaddr + p.Memsz
			}
		case elf.PT_NOTE:
			note, err := io.ReadAll(p.Open())
			if err != nil {
				continue
			}
			for off := 0; off+12 <= len(note); {
				namesz := uint64(binary.LittleEndian.Uint32(note[off:]))
				descsz := uint64(binary.LittleEndian.Uint32(note[off+4:]))
				typ := binary.LittleEndian.Uint32(note[off+8:])
				nameOff := off + 12

				/* Check each field before converting the padded length
				 * to int or slicing.  namesz+3 can wrap in uint32. */
				if namesz > uint64(len(note)-nameOff) {
					break
				}
				namePad := (namesz + 3) &^ 3
				descOff := uint64(nameOff) + namePad
				if descOff > uint64(len(note)) ||
					descsz > uint64(len(note))-descOff {
					break
				}
				descPad := (descsz + 3) &^ 3
				next64 := descOff + descPad
				if next64 > uint64(len(note)) || next64 <= uint64(off) {
					break
				}
				if namesz >= 6 && descsz >= 4 &&
					string(note[nameOff:nameOff+6]) == "TinyOS" {
					v := binary.LittleEndian.Uint32(note[descOff:])
					switch typ {
					case 1:
						w.stack = v
					case 2:
						w.heap = v
					case 3:
						w.window = v
						w.hasWindow = true
					}
				}
				off = int(next64)
			}
		}
	}
	if lo == ^uint64(0) || hi <= lo {
		return w, false
	}
	span := (hi - lo + 4095) &^ 4095
	if span > uint64(^uint32(0)) {
		return w, false
	}
	w.span = uint32(span)
	return w, true
}

// checkWindow enforces a program's TOS_WINDOW promise before it is packed.
// TOS_ONE_GRAIN() stores 0, meaning the program must fit one board grain.
func (b *Builder) checkWindow(prog string, data []byte) error {
	w, ok := windowNote(data)
	if !ok || !w.hasWindow {
		return nil
	}
	grain, defStack, guard := b.Target.WindowGeometry()
	if w.stack == 0 {
		w.stack = uint32(defStack)
	}
	limit := uint64(w.window)
	what := fmt.Sprintf("TOS_WINDOW(%d KiB)", w.window/1024)
	if w.window == 0 {
		limit = uint64(grain)
		what = "TOS_ONE_GRAIN()"
	}
	// The note fields come from an untrusted ELF.  Do the sum in uint64:
	// uint32 addition here used to wrap and let an image with a near-4 GiB
	// heap note pass the build-time check.
	need := uint64(w.span) + uint64(w.stack) + uint64(w.heap) + uint64(guard)
	if need <= limit {
		return nil
	}
	rounded := (need + uint64(grain) - 1) / uint64(grain) * uint64(grain)
	return fmt.Errorf("%s: %s no longer fits: image %d + stack %d + heap %d + guard %d = %d bytes, so it would take a %d KiB window",
		prog, what, w.span, w.stack, w.heap, guard, need, rounded/1024)
}

type Builder struct {
	Root     string
	BuildDir string
	TC       Toolchain
	QEMU     string
	Verbose  bool
	Target   Target

	Config         *Config
	BoardConfig    *BoardConfig
	Features       map[string]bool
	DriverDirs     []string
	driverDirsInit bool
	UserExclude    map[string]bool
	SourceExclude  map[string]bool
	WiFiLibDir     string
	WiFiLDScript   string
	WiFiLDSymbol   string
	LDExtra        []string
	// Config overrides for the kernel; 0/"" keeps the target's default.
	HeapBytes  int
	MaxProcs   int
	KStack     int
	Opt        string
	Threads    bool
	HistBytes  int
	ProcLite   bool
	DevNodes   bool
	Panel      bool
	FSReadOnly bool
	BootTrace  bool
	Mailbox    bool

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
		Root:          root,
		BuildDir:      filepath.Join(root, "build"),
		Features:      map[string]bool{},
		UserExclude:   map[string]bool{},
		SourceExclude: map[string]bool{},
		HistBytes:     512,
		DevNodes:      true,
		Panel:         true,
		// Same default as build.Config: an image explains itself unless a
		// config says otherwise.  This has to be here as well as in
		// LoadConfig(), because a build with no config file never runs
		// ApplyConfig() - the markers were silently compiled out of every
		// default build until the ATK acceptance table noticed.
		BootTrace: true,
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
		opt = b.Opt
	}
	if opt == "" {
		opt = b.Target.Opt
	}
	if opt == "" {
		opt = "-O2"
	}

	// Boot-progress traces on the console: on by default, off with
	// TOS_BOOT_TRACE=0 at build time.  Kept out of the config file because it
	// describes what the image should say while it boots, not what the image
	// contains - the same board wants the loud build during bring-up and the
	// quiet one afterwards.
	bootTrace := ""
	if v := strings.TrimSpace(os.Getenv("TOS_BOOT_TRACE")); v == "0" || v == "1" {
		bootTrace = "-DTOS_BOOT_TRACE=" + v
	}

	// Architecture specific flags.  Everything else is shared so both ports
	// see the same C dialect, optimiser and warning set.
	var archFlags []string
	// Which core this is, is a property of the chip, not of the board name:
	// the same RP2350 ships as the ATK board and as a Pico 2, and both need
	// the ARMv8-M flags below.  Keying these on board names silently dropped
	// them when board/rp2350 became board/atk-dnr2350am, and the image booted
	// with ARMv7-M MPU registers on an M33 (no MPU -> task switch faults ->
	// reboot loop).
	chip := ""
	if b.BoardConfig != nil {
		chip = b.BoardConfig.Chip
	}
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
		// rcc's code generator is chosen here, not by the chip: an M33 can
		// run Thumb-1, and several SoCs share a core.  The image says which
		// one it wants ([target] rcc_arch), and the loader validation in
		// targets.go rejects a choice the core cannot execute.
		if b.Target.RCCArch != "" {
			archFlags = append(archFlags, "-DRCC_ARCH_"+strings.ToUpper(b.Target.RCCArch))
		}
		// RP2040 is a Cortex-M0+ (ARMv6-M): no MPU, no BASEPRI, no
		// ldrex/strex, no CFSR/MMFAR/BFAR.  arch/arm isolates the M0+
		// specific bits behind this macro so the M3 build is unaffected.
		if chip == "rp2040" {
			archFlags = append(archFlags, "-DTOS_ARM_M0P")
			// M0+ has no LDREX/STREX.  The SMP build uses one of the
			// SIO hardware spinlocks as the gate behind the software lock
			// flag, exactly like the RP2350 port (see arch/arm/platform.c).
			if b.Target.CPUs > 1 {
				archFlags = append(archFlags, "-DTOS_ARM_SIO_SPIN")
			}
		}
		// Cortex-M33 has an MPU, but it is ARMv8-M PMSAv8 (PRBAR/PRLAR plus
		// MAIR), not the ARMv7-M one: same region model on top, different
		// registers underneath, so arch/arm/platform.c compiles its other
		// writer.  Fault reporting (CFSR/MMFAR/BFAR) stays on either way -
		// only TOS_ARM_M0P suppresses that.
		if chip == "rp2350" {
			archFlags = append(archFlags, "-DTOS_ARM_MPU_V8")
			// Bring-up aid: a 16-entry ring of the last syscall numbers,
			// dumped by arm_handle_fault().  Only the new M33 port pays for
			// it; remove once the shell is stable on real hardware.
			archFlags = append(archFlags, "-DTOS_SYSCALL_TRACE=1")
			// The two M33 cores share no global exclusive monitor, so
			// LDREX/STREX cannot give cross-core mutual exclusion (measured
			// as proc_lock double-held, OPEN-ITEMS §24.4).  The SIO block's
			// hardware spinlocks are the sanctioned fix - tell platform.c
			// to use one as the gate behind the software lock flag.
			archFlags = append(archFlags, "-DTOS_ARM_SIO_SPIN")
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
	// much of its SRAM the allocator may claim.  The config can override it.
	heap := b.HeapBytes
	if heap == 0 {
		heap = b.Target.HeapBytes
	}
	if heap > 0 {
		archFlags = append(archFlags,
			fmt.Sprintf("-DTOS_HEAP_BYTES=%d", heap))
	}

	// Kernel stack per CPU.  cpu.h and the arch start.S must agree; both read
	// the same define, so the config value cannot drift between them.  A
	// target may lower it (rp2040: the M0+ kernel is shallow enough for 8 KiB
	// and the SRAM saved goes straight to the task area).
	kstack := b.KStack
	if kstack == 0 {
		kstack = b.Target.KStack
	}
	if kstack > 0 {
		archFlags = append(archFlags,
			fmt.Sprintf("-DTOS_KSTACK_BYTES=%d", kstack))
	}

	// XIP: the boot stub maps the IROM window to a 64 KiB flash page, while
	// the code itself can start at a byte offset inside that page.
	if b.Target.XipFlashBase != 0 {
		archFlags = append(archFlags,
			fmt.Sprintf("-DTOS_XIP_MAP_BASE=0x%x", b.Target.XipMapBase),
			fmt.Sprintf("-DTOS_XIP_CODE_OFFSET=0x%x", b.Target.XipFlashBase))
	}

	// Process table size (kernel/process.c).  Left undefined on boards that
	// can run 16 or more tasks, so they keep tty-independent defaults.
	procs := b.MaxProcs
	if procs == 0 {
		procs = b.Target.MaxProcs
	}
	if procs > 0 {
		archFlags = append(archFlags,
			fmt.Sprintf("-DTOS_MAX_PROCS=%d", procs))
	}

	// The RP2350 RISC-V image is linked in flash with .data in SRAM, but
	// the boot ROM only jumps to the entry point: it does not load any
	// segment.  _start therefore copies .data from its flash LMA before
	// kmain can touch a global.  Every other RV32 target is either loaded
	// whole by QEMU or has a ROM that relocates .data itself, so nothing
	// else asks for the copy loop.
	if b.Target.Arch == "rv32" && chip == "rp2350" {
		archFlags = append(archFlags, "-DTOS_COPY_DATA")
		// RP2350's Hazard3 PMP supports NAPOT but not TOR, so a task's
		// window must be one naturally aligned power-of-two region.  The
		// window allocator rounds sizes up and aligns the base to the
		// size; see kernel/process.c and plat_board_task_protect().
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
	if bootTrace != "" {
		flags = append(flags, bootTrace)
	}
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

// withoutRVC drops the C extension from an ISA string ("rv32imac" ->
// "rv32ima"), keeping any underscore-separated extension list.  Returns ""
// when the string does not name RVC.
func withoutRVC(march string) string {
	base, ext, hasExt := strings.Cut(march, "_")
	i := strings.IndexByte(base, 'c')
	if i < 0 {
		return ""
	}
	base = base[:i] + base[i+1:]
	if hasExt {
		return base + "_" + ext
	}
	return base
}

// userRVCMode is the board's [user] rvc policy, or "" for "plain target ISA":
//
//	""      compressed instructions as the target allows
//	"no"    never use them
//	"auto"  use them unless the linked image would straddle a page boundary
//
// A board only states the policy; the build decides per program.
func (b *Builder) userRVCMode() string {
	if b.BoardConfig == nil {
		return ""
	}
	return b.BoardConfig.UserRVC
}

// userISA returns the -march flag that turns RVC off for user code, or nil
// when the target has no compressed instructions to begin with.
func (b *Builder) userISANoC() []string {
	if b.Target.Arch != "rv32" {
		return nil
	}
	if m := withoutRVC(b.Target.March); m != "" {
		return []string{"-march=" + m}
	}
	return nil
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
	if driver && b.Target.Arch == "x86" {
		// A distribution gcc/clang driver defaults to PIE, which turns the
		// link into a static PIE and rejects the 16-bit absolute relocations
		// the real-mode boot sector emits (R_386_16 against a local symbol).
		// Both stage1 and the x86 kernel are loaded at fixed addresses, so
		// "-static -no-pie" is what the scripts already assume.  This has to
		// be a driver flag: ld.lld has no --no-pie (OPEN-ITEMS 5).
		args = append(args, "-no-pie")
	}
	if extraEnv := os.Getenv("TOS_LD_EXTRA"); extraEnv != "" {
		args = append(args, strings.Fields(extraEnv)...)
	}
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
	/* Bring-up hook: extra linker fragments/roots for the WiFi blobs
	 * (-T esp32c3.rom.ld, -u wifi_probe) without baking the Espressif
	 * SDK into every build. */
	if extra := os.Getenv("TOS_LD_EXTRA"); extra != "" {
		args = append(args, strings.Fields(extra)...)
	}
	args = append(args,
		w("--gc-sections"),
		w("-z"), w("max-page-size=16"),
		w("-z"), w("now"),
		w("-s"),
		"-o", out,
	)
	args = append(args, objs...)
	if err := b.runTool(tool, args, "LD"); err != nil {
		return err
	}
	// The kernel loads these through the program header table only; see
	// TrimELFSections for why the section table is safe (and worth) dropping.
	_, err := TrimELFSections(out)
	return err
}

// filterNames drops the files whose name is already taken by a nearer board
// layer.  Used by the base/variant composition (board.conf `base =`): the
// nearest definition wins, exactly like an include path.
func filterNames(list []string, seen map[string]bool) []string {
	kept := list[:0]
	for _, f := range list {
		if seen[filepath.Base(f)] {
			continue
		}
		kept = append(kept, f)
	}
	return kept
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
	if err := b.ensureBoardConfig(); err != nil {
		return err
	}
	if err := b.validateConfig(b.Config); err != nil {
		return err
	}
	if err := b.writeBoardHeader(); err != nil {
		return err
	}
	b.ensureDriverDirs()
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
		if err := b.writeBuildConfig(); err != nil {
			return err
		}
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
	if err := b.writeBuildConfig(); err != nil {
		return err
	}
	return nil
}

// writeBuildConfig records the knobs that a menuconfig file can change, so a
// released image can be compared with the build that was tested on hardware.
// CI builds with no tos.conf (target defaults), while a local acceptance run
// usually has one - heap, opt and the program set then differ, and without
// this file that difference is invisible in the artifact.
func (b *Builder) writeBuildConfig() error {
	grain, stack, guard := b.Target.WindowGeometry()
	pick := func(v, d int) int {
		if v != 0 {
			return v
		}
		return d
	}
	opt := b.Opt
	if opt == "" {
		opt = b.Target.Opt
	}
	if opt == "" {
		opt = "-O2"
	}
	opt = opt + " (kernel) / -Oz (size-opt programs)"
	cpus := b.Target.CPUs
	if cpus == 0 {
		cpus = 1
	}
	cfg := "(none: target defaults)"
	if b.Config != nil && b.Config.Path != "" {
		cfg = b.Config.Path
	}
	var feats []string
	for k, on := range b.Features {
		if on {
			feats = append(feats, k)
		}
	}
	sort.Strings(feats)

	lines := []string{
		"target      " + b.Target.Name,
		"board       " + b.Target.Board,
		"arch        " + b.Target.Arch,
		"cpus        " + fmt.Sprint(cpus),
		"heap        " + fmt.Sprintf("%d bytes", pick(b.HeapBytes, b.Target.HeapBytes)),
		"max_procs   " + fmt.Sprint(pick(b.MaxProcs, b.Target.MaxProcs)),
		"kstack      " + fmt.Sprint(pick(b.KStack, b.Target.KStack)),
		"opt         " + opt,
		// The board's real grain/stack/guard live in board/*/layout.c; these
		// three are what checkWindow() validates TOS_WINDOW/TOS_ONE_GRAIN
		// against.  They come from board.conf via WindowGeometry(), not from a
		// switch here (B-1c, #10), so they are labelled for what they are.
		"check-grain " + fmt.Sprintf("%d bytes", grain),
		"check-stack " + fmt.Sprintf("%d bytes", stack),
		"check-guard " + fmt.Sprintf("%d bytes", guard),
		"features    " + strings.Join(feats, ","),
		"user-defines " + strings.Join(b.userDefines(), " "),
		"rcc_arch    " + rccArchLabel(b.Target.RCCArch),
		"config      " + cfg,
		"note        runtime grain/stack/guard are in board/*/layout.c; the",
		"            check-* lines are the build-time TOS_WINDOW validation",
	}
	// Provenance: which layer set each effective value.  Kept as comments so
	// the machine readers (goc32 reads the features line back) see the same
	// file they always did.  A value that looks wrong on a board is almost
	// always a layer question - "why is heap 8 KiB here?" - and the layer is
	// the first thing to check.
	src := func(set bool, cfgVal, targetVal string) string {
		if set {
			return "config:" + cfgVal
		}
		return targetVal
	}
	targetSrc := "target:targets/" + b.Target.Name + ".conf"
	boardSrc := "board:board/" + b.Target.Board + "/board.conf"
	featSrc := "board:board/" + b.Target.Board + "/board.conf"
	cfgName := ""
	if b.Config != nil && b.Config.Path != "" {
		cfgName = relName(b.Root, b.Config.Path)
	}
	if b.Config != nil && len(b.Config.Features) > 0 {
		featSrc += " + config:" + cfgName
	}
	// opt has three layers: TOS_OPT, the config, then the target's own
	// choice (see Target.Opt).  The effective value is already in `opt`;
	// this only records which layer supplied it.
	optSrc := "built-in:-O2"
	if b.Opt != "" {
		optSrc = "config:" + cfgName
	} else if b.Target.Opt != "" {
		optSrc = targetSrc
	}
	prov := []string{
		"",
		"# provenance: the layer each effective value came from",
		"#   target      " + targetSrc,
		"#   board       " + boardSrc,
		"#   cpus        " + targetSrc,
		"#   rcc_arch    " + targetSrc,
		"#   heap        " + src(b.HeapBytes != 0, cfgName, targetSrc),
		"#   max_procs   " + src(b.MaxProcs != 0, cfgName, targetSrc),
		"#   kstack      " + src(b.KStack != 0, cfgName, targetSrc),
		"#   opt         " + optSrc,
		"#   check-grain " + boardSrc,
		"#   check-stack " + boardSrc,
		"#   check-guard " + boardSrc,
		"#   features    " + featSrc,
	}
	lines = append(lines, prov...)

	out := filepath.Join(b.BuildDir, "build-config.txt")
	data := []byte(strings.Join(lines, "\n") + "\n")
	if err := os.WriteFile(out, data, 0o644); err != nil {
		return err
	}
	// A per-target copy lets goc32 pick up the feature defines for the target
	// it is compiling for even after another target was built last.  Releases
	// ship only build-config.txt.
	perTarget := filepath.Join(b.BuildDir, "build-config-"+b.Target.Name+".txt")
	if err := os.WriteFile(perTarget, data, 0o644); err != nil {
		return err
	}
	b.logf("      %s\n", relName(b.Root, out))
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
	// register frame (arch/<arch>/regs.h).  Driver directories come from the
	// build config and are added both as source roots and include roots.
	incs := []string{
		filepath.Join(b.Root, "include"),
		filepath.Join(b.BuildDir, "gen"),
		kdir,
		lfsDir,
		filepath.Join(b.Root, "board", b.Target.Board),
	}
	/* A variant board (base = <board> in board.conf) reuses the silicon's
	 * sources.  Its directory goes right after the variant's own, and before
	 * the arch default: board/rp2350-generic/board.h must win over arch/arm/board.h
	 * for a variant with no board.h of its own. */
	if bc := b.BoardConfig; bc != nil {
		for _, base := range bc.Bases {
			incs = append(incs, filepath.Join(b.Root, "board", base))
		}
	}
	incs = append(incs,
		filepath.Join(b.Root, "board"),
		filepath.Join(b.Root, "arch", b.Target.Arch),
	)

	/* Backward compatibility: an old TOS_WIFI_LIB environment build still
	 * pulls the in-tree WiFi driver directory. */
	wifiEnv := os.Getenv("TOS_WIFI_LIB") != ""
	driverDirs := append([]string(nil), b.DriverDirs...)
	if (b.feature("wifi") || wifiEnv) && !hasFileInDirs(driverDirs, "wifi_drv.c") {
		driverDirs = append(driverDirs, filepath.Join(b.Root, "drivers", "wifi", "esp32c3"))
	}
	seenDir := map[string]bool{}
	addUnique := func(list []string, d string) []string {
		d = filepath.Clean(d)
		if seenDir[d] {
			return list
		}
		seenDir[d] = true
		return append(list, d)
	}
	for _, d := range driverDirs {
		incs = addUnique(incs, d)
		if st, err := os.Stat(filepath.Join(d, "include")); err == nil && st.IsDir() {
			incs = addUnique(incs, filepath.Join(d, "include"))
		}
	}

	// The RP2350 console is a TinyUSB CDC-ACM device (drivers/usb/rp2350/tusb_shim.c).
	// third_party/tinyusb holds a trimmed upstream tree - device stack, CDC
	// class, RP2040 controller port - and port_rp2/ is the pico-sdk facade
	// that lets those upstream files compile unchanged.  The board directory
	// joins the include path so tusb_config.h is found by name.
	tusbDir := filepath.Join(b.Root, "third_party", "tinyusb")
	// TinyUSB is a chip capability, not a board name: every RP2040/RP2350
	// image has the same USB controller, so a new board on that silicon needs
	// no entry here.
	chip := ""
	if b.BoardConfig != nil {
		chip = b.BoardConfig.Chip
	}
	useTinyUSB := chip == "rp2040" || chip == "rp2350"
	if useTinyUSB {
		incs = append(incs,
			filepath.Join(tusbDir, "src"),
			filepath.Join(tusbDir, "port_rp2", "include"),
		)
	}

	// Sources come from four places: the arch-independent core, the
	// architecture port, the board support package and the config-selected
	// driver directories.  Which arch and board directory is used depends on
	// the build target.
	var srcs []string
	variantDir := filepath.Join(b.Root, "board", b.Target.Board)
	// Nearest directory first: the variant, then its base, then the base's
	// base.  Each layer's files are dropped when a nearer layer has a file
	// with the same name, so board/atk-rv32's platform.c replaces the one it
	// would otherwise inherit from rp2350rv-generic.
	var boardDirs []string
	boardDirs = append(boardDirs, variantDir)
	if bc := b.BoardConfig; bc != nil {
		for _, base := range bc.Bases {
			boardDirs = append(boardDirs, filepath.Join(b.Root, "board", base))
		}
	}
	dirs := []string{
		kdir,
		filepath.Join(b.Root, "arch", b.Target.Arch),
	}
	dirs = append(dirs, boardDirs...)
	dirs = append(dirs, driverDirs...)
	seenNames := map[string]bool{}
	for i, dir := range dirs {
		cs, err := globFiles(dir, "*.c")
		if err != nil {
			return err
		}
		ss, err := globFiles(dir, "*.S")
		if err != nil {
			return err
		}
		if i >= 2 && i < 2+len(boardDirs) {
			srcs = append(srcs, filterNames(ss, seenNames)...)
			srcs = append(srcs, filterNames(cs, seenNames)...)
			for _, f := range cs {
				seenNames[filepath.Base(f)] = true
			}
			for _, f := range ss {
				seenNames[filepath.Base(f)] = true
			}
			continue
		}
		srcs = append(srcs, ss...)
		srcs = append(srcs, cs...)
	}
	if len(b.SourceExclude) != 0 {
		kept := srcs[:0]
		for _, s := range srcs {
			if b.SourceExclude[filepath.ToSlash(filepath.Clean(s))] {
				continue
			}
			kept = append(kept, s)
		}
		srcs = kept
	}

	featDefs := b.featureDefines()
	if (b.feature("wifi") || wifiEnv) && !containsString(featDefs, "-DTOS_WIFI") {
		featDefs = append(featDefs, "-DTOS_WIFI")
	}
	if (b.feature("net") || wifiEnv) && !containsString(featDefs, "-DTOS_NET") {
		featDefs = append(featDefs, "-DTOS_NET")
	}

	// Kernel trimming.  These are compile-time: an off feature contributes
	// no code and no .bss.  The panel needs the device layer to be started
	// (hwdev_init() is what calls screen_init()), so it follows DevNodes.
	hist := b.HistBytes
	if hist < 0 || hist > 4096 {
		hist = 512
	}
	featDefs = append(featDefs,
		fmt.Sprintf("-DTOS_HIST_BYTES=%d", hist),
		fmt.Sprintf("-DTOS_PROC_LITE=%d", boolInt(b.ProcLite)),
		fmt.Sprintf("-DTOS_DEVNODES=%d", boolInt(b.DevNodes)),
		fmt.Sprintf("-DTOS_PERIPH=%d", boolInt(b.DevNodes)),
		fmt.Sprintf("-DTOS_PANEL=%d", boolInt(b.Panel)),
		fmt.Sprintf("-DTOS_BOOT_TRACE=%d", boolInt(b.BootTrace)),
		// One switch for "make this image explain itself": the boot-progress
		// lines above and the TOS_DIAG wedge reporting are the same decision
		// (bring-up/debug build vs quiet production build), so they share
		// boot_trace instead of needing two knobs that can drift apart.
		fmt.Sprintf("-DTOS_DIAG=%d", boolInt(b.BootTrace)),
	)
	if b.FSReadOnly {
		featDefs = append(featDefs, "-DTOS_LFS_RO")
	}
	if b.Mailbox {
		featDefs = append(featDefs, "-DTOS_MAILBOX=1")
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
		relCopy := filepath.ToSlash(rel)
		tasks = append(tasks, task{label: name, fn: func() error {
			b.logf("      CC  %s\n", relCopy)
			return b.compile(srcCopy, objCopy, incs, featDefs...)
		}})
	}

	/* vendored littlefs, with the kernel config force-included */
	lfsCfg := filepath.Join(kdir, "lfs_config.h")
	lfsFlags := []string{"-include", lfsCfg}
	if b.FSReadOnly {
		lfsFlags = append(lfsFlags, "-DTOS_LFS_RO")
	}
	for _, src := range []string{"lfs.c", "lfs_util.c"} {
		full := filepath.Join(lfsDir, src)
		obj := filepath.Join(b.BuildDir, "kernel", "lfs_"+strings.TrimSuffix(src, ".c")+".o")
		objs = append(objs, obj)
		srcCopy, objCopy := src, obj
		tasks = append(tasks, task{label: src, fn: func() error {
			b.logf("      CC  third_party/littlefs/%s\n", srcCopy)
			return b.compile(full, objCopy, incs, lfsFlags...)
		}})
	}

	// Targets without a disk carry the filesystem inside the kernel image:
	// a generated assembly file .incbin's build/tinyos-ramdisk-<target>.bin into a writable
	// section, which plat_block_* then serves as a RAM disk.  The esp32c3
	// target reads the partition from flash instead (board/esp32c3-generic).
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

	/* Espressif's prebuilt WiFi/PHY/coex blobs.  The config's [wifi]
	 * lib_dir is the primary source; TOS_WIFI_LIB stays as a bring-up
	 * override for one-off builds. */
	if b.BoardConfig != nil && b.BoardConfig.Chip == "esp32c3" && (b.feature("wifi") || wifiEnv) {
		dir := b.WiFiLibDir
		if dir == "" {
			if env := os.Getenv("TOS_WIFI_LIB"); env != "" {
				dir = b.abs(env)
			}
		}
		if dir == "" {
			return fmt.Errorf("wifi is enabled but no lib directory is configured (set [wifi] lib_dir or TOS_WIFI_LIB)")
		}
		for _, n := range []string{
			"libnet80211.a", "libpp.a", "libcore.a",
			"libphy.a", "libcoexist.a",
		} {
			objs = append(objs, filepath.Join(dir, n))
		}
	}

	// RV32 has no boot sector and no flat binary: QEMU and the board
	// bootloader both load the ELF directly.  A board may supply its own
	// linker script (different RAM base); the arch default is the fallback.
	if b.Target.Name != "x86" {
		// A board may supply its own linker script (different RAM base);
		// the arch default is the fallback.
		elfPath := filepath.Join(b.BuildDir, "kernel-"+b.Target.Name+".elf")
		script := filepath.Join(b.Root, "arch", b.Target.Arch, "kernel.ld")
		boardScript := b.boardFile("kernel.ld")
		if _, err := os.Stat(boardScript); err == nil {
			script = boardScript
		}
		// A board that executes code in place needs a different layout: the
		// code half links at the icache window instead of at the RAM base.
		// Keeping it in a separate script means turning XIP off is one
		// target field, not an edit to the board's normal layout.
		if b.Target.XipFlashBase != 0 {
			xip := b.boardFile("kernel-xip.ld")
			if _, err := os.Stat(xip); err != nil {
				return fmt.Errorf("target %s sets XipFlashBase but %s is missing",
					b.Target.Name, xip)
			}
			script = xip
		}
		if err := b.link(objs, script, elfPath, b.kernelLDArgs()...); err != nil {
			return err
		}
		if end, err := ELFEndAddress(elfPath); err == nil {
			// With XIP the highest segment is the code half in the flash
			// window; the number that matters for headroom is the RAM one.
			if b.Target.XipFlashBase != 0 {
				if ram, err2 := ELFEndBelow(elfPath, espXipVaddrLow); err2 == nil {
					xipVaddr := uint32(espXipVaddrLow) +
						(b.Target.XipFlashBase - b.Target.XipMapBase)
					flashEnd := b.Target.XipFlashBase + (uint32(end) - xipVaddr)
					b.logf("      kernel RAM ends at 0x%x, code at 0x%x (flash 0x%x)\n",
						ram, end, flashEnd)
				}
			} else {
				b.logf("      kernel ends at 0x%x\n", end)
			}
		}

		chip := ""
		if b.BoardConfig != nil {
			chip = b.BoardConfig.Chip
		}

		/* Which image format a board needs is a property of the silicon, not
		 * of the board: the same chip has one ROM, one XIP window and one way
		 * of scanning for an image.  Only the ESP32-C3 wants a linear flash
		 * blob.  The RP2 ROMs take a UF2 whether the filesystem lives in a
		 * flash partition or in a RAM disk linked into the image, so the
		 * packer - not this switch - decides whether there is a filesystem
		 * partition to splice in. */
		if b.Target.FSBacking == "flash" && chip == "esp32c3" {
			xipVaddr := uint32(espXipVaddrLow) +
				(b.Target.XipFlashBase - b.Target.XipMapBase)
			img, segs, err := ESPImageFromELF(elfPath, true,
				b.Target.XipFlashBase, xipVaddr)
			if err != nil {
				return err
			}
			out := filepath.Join(b.BuildDir, "tinyos-"+b.Target.Name+".bin")
			if err := os.WriteFile(out, img, 0o644); err != nil {
				return err
			}
			b.logf("      %s (%d bytes, %d segment(s))\n", relName(b.Root, out), len(img), len(segs))

			/* One file to flash at 0x0, the way MicroPython ships: the
			 * kernel image padded to the filesystem offset, then the
			 * littlefs image.  esptool writes a file linearly, so a
			 * padded blob is the same two writes in one command - and
			 * it is what a browser-based flasher or a "drop this onto
			 * the chip" instruction can use.  The split pair stays the
			 * default for flashing: it does not rewrite 0x20000..0x80000
			 * and it keeps --keep-fs possible. */
			fs, err := os.ReadFile(b.fsImage())
			if err != nil {
				return err
			}
			const fsOff = 0x80000
			full := make([]byte, fsOff+len(fs))
			for i := range full {
				full[i] = 0xff
			}
			copy(full, img)
			copy(full[fsOff:], fs)
			fullPath := filepath.Join(b.BuildDir, "tinyos-"+b.Target.Name+"-full.bin")
			if err := os.WriteFile(fullPath, full, 0o644); err != nil {
				return err
			}
			b.logf("      %s (%d bytes = kernel 0x%x..0x%x + fs 0x%x..0x%x)\n",
				relName(b.Root, fullPath), len(full), 0, len(img),
				fsOff, fsOff+len(fs))
			b.logf("      burn: esptool write-flash 0x0 %s | fs: write-flash 0x80000 %s\n",
				relName(b.Root, out), relName(b.Root, b.fsImage()))
			b.logf("      or one shot: esptool write-flash 0x0 %s\n",
				relName(b.Root, fullPath))
		}

		switch {
		case chip == "esp32c3":
			// The linear flash image was written above; it has no UF2.
		case chip == "rp2040":
			if err := b.emitRP2040UF2(elfPath); err != nil {
				return err
			}
		case chip == "rp2350" && b.Target.Arch == "arm":
			if err := b.emitRP2350UF2(elfPath); err != nil {
				return err
			}
		case chip == "rp2350" && b.Target.Arch == "rv32":
			if err := b.emitRP2350RVUF2(elfPath); err != nil {
				return err
			}
		case b.Target.FSBacking == "flash":
			return fmt.Errorf("target %s: no image format for chip %q, arch %q",
				b.Target.Name, chip, b.Target.Arch)
		}
		return nil
	}

	elfPath := filepath.Join(b.BuildDir, "kernel.elf")
	if err := b.link(objs, filepath.Join(kdir, "kernel.ld"), elfPath, b.kernelLDArgs()...); err != nil {
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
// build rv32 then rp2040 and build/tinyos-ramdisk-<target>.bin holds ARM programs, so
// flashing an esp32c3 kernel beside it gives a board that lists 14 files and
// then rejects every one of them ("not a valid ELF32 program") with nothing
// on screen to say the filesystem is somebody else's.
func (b *Builder) fsImage() string {
	name := "ramdisk.bin"
	if b.Target.Name != "" {
		name = "tinyos-ramdisk-" + b.Target.Name + ".bin"
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
	grain, _, _ := b.Target.WindowGeometry()
	userPIEFlags := append([]string{"-fPIC", fmt.Sprintf("-DTOS_APP_GRAIN=%d", grain)},
		b.userDefines()...)

	// User code uses the target's ISA, as the kernel does.  A board whose
	// protection map can cover one code run with several entries needs user
	// code without the C extension (RISC-V allows an access only when a single
	// PMP entry covers all of it, and RVC lets a 32-bit instruction start at
	// page_end-2).  The board states the policy with `[user] rvc` - "no" for
	// all programs, "auto" to fall back per program - and nothing here names a
	// board or a chip.  checkCodePages() below is the invariant either way.
	rvcMode := b.userRVCMode()
	isaNoC := b.userISANoC()
	if rvcMode != "" && isaNoC == nil {
		return nil, fmt.Errorf("[user] rvc is set but %s has no C extension to remove",
			b.Target.March)
	}
	autoRVC := rvcMode == "auto"
	if rvcMode == "no" {
		userPIEFlags = append(userPIEFlags, isaNoC...)
	}
	userCFlags := append([]string{}, userPIEFlags...)

	// crt0 is per architecture, not per board: an esp32c3 user binary is
	// bit-identical to an rv32 one (same ISA, same syscall ABI).
	// aeabi.c supplies the EABI division helpers the Cortex-M0+ needs (it
	// has no hardware divide); on the M3, rv32 and i386 nothing references
	// it and --gc-sections drops it.
	//
	// Numeric runtimes.  Both are always linked: `fix16_*` is the small/fast
	// explicit API, and ordinary `float` syntax resolves to the IEEE runtime.
	// A program that uses neither keeps neither: every function has its own
	// section, so --gc-sections drops the unused ones and the image does not
	// grow.  softfloat-min only changes which semantics softfloat32.c is
	// compiled with (see TOS_SOFTFLOAT_MIN there).
	common := []string{"crt0_" + b.Target.Arch + ".S", "lib.c", "syscall.c",
		"heap.c", "aeabi.c", "fix16.c", "floatprint.c", "faeabi.c",
		"softfloat32.c"}

	// Threads are optional code, not a kernel feature: when the config turns
	// them on, one more library and its per-arch switch are linked into every
	// program, and --gc-sections drops both from a program that never calls
	// thread_create().  When the config is off, the API does not exist at all
	// (lib.h hides it), so a stale call is a link error rather than silence.
	if b.Threads {
		common = append(common, "thread.c", "thread_switch.S")
	}

	// Kernel threads are an experimental kernel feature with a small
	// user-space shim (kthread.c).  featureDefines() supplies TOS_KTHREADS
	// to both the kernel and every program, so the API exists exactly when
	// the kernel can honour it.
	if b.feature("kthreads") {
		common = append(common, "kthread.c")
	}

	// The numeric runtimes are size-sensitive and not on any hot kernel path;
	// -Oz saves ~20% over the tree default (-O2) without changing semantics.
	sizeOpt := map[string]bool{
		"softfloat32.c": true, "floatprint.c": true, "fix16.c": true,
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
			if b.UserExclude[name] {
				continue
			}
			dirs = append(dirs, name)
			continue
		}
		if !strings.HasSuffix(name, ".c") {
			continue
		}
		prog := strings.TrimSuffix(name, ".c")
		if b.UserExclude[prog] {
			continue
		}
		if name == "lib.c" || name == "syscall.c" || name == "heap.c" ||
			name == "aeabi.c" ||
			name == "softfloat32.c" || name == "fix16.c" ||
			name == "floatprint.c" || name == "faeabi.c" ||
			name == "thread.c" || name == "kthread.c" {
			continue
		}
		/* The thread demo needs the feature it demonstrates; leaving it in
		 * an image built without threads would be a program that can only
		 * say "off". */
		if name == "threadtest.c" && !b.Threads {
			continue
		}
		if name == "mailtest.c" && !b.Mailbox {
			continue
		}
		if name == "threadbench.c" && !b.feature("kthreads") {
			continue
		}
		if name == "kthreadtest.c" && !b.feature("kthreads") {
			continue
		}
		if name == "prodcons.c" && !b.feature("kthreads") {
			continue
		}
		/* A driver's command is useless without the driver, and leaving it
		 * in would only print "this board has none". */
		switch prog {
		case "pwm":
			if !b.feature("pwm") {
				continue
			}
		case "uart":
			if !b.feature("uart1") {
				continue
			}
		case "spi":
			if !b.feature("spi") {
				continue
			}
		case "i2c":
			if !b.feature("i2c") {
				continue
			}
		case "dma":
			if !b.feature("dma") {
				continue
			}
		case "pio":
			if !b.feature("pio") {
				continue
			}
		}
		progs = append(progs, prog)
	}
	sort.Strings(progs)
	sort.Strings(dirs)

	optFlags := map[string]string{}
	for _, dir := range dirs {
		if raw, err := os.ReadFile(filepath.Join(udir, dir, "opt")); err == nil {
			if v := strings.TrimSpace(string(raw)); v != "" {
				optFlags[dir] = v
				b.logf("      OPT %s %s\n", dir, v)
			}
		}
	}

	commonA, progsA, err := b.compileUserTree(udir, incs, userCFlags, "user",
		common, progs, dirs, sizeOpt, optFlags)
	if err != nil {
		return nil, err
	}
	isDir := map[string]bool{}
	for _, d := range dirs {
		isDir[d] = true
	}

	var bins []string
	var straddlers []string
	for _, name := range append(append([]string{}, progs...), dirs...) {
		if len(progsA[name]) == 0 {
			continue // a program directory with no sources in this build
		}
		out := filepath.Join(b.BuildDir, "user", "bin", name)
		if err := b.linkPIE(append(append([]string{}, commonA...), progsA[name]...),
			filepath.Join(udir, "user.ld"), out); err != nil {
			return nil, err
		}
		if autoRVC && imageStraddles(out) {
			straddlers = append(straddlers, name)
		}
		b.logf("      LD  bin/%s\n", name)
		bins = append(bins, name)
	}

	if len(straddlers) > 0 {
		// Compile the no-RVC tree only for the offenders: the common runtime
		// once (the straddle can be in libc) and their own sources.
		var noProgs, noDirs []string
		for _, name := range straddlers {
			if isDir[name] {
				noDirs = append(noDirs, name)
			} else {
				noProgs = append(noProgs, name)
			}
		}
		commonB, progsB, err := b.compileUserTree(udir, incs,
			append(append([]string{}, userCFlags...), isaNoC...),
			filepath.Join("user", "noc"), common, noProgs, noDirs, sizeOpt, optFlags)
		if err != nil {
			return nil, err
		}
		for _, name := range straddlers {
			out := filepath.Join(b.BuildDir, "user", "bin", name)
			if err := b.linkPIE(append(append([]string{}, commonB...), progsB[name]...),
				filepath.Join(udir, "user.ld"), out); err != nil {
				return nil, err
			}
			if imageStraddles(out) {
				return nil, fmt.Errorf(
					"bin/%s: still crosses a protection-block boundary after rebuilding without RVC", name)
			}
			b.logf("      RVC bin/%s: rebuilt without compressed instructions\n", name)
		}
	}
	return bins, nil
}

// compileUserTree compiles the common runtime and every program of one ISA
// variant.  base is the shared user flag set (the target's march included;
// for the no-RVC variant its -march is repeated at the end so it wins).  sub
// is the object directory, which keeps the two variants apart.
func (b *Builder) compileUserTree(udir string, incs, base []string, sub string,
	common, progs, dirs []string, sizeOpt map[string]bool,
	optFlags map[string]string) ([]string, map[string][]string, error) {

	outRoot := filepath.Join(b.BuildDir, sub)
	if err := os.MkdirAll(outRoot, 0o755); err != nil {
		return nil, nil, err
	}
	obj := func(name string) string { return filepath.Join(outRoot, name+".o") }

	var tasks []task
	var commonObjs []string
	for _, name := range common {
		o := obj(strings.TrimSuffix(name, filepath.Ext(name)))
		commonObjs = append(commonObjs, o)
		flags := append(append([]string{}, base...), isaFrom(base)...)
		if strings.HasSuffix(name, ".S") {
			// assembler: no -fPIE, it just emits relative code.  The ISA flag
			// still matters, or clang expands `j` to `c.j`.
			flags = isaFrom(base)
		} else if sizeOpt[name] {
			flags = append(flags, "-Oz")
		}
		srcCopy, objCopy, flagsCopy := filepath.Join(udir, name), o, flags
		tasks = append(tasks, task{fn: func() error {
			return b.compile(srcCopy, objCopy, incs, flagsCopy...)
		}})
	}

	progObjs := map[string][]string{}
	for _, prog := range progs {
		o := obj(prog)
		progObjs[prog] = []string{o}
		flags := append(append([]string{}, base...), isaFrom(base)...)
		srcCopy, objCopy, flagsCopy := filepath.Join(udir, prog+".c"), o, flags
		tasks = append(tasks, task{fn: func() error {
			return b.compile(srcCopy, objCopy, incs, flagsCopy...)
		}})
	}
	for _, dir := range dirs {
		srcs, err := filepath.Glob(filepath.Join(udir, dir, "*.c"))
		if err != nil {
			return nil, nil, err
		}
		sort.Strings(srcs)
		if len(srcs) == 0 {
			continue
		}
		if err := os.MkdirAll(filepath.Join(outRoot, dir), 0o755); err != nil {
			return nil, nil, err
		}
		flags := append(append([]string{}, base...), isaFrom(base)...)
		if extra := optFlags[dir]; extra != "" {
			flags = append(flags, extra)
		}
		for _, src := range srcs {
			baseName := strings.TrimSuffix(filepath.Base(src), ".c")
			o := filepath.Join(outRoot, dir, baseName+".o")
			progObjs[dir] = append(progObjs[dir], o)
			srcCopy, objCopy, flagsCopy := src, o, flags
			tasks = append(tasks, task{fn: func() error {
				return b.compile(srcCopy, objCopy, incs, flagsCopy...)
			}})
		}
	}
	if err := b.runTasks(tasks); err != nil {
		return nil, nil, err
	}
	return commonObjs, progObjs, nil
}

// isaFrom returns the last -march flag of a user flag set.  The last -march on
// the command line wins, so re-appending it after the shared flags is how the
// no-RVC variant overrides the target's.  Empty for ARM and x86.
func isaFrom(flags []string) []string {
	for i := len(flags) - 1; i >= 0; i-- {
		if strings.HasPrefix(flags[i], "-march=") {
			return []string{flags[i]}
		}
	}
	return nil
}

// imageStraddles reports whether an instruction in the image's executable
// segment reaches across a page boundary (see instructionStraddle).
func imageStraddles(path string) bool {
	data, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	segs, err := executableSegments(data)
	if err != nil {
		return false
	}
	for _, s := range segs {
		if _, _, ok := instructionStraddle(s.code, uint32(s.vaddr)); ok {
			return true
		}
	}
	return false
}

func (b *Builder) buildFSImage(bins []string) error {
	b.logf("littlefs image\n")

	var files []RamFile
	for _, prog := range bins {
		data, err := os.ReadFile(filepath.Join(b.BuildDir, "user", "bin", prog))
		if err != nil {
			return err
		}
		if err := b.checkWindow("bin/"+prog, data); err != nil {
			return err
		}
		if err := b.checkCodePages("bin/"+prog, data); err != nil {
			return err
		}
		files = append(files, RamFile{Name: "bin/" + prog, Data: data})
	}

	/* #5: with `exec_mock` on, also stage one MOCKTOS container so a QEMU run
	 * can prove that a second container handler loads through the same table.
	 * Production builds never define the feature, so neither the file nor the
	 * handler exists there. */
	if b.feature("exec_mock") {
		data, err := os.ReadFile(filepath.Join(b.BuildDir, "user", "bin", "hello"))
		if err != nil {
			return err
		}
		mock := make([]byte, 24+len(data))
		copy(mock, "MOCKTOS\x00")
		n := uint32(len(data))
		mock[8] = byte(n)
		mock[9] = byte(n >> 8)
		mock[10] = byte(n >> 16)
		mock[11] = byte(n >> 24)
		if w, ok := windowNote(data); ok {
			le := []uint32{w.span, w.stack, w.heap}
			for i, v := range le {
				off := 12 + i*4
				mock[off] = byte(v)
				mock[off+1] = byte(v >> 8)
				mock[off+2] = byte(v >> 16)
				mock[off+3] = byte(v >> 24)
			}
		}
		copy(mock[24:], data)
		files = append(files, RamFile{Name: "bin/hello.mock", Data: mock})
	}

	extra, err := collectFiles(b.Root)
	if err != nil {
		return err
	}
	// The PIO examples are only useful (and only fit) on builds with the PIO
	// feature; the x86 image is already near its 300 KiB ramdisk limit.
	if !b.feature("pio") {
		kept := extra[:0]
		for _, f := range extra {
			if strings.HasPrefix(f.Name, "pio/") {
				continue
			}
			kept = append(kept, f)
		}
		extra = kept
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
	/* The board's ramdisk_sectors is a starting point, not a budget: it is the
	   size of the image that gets flashed, and the kernel grows the mounted
	   filesystem to the real partition (lfs_fs_grow) either way.  On a
	   flash-backed board the only hard limit is the partition itself, so a
	   staged set that does not fit (a test corpus, say) doubles the image
	   instead of failing the build.  Disk/embedded ramdisks stay fixed: there
	   the image is loaded into RAM as-is, and silently growing it is worse
	   than a clear build error. */
	size := b.Target.RamdiskSectors * 512
	maxSize := size
	if b.BoardConfig != nil && b.Target.FSBacking == "flash" {
		if fb, off := b.BoardConfig.FlashBytes, b.BoardConfig.LFSOffset; fb > off {
			maxSize = fb - off
		}
	}
	tool := filepath.Join(b.BuildDir, "mklfs.exe")
	for {
		args := []string{}
		if b.BoardConfig != nil && b.BoardConfig.WindowStackAlign != 0 {
			/* The kernel refuses a stack hint that does not divide the grain
			 * (stack_request_ok); fail here instead, with the program name. */
			args = append(args, "--grain",
				fmt.Sprintf("%d", b.BoardConfig.WindowGrain))
		}
		args = append(args, ramdiskPath, fmt.Sprintf("%d", size), stage)
		args = append(args, names...)
		err := b.runTool(tool, args, "MKFS")
		if err == nil {
			break
		}
		if size >= maxSize {
			return fmt.Errorf("%w (image already at the %d-byte partition)", err, maxSize)
		}
		size *= 2
		if size > maxSize {
			size = maxSize
		}
		if rm := os.Remove(ramdiskPath); rm != nil && !os.IsNotExist(rm) {
			return rm
		}
		b.logf("      ramdisk does not fit, retrying at %d KiB\n", size/1024)
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
