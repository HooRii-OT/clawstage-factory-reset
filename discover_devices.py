#!/usr/bin/env python3
"""Bounded mDNS PTR discovery on ONE selected IPv4 /24; no port scanner."""
import ipaddress
import json
import platform
import re
import select
import socket
import struct
import subprocess
import time

DEVICE_NAME = re.compile(r'^ClawStage-[a-zA-Z0-9-]+\.local$', re.I)


def read_name(packet, offset):
    labels, end, visited = [], None, set()
    for _ in range(128):
        if offset >= len(packet) or offset in visited:
            raise ValueError('Truncated or cyclic DNS name')
        visited.add(offset)
        size = packet[offset]
        if size == 0:
            return '.'.join(labels), end if end is not None else offset + 1
        if size & 0xc0 == 0xc0:
            if offset + 1 >= len(packet):
                raise ValueError('Truncated DNS pointer')
            if end is None:
                end = offset + 2
            offset = ((size & 0x3f) << 8) | packet[offset + 1]
        else:
            if size > 63 or offset + size + 1 > len(packet):
                raise ValueError('Invalid DNS label')
            labels.append(packet[offset + 1:offset + 1 + size].decode('ascii'))
            offset += size + 1
    raise ValueError('DNS name nesting limit')


def parse_reply(packet, queries):
    if len(packet) < 12:
        return []
    _, flags, questions, answers, authority, additional = struct.unpack('!6H', packet[:12])
    if not flags & 0x8000:
        return []
    offset, found = 12, []
    for _ in range(questions):
        _, offset = read_name(packet, offset)
        offset += 4
    for _ in range(answers + authority + additional):
        owner, offset = read_name(packet, offset)
        if offset + 10 > len(packet):
            raise ValueError('Truncated DNS record')
        kind, cls, ttl, length = struct.unpack('!HHIH', packet[offset:offset + 10])
        offset += 10
        end = offset + length
        if end > len(packet):
            raise ValueError('Truncated DNS data')
        if kind == 12 and cls & 0x7fff == 1 and ttl and owner.lower() in queries:
            hostname, name_end = read_name(packet, offset)
            if name_end > end:
                raise ValueError('DNS name exceeds record')
            if DEVICE_NAME.fullmatch(hostname):
                found.append((hostname, queries[owner.lower()]))
        offset = end
    return found


def query(reverse, sequence):
    name = b''.join(bytes([len(label)]) + label.encode('ascii') for label in reverse.split('.')) + b'\0'
    return struct.pack('!6H', sequence, 0, 1, 0, 0, 0) + name + struct.pack('!HH', 12, 1)


def command_output(args):
    return subprocess.check_output(args, text=True, stderr=subprocess.PIPE, timeout=10)


def local_address(interface=None, system=None, run=command_output):
    """Linux iproute2 or macOS native route/ifconfig; ambiguity requires a choice."""
    system = system or platform.system()
    if interface and not re.fullmatch(r'[a-zA-Z0-9_.:-]+', interface):
        raise ValueError('Invalid interface name')
    if system == 'Linux':
        if not interface:
            routes = json.loads(run(['ip', '-j', '-4', 'route', 'show', 'default']))
            interfaces = {route['dev'] for route in routes if 'dev' in route}
            if len(interfaces) != 1:
                raise ValueError('Select one network with --interface, or specify --device IPv4')
            interface = interfaces.pop()
        entries = json.loads(run(['ip', '-j', '-4', 'addr', 'show', 'dev', interface]))
        addresses = [f"{addr['local']}/{addr['prefixlen']}" for entry in entries
                     for addr in entry.get('addr_info', []) if addr.get('family') == 'inet'
                     and addr.get('scope') == 'global']
    elif system == 'Darwin':
        if not interface:
            match = re.search(r'^\s*interface:\s*(\S+)\s*$', run(['route', '-n', 'get', 'default']), re.M)
            if not match:
                raise ValueError('No default network; specify --interface or --device')
            interface = match.group(1)
        addresses = []
        for addr, mask in re.findall(r'\binet\s+(\d+\.\d+\.\d+\.\d+)\s+netmask\s+(\S+)', run(['ifconfig', interface])):
            if mask.startswith('0x'):
                mask = str(ipaddress.IPv4Address(int(mask, 16)))
            addresses.append(f'{addr}/{mask}')
    else:
        raise ValueError('Use the Windows launcher on Windows; Unix client supports Linux/macOS')
    if len(addresses) != 1:
        raise ValueError('Expected one IPv4 on the selected interface; use --device IPv4')
    address = ipaddress.IPv4Interface(addresses[0])
    if address.network.prefixlen != 24 or address.ip.is_loopback or address.ip.is_link_local:
        raise ValueError('Automatic discovery supports a LAN /24 only; use --device IPv4')
    return address


def discover(address):
    queries = {host.reverse_pointer: str(host) for host in address.network.hosts()}
    found = set()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind((str(address.ip), 0))
        udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(str(address.ip)))
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
        for _ in range(2):
            for sequence, reverse in enumerate(queries, 1):
                udp.sendto(query(reverse, sequence), ('224.0.0.251', 5353))
                time.sleep(0.005)
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if not select.select([udp], [], [], min(0.1, max(0, deadline - time.monotonic())))[0]:
                    continue
                packet, sender = udp.recvfrom(65535)
                if sender[1] != 5353:
                    continue
                try:
                    found.update(parse_reply(packet, queries))
                except (ValueError, UnicodeError, struct.error):
                    pass  # Ignore malformed/unrelated LAN traffic.
    return sorted(found)
