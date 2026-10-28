#include <atomic>
#include <cstdlib>
#include <new>

namespace {std::atomic<unsigned> lastFailure{};}
// The toolchain's nothrow overload delegates to throwing new. With IDF exception
// stubs that aborts before it can return nullptr. Firmware callers check nullptr.
void* operator new(std::size_t size,const std::nothrow_t&)noexcept{
    void* result=std::malloc(size?size:1);
    if(!result)lastFailure=static_cast<unsigned>(size);
    return result;
}
void* operator new[](std::size_t size,const std::nothrow_t& tag)noexcept{return ::operator new(size,tag);}
extern "C" unsigned passport_last_failed_allocation(){return lastFailure.load();}
