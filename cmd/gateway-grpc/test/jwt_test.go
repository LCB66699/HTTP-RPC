package gateway_test

import (
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"
)

func TestJWTCreateAndVerify(t *testing.T) {
	secret := []byte("test-secret-32bytes-here-abcdef!")
	claims := jwt.MapClaims{
		"username": "tester",
		"uid":      float64(12345),
		"exp":      float64(time.Now().Add(15 * time.Minute).Unix()),
	}

	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	tokenStr, err := token.SignedString(secret)
	if err != nil {
		t.Fatalf("sign failed: %v", err)
	}

	parsed, err := jwt.Parse(tokenStr, func(t *jwt.Token) (interface{}, error) {
		return secret, nil
	})
	if err != nil {
		t.Fatalf("verify failed: %v", err)
	}
	if !parsed.Valid {
		t.Fatal("token should be valid")
	}
}

func TestJWTWrongSecretRejected(t *testing.T) {
	secret := []byte("correct-secret-32bytes-here!!!")
	badSecret := []byte("wrong-secret-32bytes-here!!!!!")

	token := jwt.NewWithClaims(jwt.SigningMethodHS256, jwt.MapClaims{"sub": "test"})
	tokenStr, _ := token.SignedString(secret)

	_, err := jwt.Parse(tokenStr, func(t *jwt.Token) (interface{}, error) {
		return badSecret, nil
	})
	if err == nil {
		t.Fatal("should reject token signed with different key")
	}
}

func TestJWTExpiredRejected(t *testing.T) {
	secret := []byte("test-secret-32bytes-here-abcdef!")
	claims := jwt.MapClaims{
		"exp": float64(time.Now().Add(-1 * time.Hour).Unix()),
	}

	token := jwt.NewWithClaims(jwt.SigningMethodHS256, claims)
	tokenStr, _ := token.SignedString(secret)

	parsed, err := jwt.Parse(tokenStr, func(t *jwt.Token) (interface{}, error) {
		return secret, nil
	})
	_ = parsed
	if err == nil {
		t.Fatal("should reject expired token")
	}
}

func TestJWTNoPaddingBase64(t *testing.T) {
	secret := []byte("test-secret-32bytes-here-abcdef!")
	token := jwt.NewWithClaims(jwt.SigningMethodHS256, jwt.MapClaims{"test": true})
	tokenStr, _ := token.SignedString(secret)

	if len(tokenStr) > 0 {
		last := tokenStr[len(tokenStr)-1]
		if last == '=' {
			t.Fatal("base64url should not have padding")
		}
	}

	parsed, _ := jwt.Parse(tokenStr, func(t *jwt.Token) (interface{}, error) {
		return secret, nil
	})
	if !parsed.Valid {
		t.Fatal("unpadded token should be valid")
	}
}
