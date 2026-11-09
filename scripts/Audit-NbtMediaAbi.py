"""Inspect copied ELF libraries offline. Never load or execute vehicle code.

Requires pyelftools (the project's ignored .local/analysis-packages is supported).
Symbol presence is ABI evidence only, never proof of a working AA backend.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / '.local/analysis-packages'))
from elftools.elf.elffile import ELFFile


def bootstrap_calls(elf, symbols):
    """Disassemble only named bootstrap routines; never emulate or run them."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    targets = {s['st_value']: s.name for s in symbols if s['st_value']}
    relocation, plt = elf.get_section_by_name('.rel.plt'), elf.get_section_by_name('.plt')
    if relocation and plt:
        for i, entry in enumerate(relocation.iter_relocations()):
            targets[plt['sh_addr'] + 16 * (i + 1)] = symbols[entry['r_info_sym']].name + '@plt'
    selected = {'_ZN3wal27WalApplicationInterfaceImpl6initKDEv',
                '_ZN3wal27WalApplicationInterfaceImpl9destroyKDEv',
                'kdInitializeNV', 'NvEglKdInit', 'eglGetDisplay'}
    result = []
    for symbol in symbols:
        if symbol.name not in selected or symbol['st_shndx'] == 'SHN_UNDEF':
            continue
        address, size = symbol['st_value'], symbol['st_size']
        if not size or size > 4096:
            continue
        for segment in elf.iter_segments():
            base = segment['p_vaddr']
            if base <= address and address + size <= base + segment['p_filesz']:
                code = segment.data()[address-base:address-base+size]
                calls = []
                for ins in Cs(CS_ARCH_X86, CS_MODE_32).disasm(code, address):
                    if ins.mnemonic == 'call':
                        target = int(ins.op_str, 16) if ins.op_str.startswith('0x') else None
                        calls.append({'address': hex(ins.address), 'operand': ins.op_str,
                                      'target': targets.get(target, '(internal or indirect)')})
                result.append({'symbol': symbol.name, 'address': hex(address), 'direct_calls': calls})
                break
    return result


def inspect(path):
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        if elf.elfclass != 32 or elf['e_machine'] != 'EM_386' or not elf.little_endian:
            raise ValueError('Expected original-NBT ELF32 little-endian x86: ' + str(path))
        dynamic = elf.get_section_by_name('.dynamic')
        symbols = elf.get_section_by_name('.dynsym')
        symbol_list = list(symbols.iter_symbols()) if symbols else []
        names = sorted(s.name for s in symbol_list if s['st_shndx'] != 'SHN_UNDEF')
        wanted = ('kdInitializeNV', 'kdGetNativeDisplayNV', 'kdCreateWindow',
                  'kdGetDisplayNV', 'kdGetDisplayPropertyivNV', 'kdSetWindowOrderQNX',
                  'screen_create_context', 'screen_create_window',
                  'eglInitialize', 'eglGetDisplay', 'NvEglKdInit', 'NmeCreateCodecNvdecHW',
                  'NmeCreateDeviceCallbackVideo', 'NmeCreateDeviceNullVideo')
        return {'file': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                'architecture': 'ELF32 x86',
                'needed': [t.needed for t in dynamic.iter_tags() if t.entry.d_tag == 'DT_NEEDED'] if dynamic else [],
                'relevant_exports': [n for n in wanted if n in names],
                'bootstrap_call_sites': bootstrap_calls(elf, symbol_list),
                'runtime_tested': False, 'backend_ready': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('libraries', type=Path, nargs='+')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    report = {'execution': 'offline ELF parsing only',
              'libraries': [inspect(p) for p in args.libraries],
              'aa_media_advertisement_allowed': False,
              'reason': 'No validated original-NBT video, audio or input backend'}
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
