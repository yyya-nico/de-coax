// SPDX-License-Identifier: GPL-2.0-only
package dib8096

import (
	"context"
	"errors"
	"testing"
	"time"
)

func TestCachedTuneSkipsSearchAndFallsBack(t *testing.T) {
	chip := lockedChip()
	board, _ := newTestDemod(chip)
	if e := board.Initialize(); e != nil {
		t.Fatal(e)
	}
	hit, e := board.TuneCached(515142857)
	if e != nil || hit || len(board.cache) != 1 {
		t.Fatalf("initial: %v %v %v", hit, e, board.cache)
	}
	chip.queues[1284] = nil
	chip.registers[1284] = 0 // no search interrupt: manual must work
	hit, e = board.TuneCached(515142857)
	if e != nil || !hit {
		t.Fatalf("cached: %v %v", hit, e)
	}
	// A cached tune that loses lock must discard the parameters and rediscover.
	chip.registers[568] = 0
	hit, e = board.TuneCached(515142857)
	if e == nil {
		t.Fatal("expected auto-search deadline after cached loss")
	}
	if hit || len(board.cache) != 0 {
		t.Fatal("stale cache survived")
	}
}
func TestContextRejectedBeforeIO(t *testing.T) {
	chip := newFakeChip()
	board, _ := newTestDemod(chip)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, e := board.TuneContext(ctx, 515142857)
	if !errors.Is(e, context.Canceled) || len(chip.writes) != 0 {
		t.Fatal(e, len(chip.writes))
	}
}
func TestLongestLayerInterleaver(t *testing.T) {
	chip := lockedChip()
	board, _ := newTestDemod(chip)
	board.channel.layer[0] = layerParameters{segmentCount: 1, interleaving: 1}
	board.channel.layer[1] = layerParameters{segmentCount: 12, interleaving: 4}
	board.demod.tuneState = demodStep9
	chip.registers[1291] = 1 << 9
	board.demod.tune()
	if board.demod.longestIntlvLayer != 1 {
		t.Fatal(board.demod.longestIntlvLayer)
	}
	if got := board.demod.timeout.Sub(board.demod.now()); got != 1600*time.Millisecond {
		t.Fatal(got)
	}
}
