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
	KernelSectors  = 180 // 90 KiB, loaded at 0x10000
	RamdiskSectors = 600 // 300 KiB littlefs partition
	BootSectors    = 1

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
	Name    string // "x86" or "rv32"
	Arch    string // arch subdirectory
	Board   string // board subdirectory
	Triple  string // clang target triple
	March   string // -march value ("" = whatever the compiler defaults to)
	Machine string // zig -mcpu value ("" = default)
}

var Targets = map[string]Target{
	"x86":  {Name: "x86", Arch: "x86", Board: "pc", Triple: "x86-freestanding"},
	"rv32": {Name: "rv32", Arch: "rv32", Board: "qemu-virt", Triple: "riscv32-unknown-elf", March: "rv32imc"},
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
		opt = "-Oz"
	}

	// Architecture specific flags.  Everything else is shared so both ports
	// see the same C dialect, optimiser and warning set.
	var archFlags []string
	if b.Target.Name == "rv32" {
		// ilp32 = soft float, matching every RV32 MCU we target and keeping
		// the kernel free of compiler_rt's hard-float helpers.
		archFlags = []string{"-mabi=ilp32", "-mcmodel=medany"}
		if b.Target.March != "" {
			archFlags = append(archFlags, "-march="+b.Target.March)
		}
		if b.Target.Machine != "" {
			archFlags = append(archFlags, "-mcpu="+b.Target.Machine)
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
		// Size first: this is a tiny OS, so the default is -Oz (clang's
		// most aggressive size level).  Measured with clang 23.1.4 /
		// lld 23.1.4 (x86, 11 programs, Oct 2026, --gc-sections on):
		//
		//                     -O2      -Os      -Oz
		//   kernel image    42,132   34,916   32,852 bytes
		//   user programs   16,760   15,084   14,476 bytes
		//   LZ4TOS payload  11,533   10,714   10,305 bytes
		//
		// -Oz costs a little speed in hot loops; the shell and the loader
		// are I/O bound, and boot time is unchanged within noise.
		// Most of each user program is printf (~960 bytes of .text), which
		// every binary that formats output links in.
		//
		// Toolchains that reject -Oz (gcc < 12) can fall back with
		// TOS_OPT=-Os; use TOS_OPT=-O2 or -Og when debugging.
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
	args = append(args, w("-s"))
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
	// targets that embed it (RV32), so user programs and mklfs come first.
	// The PC target writes the same image into the boot disk instead.
	if b.Target.Name != "x86" {
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

	var objs []string
	var tasks []task
	for _, src := range srcs {
		name := strings.TrimSuffix(filepath.Base(src), filepath.Ext(src)) + ".o"
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
	// section, which plat_block_* then serves as a RAM disk.
	if b.Target.Name != "x86" {
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
	// bootloader both load the ELF directly.
	if b.Target.Name != "x86" {
		elfPath := filepath.Join(b.BuildDir, "kernel-"+b.Target.Name+".elf")
		script := filepath.Join(b.Root, "arch", b.Target.Arch, "kernel.ld")
		if err := b.link(objs, script, elfPath); err != nil {
			return err
		}
		if end, err := ELFEndAddress(elfPath); err == nil {
			b.logf("      kernel ends at 0x%x\n", end)
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

// writeRamdiskBlob emits the tiny assembly stub that links the littlefs
// image into the kernel as a writable buffer.  It is regenerated on every
// build because the image contents change with the user programs.
func (b *Builder) writeRamdiskBlob() (string, error) {
	img := relName(b.Root, filepath.Join(b.BuildDir, "ramdisk.bin"))
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

	common := []string{"crt0_" + b.Target.Name + ".S", "lib.c", "syscall.c"}
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
	var progs []string
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() || !strings.HasSuffix(name, ".c") {
			continue
		}
		if name == "lib.c" || name == "syscall.c" {
			continue
		}
		progs = append(progs, strings.TrimSuffix(name, ".c"))
	}
	sort.Strings(progs)

	for _, prog := range progs {
		src := filepath.Join(udir, prog+".c")
		obj := filepath.Join(b.BuildDir, "user", prog+".o")
		srcCopy, objCopy := src, obj
		tasks = append(tasks, task{fn: func() error {
			return b.compile(srcCopy, objCopy, incs, userPIEFlags...)
		}})
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

	ramdiskPath := filepath.Join(b.BuildDir, "ramdisk.bin")
	/* Start from a fresh image: mklfs adds to whatever is already there, so
	   a file the guest wrote in a previous run would survive into the next
	   build and quietly change what the image contains. */
	if err := os.Remove(ramdiskPath); err != nil && !os.IsNotExist(err) {
		return err
	}
	args := []string{
		ramdiskPath,
		fmt.Sprintf("%d", RamdiskSectors*512),
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

	ramdisk, err := os.ReadFile(filepath.Join(b.BuildDir, "ramdisk.bin"))
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

	img := make([]byte, 0, (BootSectors+KernelSectors+RamdiskSectors)*512)
	img = append(img, boot[:512]...)
	img = append(img, padTo(kernel, KernelSectors*512)...)
	img = append(img, padTo(ramdisk, RamdiskSectors*512)...)

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
