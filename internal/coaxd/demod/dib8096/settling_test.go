// SPDX-License-Identifier: GPL-2.0-only
package dib8096

import (
	"fmt"
	"testing"
	"time"
)

// Model hardware that rejects FFT discovery while the frontend is settling.
// Checking the first search write also catches a delay placed after discovery.
type settlingChip struct {
	*fakeChip
	clock *fakeClock
	ready time.Time
}

func (chip *settlingChip) Write(address byte, data []byte) error {
	if address == Address && len(data) == 4 && data[0] == 0 && data[1] == 37 && chip.clock.Now().Before(chip.ready) {
		return fmt.Errorf("FFT search started before frontend settled")
	}
	return chip.fakeChip.Write(address, data)
}

func TestSearchSettlesBeforeFFTStart(t *testing.T) {
	for _, fft := range []bool{true, false} {
		t.Run(fmt.Sprintf("fft=%v", fft), func(t *testing.T) {
			clock := &fakeClock{now: time.Unix(1000, 0)}
			chip := &settlingChip{fakeChip: newFakeChip(), clock: clock, ready: clock.Now().Add(100 * time.Millisecond)}
			board := New(chip)
			board.setTiming(clock.Now, clock.Sleep, 5*time.Second)
			state := board.demod
			state.revision = 0x8002
			state.tuneState = demodSearchNext
			state.autosearchState = autosearchSearchingGuard
			if fft {
				state.autosearchState = autosearchSearchingFFT
			}
			// The normal tune loop has already waited its 10ms step.
			clock.Sleep(10 * time.Millisecond)
			start := clock.Now()
			state.tune()
			if board.failure.err != nil {
				t.Fatal(board.failure.err)
			}
			if !fft && !clock.Now().Equal(start) {
				t.Fatal("guard search acquired an unnecessary settling delay")
			}
		})
	}
}
