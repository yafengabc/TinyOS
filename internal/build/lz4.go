package build

import "encoding/binary"

// LZ4 block format, ported from tools/lz4.c so the build does not need a C
// compiler for this step.  The kernel decoder is kernel/lz4.c.

const (
	lz4MinMatch     = 4
	lz4MaxOffset    = 65535
	lz4LastLiterals = 5
	lz4MFLimit      = 12
	lz4HashBits     = 14
	lz4HashSize     = 1 << lz4HashBits
	lz4MaxChain     = 32
)

func lz4CompressBound(n int) int {
	return n + n/255 + 16
}

func lz4Hash(p []byte) uint32 {
	seq := binary.LittleEndian.Uint32(p)
	return (seq * 2654435761) >> (32 - lz4HashBits)
}

func lz4EmitLen(dst []byte, op, n int) int {
	for n >= 255 {
		dst[op] = 255
		op++
		n -= 255
	}
	dst[op] = byte(n)
	return op + 1
}

// lz4CompressBlock encodes src into dst and returns the compressed length.
// It is a greedy hash-chain matcher: good enough for small executables.
func lz4CompressBlock(src, dst []byte) int {
	n := len(src)
	if n == 0 {
		return 0
	}

	head := make([]int32, lz4HashSize)
	for i := range head {
		head[i] = -1
	}
	chain := make([]int32, n)

	lastMatchStart := n - lz4MFLimit
	anchor, ip, op := 0, 0, 0

	for ip <= lastMatchStart {
		h := lz4Hash(src[ip:])
		cand := head[h]
		bestLen, bestOff := 0, 0

		probes := 0
		for c := cand; c >= 0 && probes < lz4MaxChain; probes++ {
			off := ip - int(c)
			if off > lz4MaxOffset {
				break
			}
			maxLen := n - lz4LastLiterals - ip
			if src[int(c)+bestLen] == src[ip+bestLen] &&
				string(src[c:c+lz4MinMatch]) == string(src[ip:ip+lz4MinMatch]) {
				ml := lz4MinMatch
				for ml < maxLen && src[int(c)+ml] == src[ip+ml] {
					ml++
				}
				if ml > bestLen {
					bestLen, bestOff = ml, off
					if ml >= 64 {
						break
					}
				}
			}
			c = chain[c]
		}

		chain[ip] = cand
		head[h] = int32(ip)

		if bestLen < lz4MinMatch {
			ip++
			continue
		}

		// literal run
		lit := ip - anchor
		tokenPos := op
		op++
		if lit >= 15 {
			dst[tokenPos] = 0xF0
			op = lz4EmitLen(dst, op, lit-15)
		} else {
			dst[tokenPos] = byte(lit << 4)
		}
		copy(dst[op:], src[anchor:ip])
		op += lit

		dst[op] = byte(bestOff)
		dst[op+1] = byte(bestOff >> 8)
		op += 2

		mlen := bestLen - lz4MinMatch
		if mlen < 15 {
			dst[tokenPos] |= byte(mlen)
		} else {
			dst[tokenPos] |= 15
			op = lz4EmitLen(dst, op, mlen-15)
		}

		// register the positions covered by the match
		for k := 1; k < bestLen && ip+k <= lastMatchStart; k++ {
			hk := lz4Hash(src[ip+k:])
			chain[ip+k] = head[hk]
			head[hk] = int32(ip + k)
		}

		ip += bestLen
		anchor = ip
	}

	// trailing literals
	lit := n - anchor
	tokenPos := op
	op++
	if lit >= 15 {
		dst[tokenPos] = 0xF0
		op = lz4EmitLen(dst, op, lit-15)
	} else {
		dst[tokenPos] = byte(lit << 4)
	}
	copy(dst[op:], src[anchor:])
	op += lit

	return op
}

// lz4DecompressBlock is the reference decoder, used by the round-trip test.
func lz4DecompressBlock(src []byte, dstCap int) ([]byte, error) {
	dst := make([]byte, 0, dstCap)
	ip := 0
	n := len(src)

	readLen := func(base int) (int, error) {
		for {
			if ip >= n {
				return 0, errTruncated
			}
			b := src[ip]
			ip++
			base += int(b)
			if b != 255 {
				return base, nil
			}
		}
	}

	for ip < n {
		token := src[ip]
		ip++

		lit := int(token >> 4)
		if lit == 15 {
			extra, err := readLen(0)
			if err != nil {
				return nil, err
			}
			lit += extra
		}
		if ip+lit > n || len(dst)+lit > dstCap {
			return nil, errCorrupt
		}
		dst = append(dst, src[ip:ip+lit]...)
		ip += lit

		if ip == n {
			break
		}
		if ip+2 > n {
			return nil, errCorrupt
		}
		offset := int(src[ip]) | int(src[ip+1])<<8
		ip += 2
		if offset == 0 || offset > len(dst) {
			return nil, errCorrupt
		}

		mlen := int(token & 0x0F)
		if mlen == 15 {
			extra, err := readLen(0)
			if err != nil {
				return nil, err
			}
			mlen += extra
		}
		mlen += 4
		if len(dst)+mlen > dstCap {
			return nil, errCorrupt
		}
		from := len(dst) - offset
		for i := 0; i < mlen; i++ {
			dst = append(dst, dst[from+i])
		}
	}
	return dst, nil
}

type lz4Error string

func (e lz4Error) Error() string { return string(e) }

const (
	errCorrupt   lz4Error = "corrupt lz4 block"
	errTruncated lz4Error = "truncated lz4 block"
)
