TLS 配置使用说明（严格模式）

概述
- 框架不再扫描固定证书路径，也不会自动生成自签名证书。
- 生产与测试均需“显式提供”TLS配置，否则 TLS 初始化会失败。

服务端用法
- 头文件：`#include "ssl_context.h"`
- 在服务器启动前调用：

```
ssl_config conf;
conf._cert_file = "/path/server.crt";
conf._key_file  = "/path/server.key";
conf._protocols = "TLSv1.2:TLSv1.3";   // 可选
conf._cipher_list = "HIGH:!aNULL:!MD5"; // 可选
conf._verify_peer = false;              // 服务端一般不校验对端
tls_set_server_config(conf);
```

客户端用法（可选）
- 头文件：`#include "ssl_context.h"`
- 发起请求前设置：

```
ssl_config c;
c._verify_peer = true;          // 是否校验服务端证书
c._ca_file = "/path/ca.pem";   // CA 文件（开启校验时建议提供）
tls_set_client_config(c);
```

注意事项
- 未调用 `tls_set_server_config` 时，TLS 初始化会打印错误并中止：
  `[tls] No certificate configured. Please call tls_set_server_config() before starting TLS.`
- 示例/测试程序（如 `test_server_simple`、`tests/smoke_https_wss`）已在代码中显式设置了仓库内测试证书路径。
