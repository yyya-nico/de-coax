// SPDX-License-Identifier: GPL-2.0-only
package receiver

func mpegCRC(p []byte) uint32 {
	crc := uint32(0xffffffff)
	for _, b := range p {
		crc ^= uint32(b) << 24
		for i := 0; i < 8; i++ {
			if crc&0x80000000 != 0 {
				crc = crc<<1 ^ 0x04c11db7
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}
func (s *TSStats) feedPAT(payload []byte, start bool) {
	if start {
		if len(payload) == 0 {
			return
		}
		pointer := int(payload[0])
		payload = payload[1:]
		if pointer > len(payload) {
			s.patSection = nil
			return
		}
		if len(s.patSection) > 0 {
			s.appendPAT(payload[:pointer])
		}
		s.patSection = nil
		s.appendPAT(payload[pointer:])
	} else if len(s.patSection) > 0 {
		s.appendPAT(payload)
	}
}
func (s *TSStats) appendPAT(data []byte) {
	s.patSection = append(s.patSection, data...)
	for len(s.patSection) >= 3 {
		p := s.patSection
		if p[0] == 0xff {
			s.patSection = nil
			return
		}
		length := int(p[1]&15)<<8 | int(p[2])
		total := 3 + length
		if length < 9 || length > 1021 {
			s.patSection = nil
			return
		}
		if len(p) < total {
			return
		}
		if p[0] == 0 && p[1]&0x80 != 0 && p[5]&1 != 0 {
			if mpegCRC(p[:total]) != 0 {
				s.BadPATCRC++
			} else {
				id := uint16(p[3])<<8 | uint16(p[4])
				found := false
				for _, v := range s.TSIDs {
					if v == id {
						found = true
						break
					}
				}
				if !found {
					s.TSIDs = append(s.TSIDs, id)
				}
			}
		}
		s.patSection = p[total:]
	}
}
