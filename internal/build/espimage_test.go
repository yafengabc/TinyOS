package build

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func twoSegments() ESPImage {
	return ESPImage{
		Entry: 0x40380000,
		Segments: []ESPSegment{
			{Addr: 0x3FC80000, Data: []byte{1, 2, 3, 4}},
			{Addr: 0x40380000, Data: []byte{5, 6, 7, 8}},
		},
		FlashMode: ESPFlashDIO,
	}
}

func TestESPImageHeaderAndChecksum(t *testing.T) {
	img, err := BuildESPImage(twoSegments())
	if err != nil {
		t.Fatalf("BuildESPImage: %v", err)
	}

	if img[0] != ESPImageMagic {
		t.Errorf("magic = 0x%x, want 0x%x", img[0], ESPImageMagic)
	}
	if img[1] != 2 {
		t.Errorf("segment count = %d, want 2", img[1])
	}
	if img[2] != ESPFlashDIO {
		t.Errorf("flash mode = %d, want %d (DIO)", img[2], ESPFlashDIO)
	}
	if img[3] != ESPFlash4MB40MHz {
		t.Errorf("flash size/freq = 0x%x, want 0x%x", img[3], ESPFlash4MB40MHz)
	}
	if got := binary.LittleEndian.Uint32(img[4:]); got != 0x40380000 {
		t.Errorf("entry = 0x%x, want 0x40380000", got)
	}
	if img[8] != 0xEE {
		t.Errorf("WP pin = 0x%x, want 0xEE (disabled)", img[8])
	}
	if got := binary.LittleEndian.Uint16(img[12:]); got != ESPChipC3 {
		t.Errorf("chip id = %d, want %d", got, ESPChipC3)
	}
	if img[23] != 0 {
		t.Errorf("hash-appended = %d, want 0 without SHA256", img[23])
	}

	/* 24 header + 2 * (8 + 4) segments = 48, padded to 63, plus the
	   checksum byte. */
	if len(img) != 64 {
		t.Fatalf("image length = %d, want 64", len(img))
	}
	if len(img)%16 != 0 {
		t.Errorf("image length %d is not a multiple of 16", len(img))
	}

	if got := binary.LittleEndian.Uint32(img[24:]); got != 0x3FC80000 {
		t.Errorf("segment 0 address = 0x%x", got)
	}
	if got := binary.LittleEndian.Uint32(img[28:]); got != 4 {
		t.Errorf("segment 0 length = %d", got)
	}
	if got := binary.LittleEndian.Uint32(img[36:]); got != 0x40380000 {
		t.Errorf("segment 1 address = 0x%x", got)
	}

	/* Recompute the XOR here rather than calling espChecksum, so the test
	   would notice a broken implementation instead of agreeing with it.
	   Payload bytes only - see TestESPImageChecksumIgnoresHeaders for why. */
	ck := byte(0xEF)
	for _, s := range twoSegments().Segments {
		for _, b := range s.Data {
			ck ^= b
		}
	}
	if img[len(img)-1] != ck {
		t.Errorf("checksum = 0x%x, want 0x%x", img[len(img)-1], ck)
	}
}

/* The ROM XORs the segment payloads and nothing else.  Linking the same
 * bytes at a different address therefore keeps the checksum, which is the
 * cheapest way to state the rule: the alternative reading ("XOR everything
 * before the checksum byte") would change it. */
func TestESPImageChecksumIgnoresHeaders(t *testing.T) {
	a := ESPImage{Entry: 0x40380000, Segments: []ESPSegment{
		{Addr: 0x40380000, Data: []byte{0xAA, 0xBB, 0xCC, 0xDD}},
	}}
	b := ESPImage{Entry: 0x40380004, Segments: []ESPSegment{
		{Addr: 0x40380004, Data: []byte{0xAA, 0xBB, 0xCC, 0xDD}},
	}}

	ia, err := BuildESPImage(a)
	if err != nil {
		t.Fatalf("BuildESPImage(a): %v", err)
	}
	ib, err := BuildESPImage(b)
	if err != nil {
		t.Fatalf("BuildESPImage(b): %v", err)
	}
	if ia[len(ia)-1] != ib[len(ib)-1] {
		t.Errorf("checksum changed with the load address (0x%x vs 0x%x); the headers must not be covered",
			ia[len(ia)-1], ib[len(ib)-1])
	}

	/* Sanity check in the other direction: different payload, different
	   checksum.  A constant would satisfy the test above. */
	b.Segments[0].Data = []byte{0xAA, 0xBB, 0xCC, 0xDE}
	ib, err = BuildESPImage(b)
	if err != nil {
		t.Fatalf("BuildESPImage(b'): %v", err)
	}
	if ia[len(ia)-1] == ib[len(ib)-1] {
		t.Errorf("checksum did not follow the payload")
	}
}

func TestESPImageSHA256(t *testing.T) {
	img := twoSegments()
	img.AppendSHA256 = true

	out, err := BuildESPImage(img)
	if err != nil {
		t.Fatalf("BuildESPImage: %v", err)
	}
	if out[23] != 1 {
		t.Fatalf("hash-appended = %d, want 1", out[23])
	}
	if len(out) != 96 {
		t.Fatalf("length = %d, want 64 + 32 digest bytes", len(out))
	}
	sum := sha256.Sum256(out[:64])
	if !bytes.Equal(sum[:], out[64:]) {
		t.Errorf("appended digest does not cover the image")
	}
}

func TestESPImageSegmentOrder(t *testing.T) {
	img := ESPImage{
		Entry: 0x3FC80000,
		Segments: []ESPSegment{
			{Addr: 0x40380000, Data: []byte{5, 6, 7, 8}},
			{Addr: 0x3FC80000, Data: []byte{1, 2, 3, 4}},
		},
	}
	out, err := BuildESPImage(img)
	if err != nil {
		t.Fatalf("BuildESPImage: %v", err)
	}
	if got := binary.LittleEndian.Uint32(out[24:]); got != 0x3FC80000 {
		t.Errorf("first segment = 0x%x, want the lowest address (0x3FC80000)", got)
	}
}

func TestESPImageRejects(t *testing.T) {
	cases := []struct {
		name string
		img  ESPImage
		want string
	}{
		{
			name: "no segments",
			img:  ESPImage{Entry: 0x3FC80000},
			want: "no segments",
		},
		{
			name: "overlap",
			/* Adjacent is fine (0x3FC80000 + 4 == 0x3FC80004), so the
			   second segment has to start inside the first one. */
			img: ESPImage{Entry: 0x3FC80000, Segments: []ESPSegment{
				{Addr: 0x3FC80000, Data: []byte{1, 2, 3, 4, 5, 6, 7, 8}},
				{Addr: 0x3FC80004, Data: []byte{9, 10, 11, 12}},
			}},
			want: "overlap",
		},
		{
			name: "entry outside every segment",
			img: ESPImage{Entry: 0x3FC90000, Segments: []ESPSegment{
				{Addr: 0x3FC80000, Data: []byte{1, 2, 3, 4}},
			}},
			want: "entry",
		},
		{
			name: "linked for another board",
			img: ESPImage{Entry: 0x80000000, Segments: []ESPSegment{
				{Addr: 0x80000000, Data: []byte{1, 2, 3, 4}},
			}},
			want: "not in ESP32-C3 memory",
		},
		{
			name: "unaligned address",
			img: ESPImage{Entry: 0x3FC80002, Segments: []ESPSegment{
				{Addr: 0x3FC80002, Data: []byte{1, 2, 3, 4}},
			}},
			want: "aligned",
		},
		{
			name: "unaligned length",
			img: ESPImage{Entry: 0x3FC80000, Segments: []ESPSegment{
				{Addr: 0x3FC80000, Data: []byte{1, 2, 3}},
			}},
			want: "multiple of 4",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			_, err := BuildESPImage(c.img)
			if err == nil {
				t.Fatalf("expected an error")
			}
			if !strings.Contains(err.Error(), c.want) {
				t.Errorf("error = %q, want it to mention %q", err, c.want)
			}
		})
	}
}

func TestESPImageTooManySegments(t *testing.T) {
	img := ESPImage{Entry: 0x3FC80000}
	for i := 0; i < 17; i++ {
		img.Segments = append(img.Segments, ESPSegment{
			Addr: 0x3FC80000 + uint32(i)*4,
			Data: []byte{1, 2, 3, 4},
		})
	}
	if _, err := BuildESPImage(img); err == nil || !strings.Contains(err.Error(), "at most") {
		t.Fatalf("err = %v, want a segment-count error", err)
	}
}

/* The qemu-virt kernel is the file most likely to be handed to this packer by
 * mistake.  If it exists, it must be refused with a message that names the
 * address rather than producing an image the chip cannot boot. */
func TestESPImageFromELFRefusesForeignBoard(t *testing.T) {
	path := filepath.Join("..", "..", "build", "kernel-rv32.elf")
	if _, err := os.Stat(path); err != nil {
		t.Skip("kernel-rv32.elf not built")
	}
	_, _, err := ESPImageFromELF(path, false)
	if err == nil {
		t.Fatalf("packaged a qemu-virt kernel without complaint")
	}
	if !strings.Contains(err.Error(), "ESP32-C3 memory") {
		t.Errorf("error = %q, want it to mention the address space", err)
	}
}
