#include "FeatherTrace.h"
extern "C" {
    #include <unwind.h>
}
/** Allocate 512 bytes in flash for our crash logs */
alignas(256) _Pragma("location=\"FLASH\"") static const uint8_t FeatherTraceFlash[512] = { 0 };
const void* FeatherTraceFlashPtr = FeatherTraceFlash;

/**
 * Struct similar to FeatherTrace::FaultData, but with strings to mark
 * where data is stored in a flash dump. All properties except mark*
 * have the same meaning as FeatherTrace::FaultData.
 * 
 * Please ensure that all values in this struct are word (4-byte) aligned, to
 * prevent decoding issues when reading directly from flash.
 */
struct alignas(uint32_t) FaultDataFlashStruct {
    uint32_t value_head = 0xFEFE2A2A;
    char marker[24] = "FeatherTrace Data Here:";
    uint32_t version = 0;
    char marker1[8] = "Caused:";
    uint32_t cause;
    char marker2[8] = "I type:";
    uint32_t interrupt_type;
    char marker3[8] = "Traced:";
    uint32_t stacktrace[MAX_STRACE];
    char marker4[8] = "Regdmp:";
    uint32_t regs[16];
    uint32_t xpsr;
    char marker5[8] = "My Bad:";
    uint32_t is_corrupted;
    char marker6[8] = "Fail #:";
    uint32_t failnum;
    char marker7[8] = "Line #:";
    int32_t line;
    char marker8[8] = "File n:";
    // may be corrupted if is_corrupted is true
    char file[64]; 
    char marker9[4] = "End";
};

typedef union {
    struct FaultDataFlashStruct data;
    alignas(FaultDataFlashStruct) uint32_t raw_u32[(sizeof(FaultDataFlashStruct)+3)/4]; // rounded to the nearest 4 bytes
    alignas(FaultDataFlashStruct) uint8_t raw_u8[sizeof(FaultDataFlashStruct)];
} FaultDataFlash_t;

typedef struct {
    unsigned last_ip;
    int strace_len;
    bool sdid_max_len;
    unsigned stacktrace[MAX_STRACE];
}  trace_arg_t;

static const uint32_t pageSizes[] = { 8, 16, 32, 64, 128, 256, 512, 1024 };

/** Global atmoic bool to check if the watchdog has been fed, we use a boolean instead of WDT_Reset because watchdog synchronization is slow */
static volatile std::atomic_bool should_feed_watchdog(false);
/** Global atomic bool to specify that last_line or last_file are being written to, determines if a fault happened while they were being written */
static volatile std::atomic_bool is_being_written(false);
/** Global variable to store the last line MARKed, written by FeatherTrace::_Mark and read by FeatherTrace::HandleFault */
static volatile int last_line = 0;
/** Global variable to store the last filename, written by FeatherTrace::_Mark and read by FeatherTrace::HandleFault */
static volatile const char* last_file = "";
/** Global variable to store function pointer we would like to call during the watchdog, if any */
static volatile void(*callback_ptr)() = nullptr;
/// We store what we know about the external context at interrupt entry in this
/// structure.
phase2_vrs p_main_context;

static unsigned saved_lr;
static unsigned saved_xpsr;

#ifdef __arm__
// should use uinstd.h to define sbrk but Due causes a conflict
extern "C" char* sbrk(int incr);
#else  // __ARM__
extern char *__brkval;
#endif  // __arm__

static int freeMemory() {
  char top;
#ifdef __arm__
  return &top - reinterpret_cast<char*>(sbrk(0));
#elif defined(CORE_TEENSY) || (ARDUINO > 103 && ARDUINO != 151)
  return &top - __brkval;
#else  // __arm__
  return __brkval ? &top - __brkval : &top - __malloc_heap_start;
#endif  // __arm__
}

extern "C" {
    _Unwind_Reason_Code __gnu_Unwind_Backtrace(
        _Unwind_Trace_Fn trace, void *trace_argument, phase2_vrs *entry_vrs);

    _Unwind_Reason_Code _Unwind_Backtrace(_Unwind_Trace_Fn trace, void * trace_argument);
}

// Derived from VECTACTIVE in
// https://developer.arm.com/docs/dui0662/a/cortex-m0-peripherals/system-control-block/interrupt-control-and-state-register
enum SCBFaultType : uint32_t {
    SCB_NONE = 0,
    SCB_HARDFAULT = 3,
    SCB_WDTEW = 18,
};

/// Takes registers from the core state and the saved exception context and
/// fills in the structure necessary for the LIBGCC unwinder.
/// @param fault_args stack pointer we would like to unwind
static void fill_phase2_vrs(volatile unsigned *fault_args)
{
    // see https://static.docs.arm.com/ddi0419/d/DDI0419D_armv6m_arm.pdf B.1.5.6 for
    // details on which registers are pushed to the stack and in what order
    p_main_context.demand_save_flags = 0;
    p_main_context.core.r[0] = fault_args[0];
    p_main_context.core.r[1] = fault_args[1];
    p_main_context.core.r[2] = fault_args[2];
    p_main_context.core.r[3] = fault_args[3];
    p_main_context.core.r[12] = fault_args[4];
    // We add +2 here because first thing libgcc does with the lr value is
    // subtract two, presuming that lr points to after a branch
    // instruction. However, exception entry's saved PC can point to the first
    // instruction of a function and we don't want to have the backtrace end up
    // showing the previous function.
    // This has been removed for ARMv6
    p_main_context.core.r[14] = fault_args[6]; // + 2; // Set link register to the previous program counter
    p_main_context.core.r[15] = fault_args[6]; // Set program counter as well
    saved_lr = fault_args[5]; // save the link register for later
    // also save xPSR so we can read it later
    saved_xpsr = fault_args[7];
    // set the stack pointer to the inactive stack minus values pushed entering the exception
    p_main_context.core.r[13] = (unsigned)(fault_args + 8); 
}

/// Callback from the unwind backtrace function.
_Unwind_Reason_Code trace_func(struct _Unwind_Context *context, void *arg)
{
    trace_arg_t* myargs = (trace_arg_t*)arg;
    unsigned ip = _Unwind_GetIP(context);
    // ignore the first entry to prevent doubling up
    if (myargs->strace_len == 0)
    {
        // stacktrace[strace_len++] = ip;
        // By taking the beginning of the function for the immediate interrupt
        // we will attempt to coalesce more traces.
        // ip = (void *)_Unwind_GetRegionStart(context);
    }
    else if (myargs->last_ip == ip)
    {
        if (myargs->strace_len == 1 
            && saved_lr != 0
            && saved_lr != _Unwind_GetGR(context, 14))
        {
            _Unwind_SetGR(context, 14, saved_lr);
            // allocator.singleLenHack++; not sure what this was for?
            return _URC_NO_REASON;
        }
        return _URC_END_OF_STACK;
    }
    if (myargs->strace_len >= MAX_STRACE - 1)
    {
        myargs->sdid_max_len = true;
        return _URC_END_OF_STACK;
    }
    myargs->stacktrace[myargs->strace_len++] = ip;
    myargs->last_ip = ip;
    // for some reason GCC keeps unwinding past the reset handler sometimes,
    // which causes yet another hardfault
    // this addresses that issue by exiting upon encountering it.
    // Add one because GetRegionStart is one off for some reason
    int (*ptr)() = (int (*)())(_Unwind_GetRegionStart(context) + 1);
    if (ptr == main) {
        return _URC_END_OF_STACK;
    }
    // ip = (void *)_Unwind_GetRegionStart(context);
    // stacktrace[strace_len++] = ip;
    return _URC_NO_REASON;
}


/// Called from the interrupt handler to take a CPU trace for the current
/// exception.
static void take_isr_cpu_trace(trace_arg_t* arg)
{
    p_main_context.demand_save_flags = 0;
    // perform the stack trace!
    phase2_vrs first_context = p_main_context;
    __gnu_Unwind_Backtrace(&trace_func, arg, &first_context);
    // This is a workaround for the case when the function in which we had the
    // exception trigger does not have a stack saved LR. In this case the
    // backtrace will fail after the first step. We manually append the second
    // step to have at least some idea of what's going on.
    if (arg->strace_len == 0)
    {
        arg->stacktrace[0] = p_main_context.core.r[15];
        arg->strace_len++;
    }
    if (arg->strace_len == 1)
    {
        // try the link register instead of the program counter
        p_main_context.core.r[14] = saved_lr;
        p_main_context.core.r[15] = saved_lr;
        arg->last_ip = 0;
        __gnu_Unwind_Backtrace(&trace_func, arg, &p_main_context);
    }
    if (arg->strace_len == 1)
    {
        arg->stacktrace[1] = saved_lr;
        arg->strace_len++;
    }
}

static void WDTReset() {
    while(WDT->STATUS.bit.SYNCBUSY);
    WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
}

static void write_to_flash(const FaultDataFlash_t& trace) {
    volatile uint32_t* flash_u32 = (volatile uint32_t*)FeatherTraceFlashPtr;
    volatile uint8_t* const flash_u8 = (volatile uint8_t*)FeatherTraceFlashPtr;
    // determine page size
    const uint32_t pagesize = pageSizes[NVMCTRL->PARAM.bit.PSZ];
    // iterate!
    const size_t pagewords = pagesize / 4;
    const size_t pagerows = pagesize * 4;
    // erase our memory
    for (size_t i = 0; i < sizeof(trace.raw_u32); i += pagerows) {
        NVMCTRL->ADDR.reg = ((uint32_t)&(flash_u8[i])) / 2;
        NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_ER;
        while (!NVMCTRL->INTFLAG.bit.READY) { }
    }
    // Disable automatic page write
    NVMCTRL->CTRLB.bit.MANW = 1;
    // iterate!
    const size_t writelen = sizeof(trace.raw_u32) / 4;
    size_t idx = 0;
    while (idx < writelen) {
        // Execute "PBC" Page Buffer Clear
        NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_PBC;
        while (NVMCTRL->INTFLAG.bit.READY == 0) { }
        // write!
        const size_t min_idx = (writelen - idx) > pagewords ? pagewords : (writelen - idx);
        for (size_t i = 0; i < min_idx; i++)
            *(flash_u32++) = trace.raw_u32[idx++];
        // flush the page if needed (every pagesize words and the last run)
        NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_WP;
        while (NVMCTRL->INTFLAG.bit.READY == 0) { }
    }
}

/**
 * @brief Generic fault handler for FeatherTrace
 * 
 * TODO: fix comment
 * 
 * @param cause The reason HandleFault was called (should not be FAULT_NONE).
 * @return This function does not return.
 */
void FeatherTrace::Fault(FeatherTrace::FaultCause cause) {
    // Check if the the interrupt was a WDT EW
    // Read SCB/ICSR, detailed here:
    // https://developer.arm.com/docs/dui0662/a/cortex-m0-peripherals/system-control-block/interrupt-control-and-state-register
    // this will tell us what kind of interrupt triggered
    uint32_t last_intr = *(uint32_t*)(0xE000ED04) & 0x001F;
    // check if it's a watchdog interrupt
    if (last_intr == SCBFaultType::SCB_WDTEW) {
        // we may just need to feed the WDT
        WDT->INTFLAG.bit.EW  = 1;        // Clear interrupt flag
        // Check if the watchdog has been "fed", if so, reset the watchdog and continue
        if (should_feed_watchdog.load()){
            should_feed_watchdog.store(false);
            WDTReset();
            return;
        }
        // else there's been a timeout, so fault!
    }
    // disable the watchdog so we aren't interrupted
    FeatherTrace::StopWDT();
    // Create a fault data object, and populate it with all the saved data
    FaultDataFlash_t trace = { {} };
    // save the interrupt type
    trace.data.interrupt_type = last_intr;
    // check if we're in a synchronous context,
    // if so we can't save registers but can backtrace normally
    if (last_intr == SCBFaultType::SCB_NONE) {
        // take a cpu trace!
        trace_arg_t arg = {};
        // run unwind_backtrace!
        _Unwind_Backtrace(&trace_func, &arg);
        // write the results to our fault data
        for (size_t i = 0; i < MAX_STRACE; i++)
            trace.data.stacktrace[i] = arg.stacktrace[i];
    }
    // else save registers and manipulate unwind.h to use alternate stack
    else {
        // write the saved registers to our trace
        for (size_t i = 0; i < 16; i++)
            trace.data.regs[i] = p_main_context.core.r[i];
        // also make sure the link register is set correctly, since we have to hack around it earlier
        trace.data.regs[14] = saved_lr;
        // save xPSR
        trace.data.xpsr = saved_xpsr;
        // take a backtrace!
        trace_arg_t arg = {};
        take_isr_cpu_trace(&arg);
        // write the results to our fault data
        for (size_t i = 0; i < MAX_STRACE; i++)
            trace.data.stacktrace[i] = arg.stacktrace[i];
    }
    // determine the cause, if cause is unknown
    if (cause == FeatherTrace::FAULT_UNKNOWN) {
        // check to see if we know what kind of interrupt we're in
        if (last_intr == SCBFaultType::SCB_WDTEW)
            trace.data.cause = FeatherTrace::FAULT_HUNG;
        else if (last_intr == SCBFaultType::SCB_HARDFAULT)
            trace.data.cause = FeatherTrace::FAULT_HARDFAULT;
        else
            trace.data.cause = FeatherTrace::FAULT_UNKNOWN;
    }
    else
        trace.data.cause = cause;
    // check if FeatherTrace may have been the cause (oops)
    trace.data.is_corrupted = is_being_written.load() ? 1 : 0;
    // write cause, line, and file info
    trace.data.line = last_line;
    // if the pointer was being written and we interrupted it, we don't want to make things worse
    if (!trace.data.is_corrupted) {
        const volatile char* index = last_file;
        uint32_t i = 0;
        for (; i < sizeof(trace.data.file) - 1 && *index != '\0'; i++)
            trace.data.file[i] = *(index++);
        trace.data.file[i] = '\0';
    }
    else 
        trace.data.file[0] = '\0'; // Corrupted!
    // read the failure number from flash, and write it + 1
    trace.data.failnum = ((FaultDataFlash_t*)FeatherTraceFlashPtr)->data.failnum + 1;
    // write the collected data to flash!
    write_to_flash(trace);
    // call the callback function if one is registered
    if (callback_ptr != nullptr)
        callback_ptr();
    // All done! the chip will now reset
    NVIC_SystemReset();
    while(true);
}

extern "C" {
    volatile void __attribute__((__noinline__)) p_load_monitor_interrupt_handler(
        volatile unsigned *exception_args, unsigned exception_return_code)
    {
        // read the stack pointer not currently in use
        fill_phase2_vrs(exception_args);
        // Call the FeatherTrace Fault handler
        FeatherTrace::Fault(FeatherTrace::FaultCause::FAULT_UNKNOWN);
    }
}

/* See FeatherTrace.h */
void FeatherTrace::StartWDT(const FeatherTrace::WDTTimeout timeout) {
    // Generic clock generator 2, divisor = 32 (2^(DIV+1))
    GCLK->GENDIV.reg = GCLK_GENDIV_ID(2) | GCLK_GENDIV_DIV(4);
    // Enable clock generator 2 using low-power 32KHz oscillator.
    // With /32 divisor above, this yields 1024Hz(ish) clock.
    GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(2) |
                        GCLK_GENCTRL_GENEN |
                        GCLK_GENCTRL_SRC_OSCULP32K |
                        GCLK_GENCTRL_DIVSEL;
    while(GCLK->STATUS.bit.SYNCBUSY);
    // WDT clock = clock gen 2
    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_WDT |
                        GCLK_CLKCTRL_CLKEN |
                        GCLK_CLKCTRL_GEN_GCLK2;
    // Enable WDT early-warning interrupt
    NVIC_DisableIRQ(WDT_IRQn);
    NVIC_ClearPendingIRQ(WDT_IRQn);
    NVIC_SetPriority(WDT_IRQn, 0); // Top priority
    NVIC_EnableIRQ(WDT_IRQn);
    // Disable watchdog for config
    WDT->CTRL.reg = 0;
    while(WDT->STATUS.bit.SYNCBUSY);
    // Enable early warning interrupt
    WDT->INTENSET.bit.EW   = 1;
    // Period = twice
    WDT->CONFIG.bit.PER    = static_cast<uint8_t>(timeout);
    // Set time of interrupt 
    WDT->EWCTRL.bit.EWOFFSET = static_cast<uint8_t>(timeout) - 1;
    // Disable window mode
    WDT->CTRL.bit.WEN      = 0;
    // Sync CTRL write
    while(WDT->STATUS.bit.SYNCBUSY); 
    // Clear watchdog interval
    WDTReset();
    // Start watchdog now!  
    WDT->CTRL.bit.ENABLE = 1;            
    while(WDT->STATUS.bit.SYNCBUSY);
    should_feed_watchdog.store(false);
}


/* See FeatherTrace.h */
void FeatherTrace::StopWDT() {
    // stop the watchdog
    WDT->CTRL.bit.ENABLE = 0;            
    while(WDT->STATUS.bit.SYNCBUSY);
}

/* See FeatherTrace.h */
void FeatherTrace::SetCallback(volatile void(*callback)()) {
    callback_ptr = callback;
}

/* See FeatherTrace.h */
void FeatherTrace::_Mark(const int line, const char* file) {
    // feed the watchdog
    should_feed_watchdog.store(true);
    // write the last marked data
    is_being_written.store(true);
    last_line = line;
    last_file = file;
    is_being_written.store(false);
    // check for a stackoverflow
    const int mem = freeMemory();
    if (mem < 0 || mem > 60000)
        FeatherTrace::Fault(FeatherTrace::FAULT_OUTOFMEMORY);
}

/* See FeatherTrace.h */
void FeatherTrace::PrintFault(Print& where) {
    // Load the fault data from flash
    const FaultDataFlash_t* trace = (FaultDataFlash_t*)FeatherTraceFlashPtr;
    // print it the printer
    if (trace->data.cause != FeatherTrace::FAULT_NONE) {
        where.print("Fault! Cause: ");
        switch (trace->data.cause) {
            case FeatherTrace::FAULT_UNKNOWN: where.println("UKNOWN"); break;
            case FeatherTrace::FAULT_HUNG: where.println("HUNG"); break;
            case FeatherTrace::FAULT_HARDFAULT: where.println("HARDFAULT"); break;
            case FeatherTrace::FAULT_OUTOFMEMORY: where.println("OUTOFMEMORY"); break;
            case FeatherTrace::FAULT_USER: where.println("USER"); break;
            default: where.println("Corrupted");
        }
        
        where.print("Fault during recording: ");
        where.println(trace->data.is_corrupted ? "Yes" : "No");
        where.print("Line: ");
        where.println(trace->data.line);
        where.print("File: ");
        where.println(trace->data.file);
        where.print("Interrupt type: ");
        where.println(trace->data.interrupt_type);
        where.print("Stacktrace: ");
        for (size_t i = 0; ; i++) {
            char buf[24];
            snprintf(buf, sizeof(buf), "0x%08lx", trace->data.stacktrace[i]);
            where.print(buf);
            if (i + 1 < MAX_STRACE 
                && trace->data.stacktrace[i + 1] != 0)
                where.print(", ");
            else
                break;
        }
        where.println();
        if (trace->data.interrupt_type != 0) {
            char buf[32];
            where.println("Registers: ");
            for (unsigned int i = 0; i < 13; i++) {
                snprintf(buf, sizeof(buf), "\tR%u: 0x%08lx", i, trace->data.regs[i]);
                where.print(buf);
            }
            snprintf(buf, sizeof(buf), "\tSP: 0x%08lx", trace->data.regs[13]);
            where.print(buf);
            snprintf(buf, sizeof(buf), "\tLR: 0x%08lx", trace->data.regs[14]);
            where.print(buf);
            snprintf(buf, sizeof(buf), "\tPC: 0x%08lx", trace->data.regs[15]);
            where.print(buf);
            snprintf(buf, sizeof(buf), "\txPSR: 0x%08lx", trace->data.xpsr);
            where.println(buf);
        }
        where.print("Failures since upload: ");
        where.println(trace->data.failnum);
    }
    else
        where.println("No fault");
}

/* See FeatherTrace.h */
bool FeatherTrace::DidFault() {
    // Load the fault data from flash
    const FaultDataFlash_t* trace = (FaultDataFlash_t*)FeatherTraceFlashPtr;
    return trace->data.cause != FeatherTrace::FAULT_NONE;
}

/* See FeatherTrace.h */
FeatherTrace::FaultData FeatherTrace::GetFault() {
    // Load the fault data from flash
    const FaultDataFlash_t* trace = (FaultDataFlash_t*)FeatherTraceFlashPtr;
    // copy all relavent data
    FaultData ret;
    ret.cause = static_cast<FeatherTrace::FaultCause>(trace->data.cause);
    ret.interrupt_type = trace->data.interrupt_type;
    for (size_t i = 0; i < MAX_STRACE; i++)
        ret.stacktrace[i] = trace->data.stacktrace[i];
    for (size_t i = 0; i < 16; i++)
        ret.regs[i] = trace->data.regs[i];
    ret.xpsr = trace->data.xpsr;
    ret.is_corrupted = trace->data.is_corrupted;
    ret.failnum = trace->data.failnum;
    ret.line = trace->data.line;
    for(size_t i = 0; i < sizeof(ret.file); i++)
        ret.file[i] = trace->data.file[i];
    return ret;
}