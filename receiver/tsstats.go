// SPDX-License-Identifier: GPL-2.0-only
package receiver

import "bytes"

// TSStats counts errors on the delivered stream; a new instance marks a retune.
// ContinuityErrors counts events, not an inferred number of missing packets.
type TSStats struct {
	Packets          uint64
	TEI              uint64
	SyncErrors       uint64
	Malformed        uint64
	ContinuityErrors uint64
	Duplicates       uint64
	Discontinuities  uint64
	PAT              uint64
	TSIDs            []uint16
	BadPATCRC        uint64
	pids             [8192]pidState
	patSection       []byte
}
type pidState struct {
	valid     bool
	cc        byte
	duplicate bool
	last      [188]byte
}

func (s *TSStats) Feed(data []byte) {
	for len(data) >= 188 {
		p := data[:188]
		data = data[188:]
		s.Packets++
		if p[0] != 0x47 {
			s.SyncErrors++
			continue
		}
		pid := int(p[1]&31)<<8 | int(p[2])
		if pid == 8191 {
			continue
		}
		st := &s.pids[pid]
		if p[1]&0x80 != 0 {
			s.TEI++
			st.valid = false
			if pid == 0 {
				s.patSection = nil
			}
			continue
		}
		afc := (p[3] >> 4) & 3
		if afc == 0 {
			s.Malformed++
			st.valid = false
			continue
		}
		payload := afc&1 != 0
		payloadOffset := 4
		if afc&2 != 0 {
			n := int(p[4])
			if n > 183 || (payload && n > 182) || (!payload && n != 183) {
				s.Malformed++
				st.valid = false
				continue
			}
			payloadOffset = 5 + n
			if n > 0 && p[5]&0x80 != 0 {
				s.Discontinuities++
				st.valid = false
				if pid == 0 {
					s.patSection = nil
				}
			}
		}
		cc := p[3] & 15
		if st.valid {
			expected := st.cc
			if payload {
				expected = (expected + 1) & 15
			}
			if payload && cc == st.cc && !st.duplicate && bytes.Equal(p, st.last[:]) {
				s.Duplicates++
				st.duplicate = true
				continue
			}
			if cc != expected {
				s.ContinuityErrors++
				if pid == 0 {
					s.patSection = nil
				}
			}
		}
		if pid == 0 && payload {
			s.PAT++
			s.feedPAT(p[payloadOffset:], p[1]&0x40 != 0)
		}
		st.valid = true
		st.cc = cc
		st.duplicate = false
		copy(st.last[:], p)
	}
	if len(data) != 0 {
		s.Malformed++
	}
}
