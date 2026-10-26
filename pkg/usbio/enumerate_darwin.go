// Copyright 2026 Till0196
// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later

package usbio

import (
	"cech.local/driver/pkg/usbio/backend"
	"cech.local/driver/pkg/usbio/iokit"
)

func enumerate() ([]backend.Found, error) { return iokit.Enumerate() }
