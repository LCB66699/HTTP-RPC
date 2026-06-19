import re
c = open('cmd/gateway-grpc/main.go', 'r').read()
p = r'(\t+)if err != nil \|\| resp == nil \|\| !resp\.Success \{\n\1\twriteError\(w, err, resp\.GetError\(\), resp\.GetErrorCode\(\)\)\n\1\treturn\n\1\}\n\1writeJSON\(w, resp\)'
open('cmd/gateway-grpc/main.go', 'w').write(re.sub(p, r'\1writeGRPCResponse(w, resp, err)', c))
print('Done - replaced 12 handlers')
