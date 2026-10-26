// SPDX-License-Identifier: GPL-2.0-only
// Package wire defines the bounded binary protocol over inherited local pipes.
package wire

import (
	"encoding/binary"
	"fmt"
	"io"
)

const (
	Ready      uint32 = 1
	Tune       uint32 = 2
	Stop       uint32 = 3
	Signal     uint32 = 4
	Quit       uint32 = 5
	Purge      uint32 = 6
	Card       uint32 = 16
	CardReply  uint32 = 0x110
	Ack        uint32 = 0x100
	TS         uint32 = 0x200
	Fault      uint32 = 0x201
	MaxPayload        = 188 * 1024
)

type Frame struct {
	Type, Generation uint32
	Data             []byte
}

func Read(r io.Reader) (f Frame, err error) {
	var h [12]byte
	if _, err = io.ReadFull(r, h[:]); err != nil {
		return
	}
	f.Type = binary.LittleEndian.Uint32(h[:])
	f.Generation = binary.LittleEndian.Uint32(h[4:])
	n := binary.LittleEndian.Uint32(h[8:])
	if n > MaxPayload {
		return f, fmt.Errorf("frame length %d exceeds limit", n)
	}
	f.Data = make([]byte, n)
	_, err = io.ReadFull(r, f.Data)
	return
}
func Write(w io.Writer, f Frame) error {
	if len(f.Data) > MaxPayload {
		return fmt.Errorf("frame too large")
	}
	var h [12]byte
	binary.LittleEndian.PutUint32(h[:], f.Type)
	binary.LittleEndian.PutUint32(h[4:], f.Generation)
	binary.LittleEndian.PutUint32(h[8:], uint32(len(f.Data)))
	if err := writeAll(w, h[:]); err != nil {
		return err
	}
	return writeAll(w, f.Data)
}
func writeAll(w io.Writer, p []byte) error {
	for len(p) > 0 {
		n, e := w.Write(p)
		if e != nil {
			return e
		}
		if n == 0 {
			return io.ErrShortWrite
		}
		p = p[n:]
	}
	return nil
}
