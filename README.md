# 📜FeatherTrace
When a microcontroller crashes or hangs, it can be quite difficult to troubleshoot what caused it. FeatherTrace is an attempt to build a system that can not only recover from a crash, but explain why the crash happened. FeatherFault supports all boards using the SAMD21 (Adafruit Feather M0, Arduino Zero, etc.), and future support is planned for the SAMD51. 

FeatherTrace is an alternative to [FeatherFault](https://github.com/OPEnSLab-OSU/FeatherFault) for advanced users. FeatherTrace requires modifications to the compilation flags of your project to function, making it incompatible with the Arduino IDE and library manager. These modifications, however, allow FeatherTrace to offer a complete stacktrace and register dump on top of FeatherFault's existing features.

## Getting Started

To use FeatherTrace, you will need a build system using GCC that allows changing compilation flags, such as [PlatformIO](https://platformio.org/) or the [Arduino CLI](https://github.com/arduino/arduino-cli/releases). The Arduino IDE does not allow for configuring compilation and as a result is not supported. You will also need the [Adafruit ASF core](https://github.com/adafruit/Adafruit_ASFcore/tree/f6ffa8b2bc2477566c8406e5f3fa883b137347f1).

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
To break these flags down:
 * `-ggdb3 -g3` - Ensure there is debugging information in the `.elf` file. More information on how these flags work [here](https://eli.thegreenplace.net/2011/02/07/how-debuggers-work-part-3-debugging-information).
 * `-fasynchronous-unwind-tables` - A GCC specific flag to enable unwinding tables. With this flag GCC will generate static tables that allow an executing program to determine all functions called before it during a given execution. This feature would normally be used to unwind the stack after an exception, however FeatherTrace hijacks it to determine where the program was when a fault occurred. This [StackOverflow post](https://stackoverflow.com/questions/53102185/what-exactly-happens-when-compiling-with-funwind-tables) goes into more depth.
 * `-Wl,--no-merge-exidx-entries` - A suggestion from this [StackOverflow post](https://stackoverflow.com/a/6947164) to prevent unnecessary optimization. I have no idea what this does, but it doesn't seem to cause any issues.

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
    FeatherTrace::StartWDT(FeatherFault::WDTTimeout::WDT_8S);
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

Once FeatherTrace is activated, it will trigger after a set time of inactivity (we specify 8 seconds above, but this value can be changed), on [memory overflow](https://learn.adafruit.com/memories-of-an-arduino?view=all), or on a [hard fault](https://www.freertos.org/Debugging-Hard-Faults-On-Cortex-M-Microcontrollers.html). Once triggered, FeatherTrace will immediately save the location of the last run `MARK` statement, the fault cause, all current register states, and a stacktrace. FeatherTrace will then reset the board immediately to prevent further damage. After the reset, FeatherTrace's saved data can be read by `FeatherTrace::PrintFault` and `FeatherTrace::GetFault`, allowing the developer to determine if/where the board has failed after it resets.

### Usage Example (TODO: Edit)

To show how this behavior works, let's assume that `unsafe_function()` in the code block below attempts to access memory that doesn't exist, causing a hard fault:
```C++
void setup() {
    // Activate featherfault
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
If we run this code without FeatherFault, we would see the serial monitor output something like this:
```
Start!
```
After which the device hard faults, causing it to wait in an infinite loop until it is reset. 

This behavior is extremely difficult to troubleshoot: as the developer, all we know is that the device failed between `Start!` and `Done`. Using more print statements, we could eventually narrow down the cause to `unsafe_function`—this process is time consuming, unreliable, and downright annoying. Instead, let's try the same code with FeatherFault activated:
```C++
void setup() {
    // Wait for serial to connect to the serial monitor
    Serial.begin(...);
    while(Serial);
    // Activate FeatherFault
    FeatherFault::PrintFault(Serial);
    FeatherFault::StartWDT(FeatherFault::WDTTimeout::WDT_8S);
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
`No fault` here indicates that FeatherFault has not been triggered yet. We change that shortly by running `unsafe_function()`, causing a hard fault. Instead of waiting in an infinite loop, however, the board is immediately reset to the start of the sketch by FeatherFault. We can then open the serial monitor again:
```
Fault! Cause: HARDFAULT
Fault during recording: No
Line: 18
File: MySketch.ino
Failures since upload: 1
Start!
```
Since the FeatherFault was triggered by the hard fault, `FeatherFault::PrintFault` will print the last file and line number `MARK`ed before the hard fault happened. In this case, line 18 of `MySketch.ino` indicates the `MARK` statement after `other_function_one()`, leading us to suspect that `unsafe_function()` is causing the issue. We can now focus on troubleshooting `unsafe_function()`.

## Additional Features TODO

## Implementation Notes TODO

### Stack Tracing

This project seeks to combine stack tracing similar to [this OpenMRN implementation](https://github.com/bakerstu/openmrn/blob/master/src/freertos_drivers/common/cpu_profile.hxx) with [FeatherFault](https://github.com/OPEnSLab-OSU/FeatherFault) fault handling and post-mortem diagnostics. This project is in early stages, will be updated as progress is made.


Some notes:
 * How faults on cortex-M works: https://www.segger.com/downloads/application-notes/AN00016
 * Normally Cortex-M has a number of failure registers, however Cortex-M0 has none of these (see https://community.arm.com/developer/ip-products/system/f/embedded-forum/3257/debugging-a-cortex-m0-hard-fault). We can still use the SCB VECACTIVE bit (https://developer.arm.com/docs/dui0662/a/cortex-m0-peripherals/system-control-block/interrupt-control-and-state-register) to tell what kind of interrupt we're in, and GCC's unwind-tables feature to tell where we've come from.
 * FeatherTrace hijacks GCC's unwind.h internal functionality to unwind the stack not currently in use by the hardfault interrupt. This implementation is mostly inspired by this: https://github.com/bakerstu/openmrn/blob/0d051659af093e03d883a9ea003773ae58ace62a/src/freertos_drivers/common/cpu_profile.hxx