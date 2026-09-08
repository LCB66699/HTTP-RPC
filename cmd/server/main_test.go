package main

import "testing"

func TestGetenvPositiveInt64(t *testing.T) {
	const key = "TEST_UPLOAD_MAX_BYTES"
	for _, tc := range []struct {
		name string
		value string
		want int64
	}{
		{name: "unset", want: 50},
		{name: "configured", value: "1024", want: 1024},
		{name: "invalid", value: "many", want: 50},
		{name: "non-positive", value: "0", want: 50},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Setenv(key, tc.value)
			if got := getenvPositiveInt64(key, 50); got != tc.want {
				t.Fatalf("got %d, want %d", got, tc.want)
			}
		})
	}
}
