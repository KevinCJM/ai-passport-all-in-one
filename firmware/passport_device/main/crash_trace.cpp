#include "esp_attr.h"
#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"
#include <cstdio>

// Keep only fault registers and the assertion label, never a memory or audio dump.
namespace {
struct Trace{uint32_t magic,pc,ra,cause,value;char assertion[128];};
RTC_NOINIT_ATTR volatile Trace trace;
}
extern "C" void __real_esp_panic_handler(panic_info_t*) __attribute__((noreturn));
extern "C" void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t* info){
    trace.magic=0;
    if(info&&info->frame){const auto* frame=static_cast<const RvExcFrame*>(info->frame);trace.pc=frame->mepc;trace.ra=frame->ra;trace.cause=frame->mcause;trace.value=frame->mtval;}
    unsigned i=0;if(g_panic_abort&&g_panic_abort_details)for(;i<sizeof(trace.assertion)-1&&g_panic_abort_details[i];++i)trace.assertion[i]=g_panic_abort_details[i];
    trace.assertion[i]=0;trace.magic=0x43524153;
    __real_esp_panic_handler(info);
}
extern "C" void passport_crash_trace(char* out,size_t size){
    if(trace.magic!=0x43524153){snprintf(out,size,"none");return;}
    char assertion[128];for(unsigned i=0;i<sizeof(assertion);++i)assertion[i]=trace.assertion[i];
    snprintf(out,size,"pc=%08lx ra=%08lx cause=%lu value=%08lx %.127s",static_cast<unsigned long>(trace.pc),static_cast<unsigned long>(trace.ra),static_cast<unsigned long>(trace.cause),static_cast<unsigned long>(trace.value),assertion);
}
