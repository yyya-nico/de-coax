// SPDX-License-Identifier: GPL-2.0-only
package dib8096

import (
	"context"
	"time"
)

// TuneContext bounds both calibration and acquisition, including the fallback.
// An individual USB operation can take its transport timeout to return.
func (board *Demod) TuneContext(ctx context.Context, frequency uint32) (bool, error) {
	if err := ctx.Err(); err != nil {
		return false, err
	}
	board.context = ctx
	defer func() { board.context = nil }()
	return board.TuneCached(frequency)
}

// TuneCached tries only parameters previously read with TMCC and MPEG lock.
// A lost lock discards the entry and performs the normal discovery once.
// Bus errors are returned immediately because replaying writes is unsafe.
func (board *Demod) TuneCached(frequencyHz uint32) (cached bool, err error) {
	if c, ok := board.cache[frequencyHz]; ok && !board.asleep {
		board.failure.err = nil
		board.channel = c
		err = board.demod.setFrontend()
		board.flush(&err)
		if err != nil {
			delete(board.cache, frequencyHz)
			return false, err
		}
		if board.demod.status == statusDataLocked && fullSegmentLock(board.demod.readLock(), &board.channel) {
			return true, board.failure.err
		}
		delete(board.cache, frequencyHz)
	}
	if err = board.Tune(frequencyHz); err != nil {
		return false, err
	}
	if board.demod.status != statusDataLocked || board.demod.readStatus()&feHasSync == 0 {
		return false, board.failure.err
	}
	// The search's early FFT result is insufficient: fetch the final TMCC.
	board.demod.getFrontend()
	if err = board.failure.err; err != nil {
		return false, err
	}
	c := board.channel
	if isManualMode(&c) && fullSegmentLock(board.demod.readLock(), &c) {
		if board.cache == nil {
			board.cache = make(map[uint32]channel)
		}
		board.cache[frequencyHz] = c
	}
	return false, nil
}

func fullSegmentLock(lock uint16, c *channel) bool {
	if lock&(1<<12) == 0 || (lock>>1)&15 != 15 {
		return false
	}
	for i, layer := range c.layer {
		if layer.segmentCount > 1 && lock&(1<<uint(7-i)) != 0 {
			return true
		}
	}
	return false
}

func (state *dib8000) tracePhase(name string, start time.Time) {
	if state.trace != nil {
		state.trace(name, state.now().Sub(start))
	}
}
