#pragma once

// HTTP-RPC 错误码：与 proto ErrorCode 枚举一致。
// 0=OK, 1=BAD_REQUEST, 2=UNAUTHENTICATED, 3=FORBIDDEN,
// 4=NOT_FOUND, 5=CONFLICT, 6=INTERNAL, 7=UNAVAILABLE
namespace rpc_error {
    const int OK = 0;
    const int BAD_REQUEST = 1;
    const int UNAUTHENTICATED = 2;
    const int FORBIDDEN = 3;
    const int NOT_FOUND = 4;
    const int CONFLICT = 5;
    const int INTERNAL = 6;
    const int UNAVAILABLE = 7;
}  // namespace rpc_error

// SET_ERROR(resp, "text", code) — set both error message and error_code
#define SET_ERROR(resp, msg, code) \
    do {                           \
        (resp)->set_error(msg);    \
        (resp)->set_error_code(code); \
    } while (0)
