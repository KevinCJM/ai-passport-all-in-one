#!/usr/bin/env python3
"""对讲机 from the Mac: talk to the badges, or listen to them (tool_tasks.cpp speaks the same protocol).

  walkie_mac.py say "你好，收到请回答"      speak a sentence (macOS say) on channel 1
  walkie_mac.py send voice.wav              send any audio file afconvert reads
  walkie_mac.py listen --seconds 30         save what the badges say to walkie.wav
  walkie_mac.py selftest                    encode, pack, parse and decode a tone

Needs libopus (brew install opus). Packets: UDP broadcast to port 7310, a 28-byte header, then one 60 ms Opus frame
(16 kHz mono, at most 120 bytes); a talk ends with three copies of an end packet (flags 1, no audio).
"""
import argparse, ctypes, math, os, random, socket, struct, subprocess, sys, tempfile, time, wave

PORT, RATE, FRAME, MAX_OPUS = 7310, 16000, 960, 120
HEADER = struct.Struct('<2sBB3sBHH16s')  # magic, version, channel, sender, flags, talk, seq, name
opus = ctypes.CDLL('/opt/homebrew/lib/libopus.dylib')
opus.opus_encoder_create.restype = opus.opus_decoder_create.restype = ctypes.c_void_p
opus.opus_encoder_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
opus.opus_decoder_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
opus.opus_encode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
opus.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]


def encoder():
    error = ctypes.c_int()
    return opus.opus_encoder_create(RATE, 1, 2048, ctypes.byref(error))  # OPUS_APPLICATION_VOIP


def decoder():
    error = ctypes.c_int()
    return opus.opus_decoder_create(RATE, 1, ctypes.byref(error))


def encode(enc, pcm: bytes) -> bytes:
    out = ctypes.create_string_buffer(MAX_OPUS)  # the size cap also caps the bitrate
    n = opus.opus_encode(enc, pcm, FRAME, out, MAX_OPUS)
    assert n > 0, n
    return out.raw[:n]


def decode(dec, data: bytes) -> bytes:
    out = ctypes.create_string_buffer(FRAME * 2)
    n = opus.opus_decode(dec, data, len(data), out, FRAME, 0)
    assert n == FRAME, n
    return out.raw


def pack(channel, sender, talk, seq, name, audio=b''):
    return HEADER.pack(b'PT', 1, channel, sender, 0 if audio else 1, talk, seq & 0xffff, name.encode()[:16]) + audio


def unpack(packet):
    magic, version, channel, sender, flags, talk, seq, name = HEADER.unpack_from(packet)
    if magic != b'PT' or version != 1:
        return None
    return channel, sender, flags & 1, talk, seq, name.split(b'\0')[0].decode(errors='replace'), packet[HEADER.size:]


def pcm16k(path) -> bytes:
    with tempfile.TemporaryDirectory() as folder:
        out = os.path.join(folder, 'out.wav')
        subprocess.run(['afconvert', '-f', 'WAVE', '-d', 'LEI16@16000', '-c', '1', path, out], check=True)
        with wave.open(out) as w:
            return w.readframes(w.getnframes())


def send(pcm: bytes, channel, to, name):
    enc, sock = encoder(), socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sender, talk = random.randbytes(3), random.randrange(65536)
    pcm += b'\0' * (-len(pcm) % (FRAME * 2))
    start = time.monotonic()
    for seq in range(len(pcm) // (FRAME * 2)):
        sock.sendto(pack(channel, sender, talk, seq, name, encode(enc, pcm[seq * FRAME * 2:(seq + 1) * FRAME * 2])), (to, PORT))
        time.sleep(max(0, start + (seq + 1) * 0.06 - time.monotonic()))  # real time, as a microphone would
    for _ in range(3):
        sock.sendto(pack(channel, sender, talk, seq + 1, name), (to, PORT)); time.sleep(0.02)
    print(f'sent {seq + 1} frames ({(seq + 1) * 0.06:.1f} s) on channel {channel} to {to}')


def listen(seconds, out, channel):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', PORT)); sock.settimeout(0.5)
    dec, frames, last, end = decoder(), [], {}, time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            packet, (host, _) = sock.recvfrom(512)
        except socket.timeout:
            continue
        p = unpack(packet)
        if not p or p[0] != channel:
            continue
        _, sender, ended, talk, seq, name, audio = p
        key = (sender, talk)
        if key in last and (seq - last[key]) & 0xffff >= 0x8000 or last.get(key) == seq:
            continue  # a copy
        if key not in last:
            print(f'{name or "?"} ({host}) talking')
        last[key] = seq
        if ended:
            print(f'{name or "?"} done')
        else:
            frames.append(decode(dec, audio))
    with wave.open(out, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(b''.join(frames))
    print(f'{len(frames)} frames ({len(frames) * 0.06:.1f} s) saved to {out}')


def selftest():
    tone = b''.join(struct.pack('<h', int(8000 * math.sin(2 * math.pi * 440 * n / RATE))) for n in range(FRAME * 5))
    enc, dec, energy = encoder(), decoder(), 0
    for seq in range(5):
        packet = pack(3, b'\1\2\3', 7, seq, '测试胸牌', encode(enc, tone[seq * FRAME * 2:(seq + 1) * FRAME * 2]))
        assert len(packet) <= 28 + MAX_OPUS and HEADER.size == 28
        channel, sender, ended, talk, got, name, audio = unpack(packet)
        assert (channel, sender, ended, talk, got, name) == (3, b'\1\2\3', 0, 7, seq, '测试胸牌')
        samples = struct.unpack(f'<{FRAME}h', decode(dec, audio))
        energy = max(energy, max(abs(s) for s in samples))
    assert energy > 3000, energy  # the tone came through
    assert unpack(pack(3, b'\1\2\3', 7, 5, 'x'))[2] == 1
    print('selftest ok')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['say', 'send', 'listen', 'selftest'])
    parser.add_argument('what', nargs='?', help='the sentence (say) or the audio file (send)')
    parser.add_argument('--channel', type=int, default=1)
    parser.add_argument('--to', default='255.255.255.255', help='broadcast address (default: the local network)')
    parser.add_argument('--name', default='Mac')
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--out', default='walkie.wav')
    args = parser.parse_args()
    if args.command == 'selftest':
        return selftest()
    if args.command == 'listen':
        return listen(args.seconds, args.out, args.channel)
    if not args.what:
        sys.exit('say needs a sentence, send a file')
    if args.command == 'say':
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, 'say.aiff')
            subprocess.run(['say', '-v', 'Tingting', '-o', path, args.what], check=True)
            pcm = pcm16k(path)
    else:
        pcm = pcm16k(args.what)
    send(pcm, args.channel, args.to, args.name)


if __name__ == '__main__':
    main()
