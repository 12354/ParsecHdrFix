"""Read-only PE export parser used by the BCrypt forwarder generator."""
import hashlib
import struct
from pathlib import Path

class PEError(ValueError):
    pass


class PE:
    def __init__(self, path=None, *, data=None):
        self.path = str(path) if path is not None else '<bytes>'
        self.raw = Path(path).read_bytes() if data is None else bytes(data)
        self.sha256 = hashlib.sha256(self.raw).hexdigest()
        if self.take(0, 2) != b'MZ':
            raise PEError('Missing DOS MZ signature')
        pe = self.u32(0x3C)
        if self.take(pe, 4) != b'PE\0\0':
            raise PEError('Missing PE signature')
        self.machine, count, self.timestamp = self.unpack('<HHI', pe + 4)
        optional_size, self.characteristics = self.unpack('<HH', pe + 20)
        optional = pe + 24
        self.magic = self.u16(optional)
        if self.magic not in (0x10B, 0x20B):
            raise PEError('Unsupported optional-header magic')
        self.bits = 64 if self.magic == 0x20B else 32
        minimum, directory_offset, number_offset = ((112, 112, 108) if self.bits == 64
                                                    else (96, 96, 92))
        if optional_size < minimum:
            raise PEError('Optional header too short')
        self.take(optional, optional_size)
        self.entry_rva = self.u32(optional + 16)
        self.image_base = (self.u64(optional + 24) if self.bits == 64
                           else self.u32(optional + 28))
        self.image_size = self.u32(optional + 56)
        self.header_size = self.u32(optional + 60)
        self.subsystem, self.dll_characteristics = self.unpack('<HH', optional + 68)
        directory_count = self.u32(optional + number_offset)
        if directory_count > (optional_size - directory_offset) // 8:
            raise PEError('Directory count exceeds optional header')
        self.directories = [self.unpack('<II', optional + directory_offset + i * 8)
                            for i in range(directory_count)]
        self.sections = []
        if count > 4096:
            raise PEError('Unreasonable section count')
        for i in range(count):
            pos = optional + optional_size + i * 40
            name, virtual_size, rva, size, offset = self.unpack('<8sIIII', pos)
            flags = self.u32(pos + 36)
            self.take(pos, 40)
            self.take(offset, size)
            self.sections.append(dict(name=name.rstrip(b'\0').decode('ascii', 'replace'),
                                      rva=rva, virtual_size=virtual_size, raw_size=size,
                                      raw_offset=offset, flags=flags))

    def take(self, offset, size):
        if offset < 0 or size < 0 or offset > len(self.raw) or size > len(self.raw) - offset:
            raise PEError(f'File range outside image: {offset:#x}+{size:#x}')
        return self.raw[offset:offset + size]

    def unpack(self, fmt, offset):
        return struct.unpack(fmt, self.take(offset, struct.calcsize(fmt)))

    def u16(self, offset): return self.unpack('<H', offset)[0]
    def u32(self, offset): return self.unpack('<I', offset)[0]
    def u64(self, offset): return self.unpack('<Q', offset)[0]

    def offset(self, rva, size=1):
        if rva < self.header_size and rva + size <= self.header_size:
            self.take(rva, size)
            return rva
        matches = [s for s in self.sections if s['rva'] <= rva and
                   rva + size <= s['rva'] + s['raw_size']]
        if len(matches) != 1:
            raise PEError(f'RVA has no unique raw backing: {rva:#x}+{size:#x}')
        pos = matches[0]['raw_offset'] + rva - matches[0]['rva']
        self.take(pos, size)
        return pos

    def read(self, rva, size): return self.take(self.offset(rva, size), size)

    def cstring(self, rva, maximum=65536):
        result = bytearray()
        for i in range(maximum):
            value = self.read(rva + i, 1)[0]
            if not value:
                return result.decode('ascii', 'strict')
            result.append(value)
        raise PEError('Unterminated PE string')

    def directory(self, index):
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def executable(self, rva):
        return any(s['flags'] & 0x20000000 and s['rva'] <= rva <
                   s['rva'] + max(s['virtual_size'], s['raw_size']) for s in self.sections)

    def exports(self):
        rva, size = self.directory(0)
        if not rva:
            return []
        fields = struct.unpack('<IIHHIIIIIII', self.read(rva, 40))
        base, functions, names, eat, name_table, ordinal_table = fields[5:]
        if functions > 1000000 or names > 1000000:
            raise PEError('Unreasonable export count')
        addresses = struct.unpack('<' + 'I' * functions, self.read(eat, functions * 4))
        aliases = {}
        for i in range(names):
            ordinal_index = struct.unpack('<H', self.read(ordinal_table + i * 2, 2))[0]
            if ordinal_index >= functions:
                raise PEError('Export name ordinal outside EAT')
            name_rva = struct.unpack('<I', self.read(name_table + i * 4, 4))[0]
            aliases.setdefault(ordinal_index, []).append(self.cstring(name_rva))
        return [dict(ordinal=base + i, names=sorted(aliases.get(i, [])), rva=address,
                     forwarder=self.cstring(address) if rva <= address < rva + size else None,
                     executable=self.executable(address))
                for i, address in enumerate(addresses) if address or i in aliases]

