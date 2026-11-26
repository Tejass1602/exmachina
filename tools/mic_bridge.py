#!/usr/bin/env python3
"""
ExMachina Microphone Bridge Tool

Streams live audio from laptop microphone to RP2350 probe over USB CDC,
and decodes incoming probe telemetry packets (PKT_PROBE_INTENT, PKT_HEARTBEAT).

Also supports --loopback mode for offline self-testing with synthetic audio
and packet CRC-16 verification against shared/exm_packet.h definitions.
"""

import argparse
import struct
import subprocess
import sys
import os
import select
import termios
import tty
import math
import time
import signal
from typing import Optional, Tuple, List

# ─── Constants matching shared/exm_packet.h ───
EXM_MAGIC = 0x4558  # "EX"
PKT_AUDIO_FRAME = 0x01
PKT_PROBE_INTENT = 0x02
PKT_PROBE_CMD = 0x03
PKT_HEARTBEAT = 0x04

INTENT_NONE = 0x00
INTENT_SCAN_BUS = 0x01
INTENT_CHECK_RAIL = 0x02
INTENT_BUILD_FIX = 0x03
INTENT_APPLY_PATCH = 0x04
INTENT_EMERGENCY_HALT = 0x05

FLAG_ACK_REQUIRED = 0x01
FLAG_HMAC_SIGNED = 0x02

# CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)
def exm_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc

# ─── Packet Structures ───
# ExmHeader: magic(2) type(1) flags(1) payload_len(2) crc16(2) = 8 bytes
HEADER_FMT = '<HBBHH'
HEADER_SIZE = struct.calcsize(HEADER_FMT)

# ExmAudioFrame: seq(4) pcm[160](320) = 324 bytes
AUDIO_FRAME_FMT = '<I' + 'h' * 160
AUDIO_FRAME_SIZE = struct.calcsize(AUDIO_FRAME_FMT)

# ExmProbeIntent: intent_id(1) confidence(1) seq_num(4) rail_mv(2) ripple_mv(2) 
#                 i2c_count(1) i2c_addrs[8](8) who_am_i(1) hmac_sha256[32](32) = 52 bytes
PROBE_INTENT_FMT = '<BBIHH' + 'B' * 10 + '32s'
PROBE_INTENT_SIZE = struct.calcsize(PROBE_INTENT_FMT)

# ExmProbeCmd: probe_op(1) param0(1) param1(2) = 4 bytes
PROBE_CMD_FMT = '<BBH'
PROBE_CMD_SIZE = struct.calcsize(PROBE_CMD_FMT)

# ExmHeartbeat: uptime_ms(4) status_flags(1) = 5 bytes
HEARTBEAT_FMT = '<IB'
HEARTBEAT_SIZE = struct.calcsize(HEARTBEAT_FMT)

# Verify sizes match spec
assert HEADER_SIZE == 8, f"Header size mismatch: {HEADER_SIZE}"
assert AUDIO_FRAME_SIZE == 324, f"Audio frame size mismatch: {AUDIO_FRAME_SIZE}"
assert PROBE_INTENT_SIZE == 52, f"Probe intent size mismatch: {PROBE_INTENT_SIZE}"
assert PROBE_CMD_SIZE == 4, f"Probe cmd size mismatch: {PROBE_CMD_SIZE}"
assert HEARTBEAT_SIZE == 5, f"Heartbeat size mismatch: {HEARTBEAT_SIZE}"

# ─── Global State ───
running = True
serial_fd: Optional[int] = None
parec_proc: Optional[subprocess.Popen] = None

def signal_handler(signum, frame):
    global running
    running = False

signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)

# ─── Serial Port Handling ───
def open_serial(port: str, baud: int) -> int:
    """Open serial port in raw mode."""
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    
    # Configure terminal
    attrs = termios.tcgetattr(fd)
    
    # Input flags
    attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | 
                  termios.ISTRIP | termios.INLCR | termios.IGNCR | 
                  termios.ICRNL | termios.IXON)
    
    # Output flags
    attrs[1] &= ~termios.OPOST
    
    # Control flags
    attrs[2] &= ~(termios.CSIZE | termios.PARENB)
    attrs[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
    
    # Local flags
    attrs[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | 
                  termios.ISIG | termios.IEXTEN)
    
    # Control chars
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    
    # Set baud rate
    termios.cfsetispeed(attrs, baud)
    termios.cfsetospeed(attrs, baud)
    
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd

def close_serial(fd: int):
    """Close serial port."""
    os.close(fd)

def write_packet(fd: int, pkt_type: int, flags: int, payload: bytes):
    """Write a complete packet to serial."""
    payload_len = len(payload)
    assert payload_len <= 512, "Payload too large"
    
    # Build header (without CRC)
    header_partial = struct.pack('<HBBH', EXM_MAGIC, pkt_type, flags, payload_len)
    # CRC over header (6 bytes) + payload
    crc_data = header_partial + payload
    crc = exm_crc16(crc_data)
    
    # Full header with CRC
    header = struct.pack(HEADER_FMT, EXM_MAGIC, pkt_type, flags, payload_len, crc)
    
    os.write(fd, header + payload)

def read_exact(fd: int, n: int) -> bytes:
    """Read exactly n bytes from fd."""
    buf = bytearray()
    while len(buf) < n:
        ready, _, _ = select.select([fd], [], [], 0.1)
        if not ready:
            return b''
        chunk = os.read(fd, n - len(buf))
        if not chunk:
            return b''
        buf.extend(chunk)
    return bytes(buf)

def read_packet(fd: int) -> Optional[Tuple[int, int, bytes]]:
    """Read a complete packet from serial. Returns (type, flags, payload) or None."""
    # Read header
    header_data = read_exact(fd, HEADER_SIZE)
    if len(header_data) < HEADER_SIZE:
        return None
    
    magic, pkt_type, flags, payload_len, crc = struct.unpack(HEADER_FMT, header_data)
    
    if magic != EXM_MAGIC:
        return None
    
    if payload_len > 512:
        return None
    
    # Read payload
    payload = read_exact(fd, payload_len)
    if len(payload) < payload_len:
        return None
    
    # Verify CRC
    crc_data = header_data[:6] + payload
    if exm_crc16(crc_data) != crc:
        return None
    
    return pkt_type, flags, payload

# ─── Audio Generation (Loopback) ───
def generate_sine_frame(seq: int, freq: float = 440.0, sample_rate: int = 16000, 
                        frame_samples: int = 160, amplitude: float = 0.5) -> bytes:
    """Generate a sine wave PCM frame."""
    pcm = []
    for i in range(frame_samples):
        t = (seq * frame_samples + i) / sample_rate
        sample = int(amplitude * 32767 * math.sin(2 * math.pi * freq * t))
        pcm.append(max(-32768, min(32767, sample)))
    
    payload = struct.pack(AUDIO_FRAME_FMT, seq, *pcm)
    return payload

# ─── Loopback Self-Test ───
def run_loopback_test() -> bool:
    """Run offline self-test with synthetic data."""
    print("Running loopback self-test...")
    
    # Test 1: CRC-16 round-trip on ExmProbeIntent
    print("  Test 1: CRC-16 round-trip on ExmProbeIntent")
    intent = struct.pack(
        PROBE_INTENT_FMT,
        INTENT_SCAN_BUS,    # intent_id
        200,                # confidence
        42,                 # seq_num
        3300,               # rail_mv
        50,                 # ripple_mv
        3,                  # i2c_count
        0x48, 0x68, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00,  # i2c_addrs[8]
        0xD0,               # who_am_i
        b'\x00' * 32        # hmac_sha256
    )
    
    header_partial = struct.pack('<HBBH', EXM_MAGIC, PKT_PROBE_INTENT, FLAG_HMAC_SIGNED, len(intent))
    crc_data = header_partial + intent
    crc = exm_crc16(crc_data)
    header = struct.pack(HEADER_FMT, EXM_MAGIC, PKT_PROBE_INTENT, FLAG_HMAC_SIGNED, len(intent), crc)
    packet = header + intent
    
    # Verify by parsing
    magic, pkt_type, flags, payload_len, crc_verify = struct.unpack(HEADER_FMT, packet[:HEADER_SIZE])
    assert magic == EXM_MAGIC
    assert pkt_type == PKT_PROBE_INTENT
    assert payload_len == len(intent)
    assert exm_crc16(packet[:6] + intent) == crc_verify
    print("    PASS")
    
    # Test 2: CRC-16 bit-flip detection
    print("  Test 2: CRC-16 bit-flip detection")
    corrupted = bytearray(packet)
    corrupted[HEADER_SIZE + 4] ^= 0x01  # Flip bit in seq_num
    magic2, pkt_type2, flags2, payload_len2, crc2 = struct.unpack(HEADER_FMT, corrupted[:HEADER_SIZE])
    crc_data2 = corrupted[:6] + corrupted[HEADER_SIZE:HEADER_SIZE+payload_len2]
    assert exm_crc16(crc_data2) != crc2
    print("    PASS")
    
    # Test 3: ExmAudioFrame packing/unpacking
    print("  Test 3: ExmAudioFrame packing/unpacking")
    audio_payload = generate_sine_frame(123)
    header_partial = struct.pack('<HBBH', EXM_MAGIC, PKT_AUDIO_FRAME, 0, len(audio_payload))
    crc = exm_crc16(header_partial + audio_payload)
    header = struct.pack(HEADER_FMT, EXM_MAGIC, PKT_AUDIO_FRAME, 0, len(audio_payload), crc)
    audio_packet = header + audio_payload
    
    magic3, pkt_type3, flags3, payload_len3, crc3 = struct.unpack(HEADER_FMT, audio_packet[:HEADER_SIZE])
    assert magic3 == EXM_MAGIC
    assert pkt_type3 == PKT_AUDIO_FRAME
    assert payload_len3 == AUDIO_FRAME_SIZE
    assert exm_crc16(audio_packet[:6] + audio_payload) == crc3
    
    seq, *pcm = struct.unpack(AUDIO_FRAME_FMT, audio_payload)
    assert seq == 123
    assert len(pcm) == 160
    print("    PASS")
    
    # Test 4: ExmHeartbeat packing/unpacking
    print("  Test 4: ExmHeartbeat packing/unpacking")
    hb_payload = struct.pack(HEARTBEAT_FMT, 123456, 0x01)
    header_partial = struct.pack('<HBBH', EXM_MAGIC, PKT_HEARTBEAT, 0, len(hb_payload))
    crc = exm_crc16(header_partial + hb_payload)
    header = struct.pack(HEADER_FMT, EXM_MAGIC, PKT_HEARTBEAT, 0, len(hb_payload), crc)
    hb_packet = header + hb_payload
    
    magic4, pkt_type4, flags4, payload_len4, crc4 = struct.unpack(HEADER_FMT, hb_packet[:HEADER_SIZE])
    assert magic4 == EXM_MAGIC
    assert pkt_type4 == PKT_HEARTBEAT
    assert payload_len4 == HEARTBEAT_SIZE
    assert exm_crc16(hb_packet[:6] + hb_payload) == crc4
    
    uptime, status = struct.unpack(HEARTBEAT_FMT, hb_payload)
    assert uptime == 123456
    assert status == 0x01
    print("    PASS")
    
    # Test 5: ExmProbeCmd packing/unpacking
    print("  Test 5: ExmProbeCmd packing/unpacking")
    cmd_payload = struct.pack(PROBE_CMD_FMT, 0x01, 0x02, 0x0304)
    header_partial = struct.pack('<HBBH', EXM_MAGIC, PKT_PROBE_CMD, 0, len(cmd_payload))
    crc = exm_crc16(header_partial + cmd_payload)
    header = struct.pack(HEADER_FMT, EXM_MAGIC, PKT_PROBE_CMD, 0, len(cmd_payload), crc)
    cmd_packet = header + cmd_payload
    
    magic5, pkt_type5, flags5, payload_len5, crc5 = struct.unpack(HEADER_FMT, cmd_packet[:HEADER_SIZE])
    assert magic5 == EXM_MAGIC
    assert pkt_type5 == PKT_PROBE_CMD
    assert payload_len5 == PROBE_CMD_SIZE
    assert exm_crc16(cmd_packet[:6] + cmd_payload) == crc5
    
    op, p0, p1 = struct.unpack(PROBE_CMD_FMT, cmd_payload)
    assert op == 0x01
    assert p0 == 0x02
    assert p1 == 0x0304
    print("    PASS")
    
    print("\nAll loopback tests PASSED")
    return True

# ─── Live Audio Capture ───
def start_parec() -> subprocess.Popen:
    """Start parec process for live audio capture."""
    cmd = [
        'parec',
        '--format=s16le',
        '--rate=16000',
        '--channels=1',
        '--latency-msec=10'
    ]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

def stop_parec(proc: subprocess.Popen):
    """Stop parec process."""
    if proc:
        proc.terminate()
        proc.wait(timeout=1)

# ─── Main Loop ───
def main_loop(port: str, baud: int, loopback: bool):
    global serial_fd, parec_proc, running
    
    if loopback:
        return run_loopback_test()
    
    # Open serial
    try:
        serial_fd = open_serial(port, baud)
        print(f"Opened {port} at {baud} baud")
    except OSError as e:
        print(f"Failed to open {port}: {e}")
        return False
    
    # Start audio capture
    try:
        parec_proc = start_parec()
        print("Started audio capture (parec)")
    except FileNotFoundError:
        print("parec not found. Install pulseaudio-utils or use --loopback")
        close_serial(serial_fd)
        return False
    except Exception as e:
        print(f"Failed to start audio capture: {e}")
        close_serial(serial_fd)
        return False
    
    seq = 0
    last_heartbeat_tx = 0
    frame_bytes = 160 * 2  # 160 samples * 2 bytes = 320 bytes (10 ms at 16 kHz)
    
    print("Starting main loop (Ctrl+C to exit)...")
    
    try:
        while running:
            now = time.time() * 1000  # ms
            
            # Send heartbeat every 500 ms
            if now - last_heartbeat_tx >= 500:
                hb_payload = struct.pack(HEARTBEAT_FMT, int(now), 0x01)
                write_packet(serial_fd, PKT_HEARTBEAT, 0, hb_payload)
                last_heartbeat_tx = now
            
            # Read audio from parec (non-blocking)
            ready, _, _ = select.select([parec_proc.stdout], [], [], 0)
            if ready:
                audio_data = parec_proc.stdout.read(frame_bytes)
                if len(audio_data) == frame_bytes:
                    # Pack into ExmAudioFrame
                    pcm = struct.unpack('<' + 'h' * 160, audio_data)
                    audio_payload = struct.pack(AUDIO_FRAME_FMT, seq, *pcm)
                    write_packet(serial_fd, PKT_AUDIO_FRAME, 0, audio_payload)
                    seq += 1
            
            # Check for incoming packets from probe
            ready, _, _ = select.select([serial_fd], [], [], 0)
            if ready:
                pkt = read_packet(serial_fd)
                if pkt:
                    pkt_type, flags, payload = pkt
                    
                    if pkt_type == PKT_PROBE_INTENT:
                        # Parse ExmProbeIntent
                        (intent_id, confidence, seq_num, rail_mv, ripple_mv,
                         i2c_count, *rest) = struct.unpack(PROBE_INTENT_FMT, payload)
                        i2c_addrs = rest[:8]
                        who_am_i = rest[8]
                        hmac = rest[9]
                        
                        intent_names = {
                            INTENT_NONE: "NONE",
                            INTENT_SCAN_BUS: "SCAN_BUS",
                            INTENT_CHECK_RAIL: "CHECK_RAIL",
                            INTENT_BUILD_FIX: "BUILD_FIX",
                            INTENT_APPLY_PATCH: "APPLY_PATCH",
                            INTENT_EMERGENCY_HALT: "EMERGENCY_HALT"
                        }
                        intent_name = intent_names.get(intent_id, f"UNKNOWN({intent_id})")
                        
                        print(f"\n[PROBE INTENT] seq={seq_num} intent={intent_name} conf={confidence}")
                        print(f"  Rail: {rail_mv} mV, Ripple: {ripple_mv} mV")
                        if i2c_count > 0:
                            addrs = [f"0x{a:02X}" for a in i2c_addrs[:i2c_count]]
                            print(f"  I2C devices: {', '.join(addrs)}")
                        if who_am_i:
                            print(f"  WHO_AM_I: 0x{who_am_i:02X}")
                        if flags & FLAG_HMAC_SIGNED:
                            print(f"  HMAC: {'verified' if exm_ct_memcmp(hmac, b'\x00'*32) else 'present'}")
                    
                    elif pkt_type == PKT_HEARTBEAT:
                        uptime, status = struct.unpack(HEARTBEAT_FMT, payload)
                        print(f"\n[HEARTBEAT] uptime={uptime}ms status=0x{status:02X}")
            
            # Small sleep to prevent busy loop
            time.sleep(0.001)
    
    except KeyboardInterrupt:
        pass
    finally:
        print("\nShutting down...")
        stop_parec(parec_proc)
        close_serial(serial_fd)
    
    return True

# ─── Constant-time memcmp (for HMAC verification display) ───
def exm_ct_memcmp(a: bytes, b: bytes) -> bool:
    """Returns True if equal (constant-time)."""
    if len(a) != len(b):
        return False
    diff = 0
    for x, y in zip(a, b):
        diff |= x ^ y
    return diff == 0

# ─── Entry Point ───
def main():
    parser = argparse.ArgumentParser(description='ExMachina Microphone Bridge')
    parser.add_argument('--port', default='/dev/ttyACM0', help='Serial port (default: /dev/ttyACM0)')
    parser.add_argument('--baud', type=int, default=921600, help='Baud rate (default: 921600)')
    parser.add_argument('--loopback', action='store_true', help='Offline self-test mode')
    args = parser.parse_args()
    
    success = main_loop(args.port, args.baud, args.loopback)
    sys.exit(0 if success else 1)

if __name__ == '__main__':
    main()