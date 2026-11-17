package build

import "testing"

// TestRP2350RoundToSector pins the flash-sector rounding an embedded image
// needs.  The RP2350 boot ROM keeps the BOOTSEL drive mounted (no reboot) when
// the UF2's flat image ends mid-sector; flash-backed images are already
// aligned because the littlefs partition is 512 KiB, but an embedded image
// ends at endOff+20 and was not.  Found on the ATK board with rp2350-generic.
func TestRP2350RoundToSector(t *testing.T) {
	cases := []struct{ in, want uint32 }{
		{0, 0},
		{1, 4096},
		{4095, 4096},
		{4096, 4096},
		{4097, 8192},
		{416756, 417792}, // a real embedded-image length (0x65BF4)
	}
	for _, c := range cases {
		if got := rp2350RoundToSector(c.in); got != c.want {
			t.Errorf("rp2350RoundToSector(%d) = %d, want %d", c.in, got, c.want)
		}
	}
	// The invariant the ROM actually needs: the result is sector-aligned and
	// never smaller than the input.
	for _, n := range []uint32{0, 1, 4095, 4096, 5000, 416756, 884736} {
		got := rp2350RoundToSector(n)
		if got%4096 != 0 || got < n {
			t.Errorf("rp2350RoundToSector(%d) = %d: not sector-aligned or too small", n, got)
		}
	}
}
