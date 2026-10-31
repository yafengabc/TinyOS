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

	// The kernel (image + .bss) must end below the first page the physical
	// allocator manages (PHYS_START = 0x400000 in kernel/pmm.h), otherwise
	// the allocator would hand out pages that overlap the kernel.
	KernelMaxEnd = 0x400000
)

// Target describes which architecture/board to build for.
type Target struct {
	Name    string // "x86" or "rv32"
	Arch    string // arch subdirectory
	Board   string // board subdirectory
	Triple  string // clang target triple
	Machine string // zig -mcpu value ("" = default)
}

var Targets = map[string]Target{
	"x86":  {Name: "x86", Arch: "x86", Board: "pc", Triple: "x86-freestanding"},
	"rv32": {Name: "rv32", Arch: "rv32", Board: "qemu-virt", Triple: "riscv32-freestanding", Machine: "generic_rv32"},
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
		opt = "-Os"
	}

	// Architecture specific flags.  Everything else is shared so both ports
	// see the same C dialect, optimiser and warning set.
	var archFlags []string
	if b.Target.Name == "rv32" {
		archFlags = []string{"-mabi=ilp32", "-mcmodel=medany"}
		if b.Target.Machine != "" {
			archFlags = append(archFlags, "-mcpu="+b.Target.Machine+"+c")
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
		// Size first: this is a tiny OS, and -Os is measurably smaller than
		// -O2 for every part without slowing the boot measurably.  Measured
		// with clang 23.1.4 / lld 23.1.4 (x86, 10 programs, Sep 2026):
		//
		//   kernel image      50,132 -> 40,708 bytes  (-19%)
		//   user programs     22,360 -> 19,896 bytes  (-11%)
		//   hello              2,216 ->  2,008 bytes  ( -9%)
		//
		// Most of each user program is printf (~960 bytes of .text), which
		// every binary that formats output links in.  Set TOS_OPT=-O2 (or
		// -Og) when debugging.
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

func (b *Builder) link(objs []string, script, out string, extra ...string) error {
	var args []string
	var tool string

	if b.Target.Name != "x86" {
		// zig cc insists on pulling in its compiler_rt, which is built with
		// the hard-float ABI and refuses to mix with our soft-float
		// objects.  Drive the bundled lld directly instead.
		tool = b.TC.CC[0]
		args = []string{"ld.lld", "-m", "elf32lriscv"}
	} else {
		tool = b.TC.LD[0]
		args = append([]string{}, b.TC.LD[1:]...)
	}

	args = append(args, "-T", script, "-nostdlib", "-static")
	args = append(args, extra...)
	if b.Target.Name != "x86" {
		// no symbol table in the final image: it is only needed for
		// debugging, and `zm` strips it without touching the loadable
		// segments
		args = append(args, "-s")
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
	args := append([]string{}, b.TC.LD[1:]...)
	args = append(args,
		"-T", script,
		"-nostdlib",
	)
	// zig cc parses -Wl arguments itself and rejects -pie, while clang only
	// forwards -pie to lld through the -Wl form (a bare -pie is dropped as
	// an unused compile flag).  Pick the spelling the driver understands.
	if b.TC.UsesZig {
		args = append(args, "-pie")
	} else {
		args = append(args, "-Wl,-pie")
	}
	args = append(args,
		"-Wl,--gc-sections",
		"-Wl,-z,max-page-size=16",
		"-Wl,-z,now",
		"-s",
		"-o", out,
	)
	args = append(args, objs...)
	return b.runTool(b.TC.LD[0], args, "LD")
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

	// RV32/ARM boot straight from the loader or ROM, so there is no boot
	// sector, no ATA image and (for now) no filesystem partition.
	if b.Target.Name != "x86" {
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
	if err := b.buildImage(bins); err != nil {
		return err
	}
	return nil
}

func (b *Builder) buildBoot() error {
	b.logf("[1/5] boot sector\n")
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
	b.logf("[2/5] kernel\n")
	kdir := filepath.Join(b.Root, "kernel")
	lfsDir := filepath.Join(b.Root, "third_party", "littlefs")
	incs := []string{filepath.Join(b.Root, "include"), kdir, lfsDir}

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
	// .bss is not part of the image, but it still occupies RAM at the end
	// of the kernel and must end below the managed heap area (0x400000),
	// otherwise pmm_alloc could hand out pages that overlap the kernel.
	end, err := ELFEndAddress(elfPath)
	if err != nil {
		return err
	}
	if end > KernelMaxEnd {
		return fmt.Errorf("kernel ends at 0x%x (.bss included), which exceeds the managed RAM start at 0x%x",
			end, KernelMaxEnd)
	}
	b.logf("      image %d / %d bytes, footprint ends at 0x%x (heap at 0x%x, %d KiB headroom)\n",
		len(flat), maxBytes, end, KernelMaxEnd, (KernelMaxEnd-end)/1024)
	return os.WriteFile(filepath.Join(b.BuildDir, "kernel.bin"), flat, 0o644)
}

// buildMklfs compiles the host-side littlefs image builder.  It links the
// vendored littlefs against the host libc so the on-disk format is exactly
// the one the kernel mounts.  We deliberately do not pass the project's
// freestanding include/ shims here: this is a normal host program.
func (b *Builder) buildMklfs() error {
	b.logf("[4/5] mklfs (host tool)\n")
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
	b.logf("[3/5] user programs\n")
	udir := filepath.Join(b.Root, "user")
	incs := []string{udir, filepath.Join(b.Root, "include"), filepath.Join(b.Root, "kernel")}

	// User programs are position independent: the kernel relocates them to
	// the board's APP_BASE, so one binary runs on QEMU and on real hardware.
	// -fPIC (not just -fPIE) is needed because the linker rejects GOT-free
	// absolute references against local symbols in a shared-style link.
	userPIEFlags := []string{"-fPIC"}

	common := []string{"crt0.S", "lib.c", "syscall.c"}
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

func (b *Builder) buildImage(bins []string) error {
	b.logf("[5/5] littlefs image\n")

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
	b.logf("      %s (%d KiB, %d files)\n", relName(b.Root, imgPath), len(img)/1024, len(files))
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
