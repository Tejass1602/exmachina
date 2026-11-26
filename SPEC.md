# ExMachina — Full Technical Specification & Autonomous Build Plan

## 1. System Architecture & Research Foundations

**ExMachina** is a split-edge, hardware-gated electronics workbench copilot combining a **Raspberry Pi Pico 2 W (`RP2350A`) Smart Bench Probe** with a **Linux/CUDA Host Brain (`exmd`)** on an NVIDIA RTX 3050 Laptop (`sm_86`, 6 GB VRAM).

Every core subsystem adopts a specific published engineering or research methodology:

1. **Indirect Prompt Injection Defense via Out-of-Band Hardware Capability Tokens**
   - *Reference:* Greshake et al., *"Not what you have signed up for: Compromising Real-World LLM-Integrated Applications with Indirect Prompt Injection"* (AISec 2023); Krawczyk, Bellare, & Canetti, *"HMAC: Keyed-Hashing for Message Authentication"* (RFC 2104, 1997).
   - *Implementation:* Any local LLM agent reading untrusted datasheets, compiler logs, or source repos can be hijacked to run destructive commands. ExMachina enforces an out-of-band hardware gate: `exmd` refuses to execute privileged patches or hardware stimulus commands unless authorized by a `PKT_PROBE_INTENT` packet carrying a monotonic `seq_num` and a 32-byte **RFC 2104 HMAC-SHA256** tag signed by the RP2350 using a secret key (OTP-locked in production; `EXM_DEV_HMAC_KEY` during prototyping). Verification MUST use constant-time comparison (`exm_ct_memcmp`) and reject `seq_num <= last_valid_seq`.

2. **Microcontroller Keyword Spotting (KWS) Pipeline**
   - *Reference:* Zhang et al., *"Hello Edge: Keyword Spotting on Microcontrollers"* (Arm Research, arXiv:1711.07128, 2017).
   - *Implementation:* Follows the Arm DS-CNN / CMSIS-DSP front-end spec: 16 kHz mono 16-bit PCM, 30–40 ms analysis frames with 10–20 ms stride, Hamming windowing, 256-pt Real FFT, and log-Mel / MFCC feature extraction quantized to INT8, keeping total SRAM under 128 KB on Cortex-M33. During prototyping (before I2S MEMS mic integration), `tools/mic_bridge.py` streams 10 ms (160-sample) live laptop mic frames over USB CDC into the RP2350 static ping-pong ring buffer.

3. **Zero-Driver-Stall GPU Arena Allocation & Warp-Shuffle Reductions**
   - *Reference:* Harris, *"Optimizing Parallel Reduction in CUDA"* (NVIDIA, 2007); NVIDIA Ampere Tuning Guide (`sm_86`).
   - *Implementation:* Avoids 0.2–2.0 ms `cudaMalloc`/`cudaFree` driver stalls by sub-allocating from a single pre-allocated 64 MB VRAM slab (`Arena`) using 256-byte bitwise alignment `(offset + 255) & ~255` for 100% coalesced L2/global loads and $O(1)$ per-query reset (`offset = 0`). Cosine similarity uses warp-level shuffle reductions (`__shfl_down_sync(0xFFFFFFFF, val, offset)`) to eliminate shared-memory bank conflicts.

4. **Hybrid Dense + Lexical Retrieval with Reciprocal Rank Fusion (RRF)**
   - *Reference:* Cormack, Clarke, & Büttcher, *"Reciprocal Rank Fusion outperforms Condorcet and individual Rank Learning Methods"* (SIGIR 2009); Robertson & Zaragoza, *"The Probabilistic Relevance Framework: BM25 and Beyond"* (FnTIR 2009).
   - *Implementation:* Dense embeddings blur exact hex addresses (`0x76`), register names (`WHO_AM_I`), and pin labels (`GP26`). ExMachina fuses L2-normalized 384-dim CUDA Cosine Similarity with Okapi BM25 ($k_1 = 1.5$, $b = 0.75$) via:
     `RRF(d) = 1.0f / (60.0f + rank_dense(d)) + 1.0f / (60.0f + rank_sparse(d))`
     followed by GPU Top-K selection (`K = 8`).

5. **Memory-Constrained Local Fine-Tuned Reasoning & Grammar Enforcement**
   - *Reference:* Dettmers et al., *"QLoRA: Efficient Finetuning of Quantized LLMs"* (NeurIPS 2023); Hooper et al., *"KVQuant: Towards 10 Million Context Length LLM Inference with KV Cache Quantization"* (2024).
   - *Implementation:* `exmd` targets a local `llama-server` (`127.0.0.1:8080`) running a 7B/8B hardware/code model compressed via `imatrix` `IQ3_M`/`IQ4_XS` (~3.5–4.1 GB VRAM) with `Q8_0` KV-cache quantization, leaving ~2 GB VRAM headroom for the CUDA Arena and desktop compositor. Responses are locked via GBNF grammars to output valid Unified Diffs or structured JSON bench diagnostics.

---

## 2. Hardware & Electrical Interface (`RP2350A` Pico 2 W)

| Pin / Peripheral | Function | Electrical / Circuit Detail |
| :--- | :--- | :--- |
| `GP0` / `GP1` (`i2c0`) | Digital Bus Sniffer | 3.3V I2C sweep (`0x08..0x77`) + `WHO_AM_I` register probe (`0x0F`, `0x75`, `0xD0`). |
| `GP26` (`ADC0`) | Rail Voltage & Ripple Probe | Behind 10:1 resistor divider (`100 kOhm` / `10 kOhm`, safe up to ~30V). 256 samples in 2 ms -> `rail_mv = mean * 11`, `ripple_mv = (max - min) * 11`. |
| `GP27` (`ADC1`) | Component / Continuity Probe | 1 kOhm pull-up to 3.3V reference for resistor/diode/short sanity checks. |
| `USB` (Full-Speed) | Composite CDC-ACM + HID | `/dev/ttyACM0` (921600 baud raw binary) + USB HID keyboard for emergency `Ctrl+C` halt. |
| `WL_GPIO0` | Status / Watchdog LED | Heartbeat indicator; blinks rapidly if host `exmd` heartbeat times out (> 2000 ms). |

---

## 3. Shared Wire Protocol Contract (`shared/exm_packet.h`)

All USB CDC frames use little-endian packed C structs (`#pragma pack(push, 1)`). Both `host/` and `firmware/` MUST include `shared/exm_packet.h`.

### Packet Header (`ExmHeader`, Exact Size: 8 Bytes)
- `uint16_t magic;` — Fixed `0x4558` (ASCII `"EX"`).
- `uint8_t  type;` — `ExmPacketType` enum.
- `uint8_t  flags;` — Bitmask (`0x01` = ACK required, `0x02` = HMAC signed).
- `uint16_t payload_len;` — Exact byte size of payload struct (max `512`).
- `uint16_t crc16;` — CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`) over `[magic..payload_len]` (6 bytes) + `payload_len` payload bytes.

### Enums & Payloads
- `ExmPacketType` (`uint8_t`): `PKT_AUDIO_FRAME = 0x01`, `PKT_PROBE_INTENT = 0x02`, `PKT_PROBE_CMD = 0x03`, `PKT_HEARTBEAT = 0x04`.
- `ExmIntentId` (`uint8_t`): `INTENT_NONE = 0x00`, `INTENT_SCAN_BUS = 0x01`, `INTENT_CHECK_RAIL = 0x02`, `INTENT_BUILD_FIX = 0x03`, `INTENT_APPLY_PATCH = 0x04`, `INTENT_EMERGENCY_HALT = 0x05`.
- `ExmProbeOp` (`uint8_t`): `PROBE_OP_SCAN_I2C = 0x01`, `PROBE_OP_READ_RAIL = 0x02`, `PROBE_OP_PULSE_STIM = 0x03`.
- **`ExmAudioFrame` (324 bytes):** `uint32_t seq; int16_t pcm[160];`
- **`ExmProbeIntent` (52 bytes):** `uint8_t intent_id; uint8_t confidence; uint32_t seq_num; uint16_t rail_mv; uint16_t ripple_mv; uint8_t i2c_count; uint8_t i2c_addrs[8]; uint8_t who_am_i; uint8_t hmac_sha256[32];` (HMAC computed over the first 20 bytes).
- **`ExmProbeCmd` (4 bytes):** `uint8_t probe_op; uint8_t param0; uint16_t param1;`
- **`ExmHeartbeat` (5 bytes):** `uint32_t uptime_ms; uint8_t status_flags;`

---

## 4. Resource & Performance Budgets
- **Host VRAM (RTX 3050 6 GB):** `64 MB` Arena; `< 4.2 GB` LLM + KV cache; `< 1.0 ms` CUDA Cosine + RRF + Top-K on `N = 10,000`, `D = 384`, `K = 8`.
- **Firmware SRAM (RP2350A 520 KB):** Zero `malloc`/`free` in firmware; all audio/FFT/I2C buffers statically allocated; `< 128 KB` total RAM usage.

---

## 5. Ordered Milestones (Execute Strictly One at a Time)

### - [ ] **M1: Shared Wire Protocol & Flat CUDA Arena Allocator**
- **Deliverables:**
  1. `shared/exm_packet.h`: All packed structs above, `static_assert` checks (`sizeof(ExmHeader) == 8`, `sizeof(ExmAudioFrame) == 324`, `sizeof(ExmProbeIntent) == 52`, `sizeof(ExmProbeCmd) == 4`, `sizeof(ExmHeartbeat) == 5`), and `static inline uint16_t exm_crc16(const uint8_t* data, size_t len)`.
  2. `host/CMakeLists.txt`: CMake >= 3.24, C++20/CUDA20, `CMAKE_CUDA_ARCHITECTURES 86`, `-O3 -lineinfo -Wall -Wextra`, include dirs `../shared` and `include`. Builds `exm-bench`.
  3. `host/include/core/arena.cuh`: Flat C-style `struct Arena { uint8_t* base_ptr; size_t capacity; size_t offset; };` with free functions `arena_init`, `arena_alloc` (256-byte aligned, prints error and aborts on OOM), `arena_reset`, `arena_free`, `EXM_CUDA_CHECK` macro, and flat `struct CudaTimer` (`cuda_timer_init`, `cuda_timer_start`, `cuda_timer_stop_ms`, `cuda_timer_destroy`).
  4. `host/src/main_bench.cu`: Queries RTX 3050 properties, runs CRC-16 round-trip + bit-flip corruption test on `ExmProbeIntent`, benchmarks 1,000 iterations of `cudaMalloc`/`cudaFree` vs `arena_alloc`/`arena_reset` (4 MB each), and runs a 4 MB float write kernel to report effective GB/s.
- **Acceptance Gate:** `cmake -B host/build -S host && cmake --build host/build -j && ./host/build/exm-bench` exits `0`, prints `Arena` speedup > 100x, and passes CRC-16 tests.

### - [ ] **M2: Live Laptop Mic & Telemetry Bridge (`tools/mic_bridge.py`)**
- **Deliverables:**
  1. `tools/mic_bridge.py`: Zero-external-dependency Python 3 script (`struct`, `subprocess`, `argparse`, `termios`, `select`, `math`) with optional `pyserial` fallback.
  2. **CLI Flags:** `--port /dev/ttyACM0`, `--baud 921600`, `--loopback` (offline self-test: generates synthetic 440 Hz sine PCM frames, packs/unpacks `ExmAudioFrame`, `ExmProbeIntent`, and `ExmHeartbeat`, and verifies CRC-16 matches `shared/exm_packet.h`).
  3. **Live Audio Capture:** Spawns `parec --format=s16le --rate=16000 --channels=1`, reads 320-byte (10 ms) chunks, frames into `PKT_AUDIO_FRAME`, transmits to serial, and concurrently decodes/prints incoming `PKT_PROBE_INTENT` and `PKT_HEARTBEAT` packets.
- **Acceptance Gate:** `python3 -m py_compile tools/mic_bridge.py && python3 tools/mic_bridge.py --loopback` exits `0` with verified CRC-16 round-trip output.

### - [ ] **M3: CUDA Hybrid Cosine + RRF + Top-K Retrieval Kernel**
- **Deliverables:**
  1. `host/include/kernels/rrf_topk.cuh` and `host/src/kernels/rrf_topk.cu`:
     - `launch_cosine_sim(...)`: Computes dot product of a normalized 384-dim query vector against `N` normalized 384-dim document vectors in `Arena` using warp-shuffle (`__shfl_down_sync`) reductions.
     - `launch_rrf_topk(...)`: Takes `N` dense scores + `N` sparse BM25 scores, computes dense/sparse ranks, evaluates `RRF(d) = 1.0f/(60.0f + r_dense) + 1.0f/(60.0f + r_sparse)`, and extracts Top-K (`K = 8`) indices and scores.
  2. Update `host/src/main_bench.cu`: Add deterministic CPU reference for `N = 10000`, `D = 384`, `K = 8`. Assert `max_abs_err < 1e-5` on cosine similarity, exact Top-8 match against CPU reference, and GPU kernel latency `< 1.0 ms`.
- **Acceptance Gate:** `exm-bench` builds cleanly and passes all accuracy and `< 1.0 ms` latency assertions.

### - [ ] **M4: Structural Datasheet & Code Chunk Indexer (`host/src/ast/`)**
- **Deliverables:**
  1. `host/include/ast/indexer.h` and `host/src/ast/indexer.cpp`:
     - Parses `.c`, `.h`, `.cu`, `.cuh`, `.cpp`, and `.md` (datasheet/reference manual) files into flat `ExmChunk` structs (path, symbol/heading, line range, text, token counts).
     - Computes Okapi BM25 (`k1 = 1.5f`, `b = 0.75f`) lexical scores.
     - Implements deterministic 384-dim L2-normalized feature-hashing embedder so the CUDA retrieval pipeline works 100% offline without external embedding services.
  2. Unit test in `exm-bench` indexing `shared/exm_packet.h` and asserting query `"ExmProbeIntent hmac_sha256"` returns the exact struct chunk at Rank #1.
- **Acceptance Gate:** `exm-bench` passes end-to-end indexer + CUDA hybrid retrieval test.

### - [ ] **M5: Linux Host Daemon (`exmd`), HMAC Gate & Compiler Sandbox (`host/src/daemon/`)**
- **Deliverables:**
  1. `host/include/daemon/hmac_sha256.h`: Self-contained, zero-dependency C SHA-256 and RFC 2104 HMAC-SHA256 with constant-time `exm_ct_memcmp` and RFC 4231 test vector verification.
  2. `host/include/daemon/sandbox.h` and `host/src/daemon/sandbox.cpp`: Creates `/tmp/exm-sandbox-XXXXXX`, copies target files, applies Unified Diff, runs `nvcc`/`g++` verification, and cleans up `/tmp`.
  3. `host/grammars/unified_diff.gbnf`: Strict GBNF grammar for Unified Diff and JSON bench diagnostics.
  4. `host/src/daemon/exmd.cpp` (added as `exmd` target in `host/CMakeLists.txt`):
     - Flags: `--port /dev/ttyACM0`, `--llm-url http://127.0.0.1:8080/completion`, `--mock-serial`.
     - Uses Linux `epoll` + `timerfd` (500 ms `PKT_HEARTBEAT` TX), fuses `ExmProbeIntent` electrical telemetry with M4+M3 CUDA RAG context, and posts to `llama-server` over POSIX sockets.
     - `--mock-serial` tests: (1) valid signed `PKT_PROBE_INTENT` accepted, (2) tampered HMAC rejected, (3) replayed `seq_num` rejected, (4) sandbox compile gate passes.
- **Acceptance Gate:** `./host/build/exmd --mock-serial` exits `0` with all 4 security and sandbox checks passing.

### - [ ] **M6: Zephyr Smart Bench Probe Firmware (`firmware/`)**
- **Deliverables:**
  1. `firmware/CMakeLists.txt`, `firmware/prj.conf`, `firmware/src/main.c` targeting `rpi_pico2/rp2350a/m33/w`.
  2. Enables USB CDC-ACM (`CONFIG_USB_DEVICE_STACK`, `CONFIG_USB_CDC_ACM`), I2C (`CONFIG_I2C`), and ADC (`CONFIG_ADC`).
  3. Implements:
     - `probe_scan_i2c()`: Sweeps `i2c0` (`0x08..0x77`) and reads `WHO_AM_I` (`0x0F`, `0x75`, `0xD0`).
     - `probe_read_rail()`: Samples `ADC0` (`GP26`) 256 times, scales by 11x, computes `rail_mv` and `ripple_mv`.
     - Static 320-byte ping-pong audio ring buffer receiving `PKT_AUDIO_FRAME` + Hamming/RMS energy trigger stub.
     - Signs `ExmProbeIntent` via shared HMAC-SHA256 helper and transmits over USB CDC with 500 ms `PKT_HEARTBEAT` watchdog.
- **Acceptance Gate:** `west build -b rpi_pico2/rp2350a/m33/w firmware -d firmware/build` exits `0` (or passes strict syntax check if Zephyr env is not sourced).
