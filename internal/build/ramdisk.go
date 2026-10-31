package build

import (
	"debug/elf"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

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
