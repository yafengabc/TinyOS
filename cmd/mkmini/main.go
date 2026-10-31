package main

// mkmini is a standalone converter: it turns a linked ELF32 executable into
// TinyOS's mini container format (optionally LZ4 compressed) and can dump
// the header of an existing container.
//
// usage:
//   mkmini <input.elf> <output.tosx> [-nocompress]
//   mkmini -i <file.tosx>
//
// It shares the conversion code with `tos build`, so what you see here is
// exactly what the build produces.

import (
	"flag"
	"fmt"
	"os"

	"tinyos/internal/build"
)

func main() {
	dump := flag.Bool("i", false, "inspect an existing .tosx file instead of converting")
	verify := flag.Bool("v", false, "verify a container decodes to exactly file_size bytes")
	noCompress := flag.Bool("nocompress", false, "store the image uncompressed")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, `usage:
  mkmini [-nocompress] <input.elf> <output.tosx>
  mkmini -i <file.tosx>
  mkmini -v <file.tosx>
`)
	}
	flag.Parse()

	if *verify {
		args := flag.Args()
		if len(args) != 1 {
			flag.Usage()
			os.Exit(2)
		}
		data, err := os.ReadFile(args[0])
		if err != nil {
			fail(err)
		}
		img, err := build.ParseMini(data)
		if err != nil {
			fail(err)
		}
		got, err := build.VerifyMini(img)
		if err != nil {
			fmt.Printf("FAIL %s: %v\n", args[0], err)
			os.Exit(1)
		}
		fmt.Printf("OK %s: %d bytes decoded (mem %d, entry 0x%08x)\n",
			args[0], got, img.MemSize, img.Entry)
		return
	}

	if *dump {
		args := flag.Args()
		if len(args) != 1 {
			flag.Usage()
			os.Exit(2)
		}
		data, err := os.ReadFile(args[0])
		if err != nil {
			fail(err)
		}
		img, err := build.ParseMini(data)
		if err != nil {
			fail(err)
		}
		fmt.Printf("magic      %q\n", data[0:4])
		fmt.Printf("version    %d\n", data[4])
		fmt.Printf("flags      0x%02x%s\n", data[5], compressNote(img.Compressed))
		fmt.Printf("entry      0x%08x\n", img.Entry)
		fmt.Printf("load       0x%08x\n", img.LoadAddr)
		fmt.Printf("mem size   %d\n", img.MemSize)
		fmt.Printf("payload    %d (file=%d)\n", img.PayloadLen, img.FileSize)
		fmt.Printf("total      %d bytes\n", len(data))
		return
	}

	args := flag.Args()
	if len(args) != 2 {
		flag.Usage()
		os.Exit(2)
	}

	img, err := build.ELFToMini(args[0], !*noCompress)
	if err != nil {
		fail(err)
	}
	if err := os.WriteFile(args[1], img.Data, 0o644); err != nil {
		fail(err)
	}

	st, err := os.Stat(args[0])
	if err != nil {
		fail(err)
	}
	ratio := 100 * float64(len(img.Data)) / float64(st.Size())
	fmt.Printf("%s: %d -> %d bytes (%.0f%%)%s\n",
		args[0], st.Size(), len(img.Data), ratio, compressNote(img.Compressed))
}

func compressNote(compressed bool) string {
	if compressed {
		return " (lz4)"
	}
	return " (stored)"
}

func fail(err error) {
	fmt.Fprintln(os.Stderr, "mkmini:", err)
	os.Exit(1)
}
