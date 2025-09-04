# HTTPS证书验证测试框架

基于MyFrame框架和OpenSSL实现的HTTPS证书验证测试工具，完全模拟标准OpenSSL客户端/服务端的证书验证机制。

## 🏗️ 框架组件

### 核心文件
- `ssl_cert_generator.sh` - SSL证书生成脚本
- `https_server_validation.cpp` - HTTPS验证服务器
- `https_client_validation.cpp` - HTTPS验证客户端  
- `https_validation_framework.cpp` - 测试框架主程序
- `run_https_validation_tests.sh` - 自动化测试脚本
- `Makefile_https_validation` - 编译配置

### 证书文件结构
```
test/
├── ca.pem              # CA根证书
├── ca.key              # CA私钥
├── server.crt          # 服务器证书
├── server.key          # 服务器私钥
├── client.crt          # 客户端证书 (双向认证)
├── client.key          # 客户端私钥
└── invalid.crt         # 无效证书 (错误测试)
```

## 🚀 快速开始

### 1. 生成测试证书
```bash
./ssl_cert_generator.sh
```

### 2. 编译测试程序
```bash
make -f Makefile_https_validation all
```

### 3. 运行完整测试
```bash
./https_validation_framework
```

## 🧪 验证场景

### 基础验证测试
1. **无证书验证连接** - 跳过证书验证的基础HTTPS连接
2. **CA证书验证** - 使用CA证书验证服务器证书
3. **证书链验证** - 完整证书链路径验证
4. **SAN扩展验证** - Subject Alternative Name验证

### 双向认证测试  
5. **客户端证书认证** - mTLS双向认证成功
6. **缺失客户端证书** - mTLS认证失败处理

### 错误处理测试
7. **无效CA证书** - 错误CA证书处理
8. **过期证书** - 过期证书检测
9. **证书链断裂** - 不完整证书链处理

## 🔧 使用方式

### 启动验证服务器
```bash
# 基础HTTPS服务器
./https_server_validation 8443

# 双向认证服务器
./https_server_validation --client-cert 8444
```

### 运行客户端验证
```bash
# 基础验证
./https_client_validation 127.0.0.1 8443

# 使用CA证书验证
./https_client_validation --ca ca.pem 127.0.0.1 8443

# 双向认证
./https_client_validation --ca ca.pem --cert client.crt --key client.key 127.0.0.1 8444
```

### 测试端点
- `/cert/info` - 证书信息查询
- `/cert/verify` - 证书验证状态
- `/health` - 健康检查

## 🔐 证书验证机制

### 与OpenSSL标准一致
本框架实现的证书验证机制与标准OpenSSL应用完全一致：

1. **证书链验证** - 从服务器证书到CA根证书的完整验证
2. **有效期检查** - 证书有效期时间范围验证
3. **签名验证** - 数字签名有效性检查
4. **主机名验证** - SAN扩展或CN字段主机名匹配
5. **撤销检查** - 支持CRL/OCSP撤销状态检查

### 验证配置
```cpp
ssl_config config;
config._enable = true;
config._cert_file = "server.crt";
config._key_file = "server.key";
config._ca_file = "ca.pem";
config._verify_peer = true;          // 启用对等验证
config._verify_depth = 4;            // 证书链深度
config._protocols = "TLSv1.2:TLSv1.3"; // 支持的TLS版本
config._cipher_list = "HIGH:!aNULL:!MD5"; // 加密套件
```

## 📊 测试输出示例

```
🧪 执行: 使用CA证书验证服务器
   目标: https://127.0.0.1:8443
   验证: 启用
   CA: ca.pem

[CERT-VERIFY] 深度0: /C=CN/ST=Test/L=Test/O=MyFrameTest/CN=localhost
[CERT-VERIFY] ✅ 验证通过

📋 服务器证书详细信息:
   序列号: 1234567890ABCDEF
   有效期开始: Jan  1 00:00:00 2025 GMT
   有效期结束: Dec 31 23:59:59 2025 GMT
   SAN扩展: DNS:localhost IP:127.0.0.1

✅ 通过 (45.2ms)
   TLS: TLSv1.3 | 加密: TLS_AES_256_GCM_SHA384
```

## 🎯 验证要点

1. **证书格式** - 使用PEM格式证书，与OpenSSL标准一致
2. **验证回调** - 实现自定义验证回调函数，记录验证过程
3. **错误处理** - 完整的SSL错误码处理和错误信息输出
4. **性能测量** - 连接时间测量，评估SSL握手性能
5. **兼容性测试** - 支持不同TLS版本和加密套件

## 🔍 故障排除

### 常见问题
1. **编译失败** - 检查OpenSSL开发库安装
2. **证书错误** - 重新运行证书生成脚本
3. **连接失败** - 检查服务器是否正常启动
4. **验证失败** - 检查CA证书路径和权限

### 调试模式
```bash
# 启用详细SSL调试输出
export OPENSSL_DEBUG=1
./https_validation_framework
```

---
*本测试框架确保您的HTTPS实现与标准OpenSSL应用具有相同的安全验证级别*