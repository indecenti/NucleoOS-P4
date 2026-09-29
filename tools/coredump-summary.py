"""Summarize an ESP32-P4 core dump WITHOUT the matching ELF.

espcoredump refuses to decode when the dump's app SHA differs from the ELF (and needs gdb). When the
image that crashed is gone, the dump still tells a lot on its own: the panic reason, whether it hit
in an ISR, the panic frame registers, every task with its stack bounds and saved SP (overflow check).
Symbolize the printed PCs with addr2line on the closest build: IRAM addresses usually carry over
between nearby releases, flash (.text 0x40xxxxxx) ones do not.

Usage (IDF venv python, it has pyelftools):
  python tools/coredump-summary.py coredump.bin            # raw image from /api/crash/dump
  python tools/coredump-summary.py part.bin --partition    # esptool read_flash of the whole partition
"""
import argparse
import io
import struct
import sys

from elftools.elf.elffile import ELFFile

REGS = ['pc', 'ra', 'sp', 'gp', 'tp', 't0', 't1', 't2', 's0', 's1', 'a0', 'a1', 'a2', 'a3', 'a4', 'a5',
        'a6', 'a7', 's2', 's3', 's4', 's5', 's6', 's7', 's8', 's9', 's10', 's11', 't3', 't4', 't5', 't6']
# RISC-V panic handler: raw exception code for faults; panic-interrupt number (CLIC offset removed)
# for watchdog / cache / memprot / stack guard (esp_private/panic_reason.h, soc.h).
REASONS = {0: 'instruction address misaligned', 1: 'instruction access fault', 2: 'illegal instruction',
           3: 'breakpoint', 4: 'load address misaligned', 5: 'load access fault',
           6: 'store address misaligned', 7: 'store access fault', 11: 'ecall',
           24: 'interrupt watchdog: CPU0 stopped ticking', 24 | (1 << 12): 'interrupt watchdog: CPU1 stopped ticking',
           25: 'cache error', 26: 'memory protection fault', 27: 'stack protection fault (HW stack guard)'}
HEADER = 24   # core_dump_header_t before the ELF (flash format, ELF + CRC32)
TCB_TOP, TCB_STACK, TCB_NAME, TCB_NAME_LEN = 0x00, 0x30, 0x34, 16   # IDF FreeRTOS TCB_t layout


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('dump')
    ap.add_argument('--partition', action='store_true', help='input is the whole coredump partition')
    args = ap.parse_args()

    raw = open(args.dump, 'rb').read()
    data_len, version = struct.unpack_from('<II', raw, 0)
    if args.partition:
        raw = raw[:data_len]
    if len(raw) < data_len or raw[HEADER:HEADER + 4] != b'\x7fELF':
        sys.exit('not a flash core dump (ELF format) image')
    elf = ELFFile(io.BytesIO(raw[HEADER:data_len - 4]))

    loads, tasks, extra, app_sha = [], [], None, '?'
    for seg in elf.iter_segments():
        if seg['p_type'] == 'PT_LOAD':
            loads.append((seg['p_vaddr'], seg.data()))
        elif seg['p_type'] == 'PT_NOTE':
            d, o = seg.data(), 0
            while o + 12 <= len(d):
                nsz, dsz, typ = struct.unpack_from('<III', d, o)
                o += 12
                name = d[o:o + nsz].rstrip(b'\0').decode(errors='replace')
                o += (nsz + 3) & ~3
                desc = d[o:o + dsz]
                o += (dsz + 3) & ~3
                if name == 'CORE' and typ == 1:          # NT_PRSTATUS: pid = TCB address, regs at +72
                    tasks.append((struct.unpack_from('<I', desc, 24)[0],
                                  dict(zip(REGS, struct.unpack_from('<32I', desc, 72)))))
                elif name == 'ESP_EXTRA_INFO':           # {crashed task TCB, isr_context}
                    extra = struct.unpack_from('<%dI' % (len(desc) // 4), desc)
                elif name == 'ESP_CORE_DUMP_INFO':
                    app_sha = desc[4:].split(b'\0')[0].decode(errors='replace')

    def rd32(addr):
        for va, blob in loads:
            if va <= addr and addr + 4 <= va + len(blob):
                return struct.unpack_from('<I', blob, addr - va)[0]
        return None

    def tcb(addr):
        name = b''.join(struct.pack('<I', rd32(addr + TCB_NAME + i) or 0) for i in range(0, TCB_NAME_LEN, 4))
        return name.split(b'\0')[0].decode(errors='replace'), rd32(addr + TCB_TOP), rd32(addr + TCB_STACK)

    print(f'dump: {data_len} B, format 0x{version:x}, app ELF sha {app_sha}')
    crashed = extra[0] if extra else tasks[0][0]
    isr = bool(extra[1]) if extra and len(extra) > 1 else None
    regs = next(r for t, r in tasks if t == crashed)
    name, _, stack = tcb(crashed)
    print(f"crashed task: '{name}' (TCB 0x{crashed:08x}), isr_context={isr}")
    print('  pc 0x%08x  ra 0x%08x  sp 0x%08x' % (regs['pc'], regs['ra'], regs['sp']))

    # The panic handler pushes an RvExcFrame (32 GPRs + mstatus, mtvec, mcause, mtval, mhartid)
    # just below the faulting SP; find it by its saved mepc/sp pair.
    for base in range(regs['sp'] - 0x100, regs['sp'], 4):
        if rd32(base) == regs['pc'] and rd32(base + 8) == regs['sp']:
            mstatus, mtvec, mcause, mtval, hart = (rd32(base + 128 + 4 * i) for i in range(5))
            print(f'  mcause {mcause} = {REASONS.get(mcause, "unknown")}; mtval 0x{mtval:08x}; hart {hart}')
            print('  a0-a7 ' + ' '.join('%08x' % rd32(base + 40 + 4 * i) for i in range(8)))
            break
    else:
        print('  (panic frame not found in the dumped memory)')

    # used~ = TCB - saved pxTopOfStack: exact for xTaskCreate stacks (allocated right below the TCB),
    # meaningless for static/caps stacks (not printed when implausible). A task that was RUNNING on
    # the other core at the panic has stale registers (e.g. pc 0x4); the crashed task in an ISR shows
    # the ISR stack SP. Any other SP below its stack base is a real overflow.
    print('\ntasks:')
    for t, r in dict(tasks).items():
        nm, top, base = tcb(t)
        used = (t - top) if top and top <= t else None
        flag = ''
        if base and r['sp'] < base and (r['sp'] >> 24) == (base >> 24):
            flag = '  <-- ISR stack' if (t == crashed and isr) else '  <-- SP below stack base (overflow or stale)'
        print(f'  {nm:16s} tcb 0x{t:08x} stack 0x{(base or 0):08x} sp 0x{r["sp"]:08x} pc 0x{r["pc"]:08x}'
              f'{"  used~%d B" % used if used is not None and used < 0x10000 else ""}{flag}')


if __name__ == '__main__':
    main()
