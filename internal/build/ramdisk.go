package build

import (
	"debug/elf"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// TrimELFSections drops the section header table - and the section name
// string table that only exists to serve it - from a linked user program.
//
// kernel/elf.c finds PT_LOAD / PT_NOTE / PT_DYNAMIC through the program
// header table and never reads e_shoff, so those bytes are ballast: across
// the 36 programs actually staged into the image they average 336 B each
// (12 088 B in total).  They also inflate the
// LZ4TOS raw_size, which is exactly how much kernel heap exec() allocates
// before it can load the image.  -s strips symbols but cannot remove the
// section table itself.
//
// Only the safe shape is rewritten: little-endian ELF32, section table at
// EOF, and nothing any program header points at living in the tail.  A
// linker that lays its output out differently keeps its bytes - saving a
// few hundred bytes is never worth a program that will not load.  All four
// section header fields are zeroed so the result is a valid "no sections"
// ELF rather than a truncated one.  Returns the number of bytes removed.
func TrimELFSections(path string) (int, error) {
	st, err := os.Stat(path)
	if err != nil {
		return 0, err
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	if len(data) < 52 || string(data[:4]) != "\x7fELF" ||
		data[4] != 1 || data[5] != 1 { // ELFCLASS32, ELFDATA2LSB
		return 0, nil
	}

	shoff := uint64(binary.LittleEndian.Uint32(data[32:]))
	shentsize := uint64(binary.LittleEndian.Uint16(data[46:]))
	shnum := uint64(binary.LittleEndian.Uint16(data[48:]))
	if shoff == 0 || shnum == 0 || shentsize == 0 {
		return 0, nil
	}
	if shoff+shentsize*shnum != uint64(len(data)) {
		return 0, nil // section table is not the tail
	}

	phoff := uint64(binary.LittleEndian.Uint32(data[28:]))
	phentsize := uint64(binary.LittleEndian.Uint16(data[42:]))
	phnum := uint64(binary.LittleEndian.Uint16(data[44:]))
	if phentsize < 32 || phnum == 0 ||
		phoff+phentsize*phnum > uint64(len(data)) {
		return 0, nil
	}

	// Everything the loader can reach ends here.  Trimming to that point
	// also removes .shstrtab and any padding before the section table.
	var keep uint64
	for i := uint64(0); i < phnum; i++ {
		o := phoff + i*phentsize
		poff := uint64(binary.LittleEndian.Uint32(data[o+4:]))
		filesz := uint64(binary.LittleEndian.Uint32(data[o+16:]))
		if poff > uint64(len(data)) || poff+filesz > uint64(len(data)) {
			return 0, nil
		}
		if poff+filesz > keep {
			keep = poff + filesz
		}
	}
	if keep == 0 || keep > shoff || keep < 52 {
		return 0, nil
	}

	binary.LittleEndian.PutUint32(data[32:], 0) // e_shoff
	binary.LittleEndian.PutUint16(data[46:], 0) // e_shentsize
	binary.LittleEndian.PutUint16(data[48:], 0) // e_shnum
	binary.LittleEndian.PutUint16(data[50:], 0) // e_shstrndx
	trimmed := data[:keep]
	if err := os.WriteFile(path, trimmed, st.Mode().Perm()); err != nil {
		return 0, err
	}
	return len(data) - len(trimmed), nil
}

// RamFile is a single file inside the TinyOS filesystem image.
type RamFile struct {
	Name string
	Data []byte
}

// ELFToFlat converts an ELF32 executable into a raw image laid out at its
// link-time virtual addresses. The returned base is the lowest VMA.
//
// Only the file-backed part of each PT_LOAD segment is emitted: the tail of
// a segment where p_memsz > p_filesz is .bss, which the kernel zeroes itself
// (kernel/entry.S), so writing those zeros into the disk image would just
// waste sectors and push the kernel past the boot loader's read budget.
func ELFToFlat(path string) (data []byte, base uint64, err error) {
	raw, err := os.Open(path)
	if err != nil {
		return nil, 0, err
	}
	defer raw.Close()

	f, err := elf.NewFile(raw)
	if err != nil {
		return nil, 0, err
	}
	defer f.Close()

	if f.Class != elf.ELFCLASS32 || f.Machine != elf.EM_386 {
		return nil, 0, fmt.Errorf("%s: not an i386 ELF32 file", path)
	}

	type seg struct {
		vaddr, filesz, off uint64
		buf                []byte
	}
	var segs []seg
	base = ^uint64(0)
	var end uint64

	for _, p := range f.Progs {
		if p.Type != elf.PT_LOAD {
			continue
		}
		s := seg{vaddr: p.Vaddr, filesz: p.Filesz, off: p.Off}
		if s.filesz > 0 {
			s.buf = make([]byte, s.filesz)
			if _, err := io.ReadFull(io.NewSectionReader(raw, int64(p.Off), int64(p.Filesz)), s.buf); err != nil {
				return nil, 0, err
			}
			// extend with p_filesz, not p_memsz (.bss stays off the image)
			if p.Vaddr+p.Filesz > end {
				end = p.Vaddr + p.Filesz
			}
		}
		segs = append(segs, s)
		if p.Vaddr < base {
			base = p.Vaddr
		}
	}
	if len(segs) == 0 {
		return nil, 0, fmt.Errorf("%s: no loadable segments", path)
	}
	if end <= base {
		return nil, 0, fmt.Errorf("%s: no file-backed data in any PT_LOAD segment", path)
	}

	out := make([]byte, end-base)
	for _, s := range segs {
		if s.filesz == 0 {
			continue
		}
		copy(out[s.vaddr-base:s.vaddr-base+s.filesz], s.buf)
	}
	return out, base, nil
}

// ELFEndAddress returns the highest virtual address occupied by the ELF,
// including .bss (p_memsz), so the layout can be checked for collisions.
// ELFEndBelow is ELFEndAddress with an upper bound: segments at or above limit
// are ignored.  A kernel that executes its code in place has two regions - one
// in RAM, bounded by the task area (__app_base), and one in the flash window
// - and the headroom only means anything for the first.
func ELFEndBelow(path string, limit uint64) (uint64, error) {
	f, err := elf.Open(path)
	if err != nil {
		return 0, err
	}
	defer f.Close()

	var end uint64
	for _, p := range f.Progs {
		if p.Type != elf.PT_LOAD || p.Vaddr >= limit {
			continue
		}
		if v := p.Vaddr + p.Memsz; v > end {
			end = v
		}
	}
	return end, nil
}

func ELFEndAddress(path string) (uint64, error) {
	f, err := elf.Open(path)
	if err != nil {
		return 0, err
	}
	defer f.Close()

	var end uint64
	for _, p := range f.Progs {
		if p.Type != elf.PT_LOAD {
			continue
		}
		if v := p.Vaddr + p.Memsz; v > end {
			end = v
		}
	}
	if end == 0 {
		return 0, fmt.Errorf("%s: no loadable segments", path)
	}
	return end, nil
}

func ReadFileIfExists(path string) ([]byte, bool) {
	b, err := os.ReadFile(path)
	return b, err == nil
}

func collectFiles(root string) ([]RamFile, error) {
	var files []RamFile
	dir := filepath.Join(root, "files")
	if _, err := os.Stat(dir); err != nil {
		return files, nil
	}
	err := filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		rel, err := filepath.Rel(dir, path)
		if err != nil {
			return err
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		files = append(files, RamFile{Name: filepath.ToSlash(rel), Data: data})
		return nil
	})
	return files, err
}

func relName(root, path string) string {
	r, err := filepath.Rel(root, path)
	if err != nil {
		return path
	}
	return strings.ReplaceAll(r, `\`, "/")
}
