# CPU–SAU 接口冻结协议

建议沿用 Spirit 的 HC 接口。

## 接口信号

| 信号 | 方向 | 含义 |
|---|---|---|
| `csr_addr[11:0]` | CPU → SAU | SAU 寄存器/操作地址 |
| `csr_re` | CPU → SAU | 读请求 |
| `csr_we` | CPU → SAU | 写请求 |
| `csr_write_type[1:0]` | CPU → SAU | Write/Set/Clear |
| `csr_wdata[63:0]` | CPU → SAU | `{rs2, rs1}` |
| `csr_vestart[31:0]` | CPU → SAU | SAU 指令固定为 0 |
| `csr_valid` | SAU → CPU | SAU 完成本次请求 |
| `csr_rdata[31:0]` | SAU → CPU | 返回 CPU 的结果 |

## 握手约定

- CPU 持续保持请求及请求字段不变，直到收到 `csr_valid=1`。
- SAU 不能把持续多周期的同一个请求重复接收。
- 即使是 `MSETINS` 写请求，SAU 也必须返回一次 `csr_valid`，否则 CPU 会一直等待。
- `csr_valid` 只拉高一个周期。
