// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"time"

	"cech.local/driver/internal/coaxd/usb/bridge/mb86h47"
	"cech.local/driver/pkg/usbio"
	"cech.local/driver/receiver"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
func run() error {
	channels := flag.String("channels", "27", "physical channels, comma separated")
	duration := flag.Duration("duration", 5*time.Second, "capture duration per channel")
	output := flag.String("out", "", "TS output file (single channel)")
	list := flag.Bool("list", false, "list USB devices without opening")
	repeats := flag.Int("repeat", 1, "repeat the channel sequence in one device session")
	selector := flag.String("device", "", "USB selector")
	flag.Parse()
	if *list {
		devices, e := usbio.Devices()
		if e != nil {
			return e
		}
		return json.NewEncoder(os.Stdout).Encode(devices)
	}
	if *duration <= 0 || *repeats < 1 {
		return errors.New("duration and repeat must be positive")
	}
	var frequencies []uint32
	for _, s := range strings.Split(*channels, ",") {
		ch, e := strconv.Atoi(strings.TrimSpace(s))
		if e != nil {
			return e
		}
		f, e := receiver.Frequency(ch)
		if e != nil {
			return e
		}
		frequencies = append(frequencies, f)
	}
	if *output != "" && (len(frequencies) != 1 || *repeats != 1) {
		return errors.New("--out requires one channel and one repetition")
	}
	interrupted := make(chan os.Signal, 1)
	signal.Notify(interrupted, os.Interrupt)
	defer signal.Stop(interrupted)
	start := time.Now()
	r, e := receiver.Open(*selector)
	if e != nil {
		return e
	}
	defer func() {
		if e := r.Close(); e != nil {
			fmt.Fprintln(os.Stderr, "close:", e)
		}
	}()
	fmt.Fprintf(os.Stderr, "initialized in %v\n", time.Since(start))
	var out *os.File
	if *output != "" {
		out, e = os.OpenFile(*output, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0644)
		if e != nil {
			return e
		}
		defer out.Close()
	}
	buffer := make([]byte, 188*1024)
	var failed bool
	for iteration := 0; iteration < *repeats; iteration++ {
		for _, frequency := range frequencies {
			select {
			case <-interrupted:
				return errors.New("interrupted")
			default:
			}
			start := time.Now()
			result, err := r.Tune(frequency)
			report := struct {
				Iteration     int
				Tune          receiver.TuneResult
				FirstPacketMS int64
				Bytes         int64
				Timeouts      int
				MaxGapMS      int64
				Stats         *receiver.TSStats
				Framing       mb86h47.FramingStats
				Error         string
			}{Iteration: iteration + 1, Tune: result, Stats: new(receiver.TSStats)}
			if err != nil {
				report.Error = err.Error()
				failed = true
				json.NewEncoder(os.Stdout).Encode(report)
				if !errors.Is(err, receiver.ErrNoLock) {
					return err
				}
				continue
			}
			end := time.Now().Add(*duration)
			lastData := time.Now()
			for time.Now().Before(end) {
				select {
				case <-interrupted:
					return errors.New("interrupted")
				default:
				}
				n, err := r.Read(buffer)
				if n > 0 {
					if report.Bytes == 0 {
						report.FirstPacketMS = time.Since(start).Milliseconds()
					}
					gap := time.Since(lastData).Milliseconds()
					if gap > report.MaxGapMS {
						report.MaxGapMS = gap
					}
					lastData = time.Now()
					report.Stats.Feed(buffer[:n])
					report.Bytes += int64(n)
					if out != nil {
						if _, e = out.Write(buffer[:n]); e != nil {
							return e
						}
					}
				}
				if err != nil {
					if errors.Is(err, usbio.ErrTimeout) {
						report.Timeouts++
					} else {
						report.Error = err.Error()
						failed = true
						break
					}
				}
			}
			if report.Bytes == 0 {
				report.Error = "no TS data"
				failed = true
			}
			report.Framing = r.FramingStats()
			if report.Stats.TEI+report.Stats.SyncErrors+report.Stats.Malformed+report.Stats.ContinuityErrors+report.Stats.BadPATCRC+report.Framing.SyncLosses > 0 || len(report.Stats.TSIDs) != 1 {
				report.Error = "TS validation failed; see Stats and Framing"
				failed = true
			}
			if err := r.Stop(); err != nil {
				return err
			}
			if err := json.NewEncoder(os.Stdout).Encode(report); err != nil {
				return err
			}
		}
	}
	if failed {
		return errors.New("one or more reception attempts failed")
	}
	return nil
}
