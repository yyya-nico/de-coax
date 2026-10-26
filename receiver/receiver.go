// SPDX-License-Identifier: GPL-2.0-only
// Package receiver owns one CECH-ZD1 J. The caller serializes control operations;
// Read may run alongside card operations, but must stop before tuning or closing.
package receiver

import (
	"context"
	"errors"
	"fmt"
	"time"

	"cech.local/driver/internal/coaxd/demod/dib8096"
	"cech.local/driver/internal/coaxd/usb/bridge/mb86h47"
	"cech.local/driver/pkg/iso7816"
	"cech.local/driver/pkg/usbio"
)

var IDs = []usbio.ID{{Vendor: 0x054c, Product: 0x04b2}}

var ErrNoLock = errors.New("no full-segment lock")

type Receiver struct {
	transport *usbio.Transport
	bridge    *mb86h47.Bridge
	demod     *dib8096.Demod
	streaming bool
	card      *iso7816.Session
}

func Open(selector string) (_ *Receiver, err error) {
	t, err := usbio.Open(selector, IDs)
	if err != nil {
		return nil, err
	}
	defer func() {
		if err != nil {
			t.Close()
		}
	}()
	if err = t.SetAlternate(0, 0); err != nil {
		return nil, err
	}
	b := mb86h47.New(t)
	if err = b.Start(); err != nil {
		return nil, err
	}
	if err = b.ResetDemodulator(); err != nil {
		return nil, err
	}
	if err = b.Write(dib8096.Address, []byte{5, 7, 0, 3}); err != nil {
		return nil, err
	}
	d := dib8096.New(b)
	if err = d.Initialize(); err != nil {
		return nil, err
	}
	if err = d.SetGPIO(1, true); err != nil {
		return nil, err
	}
	return &Receiver{transport: t, bridge: b, demod: d, card: iso7816.New(b.CardLine())}, nil
}

// Card operations may run alongside Read, but must be serialized with tuning.
func (r *Receiver) CardStatus() (bool, []byte, error) {
	present, err := r.card.Present()
	if err != nil || !present {
		r.card.Invalidate()
	}
	return present, r.card.ATR(), err
}
func (r *Receiver) CardReset() ([]byte, error)               { return r.card.Reset() }
func (r *Receiver) CardTransmit(apdu []byte) ([]byte, error) { return r.card.Transmit(apdu) }

// Frequency returns the Japanese terrestrial channel centre, including +1/7 MHz.
func Frequency(channel int) (uint32, error) {
	if channel < 13 || channel > 62 {
		return 0, fmt.Errorf("physical channel must be 13..62: %d", channel)
	}
	return uint32(473142857 + (channel-13)*6000000), nil
}

type TuneResult struct {
	FrequencyHz uint32
	TuneMS      int64
	StartMS     int64
	Status      dib8096.Status
	Cached      bool
	PhasesMS    map[string]int64
}

func (r *Receiver) Tune(frequency uint32) (result TuneResult, err error) {
	if err = r.Stop(); err != nil {
		return
	}
	defer func() {
		if err != nil {
			err = errors.Join(err, r.bridge.StopStream(), r.demod.SetOutputMode(dib8096.OutputHighZ))
		}
	}()
	result.FrequencyHz = frequency
	start := time.Now()
	result.PhasesMS = make(map[string]int64)
	r.demod.Trace = func(name string, d time.Duration) { result.PhasesMS[name] += d.Milliseconds() }
	defer func() { r.demod.Trace = nil }()
	r.bridge.PrepareStream()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if result.Cached, err = r.demod.TuneContext(ctx, frequency); err != nil {
		return
	}
	result.TuneMS = time.Since(start).Milliseconds()
	if result.Status, err = r.demod.Status(); err != nil {
		return
	}
	if !result.Status.Locked || !result.Status.FullSegmentLocked {
		return result, fmt.Errorf("%w at %d Hz (%v)", ErrNoLock, frequency, result.Status.Names)
	}
	if err = r.demod.SetOutputMode(dib8096.OutputParallelGatedClock); err != nil {
		return
	}
	if err = r.bridge.StartStream(); err != nil {
		return
	}
	r.streaming = true
	result.StartMS = time.Since(start).Milliseconds()
	return
}

func (r *Receiver) Status() (dib8096.Status, error)    { return r.demod.Status() }
func (r *Receiver) FramingStats() mb86h47.FramingStats { return r.bridge.FramingStats() }

// Purge replaces the bridge/USB stream epoch while keeping RF acquisition.
func (r *Receiver) Purge() error {
	if !r.streaming {
		return nil
	}
	if err := r.Stop(); err != nil {
		return err
	}
	r.bridge.PrepareStream()
	if err := r.demod.SetOutputMode(dib8096.OutputParallelGatedClock); err != nil {
		return err
	}
	if err := r.bridge.StartStream(); err != nil {
		return err
	}
	r.streaming = true
	return nil
}
func (r *Receiver) Read(buffer []byte) (int, error) { return r.bridge.ReadStream(buffer, 100) }
func (r *Receiver) Stop() error {
	if !r.streaming {
		return nil
	}
	r.streaming = false
	return errors.Join(r.bridge.StopStream(), r.demod.SetOutputMode(dib8096.OutputHighZ))
}
func (r *Receiver) Close() error {
	r.card.Invalidate()
	return errors.Join(r.Stop(), r.bridge.PowerDownCard(), r.demod.SetGPIO(1, false), r.demod.Sleep(), r.transport.Close())
}
