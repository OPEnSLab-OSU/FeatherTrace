#pragma once

#include <Arduino.h>
#include <atomic>
#include <Adafruit_ASFcore.h>
#include <reset.h>
#include <sam.h>
#include "ShortFile.h"

#define MAX_STRACE 20

/**
 * Welcome to FeatherTrace
 * For more information on how to use this library, please see the [README](../README.md).
 */

namespace FeatherTrace {
    // TODO: encode all NVIC types
    /** Enumeration for possible causes for fault */
    enum FaultCause : uint32_t {
        FAULT_NONE = 0,
        /** The watchdog was triggered */
        FAULT_HUNG = 1,
        /** An invalid instruction was executed, or an invalid memory address was accessed */
        FAULT_HARDFAULT = 2,
        /** The heap has crossed into the stack, and the memory is corrupted (see https://learn.adafruit.com/memories-of-an-arduino?view=all) */
        FAULT_OUTOFMEMORY = 3
    };

    enum class WDTTimeout : uint8_t {
        WDT_8MS = 1,
        WDT_15MS = 2,
        WDT_31MS = 3,
        WDT_62MS = 4,
        WDT_125MS = 5,
        WDT_250MS = 6,
        WDT_500MS = 7,
        WDT_1S = 8,
        WDT_2S = 9,
        WDT_4S = 10,
        WDT_8S = 11
    };

    enum class FaultType : uint32_t {
        None = 0,
        HardFault = 3,
        WDTEarlyWarning = 18,

    };

    /** Struct containg information about the last fault. */
    struct FaultData {
        /** uin8_t indicating the cause of the fault. */
        FeatherTrace::FaultCause cause;
        /** Whether or not the fault happened while FeatherTrace was recording line information (1 if true, 0 if not) */
        uint8_t is_corrupted;
        /** Number of times FeatherTrace has detected a failure since the device was last programmed */
        uint32_t failnum;
        /** The line number of the last MARK statement before failure (for a memory fault will be the MARK where the fault happened) */
        int32_t line;
        /** The filename where this line was taken from, may be corrupted if is_corrupted is 1 */
        char file[64];
        /** A list of addresses forming a backtrace to where the fault happened */
        uint32_t stacktrace[MAX_STRACE];
    };

    /**
     * Starts the watchdog timer with a specified timeout. On the event
     * that the watchdog timer runs out (if MARK is not called within
     * the timeout period) a fault will be triggered with cause
     * FeatherTrace::FAULT_HUNG and the board will reset.
     * 
     * You do not need to call this function to use FeatherTrace,
     * however the FeatherTrace::FAULT_HUNG handler will not run
     * without it.
     * 
     * This funtionality is implemented in terms of the early warning
     * interrupt. As a result, the maximum and minimum possible delays
     * for the WDT are not available.
     * @param timeout Timeout to use for the WDT.
     */
    void StartWDT(const WDTTimeout timeout);

    /**
     * Stop the watchdog timer. Use this fuction if you are planning
     * on sleeping or performing an extended task in which you do
     * not want the watchdog timer to interrupt.
     * 
     * In order for the watchdog to function after this function
     * is called, you must start it again with FeatherTrace::StartWDT.
     */
    void StopWDT();
    
    /**
     * Set a callback function to be called whenever FeatherTrace triggered.
     * This function should be a volatile void function.
     * 
     * Please note that this function MUST be reentrent, and MUST NOT 
     * cause a fault itself, othwise breaking things even further. 
     * Be careful!
     * 
     * @param callback function to call on fault, nullptr if none
     */
    void SetCallback(volatile void(*callback)());

    /**
     * Prints information about the fault to a print stream (such as the
     * serial monitor) in a human readable format. This function is 
     * useful if you are debugging with the serial monitor.
     * @param where The print stream to output to (ex. Serial).
     */
    void PrintFault(Print& where);

    /**
     * Returns whether or not FeatherTrace has detected a fault since
     * this device was last programmed.
     * @return true if a fault has occurred, false if not.
     */
    bool DidFault();

    /**
     * Returns a FeatherTrace::FaultData struct containing information
     * about the last fault to occur. If no fault has occured, this
     * function will return a struct of all zeros.
     */
    FaultData GetFault();

    /** Private utility function called by the MARK macro */
    void _Mark(const int line, const char* file);
}
void HandleFault();

/** 
 * Macro to track the last place where the program was alive.
 * Place this macro frequently around your code so FeatherTrace
 * knows where a fault happened. For example:
 * ```C++
 * MARK;
 * while (sketchyFunction()) {
 *   MARK;
 *   moreSketchyThings(); MARK;
 * }
 * ```
 * Every call to MARK will store the current line # and filename
 * to some global varibles, allowing FeatherTrace to determine
 * where the failure happened when the program faults.
 * 
 * This macro is a proxy for FeatherTrace::_Mark, allowing it to 
 * grab the line # and filename.
 */
#define MARK { constexpr const char* const filename = __SHORT_FILE__; FeatherTrace::_Mark(__LINE__,  filename); }

extern "C" {
    /// This struct definition mimics the internal structures of libgcc in
    /// arm-none-eabi binary. It's not portable and might break in the future.
    struct core_regs
    {
        unsigned r[16];
    };

    /// This struct definition mimics the internal structures of libgcc in
    /// arm-none-eabi binary. It's not portable and might break in the future.
    typedef struct
    {
        unsigned demand_save_flags;
        struct core_regs core;
    } phase2_vrs;

    extern phase2_vrs p_main_context;
    
    volatile void __attribute__((__noinline__)) p_load_monitor_interrupt_handler(
        volatile unsigned *exception_args, unsigned exception_return_code);

    static void __attribute__((__naked__)) p_handler()
    {
        __asm volatile(".thumb\n"
                        ".syntax unified\n"
                        // store r0-r14 to main_context.core
                        "mov  r0, %0 \n"
                        "str  r4, [r0, #4*4] \n"
                        "str  r5, [r0, #5*4] \n"
                        "str  r6, [r0, #6*4] \n"
                        "str  r7, [r0, #7*4] \n"
                        "movs  r1, #8*4\n"
                        "add r0, r1\n"
                        "mov r1, r8\n"
                        "str r1, [r0, #0]\n"
                        "mov r1, r9\n"
                        "str r1, [r0, #1*4]\n"
                        "mov r1, r10\n"
                        "str r1, [r0, #2*4]\n"
                        "mov r1, r11\n"
                        "str r1, [r0, #3*4]\n"
                        "mov r1, r12\n"
                        "str r1, [r0, #4*4]\n"
                        "mov r1, r13\n"
                        "str r1, [r0, #5*4]\n"
                        "mov r1, r14\n"
                        "str r1, [r0, #6*4]\n"
                        :
                        : "r"(p_main_context.core.r)
                        : "r0", "r1");
        __asm volatile( ".thumb\n"
                        ".syntax unified\n"
                        // write the correct stack pointer to r0
                        " mov   r1, lr\n"
                        " movs   r7, #4\n"
                        " tst   r1, r7\n"
                        " bne a\n"
                        " mrs r0, msp\n"
                        " b done\n"
                        "a: \n"
                        " mrs r0, psp\n"
                        "done:\n"
                        " mov r1, lr \n"
                        // call p_load_monitor_interrupt_handler
                        " ldr r2,  =p_load_monitor_interrupt_handler  \n"
                        " bx  r2  \n"
                        :
                        :
                        : "r0", "r1", "r2");
    }
}

#define FEATHERTRACE_BIND_HANDLER(_name) extern "C" {\
    void _name() __attribute__ ((alias("p_handler"))); }

#define FEATHERTRACE_BIND_ALL() \
    FEATHERTRACE_BIND_HANDLER(HardFault_Handler) \
    FEATHERTRACE_BIND_HANDLER(WDT_Handler)