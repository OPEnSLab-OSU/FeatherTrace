# 📜FeatherTrace
When a microcontroller crashes or hangs, it can be quite difficult to troubleshoot what caused it. FeatherTrace is an attempt to build a system that can not only recover from a crash, but explain why the crash happened. FeatherTrace supports all boards using the SAMD21 (Adafruit Feather M0, Arduino Zero, etc.), and future support is planned for the SAMD51.

FeatherTrace is an alternative to [FeatherFault](https://github.com/OPEnSLab-OSU/FeatherFault) for **advanced users**. FeatherTrace requires modifications to the compilation flags of your project to function, making it incompatible with the Arduino IDE and library manager. These modifications, however, allow FeatherTrace to offer a complete stacktrace and register dump on top of FeatherFault's existing features.

## Getting Started

To use FeatherTrace, you will need a build system using GCC that allows changing compilation flags, such as [PlatformIO](https://platformio.org/) or the [Arduino CLI](https://github.com/arduino/arduino-cli/releases). The Arduino IDE does not allow for configuring compilation and as a result is not supported. You will also need the [Adafruit ASF core](https://github.com/adafruit/Adafruit_ASFcore/tree/f6ffa8b2bc2477566c8406e5f3fa883b137347f1).

It is also highly recommended that a system be in place to track generated ELF files. More information can be found under [Tracking ELF Files](#tracking-elf-files). 

### Setting Compilation Flags

Once the above items are setup, configure your project to add the following flags to compilation:
```
-ggdb3 -g3 -fasynchronous-unwind-tables -Wl,--no-merge-exidx-entries
```
Using PlatformIO this would mean adding the following to `platformio.ini`:
```ini
build_flags = -Os -ggdb3 -g3 -fasynchronous-unwind-tables -Wl,--no-merge-exidx-entries
```
Using the ArduinoCLI you can add the following option to your `compile` command:
```
--build-properties "build.extra_flags=-ggdb3 -g3 -fasynchronous-unwind-tables -Wl,--no-merge-exidx-entries"
```
More information on these flags can be found in [Compile Flags](#compile-flags).

### Using FeatherTrace

Once your project is setup, FeatherTrace can be activated by adding the following lines to the beginning of a sketch:
```C++
#include "FeatherTrace.h"
FEATHERTRACE_BIND_ALL()

void setup() {
    Serial.begin(...);
    while(!Serial);
    FeatherTrace::PrintFault(Serial);
    Serial.flush();
    FeatherTrace::StartWDT(FeatherTrace::WDTTimeout::WDT_8S);
    ...
}
```
and decorating the sketch code with `MARK` statements, making sure to surround suspicious code sections with them. `MARK` may not be used more than once per line, and must be used both before and after the suspected code:
```C++
void loop() {
    // Mark a function
    MARK; 
    do_something_suspicous(); 
    MARK;

    // Mark a loop
    MARK;
    while (unsafe_function_one() == true) { MARK;
        // Ignore safe functions, but mark the unsafe ones
        // Which functions are 'unsafe' is up to the programmer
        safe_function_one();
        safe_function_two();
        safe_function_three();
        MARK;
        unsafe_function_two();
        MARK;
    }
}
```

Once FeatherTrace is activated, it will trigger after a set time of inactivity (we specify 8 seconds above, but this value can be changed), on [memory overflow](https://learn.adafruit.com/memories-of-an-arduino?view=all), or on a [hard fault](https://www.freertos.org/Debugging-Hard-Faults-On-Cortex-M-Microcontrollers.html). Once triggered, FeatherTrace will immediately save the location of the last run `MARK` statement, the fault cause, all current register states, and a stacktrace to non-volatile memory. FeatherTrace will then reset the board immediately to prevent further damage. After the reset, FeatherTrace's saved data can be read by `FeatherTrace::PrintFault` and `FeatherTrace::GetFault`, allowing the developer to determine if/where the board has failed after it resets.

### Usage

To show how this behavior works, let's assume that `unsafe_function()` in the code block below attempts to access memory that doesn't exist, causing a hard fault:
```C++
void setup() {
    // Activate feathertrace
    // Wait for serial to connect to the serial monitor
    Serial.begin(...);
    while(!Serial);
    // begin code
    Serial.println("Start!");
    other_function_one();
    unsafe_function(); // oops
    other_function_two();
    Serial.println("Done!");
}
```
If we run this code without FeatherTrace, we would see the serial monitor output something like this:
```
Start!
```
After which the device hard faults, causing it to wait in an infinite loop until it is reset. 

This behavior is extremely difficult to troubleshoot: as the developer, all we know is that the device failed between `Start!` and `Done`. Using more print statements, we could eventually narrow down the cause to `unsafe_function`—this process is time consuming, unreliable, and downright annoying. Instead, let's try the same code with FeatherTrace activated:
```C++
void setup() {
    // Wait for serial to connect to the serial monitor
    Serial.begin(...);
    while(Serial);
    // Activate FeatherTrace
    FeatherTrace::PrintFault(Serial);
    FeatherTrace::StartWDT(FeatherTrace::WDTTimeout::WDT_8S);
    // begin code
    MARK;
    Serial.println("Start!");
    MARK;
    other_function_one();
    MARK;
    unsafe_function(); // oops
    MARK;
    other_function_two();
    MARK;
    Serial.println("Done!");
    MARK;
}
```
Running that sketch, we would see the following serial monitor output:
```
No fault
Start!
```
`No fault` here indicates that FeatherTrace has not been triggered yet. We change that shortly by running `unsafe_function()`, causing a hard fault. Instead of waiting in an infinite loop, however, the board is immediately reset to the start of the sketch by FeatherTrace. We can then open the serial monitor again:
```
Fault! Cause: HARDFAULT
Fault during recording: No
Line: 11
File: main.cpp
Interrupt type: 3
Stacktrace: 0x00002182, 0x0000219a, 0x000021f2, 0x00003bc6
Registers:
        R0: 0x000007d0  R1: 0x00006374  R2: 0x41004400  R3: 0x000007cf  R4: 0x00017675  R5: 0x0000000d  R6: 0x20000240
        R7: 0x20000224  R8: 0xcffdbfd6  R9: 0xffffff7b  R10: 0xffffffff R11: 0xf9ffffdf R12: 0x20000760 SP: 0x20007fc8
        LR: 0x00002171  PC: 0x00002182  xPSR: 0x21000000
Failures since upload: 1
```
Since the FeatherTrace was triggered by the hard fault, `FeatherTrace::PrintFault` will print the last file and line number `MARK`ed before the hard fault happened. In this case, line 18 of `MySketch.ino` indicates the `MARK` statement after `other_function_one()`, leading us to suspect that `unsafe_function()` is causing the issue. We can now focus on troubleshooting `unsafe_function()`.

### Advanced Usage

In addition to line/file information, FeatherTrace will also print out the interrupt type, stacktrace, and register dump, allowing the developer to infer the exact state of the CPU during the fault. This information can be very useful when debugging a fault in a library or system function. 

To demonstrate this behavior, suppose we have a sketch depending on a fictional external library `MyLibrary`:
```C++
// In main.cpp
#include "MyLibrary.h"

void setup() {
    MARK;
    MyLibrary.dosomething();
    MARK;
}

// In MyLibrary.cpp
void MyLibrary::dosomething() {
    // 500+ lines of uncommented code
    unsafe_function(); // do something faulty
    // another 500+ lines of uncommented code
}
```
Running this sketch would generate the following output from FeatherTrace:
```
Fault! Cause: HARDFAULT
Fault during recording: No
Line: 5
File: main.cpp
Interrupt type: 3
Stacktrace: 0x00002182, 0x0000219a, 0x000021f2, 0x00003bc6
Registers:
        R0: 0x000007d0  R1: 0x00006374  R2: 0x41004400  R3: 0x000007cf  R4: 0x00017675  R5: 0x0000000d  R6: 0x20000240
        R7: 0x20000224  R8: 0xcffdbfd6  R9: 0xffffff7b  R10: 0xffffffff R11: 0xf9ffffdf R12: 0x20000760 SP: 0x20007fc8
        LR: 0x00002171  PC: 0x00002182  xPSR: 0x21000000
Failures since upload: 1
```
In this case, the line and file number point to the `MARK` statement just above `MyLibrary.dosomething()`, indicating the fault could be anywhere inside that function. As `MyLibrary::dosomething` is easily 1000+ lines, `MARK`ing the entire body is a less than ideal approach. Instead, FeatherTrace outputs a stacktrace we can use to recover line/file information in files even where `MARK` is not present. Unlike the line/file information generated by `MARK`, however, stacktrace values are raw addresses and require a debugging program to translate. FeatherTrace provides the [recover_trace python script](./tools/recover_trace/recover_trace.py) script to decode these addresses, however GDB `info line` or `arm-none-eabr-addr2line` can also be used. More details can be found under [Decoding a Stacktrace](#decoding-a-stacktrace).

In this example, running `recover_trace decode 0x00002182 0x0000219a 0x000021f2 0x00003bc6` produces the following output:
```
Decoded Stacktrace:
    0x00002182: unsafe_function() at src/MyLibrary.cpp:10        
    0x0000219a: MyLibrary::dosomething() at src/MyLibrary.cpp:18
    0x000021f2: setup() at src/main.cpp:36
    0x00003bc6: main() at .../framework-arduino-samd-adafruit/cores/arduino/main.cpp:50
```
Using this information we see that the project faulted at `unsafe_function:10`, and can now focus on troubleshooting.

## Additional Features

### Getting Fault Data In The Sketch

While most projects should only need traces on the serial monitor, some (such as remote deployments) will need to log the data to other mediums. To do this, FeatherTrace has the `FeatherTrace::DidFault` and `FeatherTrace::GetFault` functions to check if a fault has occurred, and to get the last fault trace. For more information on these functions, please see [FeatherTrace.h](./src/FeatherTrace.h).

### Getting Fault Data Without Serial

If a serial connection cannot be established while the sketch is running, but the board is able to communicate in bootloader mode, the [recover_trace python script](./tools/recover_trace/recover_trace.py) can download and read FeatherTrace trace data using the bootloader. Simply follow the setup instructions contained in the script, reset the board into bootloader mode, and run:
```
python ./recover_trace.py recover <comport>
```

### Decoding a Stacktrace

Stacktrace values outputted by FeatherTrace are raw addresses, and must be decoded by debugger to be useful to a developer. Decoding these values requires the ELF file for the build being debugged (see [Tracking ELF Files](#tracking-elf-files)), which can be found with other build artifacts. Given this file and the stacktrace output, there are a number of ways to translate these values. All three of these methods will produce equivalent output.

#### Using recover_trace

The [recover_trace python script](./tools/recover_trace/recover_trace.py) can be used to decode a stacktrace. Simply follow the setup instructions contained in the script, and run:
```
python ./recover_trace.py decode -e <elffile> <stacktrace values>
```

#### Using arm-none-eabi-addr2line

Alternatively, these addresses can be translated using the tools provided by ARM. `arm-none-eabi-addr2line` will usually be installed by your development environment along with the `arm-none-eabi` tool suite, so it can be a convenient alternative to the bundled python script. To decode a stacktrace with this tool simply run:
```
arm-none-eabi-addr2line -e <elffile> -pfsCa <stacktrace values>
```

#### Using GDB

Finally, these addresses can be translated using GDB's built in tools. Simply open a GDB session with the correct ELF file and run:
```
info line *<stacktrace address>
```

### Running Code When The Device Faults

Some code may be needed to perform cleanup of external devices after FeatherTrace causes an unexpected reset. There are two general method for this: a safe one, and an unsafe one. While the safe method is generally recommended, access to the state of the program may be needed during the fault,in which case the unsafe method is necessary.

#### Safe Method

The safe method uses `FeatherTrace::DidFault` at the beginning of setup:
```C++
void setup() {
    ...
    if (FeatherTrace::DidFault()) {
        // perform cleanup here
        cleanup_code();
    }
    ...
}
```
Since FeatherTrace resets the board immediately upon failure, `cleanup_code()` will run every time FeatherTrace is triggered. When writing the `cleanup_code()` routine, remember that the program state has been entirely cleared, and any devices or variables in the sketch must be initialized before they can be used (ex. `Serial.begin` must be called to use Serial). If access to a variable value before the device is reset is needed, please see the unsafe method below.

#### Unsafe Method

The unsafe method uses `FeatherTrace::SetCallback` to register a function to be called before the device is reset:
```C++
volatile void cleanup_code() {
    // perform cleanup here
    // can also read global variables
}

void setup() {
    ...
    FeatherTrace::SetCallback(cleanup_code);
    ...
}
```
`cleanup_code()` will be called after FeatherTrace stores a trace, but before the device is reset—allowing it to access global variables and devices in the faulted state. Note that this implementation has a few major caveats:
 * The callback (`cleanup_code`) must be [interrupt safe](https://www.arduino.cc/reference/en/language/functions/external-interrupts/attachinterrupt/) (cannot use `delay`, `Serial`, etc.).
 * The callback must be *extremely careful* when accessing memory outside of itself. All memory should be assumed corrupted unless proven otherwise. Pointers should be treated with extra caution.
 * The callback must execute in less time than the specified WDT timeout, or it will be reset by the watchdog timer.
 * If the callback itself faults, an infinite loop will be triggered.

Because of the above restrictions, it is *highly* recommended that the safe method is used wherever possible.

## Implementation Notes

### Tracking ELF Files

FeatherTrace requires the ELF file *for the build being debugged* to translate stacktrace addresses. In other words, FeatherTrace needs the ELF file generated from the *exact* project that is currently running on the device—including source code, external dependencies, compiler flags, and so on. This means that to effectively use stacktracing, a developer must have a system to keep track of the ELF files associated with different devices, including devices deployed remotely.

A possible solution to ELF tracking is proper git practices such as the [gitflow workflow](https://www.atlassian.com/git/tutorials/comparing-workflows/gitflow-workflow), ensuring a timeline of reproducible builds and associating releases with deployed devices. Another approach is ensuring that the correct ELF file on the devices' storage (SDCard, flash, etc.).

### Failure Modes

FeatherTrace currently handles three failure modes: hanging, [memory overflow](https://learn.adafruit.com/memories-of-an-arduino?view=all), and [hard fault](https://www.freertos.org/Debugging-Hard-Faults-On-Cortex-M-Microcontrollers.html). When any of these failure modes are triggered, FeatherTrace will immediately write the information from the last `MARK` to flash memory, and cause a system reset. `FeatherTrace::PrintFault`, `FeatherTrace::GetFault`, and `FeatherTrace::DidFault` read this flash memory to retrieve information regarding the last fault.

#### Hanging Detection

Hanging detection is implemented using the SAMD watchdog timer early warning interrupt. As a result, FeatherTrace will not detect hanging unless `FeatherTrace::StartWDT` is called somewhere in the beginning of the sketch. Note that similar to normal watchdog operation, FeatherTraces detection must be periodically resetting using `MARK` macro; this means that the `MARK` macro must be placed such that it is called at least periodically under the timeout specified. In long operations that cannot be `MARK`ed (sleep being an example), use `FeatherTrace::StopWDT` to disable the watchdog during that time.

Behind the scenes watchdog feeding is implemented in terms of a global atomic boolean which determines if the device should fault during the watchdog interrupt, as opposed to the standard register write found in SleepyDog and other libraries. This decision was made because feeding the WDT on the SAMD21 is [extremely slow (1-5ms)](https://www.avrfreaks.net/forum/c21-watchdog-syncing-too-slow), which is unacceptable for the `MARK` macro (see https://github.com/OPEnSLab-OSU/FeatherFault/issues/4). Note that due to this implementation, the watchdog interrupt happens regularly and may take an extended period of time (1-5ms), causing possible timing issues with other code.

#### Memory Overflow Detection

Memory overflow detection is implemented by checking the top of the heap against the top of the stack. If the stack is overwriting the heap, memory is assumed to be corrupted and the board is immediately reset. This check is performed inside the `MARK` macro.

#### Hard Fault Detection

Hard Fault detection is implemented using the existing hard fault interrupt vector built into ARM. This interrupt is normally [defined as a infinite loop](https://github.com/adafruit/ArduinoCore-samd/blob/bf24e95f7ef7b41201d4389ef47b858b14ca58dd/cores/arduino/cortex_handlers.c#L43), however FeatherTrace overrides this handler to allow for tracing and a graceful recovery. This feature is activated when FeatherTrace is included in the sketch.

### Stacktracing

FeatherTrace's stacktrace implementation is derived from the [OpenMRN implementation](https://github.com/bakerstu/openmrn/blob/master/src/freertos_drivers/common/cpu_profile.hxx), with modifications for Cortex-M0+ support. 

At it's core, the OpenMRN stacktrace is based on [_Unwind_Backtrace](https://stackoverflow.com/questions/6254058/how-to-get-fullstacktrace-using-unwind-backtrace-on-sigsegv), a GCC-specific function allowing a developer to read every stack frame starting from the current one.

The ARM Cortex-M has [two stack pointers](https://static.docs.arm.com/ddi0419/d/DDI0419D_armv6m_arm.pdf#page=196), `MSP` and `PSP`, which it switches between when handling exceptions. In other words, code inside an exception has an entirely different stack than code outside an exception. When handling an exception, FeatherTrace must unwind the stack outside the exception from inside the exception—reading stacktrace saved on the other stack from inside the exception handler. 

As `_Unwind_Backtrace` will [only unwind the currently active stack](https://stackoverflow.com/questions/47331426/stack-backtrace-for-arm-core-using-gcc-compiler-when-there-is-a-msp-to-psp-swit/50923698#50923698), FeatherTrace must call the underlying implementation function `__gnu_Unwind_Backtrace` to target the inactive stack. `__gnu_Unwind_Backtrace` requires the exact state of all 14 registers to perform a trace—normally these values would be populated at the time the trace is taken, however in FeatherTrace's case this means retrieving the values of these registers at the exact moment after the fault. To accomplish this, FeatherTrace uses a [naked](https://gcc.gnu.org/onlinedocs/gcc/ARM-Function-Attributes.html) assembly interrupt handler (`p_handler`) to save the register values immediately after the exception is thrown. This handler is only able to save registers 4-14, however, as registers r0-3 are [modified during exception entry](https://static.docs.arm.com/ddi0419/d/DDI0419D_armv6m_arm.pdf#page=196). To retrieve these registers, the handler must also pass the inactive stack pointer to a function (`fill_phase2_vrs`) which reads these registers from the inactive stack after they were [pushed during the exception](https://static.docs.arm.com/ddi0419/d/DDI0419D_armv6m_arm.pdf#page=196). With this saved register set `__gnu_Unwind_Backtrace` is fooled into thinking it is performing a backtrace *just before* the fault happened, allowing us to extract information about the fault itself.

Additional information on this approach can be found in [this StackOverflow post](https://stackoverflow.com/questions/47331426/stack-backtrace-for-arm-core-using-gcc-compiler-when-there-is-a-msp-to-psp-swit/50923698#50923698).

### Compile Flags

FeatherTrace requires the following additional compile flags to function correctly:
```
-ggdb3 -g3 -fasynchronous-unwind-tables -Wl,--no-merge-exidx-entries
```
Breaking these flags down:
 * `-ggdb3 -g3` - Ensure there is debugging information in the `.elf` file. More information on how these flags work [here](https://eli.thegreenplace.net/2011/02/07/how-debuggers-work-part-3-debugging-information).
 * `-fasynchronous-unwind-tables` - A GCC specific flag to enable unwinding tables, required for `_Unwind_Backtrace` to function. With this flag GCC will generate static tables that allow an executing program to determine all functions called before it during a given execution. This feature would normally be used to unwind the stack after an exception, however FeatherTrace hijacks it to determine where the program was when a fault occurred. This [StackOverflow post](https://stackoverflow.com/questions/53102185/what-exactly-happens-when-compiling-with-funwind-tables) goes into more depth on the functionality of this flag.
 * `-Wl,--no-merge-exidx-entries` - A suggestion from this [StackOverflow post](https://stackoverflow.com/a/6947164) to prevent dangerous optimizations. I have no idea what this does, but it doesn't seem to cause any issues.


### Useful Resources
 * [How faults on cortex-M work](https://www.segger.com/downloads/application-notes/AN00016).
 * Normally Cortex-M has a number of failure registers, however Cortex-M0 [has none of these](https://community.arm.com/developer/ip-products/system/f/embedded-forum/3257/debugging-a-cortex-m0-hard-fault). We can still use the [SCB VECACTIVE bit](https://developer.arm.com/docs/dui0662/a/cortex-m0-peripherals/system-control-block/interrupt-control-and-state-register) to tell what kind of interrupt we're in, and GCC's unwind-tables feature to tell where we've come from.