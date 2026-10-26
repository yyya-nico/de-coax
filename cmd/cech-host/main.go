// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"cech.local/driver/internal/wire"
	"cech.local/driver/pkg/iso7816"
	"cech.local/driver/pkg/usbio"
	"cech.local/driver/receiver"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"sync"
	"time"
)

func main() {
	if e := run(); e != nil {
		fmt.Fprintln(os.Stderr, e)
		os.Exit(1)
	}
}
func run() error {
	r, e := receiver.Open("")
	if e != nil {
		_ = wire.Write(os.Stdout, wire.Frame{Type: wire.Ready, Data: []byte(e.Error())})
		return e
	}
	defer r.Close()
	var output sync.Mutex
	send := func(kind, gen uint32, data []byte) error {
		output.Lock()
		defer output.Unlock()
		return wire.Write(os.Stdout, wire.Frame{Type: kind, Generation: gen, Data: data})
	}
	if e = send(wire.Ready, 0, nil); e != nil {
		return e
	}
	commands := make(chan wire.Frame, 8)
	readErrors := make(chan error, 1)
	go func() {
		for {
			f, e := wire.Read(os.Stdin)
			if e != nil {
				readErrors <- e
				return
			}
			commands <- f
		}
	}()
	var generation uint32
	var stopPump chan struct{}
	var pumpDone chan struct{}
	pumpErrors := make(chan error, 1)
	stopStream := func() {
		if stopPump != nil {
			close(stopPump)
			<-pumpDone
			stopPump = nil
		}
	}
	defer stopStream()
	startStream := func() {
		stopPump = make(chan struct{})
		pumpDone = make(chan struct{})
		stop, done, gen := stopPump, pumpDone, generation
		go func() {
			defer close(done)
			buffer := make([]byte, 188*1024)
			for {
				select {
				case <-stop:
					return
				default:
				}
				n, e := r.Read(buffer)
				if n > 0 {
					if err := send(wire.TS, gen, buffer[:n]); err != nil {
						select {
						case pumpErrors <- err:
						default:
						}
						return
					}
				}
				if e != nil && !errors.Is(e, usbio.ErrTimeout) {
					select {
					case pumpErrors <- e:
					default:
					}
					return
				}
			}
		}()
	}
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case e := <-readErrors:
			if errors.Is(e, io.EOF) {
				return nil
			}
			return e
		case e := <-pumpErrors:
			_ = send(wire.Fault, generation, []byte(e.Error()))
			return e
		case <-ticker.C:
			if stopPump == nil {
				continue
			}
			status, e := r.Status()
			if e != nil {
				_ = send(wire.Fault, generation, []byte(e.Error()))
				return e
			}
			var cnr float32
			if status.FullSegmentLocked {
				cnr = float32(status.CNR)
			}
			var p [4]byte
			binary.LittleEndian.PutUint32(p[:], math.Float32bits(cnr))
			if e = send(wire.Signal, generation, p[:]); e != nil {
				return e
			}
		case f := <-commands:
			if f.Type == wire.Card {
				if f.Generation != generation || len(f.Data) < 4 {
					return errors.New("invalid card request")
				}
				op := binary.LittleEndian.Uint32(f.Data)
				var data []byte
				var err error
				switch op {
				case 0:
					var present bool
					var atr []byte
					present, atr, err = r.CardStatus()
					data = make([]byte, 2, len(atr)+2)
					if present {
						data[0] = 1
					}
					if len(atr) > 0 {
						data[1] = 1
					}
					data = append(data, atr...)
				case 1:
					data, err = r.CardReset()
				case 2:
					if len(f.Data) <= 4 || len(f.Data) > 516 {
						err = errors.New("invalid APDU size")
					} else {
						data, err = r.CardTransmit(f.Data[4:])
					}
				default:
					err = errors.New("invalid card operation")
				}
				code := uint32(0)
				if err != nil {
					data = nil
					switch {
					case errors.Is(err, iso7816.ErrNoCard):
						code = 1
					case errors.Is(err, iso7816.ErrTimeout):
						code = 2
					case errors.Is(err, usbio.ErrGone):
						code = 3
					default:
						code = 4
					}
				}
				reply := binary.LittleEndian.AppendUint32(nil, code)
				reply = append(reply, data...)
				if e = send(wire.CardReply, generation, reply); e != nil {
					return e
				}
				continue
			}
			switch f.Type {
			case wire.Tune:
				stopStream()
				generation = f.Generation
				if len(f.Data) != 4 {
					return errors.New("invalid tune request")
				}
				result, err := r.Tune(binary.LittleEndian.Uint32(f.Data))
				fmt.Fprintf(os.Stderr, "tune: %+v error=%v\n", result, err)
				if err != nil {
					if e = send(wire.Ack, generation, []byte(err.Error())); e != nil {
						return e
					}
					if !errors.Is(err, receiver.ErrNoLock) {
						return err
					}
					continue
				}
				if e = send(wire.Ack, generation, nil); e != nil {
					return e
				}
				startStream()
			case wire.Stop:
				stopStream()
				generation = f.Generation
				e = r.Stop()
				var data []byte
				if e != nil {
					data = []byte(e.Error())
				}
				if err := send(wire.Ack, generation, data); err != nil {
					return err
				}
				if e != nil {
					return e
				}
			case wire.Purge:
				active := stopPump != nil
				stopStream()
				generation = f.Generation
				e = r.Purge()
				var data []byte
				if e != nil {
					data = []byte(e.Error())
				}
				if err := send(wire.Ack, generation, data); err != nil {
					return err
				}
				if e != nil {
					return e
				}
				if active {
					startStream()
				}
			case wire.Quit:
				return nil
			default:
				return errors.New("unknown command")
			}
		}
	}
}
