## MyFrame Ê¹ÓÃÖ¸ÄÏ£¨¿Í»§¶Ë / ·şÎñ¶Ë / ¶àĞ­Òé / ×Ô¶¨ÒåĞ­Òé£©

±¾Ö¸ÄÏ½éÉÜÈçºÎÔÚ MyFrame ÖĞ£º
- Ê¹ÓÃ·şÎñ¶Ë¿ò¼Ü²¢×¢²á¶àÖÖĞ­Òé£¨HTTP/HTTPS¡¢WS/WSS¡¢×Ô¶¨ÒåĞ­Òé£©
- Ê¹ÓÃ¿Í»§¶Ë£¨ÊÂ¼şÑ­»·°æ£©·ÃÎÊ¶àÖÖĞ­Òé
- À©Õ¹²¢¼¯³É×Ô¶¨ÒåĞ­Òé£¨·şÎñ¶ËÓë¿Í»§¶Ë£©

### 1. ·şÎñ¶Ë£º¶àĞ­Òé¼àÌıÓë·Ö·¢

- Èë¿ÚÀà£º`server`£¨²Î¼û `include/server.h`£©
- ÒµÎñ¹¤³§£º`MultiProtocolFactory`£¨²Î¼û `include/multi_protocol_factory.h`£©
  - ×Ô¶¯Ğ­Òé¼ì²âÓÉ `protocol_detect_process` Íê³É£¬Ì½²âÆ÷ÔÚ `core/protocol_probes.h` ÖĞ¶¨Òå£º
    - `TlsProbe`¡¢`HttpProbe`¡¢`WsProbe`¡¢`Http2Probe`¡¢`CustomProbe`
- ×î¼òÊ¾Àı£º`examples/simple_https_server.cpp` / `examples/simple_wss_server.cpp` / `examples/xproto_server.cpp`

Ê¾Àı£¨Õª×Ô `examples/xproto_server.cpp`£©£º
```cpp
auto handler = std::make_shared<XProtoHandler>(); // ÒµÎñ»Øµ÷£¬¼û core/app_handler.h
auto biz = std::make_shared<MultiProtocolFactory>(handler.get(), MultiProtocolFactory::Mode::Auto);
server srv(2);
srv.bind("127.0.0.1", 7790);
srv.set_business_factory(biz);
srv.start();
srv.join();
```

Òª×¢²á/¿ØÖÆÌ½²âÆ÷Ë³Ğò£ºÔÚ `core/multi_protocol_factory_impl.cpp` ÄÚÍ¨¹ı `_mode` Ñ¡Ôñ/ÔöÉ¾ `add_probe()` ¼´¿É¡£

### 2. ¿Í»§¶Ë£ºÊÂ¼şÑ­»·Â·ÓÉ£¨HTTP/HTTPS/WS/WSS£©

- Èë¿Ú£º`core/client_conn_router.h/.cpp` Ìá¹© `ClientConnRouter`
- URL scheme ×Ô¶¯Ñ¡Ôñ builder£º`http/https/ws/wss`
- HTTPS/WSS Ê¹ÓÃ `ClientSslCodec`£¨`core/client_ssl_codec.h`£©·Ç×èÈû `SSL_connect`

Ê¾Àı£º`examples/client_conn_factory_example.cpp`
```bash
build/examples/client_conn_factory_example http://127.0.0.1:7782/hello
build/examples/client_conn_factory_example https://127.0.0.1:443/
build/examples/client_conn_factory_example ws://127.0.0.1:7782/websocket
build/examples/client_conn_factory_example wss://127.0.0.1:443/ws
```

### 3. Ö±½ÓÊ¹ÓÃ out_connect<PROCESS> ¾«Ï¸¿ØÖÆ

ÊÊºÏĞèÒª¹ı³Ì¼¶¿ØÖÆ¡¢¸´ÓÃ¿ò¼ÜÊÂ¼şÑ­»·µÄ³¡¾°¡£
- HTTP ¿Í»§¶Ë£º`out_connect<http_req_process>` + `http_client_data_process`
  - Ê¾Àı£º`examples/http_out_client.cpp`
- WS ¿Í»§¶Ë£º`out_connect<web_socket_req_process>` + `web_socket_data_process`£¨»ò `app_ws_data_process`£©

### 4. ×Ô¶¨ÒåĞ­Òé£¨·şÎñ¶Ë£©

²½Öè£º
1) ÔÚ `core/protocol_probes.h` ÀïĞÂÔö `class XProtoProbe : public IProtocolProbe`£º
   - `match()`£º»ùÓÚÄ§Êı/Ç°×ºÅĞ¶Ï£¨ÀıÈçÇ° 4 ×Ö½ÚÎª `"XPRT"`£©
   - `create()`£º·µ»ØÄãµÄ `base_data_process` ÊµÀı£¨Èç `xproto_process`£©
2) ÔÚ `protocol_detect_process` ³õÊ¼»¯Ê± `add_probe(std::unique_ptr<XProtoProbe>(...))`
3) ÔÚ `xproto_process::process_recv_buf()` ÖĞ½âÎöÖ¡²¢µ÷ÓÃ `IAppHandler::on_custom(...)` »Øµ÷

¿ìËÙÆğ²½£º¿É¸´ÓÃ `custom_stream_process`£¨ÒÑÔÚ `CustomProbe` ÖĞÊ¾Àı»¯£©£¬Ëü»á°ÑÊÕµ½µÄÊı¾İ»Øµ÷¸ø `on_custom()` ²¢»ØÏÔ¡£

²Î¿¼Ê¾Àı£º`examples/xproto_server.cpp`

### 5. ×Ô¶¨ÒåĞ­Òé£¨¿Í»§¶Ë£©

·½°¸ A£º»ùÓÚÂ·ÓÉÆ÷ĞÂÔö scheme ¶ÔÓ¦µÄ builder
- ÔÚ `ClientConnRouter` ÖĞ×¢²á£º
```cpp
router.register_factory("xproto", [](const std::string& url, const ClientBuildCtx& ctx){
    // ½âÎö URL -> host/port
    // auto conn = std::make_shared< out_connect<xproto_req_process> >(host, port);
    // auto proc = new xproto_req_process(conn);
    // proc->set_process(new xproto_client_data_process(proc, ...));
    // conn->set_process(proc); conn->set_net_container(ctx.container); conn->connect();
    // return std::static_pointer_cast<base_net_obj>(conn);
});
```

·½°¸ B£º»ùÓÚÏÖÓĞ WS/HTTP Í¨µÀ·â×°ÄãµÄÔØºÉ£¨ÊÊºÏËíµÀ»¯/¿ìËÙÑéÖ¤£©
- Ö±½ÓÓÃ `ws://`/`wss://` builder£¬Êı¾İ½»ÓÉ·şÎñ¶Ë `on_ws` ´¦Àí¡£

²Î¿¼Ê¾Àı£º`examples/xproto_client.cpp`£¨ÑİÊ¾ÓÃ ws Í¨µÀ£©¡£

### 6. ÔËĞĞÊ¾Àı

¹¹½¨È«²¿£º
```bash
./scripts/build_cmake.sh all
```

·şÎñ¶Ë£º
```bash
build/examples/xproto_server 7790
```

¿Í»§¶Ë£¨ÊÂ¼şÑ­»·Â·ÓÉ£©£º
```bash
build/examples/client_conn_factory_example ws://127.0.0.1:7790/
```

### 7. TLS ¿Í»§¶ËÅäÖÃ

`ClientSslCodec` Ä¬ÈÏ²»Ç¿ÖÆÖ¤ÊéĞ£Ñé£¬¿ÉÔÚºóĞø°æ±¾À©Õ¹£º
- Ôö¼Ó¿Í»§¶Ë TLS ÅäÖÃ½Ó¿Ú£¬ÉèÖÃ CA¡¢Ğ£Ñé¿ª¹ØÓë SNI£»
- ÔÚ°²×° `ClientSslCodec` Ç°ÉèÖÃĞ£Ñé²ßÂÔ¡£

### 8. FAQ

- ÈçºÎ¸ÄÌ½²âÓÅÏÈ¼¶£¿
  - µ÷Õû `protocol_detect_process` ÖĞ `add_probe()` µÄË³Ğò¡£
- ÄÜÖ§³Ö HTTP/2 ¿Í»§¶ËÂğ£¿
  - µ±Ç°ÊÇ HTTP/1.1 ¿Í»§¶Ë¡£¿ÉĞÂÔö `h2_client`£¨º¬ ALPN¡¢HPACK¡¢Ö¡Á÷¿Ø£©£¬²¢×¢²áµ½Â·ÓÉÆ÷¡£



### é™„ï¼šHTTPS å®¢æˆ·ç«¯/æœåŠ¡ç«¯è¯ä¹¦é…ç½®ä¸ mTLS ç¤ºä¾‹

ä»¥ä¸‹ç¤ºä¾‹åŸºäºæ¡†æ¶å†…ç½®çš„ TLS è¿è¡Œæ—¶é…ç½®æ¥å£ï¼ˆssl_context.hï¼‰ä¸ç¯å¢ƒå˜é‡å…œåº•ç­–ç•¥ï¼Œé€‚ç”¨äº examples åŠä¸šåŠ¡ä»£ç ã€‚

- å…³é”®ç¯å¢ƒå˜é‡ï¼ˆå¯é€‰ï¼Œä¾¿äºå¿«é€Ÿè¯•éªŒï¼‰
  - æœåŠ¡ç«¯ï¼š
    - `MYFRAME_SSL_CERT`ã€`MYFRAME_SSL_KEY`ï¼šæœåŠ¡å™¨è¯ä¹¦/ç§é’¥ï¼ˆPEMï¼‰ã€‚
    - `MYFRAME_SSL_PROTOCOLS`ï¼šå¯ç”¨çš„åè®®èŒƒå›´ï¼ˆå¦‚ `TLSv1.2,TLSv1.3`ï¼‰ã€‚
    - `MYFRAME_SSL_ALPN`ï¼šALPN åˆ—è¡¨ï¼ˆå¦‚ `h2,http/1.1`ï¼‰ã€‚
    - `MYFRAME_SSL_VERIFY`ï¼šæ˜¯å¦éªŒè¯å¯¹ç«¯è¯ä¹¦ï¼ˆ`1/true` å¼€å¯ï¼Œç”¨äº mTLSï¼‰ã€‚
  - å®¢æˆ·ç«¯ï¼š
    - `MYFRAME_SSL_VERIFY`ï¼šæ˜¯å¦éªŒè¯æœåŠ¡ç«¯è¯ä¹¦ï¼ˆ`1/true` å¼€å¯ï¼‰ã€‚
    - `MYFRAME_SSL_CA`ï¼šCA è¯ä¹¦è·¯å¾„ã€‚
    - `MYFRAME_SSL_CLIENT_CERT`ã€`MYFRAME_SSL_CLIENT_KEY`ï¼šå®¢æˆ·ç«¯è¯ä¹¦/ç§é’¥ï¼ˆç”¨äº mTLSï¼‰ã€‚

- æœåŠ¡ç«¯æœ€å°ä»£ç é…ç½®ï¼ˆC++ï¼‰
```cpp
#include "ssl_context.h"
#include "tls_runtime.cpp" // æˆ–é“¾æ¥åˆ°æ ¸å¿ƒåº“åä»…åŒ…å«å¤´æ–‡ä»¶

void setup_tls_server() {
    ssl_config conf;
    conf._cert_file = "tests/common/cert/server.crt";
    conf._key_file  = "tests/common/cert/server.key";
    conf._protocols = "TLSv1.2,TLSv1.3";
    // è‹¥éœ€ mTLSï¼šè¦æ±‚æ ¡éªŒå¯¹ç«¯è¯ä¹¦ï¼Œå¹¶æŒ‡å®š CA
    // conf._verify_peer = true;
    // conf._ca_file = "tests/common/cert/ca.crt";
    tls_set_server_config(conf);
}

int main() {
    setup_tls_server();
    // å¯åŠ¨ https_server æˆ–ä»»ä½•åŸºäº server çš„ TLS æœåŠ¡
}
```

- å®¢æˆ·ç«¯æœ€å°ä»£ç é…ç½®ï¼ˆC++ï¼‰
```cpp
#include "ssl_context.h"

void setup_tls_client_verify_only() {
    ssl_config conf;
    conf._verify_peer = true;            // éªŒè¯æœåŠ¡ç«¯è¯ä¹¦
    conf._ca_file = "tests/common/cert/ca.crt"; // å—ä¿¡ CA
    tls_set_client_config(conf);
}

void setup_tls_client_mtls() {
    ssl_config conf;
    conf._verify_peer = true;
    conf._ca_file = "tests/common/cert/ca.crt";
    conf._cert_file = "tests/common/cert/client.crt"; // å®¢æˆ·ç«¯è¯ä¹¦
    conf._key_file  = "tests/common/cert/client.key"; // å®¢æˆ·ç«¯ç§é’¥
    tls_set_client_config(conf);
}
```

- è¿è¡Œç¤ºä¾‹
```bash
# æ–¹å¼Aï¼šä½¿ç”¨ç¯å¢ƒå˜é‡ï¼ˆå¿«é€Ÿï¼‰
export MYFRAME_SSL_CERT=tests/common/cert/server.crt
export MYFRAME_SSL_KEY=tests/common/cert/server.key
export MYFRAME_SSL_ALPN="h2,http/1.1"
./build_dbg/examples/https_server 8443 &

# å®¢æˆ·ç«¯æ ¡éªŒ + CA
export MYFRAME_SSL_VERIFY=1
export MYFRAME_SSL_CA=tests/common/cert/ca.crt
./build_dbg/examples/router_client https://127.0.0.1:8443/hello

# æ–¹å¼Bï¼šåœ¨ä»£ç ä¸­è®¾ç½®ï¼ˆæ›´å¯æ§ï¼‰
# å‚è€ƒä»¥ä¸Š C++ ç‰‡æ®µï¼Œè°ƒç”¨ tls_set_server_config / tls_set_client_config åå†å¯åŠ¨ç¤ºä¾‹
```

- mTLSï¼ˆåŒå‘è®¤è¯ï¼‰
```bash
# æœåŠ¡ç«¯å¼ºåˆ¶æ ¡éªŒå®¢æˆ·ç«¯è¯ä¹¦ï¼ˆä»£ç  conf._verify_peer=true ä¸” conf._ca_file ä¸º CA è·¯å¾„ï¼‰
# å®¢æˆ·ç«¯æä¾›è¯ä¹¦ä¸ç§é’¥
export MYFRAME_SSL_VERIFY=1
export MYFRAME_SSL_CA=tests/common/cert/ca.crt
export MYFRAME_SSL_CLIENT_CERT=tests/common/cert/client.crt
export MYFRAME_SSL_CLIENT_KEY=tests/common/cert/client.key
./build_dbg/examples/router_client https://127.0.0.1:8443/api/status
```

- H2/ALPN è¯´æ˜
  - æœåŠ¡ç«¯ ALPN ç¼ºçœå…è®¸ `h2,http/1.1`ï¼Œå¯é€šè¿‡ `MYFRAME_SSL_ALPN` ç²¾ç¡®æ§åˆ¶ã€‚
  - å®¢æˆ·ç«¯åœ¨ `https://` è·¯å¾„é»˜è®¤å‘èµ· ALPN `h2,http/1.1`ï¼Œåå•†åˆ° `h2` åˆ™è‡ªåŠ¨èµ° HTTP/2ï¼ˆå¤±è´¥å›é€€ HTTP/1.1ï¼‰ã€‚
  - å¼ºåˆ¶ HTTP/2 å¯ä½¿ç”¨ `h2://host[:port]/path`ã€‚

- å¸¸è§é—®é¢˜
  - è‡ªç­¾è¯ä¹¦ä½†å¼€å¯æ ¡éªŒï¼šéœ€æä¾› CA è·¯å¾„ï¼ˆ`MYFRAME_SSL_CA` æˆ– `tls_set_client_config` ä¸­ `_ca_file`ï¼‰ã€‚
  - mTLSï¼šæœåŠ¡ç«¯éœ€å¼€å¯ `_verify_peer=true` å¹¶è®¾å®š `_ca_file`ï¼›å®¢æˆ·ç«¯éœ€é…ç½®è‡ªèº« `client.crt/key`ã€‚
  - è¯ä¹¦åŠ è½½ä¸æ¡æ‰‹å¼‚å¸¸ï¼šæ£€æŸ¥è·¯å¾„ã€æƒé™ä»¥åŠåè®®èŒƒå›´ï¼ˆ`MYFRAME_SSL_PROTOCOLS`ï¼‰ã€‚

### é™„ï¼šé»˜è®¤è¯·æ±‚å¤´ä¸ gzip å‹ç¼©

- HTTP/1.1 å®¢æˆ·ç«¯
  - è‹¥æœªåœ¨ä¸šåŠ¡ headers ä¸­æ˜¾å¼æä¾›ï¼Œé»˜è®¤æ·»åŠ ï¼š
    - `User-Agent: myframe-http-client`
    - `Accept-Encoding: gzip`
    - å¹¶è‡ªåŠ¨è¡¥å…¨ `Host` ä¸ `Connection: close`
- HTTP/2 å®¢æˆ·ç«¯
  - è‹¥æœªåœ¨ä¸šåŠ¡ headers ä¸­æ˜¾å¼æä¾›ï¼Œé»˜è®¤æ·»åŠ ï¼š
    - `user-agent: myframe-h2-client`
    - `accept-encoding: gzip`
- è‡ªåŠ¨è§£å‹ï¼ˆå¯é€‰ï¼‰
  - æ„å»ºæ—¶æ£€æµ‹åˆ° zlib æ—¶ï¼Œå¯¹å« `gzip` ç¼–ç çš„å“åº”ä½“è‡ªåŠ¨è§£å‹å¹¶æ‰“å°è§£å‹åçš„é•¿åº¦/å†…å®¹ï¼ˆå°ä½“ç§¯æ—¶ï¼‰ã€‚
  - æœªå¯ç”¨ zlib æ—¶ä¿æŒåŸå§‹è¾“å‡ºã€‚
- å…³é—­é»˜è®¤ Accept-Encodingï¼ˆå¯é€‰ï¼‰
  - ç¯å¢ƒå˜é‡ï¼š`MYFRAME_NO_DEFAULT_AE=1`ï¼ˆHTTP/1.1 ä¸ HTTP/2 å‡ç”Ÿæ•ˆï¼‰ã€‚
  - è·¯ç”±å®¢æˆ·ç«¯ï¼š`router_client ... --no-accept-encoding`ï¼ˆå†…éƒ¨è®¾ç½®ä¸Šè¿°ç¯å¢ƒå˜é‡ï¼‰ã€‚
