# Mach-O assembly → ELF assembly, for the Android build of the host's .S files (entry.S, nvn_entry.S, nvn_thunks.S), which are written for Apple's assembler.
# Usage: asm_elf.py <input .S> <output .S>
#   sym@PAGEOFF → :lo12:sym, sym@PAGE → sym (adrp takes the page of a plain symbol in ELF)
#   _name → name (Mach-O prefixes C symbols with an underscore), with the renames below for Darwin functions the Android build provides under another name
#   every .globl symbol is also .hidden: the host is one shared library, and adrp / b to a preemptible symbol does not link
#   .private_extern → .hidden
import re, sys

RENAME = {'mach_absolute_time': 'dm_mach_absolute_time'}

def convert(text):
    out = []
    for line in text.splitlines():
        code, sep, comment = line.partition('//')
        code = re.sub(r'(?<![\w.:])_([A-Za-z]\w*)', lambda m: RENAME.get(m.group(1), m.group(1)), code)
        code = re.sub(r'\b(\w+)@PAGEOFF\b', r':lo12:\1', code)
        code = re.sub(r'\b(\w+)@PAGE\b', r'\1', code)
        code = code.replace('.private_extern', '.hidden')
        code = re.sub(r'^(\s*)\.cstring\b', r'\1.section .rodata.str1.1,"aMS",@progbits,1', code)
        out.append(code + sep + comment)
        g = re.match(r'\s*\.globl\s+(\w+)', code)
        if g: out.append(f'    .hidden {g.group(1)}')
    return '\n'.join(out) + '\n'

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, encoding='utf-8') as f: text = f.read()
    res = convert(text)
    try:
        with open(dst, encoding='utf-8') as f:
            if f.read() == res: sys.exit(0)   # unchanged: keep the file's time, so the build does not reassemble it
    except FileNotFoundError: pass
    with open(dst, 'w', encoding='utf-8', newline='\n') as f: f.write(res)
