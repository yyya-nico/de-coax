// SPDX-License-Identifier: GPL-2.0-only
package receiver

import (
	"encoding/binary"
	"testing"
)

func packet(pid int, cc byte) []byte {
	p := make([]byte, 188)
	p[0] = 0x47
	p[1] = byte(pid >> 8)
	p[2] = byte(pid)
	p[3] = 0x10 | cc
	return p
}
func TestContinuity(t *testing.T) {
	var s TSStats
	s.Feed(packet(100, 15))
	s.Feed(packet(100, 0))
	s.Feed(packet(100, 0))
	if s.Duplicates != 1 || s.ContinuityErrors != 0 {
		t.Fatalf("duplicate handling: %+v", s)
	}
	s.Feed(packet(100, 0))
	s.Feed(packet(100, 4))
	if s.ContinuityErrors != 2 {
		t.Fatal(s.ContinuityErrors)
	}
	p := packet(100, 8)
	p[3] = 0x38
	p[4] = 1
	p[5] = 0x80
	s.Feed(p)
	s.Feed(packet(100, 9))
	if s.ContinuityErrors != 2 || s.Discontinuities != 1 {
		t.Fatal("discontinuity")
	}
	p = packet(100, 10)
	p[1] |= 0x80
	s.Feed(p)
	s.Feed(packet(100, 2))
	if s.TEI != 1 || s.ContinuityErrors != 2 {
		t.Fatal("TEI reset")
	}
}
func TestAdaptationAndReset(t *testing.T) {
	var s TSStats
	s.Feed(packet(100, 1))
	p := packet(100, 1)
	p[3] = 0x21
	p[4] = 183
	s.Feed(p)
	s.Feed(packet(100, 2))
	p[4] = 184
	s.Feed(p)
	if s.ContinuityErrors != 0 || s.Malformed != 1 {
		t.Fatal("adaptation validation")
	}
	var next TSStats
	next.Feed(packet(100, 9))
	if next.ContinuityErrors != 0 {
		t.Fatal("retune")
	}
}
func TestPATCRC(t *testing.T) {
	section := []byte{0, 0xb0, 13, 0x12, 0x34, 0xc1, 0, 0, 0, 1, 0xe1, 0}
	section = binary.BigEndian.AppendUint32(section, mpegCRC(section))
	p := packet(0, 0)
	for i := 4; i < len(p); i++ {
		p[i] = 0xff
	}
	p[1] |= 0x40
	p[4] = 0
	copy(p[5:], section)
	var s TSStats
	s.Feed(p)
	if len(s.TSIDs) != 1 || s.TSIDs[0] != 0x1234 || s.BadPATCRC != 0 {
		t.Fatal(s.TSIDs, s.BadPATCRC)
	}
	p[3] = 0x11
	p[10] ^= 0x10
	s.Feed(p)
	if s.BadPATCRC != 1 {
		t.Fatal(s.BadPATCRC)
	}
}
