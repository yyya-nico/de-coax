// SPDX-License-Identifier: GPL-2.0-only
package wire

import (
	"bytes"
	"encoding/binary"
	"testing"
)

func TestFrames(t *testing.T) {
	var b bytes.Buffer
	want := Frame{TS, 42, []byte{1, 2, 3}}
	if e := Write(&b, want); e != nil {
		t.Fatal(e)
	}
	f, e := Read(&b)
	if e != nil || f.Type != TS || f.Generation != 42 || !bytes.Equal(f.Data, want.Data) {
		t.Fatal(f, e)
	}
	var h [12]byte
	binary.LittleEndian.PutUint32(h[8:], MaxPayload+1)
	if _, e = Read(bytes.NewReader(h[:])); e == nil {
		t.Fatal("unbounded frame")
	}
}
