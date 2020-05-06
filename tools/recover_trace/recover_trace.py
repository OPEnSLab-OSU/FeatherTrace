# Python tool to extract FeatherTrace data from a Feather M0 board in bootloader mode
# Uses BOSSAC to read the contents of the Feather's flash memory, and then extracts
# the FeatherTrace trace data (if any is present), printing them to the screen
# in a human readable format.
# This tool is designed to be used in situations where a board is researting/failing
# such that it refuses all Serial connections, allowing trace data to be retrieved
# without a functioning sketch.
# Author: Noah Koontz
#
# Dependencies:
#   Python 3.x - Available on windows, linux and mac. See https://realpython.com/installing-python/
#   click - Install with 'sudo pip3 install click' (omit sudo on windows)
#   pyserial - Install with 'sudo pip3 install pyserial' (omit sudo on windows)
#   pyocd - Install with 'sudo pip3 install pyocd' (omit sudo on windows)
#   bossac - You will need to install BOSSA (https://www.shumatech.com/web/products/bossa), and locate
#       bossac from the installation (In windows: C:\Program Files (x86)\BOSSA\bossac.exe). From there, you 
#       can either copy the binary into the same directory as this script, or add the BOSSA folder to your path.

import click
import time
import subprocess
import serial
import enum
from serial.tools import list_ports
import mmap
import struct
import array
from collections import namedtuple
import os
import shutil
import re
from elftools.elf.elffile import ELFFile
from pyocd.debug.elf.decoder import DwarfAddressDecoder

# These values are specific to the Adafruit Feather M0 USB configuration
PID_SKETCH = (0x800b, 0x801B)
PID_BOOTLOADER = (0x000b, 0x0015, 0x001B)
VID = 0x239a

# These values indicate where and what FeatherTrace trace data is stored in flash
FEATHERTRACE_HEAD = 0xFEFE2A2A
FEATHERTRACE_STRING = b'FeatherTrace Data Here:\0'
# This must be changed to reflect changes in the FeatherTraceFlash struct
FEATHERTRACE_STRUCT_FMT = '< I 24s I 8s I 8s I 8s 32I 8s 16I I 8s I 8s I 8s i 8s 64s 4s'
FEATHERTRACE_STRUCT_FIELDS = 'value_head marker version marker1 cause marker2 interrupt_type marker3 stacktrace marker4 regs xpsr marker5 is_corrupted marker6 failnum marker7 line marker8 file marker9'
FEATHERTRACE_STRUCT_NAMEDTUPLE = namedtuple('FeatherTraceData', FEATHERTRACE_STRUCT_FIELDS)

class FaultCause(enum.Enum):
    FAULT_NONE = 0
    FAULT_UNKNOWN = 1
    FAULT_HUNG = 2
    FAULT_HARDFAULT = 3
    FAULT_OUTOFMEMORY = 4
    FAULT_USER = 5

def get_feather_serial_ports(grep):
    boards_all = list_ports.grep(grep)
    # get the COM port, if the PID matches the sketch or bootloader and the VID is adafruit
    return [ (board.device, board.pid) for board in boards_all
        if board.vid == VID
        and (board.pid in PID_SKETCH or board.pid in PID_BOOTLOADER)]

def reset_board_bootloader(address):
    # stolen from http://markparuzel.com/?p=230
    ser = serial.Serial(timeout=2, inter_byte_timeout=2)
    ser.port = address
    ser.open()
    ser.baudrate = 1200 # This is magic.
    ser.flush()
    ser.rts = True
    ser.flush()
    ser.dtr = False
    ser.flush()
    ser.close()

def download_board_flash(port, bossac, tmpfilepath):
    # run BOSSAC, telling it to read from our specified port into our specified file!
    ret = subprocess.run([bossac, f'--port={ port }', '--offset=0x2000', '-r', tmpfilepath])
    if ret.returncode == 0:
        return True
    else:
        return False

def get_fault_data(byte_data):
    # todo: fix struct unpacking to collate arrays
    unpacked = struct.unpack(FEATHERTRACE_STRUCT_FMT, byte_data)
    # group together the stacktrace and regs arrays
    stacktrace = unpacked[8:40]
    regs = unpacked[41:57]
    # recreate the tuple with these new arrays
    true_unpacked = unpacked[:8] + (stacktrace, unpacked[40], regs) + unpacked[57:]
    return FEATHERTRACE_STRUCT_NAMEDTUPLE._make(true_unpacked)

def print_stack_trace(elf_path, addresses, indent):
    try:
        elffile = ELFFile(elf_path)
        decoder = DwarfAddressDecoder(elffile)
        for addr in addresses:
            function = decoder.get_function_for_address(addr)
            line = decoder.get_line_for_address(addr)
            indent_str = ''.join(['\t' for x in range(indent)])
            funcname = function.name.decode() if function is not None else 'unknown'
            dirname = line.dirname.decode() if line is not None else 'unknown'
            filename = line.filename.decode() if line is not None else 'unknown'
            linenum = line.line if line is not None else 'unknown'
            click.echo(f'{ indent_str }{ format(addr, "#010x") }: { funcname }() at { dirname }/{ filename }:{ linenum }')
    except Exception as ex:
        click.echo(f'Error while decoding stacktrace: {ex}')

# Click setup and commands:
@click.group()
def recover_trace():
    """FeatherTrace Trace Data Recovery Tool.
    
    Extracts FeatherTrace trace data from an otherwise unavailable board. Requires
    a USB connection to a board in bootloader mode to function.
    """
    pass

@recover_trace.command(short_help='Attempt to reset a board into bootloader mode using a Serial connection')
@click.option('--attempt-count', '-a', default=10, show_default=True,
    help='Number of times to retry resetting the device',) 
@click.option('--attempt-wait', '-w', default=1000, show_default=True,
    help='Milliseconds to wait inbetween attempts') 
@click.option('--force', '-f', is_flag=True,
    help='Disable all checks that the COM port is valid (not recommended)') 
@click.argument('port')
def reset_board(attempt_count, attempt_wait, force, port):
    """
    Attempts to reset a FeatherM0 on the COM port specified by the first argument into Bootloader mode.
    This command uses the 1200 baud trick (see http://markparuzel.com/?p=230)
    to signal a Feather M0 that it should reset into Bootloader mode. This trick
    usually takes a few attempts to work if the board is responsive. Note that 
    when the board is reset into bootloader mode using this trick, it will stay
    in bootloader mode until a sketch is uploaded to it, including across power
    cycles.

    This tool will not work if the Feather M0 is hung or in a failure state. 
    If this is the case, you will need to manually put the board into bootloader mode
    using the reset button.
    """
    # test that the COM port exists, and has a device on it
    if force == False:
        ports = get_feather_serial_ports(f'^{ port }$')
        if len(ports) == 0:
            click.echo(f'Failed to find a device on port "{ port }". Use --force to override this error.', err=True)
            exit(1)
        if ports[0][1] in PID_BOOTLOADER:
            click.echo('Device is already in bootloader mode. Use --force to override this error.', err=True)
            exit(1)
    # all good! attempt to reset the device n times
    for i in range(attempt_count):
        click.echo(f'Attempting to reset board on { port }...')
        reset_board_bootloader(port)
        time.sleep(attempt_wait / 1000.0)
        # check to see if the board reset
        ports = get_feather_serial_ports(f'^{ port }$')
        if len(ports) == 0:
            # if forcing, don't find the new port
            if force == True:
                click.echo('Board successfully reset!')
                exit(0)
            # else attempt to find the new port
            time.sleep(1.0)
            ports = get_feather_serial_ports('.*')
            bootloader_ports = [port for port, pid in ports if pid in PID_BOOTLOADER]
            if len(bootloader_ports) != 0:
                click.echo(f'Board successfully reset! New COM port is { bootloader_ports[0] }')
                exit(0)
            else:
                click.echo('COM port dissapeared, but unable to find port corresponding to bootloader', err=True)
                exit(1)
    click.echo('Board failed to reset!', err=True)
    exit(1)

@recover_trace.command(short_help='Extract FeatherTrace trace data from a Feather M0 in bootloader mode')
@click.option('--force', '-f', is_flag=True,
    help='Disable all checks that the COM port is valid (not recommended)') 
@click.option('--bossac-path', '-u', type=click.Path(dir_okay=False), default=None,
    help='Location of the BOSSAC uploader tool, see installation instructions for more information. If this option is not specified BOSSAC must be in your PATH.')
@click.option('--elf-path', '-e', type=click.File(mode='rb'), default=None,
    help='Location of the ELF file for addr2line to interpret debug symbols from. Must be from the same build as is running on the Feather M0 for stacktrace decoding to work correctly.')
@click.option('--bin-path', '-b', type=click.Path(dir_okay=False), default='./flash.bin',
    help='Location to place temporarily place the flash data')
@click.argument('port')
def recover(force, bossac_path, elf_path, bin_path, port):
    """
    Uses BOSSAC to extract FeatherTrace data from the flash memory of a Feather M0
    in bootloader mode. The first argument specifies the COM port to extract from.

    Note that --bossac-path must point to a valid BOSSAC executable, see the installation
    instructions for more infomation on how to install BOSSA. Additionally, a valid ELF (-e)
    must be specified to enable stack trace decoding.
    """
    # check that BOSSAC exists
    if bossac_path == None:
        bossac_path = shutil.which('bossac')
        if bossac_path == None:
            click.echo(f'Failed to find bossac executable, did you install BOSSA?', err=True)
            exit(1)
    elif not os.path.isfile(bossac_path):
        click.echo('Invalid bossac path specified')
        exit(1)
    # test that the COM port exists, and has a device on it
    if force == False:
        ports = get_feather_serial_ports(f'^{ port }$')
        if len(ports) == 0:
            click.echo(f'Failed to find a device on port "{ port }". Use --force to override this error.', err=True)
            exit(1)
        if ports[0][1] in PID_SKETCH:
            click.echo('Device is not in bootloader mode. Use --force to override this error.', err=True)
            exit(1)
    # all good! attempt to read the flash, storing the result in a temporary file
    click.echo('Downloading flash...')
    if not download_board_flash(port, bossac_path, bin_path):
        click.echo('Download from flash failed!', err=True)
        exit(1)
    # read the temporary file, looking for a feathertrace trace
    exit_status = 1
    with open(bin_path, 'rb') as binfile, mmap.mmap(binfile.fileno(), 0, access=mmap.ACCESS_READ) as fmap:
        start = 0
        # seek to the special binary sequence feathertrace uses to indicate a trace block
        while True:
            idx = fmap.find(bytearray(FEATHERTRACE_HEAD.to_bytes(4, byteorder='little')), start)
            if idx == -1:
                click.echo('Could not find FeatherTrace data! Did the device fault?', err=True)
                exit_status = 1
                break
            # unpack, and check that the result makes sense
            data = get_fault_data(fmap[idx:(idx + struct.calcsize(FEATHERTRACE_STRUCT_FMT))])
            if data.value_head == FEATHERTRACE_HEAD and data.marker == FEATHERTRACE_STRING:
                click.echo('Found trace data!')
                click.echo(f'\tFault: { FaultCause(data.cause) }')
                click.echo(f'\tFaulted during recording: { "Yes" if data.is_corrupted > 0 else "No" }')
                click.echo(f'\tLast Marked Line: { data.line }')
                click.echo(f'\tLast Marked File: { data.file.split(bytes.fromhex("00"), 1)[0] }')
                click.echo(f'\tInterrupt type: { data.interrupt_type }')
                # print decoded stacktrace if all tools needed are present
                hexfmt = '{:#010x}'
                if elf_path != None:
                    click.echo('\tDecoded Stacktrace (may take a moment): ')
                    print_stack_trace(elf_path, [ addr for addr in data.stacktrace if addr != 0 ], 2)
                # else print the normal stacktrace
                else:
                    fmted_trace = ', '.join([ hexfmt.format(addr) for addr in data.stacktrace if addr != 0 ])
                    click.echo(f'\tStacktrace: { fmted_trace }')
                # if the interrupt was asynchrounous, read the saved registers
                if data.interrupt_type != 0:
                    click.echo('\tRegisters:')
                    # print the first 13 registers
                    first_regs = [ 'R{:} {:#010x}'.format(i, regval) for i,regval in enumerate(data.regs[:13]) ]
                    fmted_regs_line1 = ', '.join(first_regs[:7])
                    click.echo(f'\t\t{ fmted_regs_line1 }\t')
                    fmted_regs_line2 = ', '.join(first_regs[7:])
                    click.echo(f'\t\t{ fmted_regs_line2 }\t')
                    # print the special ones
                    click.echo(f'\t\tSP: { hexfmt.format(data.regs[13]) }\tLR: { hexfmt.format(data.regs[14]) }\tPC: { hexfmt.format(data.regs[15]) }\txPSR: { hexfmt.format(data.xpsr) }')
                click.echo(f'\tFailures since upload: { data.failnum }')
                # exit success
                exit_status = 0
                break
            # else keep going
            start = idx + 4
    # delete the temporary file
    os.remove(bin_path)
    exit(exit_status)

@recover_trace.command(short_help='Decodes stacktrace addresses into line/fine information')
@click.option('--elf-path', '-e', type=click.File(mode='rb'), required=True,
    help='Location of the ELF file for addr2line to interpret debug symbols from. Must be from the same build as is running on the Feather M0 for stacktrace decoding to work correctly.')
@click.argument('stacktrace', nargs=-1)
def decode(elf_path, stacktrace):
    """
    Decode a stacktrace outputted from FeatherTrace into line/file information. Requires
    the ELF file from the exact build currently running on the device being debugged,
    otherwise the output will be innaccurate.

    Stacktrace values must be hexidecimal and space seperated, but may also contain commas and
    prefixes.
    """
    if stacktrace is None or len(stacktrace) == 0:
        exit(0)
    # parse the stacktrace, spliting it into hex numbers
    STACKTRACE_FMT = r'^[,\s]{0,2}(?:0x)?([0-9A-Fa-f]{1,8})[,\s]{0,2}$'
    stripped_addrs = []
    for addr in stacktrace:
        match = re.match(STACKTRACE_FMT, addr)
        if match is None:
            click.echo(f'Discarding invalid address { addr }')
        else:
            stripped_addrs.append(int(match.string[match.regs[1][0]:match.regs[1][1]], 16))
    # decode and print information!
    click.echo('Decoded stacktrace (may take a moment):')
    print_stack_trace(elf_path, stripped_addrs, 1)
    exit(0)

if __name__ == '__main__':
    recover_trace()
