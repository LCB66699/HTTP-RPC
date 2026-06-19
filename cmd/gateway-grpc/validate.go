package main

import (
	"encoding/json"
	"net/http"
)

// validateLogin 校验登录请求
func validateLogin(username, password string) (string, int) {
	if username == "" {
		return "username required", http.StatusBadRequest
	}
	if password == "" {
		return "password required", http.StatusBadRequest
	}
	return "", 0
}

// validateRegister 校验注册请求
func validateRegister(username, password string) (string, int) {
	if len(username) < 3 || len(username) > 20 {
		return "username must be 3-20 characters", http.StatusBadRequest
	}
	if len(password) < 6 {
		return "password must be at least 6 characters", http.StatusBadRequest
	}
	return "", 0
}

// validateSheetCreate 校验创建表格请求
func validateSheetCreate(name, headersJSON, dataJSON string) (string, int) {
	if name == "" {
		return "name required", http.StatusBadRequest
	}
	if !json.Valid([]byte(headersJSON)) {
		return "headers_json is not valid JSON", http.StatusBadRequest
	}
	if !json.Valid([]byte(dataJSON)) {
		return "data_json is not valid JSON", http.StatusBadRequest
	}
	return "", 0
}

// validateSheetUpdate 校验更新表格请求
func validateSheetUpdate(name, headersJSON, dataJSON string) (string, int) {
	return validateSheetCreate(name, headersJSON, dataJSON)
}

// validateFileUpload 校验文件上传 — filename 非空，size > 0
func validateFileUpload(filename string, size int64) (string, int) {
	if filename == "" {
		return "filename required", http.StatusBadRequest
	}
	if size <= 0 {
		return "file is empty", http.StatusBadRequest
	}
	return "", 0
}

// validateSearch 校验搜索请求
func validateSearch(query string) (string, int) {
	if query == "" {
		return "q required", http.StatusBadRequest
	}
	return "", 0
}

// validateChangePassword 校验改密请求
func validateChangePassword(oldPwd, newPwd string) (string, int) {
	if oldPwd == "" || newPwd == "" {
		return "old_password and new_password required", http.StatusBadRequest
	}
	if len(newPwd) < 6 {
		return "new password must be at least 6 characters", http.StatusBadRequest
	}
	return "", 0
}
