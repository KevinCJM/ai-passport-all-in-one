"""Match a known board before opening a serial port or toggling reset lines."""
import re


def paired_port(mac, ports):
    identity = re.sub('[:-]', '', mac).lower()
    if not re.fullmatch('[0-9a-f]{12}', identity):
        raise ValueError('invalid paired device identity')
    matches = [p.device for p in ports if p.vid == 0x303a and
               re.sub('[:-]', '', p.serial_number or '').lower() == identity]
    if len(matches) != 1:
        raise ConnectionError('已配对的 AI Passport 不在线或身份不唯一；不打开其他串口')
    return matches[0]
