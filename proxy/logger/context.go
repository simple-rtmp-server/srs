// Copyright (c) 2024 Winlin
//
// SPDX-License-Identifier: MIT
package logger

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
)

type key string

var cidKey key = "cid.proxy.ossrs.org"

// generateContextID generates a random context id in string.
func generateContextID() string {
	randomBytes := make([]byte, 32)
	_, _ = rand.Read(randomBytes)
	hash := sha256.Sum256(randomBytes)
	hashString := hex.EncodeToString(hash[:])
	cid := hashString[:7]
	return cid
}

// WithContext creates a new context with cid, which will be used for log.
func WithContext(ctx context.Context) context.Context {
	return context.WithValue(ctx, cidKey, generateContextID())
}

// ContextID returns the cid in context, or empty string if not set.
func ContextID(ctx context.Context) string {
	if cid, ok := ctx.Value(cidKey).(string); ok {
		return cid
	}
	return ""
}
