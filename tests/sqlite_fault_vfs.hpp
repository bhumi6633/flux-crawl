#pragma once
#include "fault_plan.hpp"
#include <sqlite3.h>
#include <cstddef>
namespace certification {
// Scoped wrapper of the platform VFS: inject a REAL SQLite write error without
// filling disk or modifying production connection code. All other I/O delegates.
class FaultVfs {
    struct File { sqlite3_file base; sqlite3_file* real; };
    inline static FaultPlan plan_;
    inline static int error_=SQLITE_FULL;
    sqlite3_vfs wrapped_{};
    sqlite3_vfs* original_;
    static File* file(sqlite3_file* f) { return reinterpret_cast<File*>(f); }
    static int close(sqlite3_file* f) { auto* r=file(f)->real; const auto code=r->pMethods->xClose(r); f->pMethods=nullptr; return code; }
    static int read(sqlite3_file* f,void* p,int n,sqlite3_int64 o) { auto* r=file(f)->real; return r->pMethods->xRead(r,p,n,o); }
    static int write(sqlite3_file* f,const void* p,int n,sqlite3_int64 o) { if(plan_.fires()) {return error_;} auto* r=file(f)->real; return r->pMethods->xWrite(r,p,n,o); }
    static int truncate(sqlite3_file* f,sqlite3_int64 n) { auto* r=file(f)->real; return r->pMethods->xTruncate(r,n); }
    static int sync(sqlite3_file* f,int n) { auto* r=file(f)->real; return r->pMethods->xSync(r,n); }
    static int size(sqlite3_file* f,sqlite3_int64* n) { auto* r=file(f)->real; return r->pMethods->xFileSize(r,n); }
    static int lock(sqlite3_file* f,int n) { auto* r=file(f)->real; return r->pMethods->xLock(r,n); }
    static int unlock(sqlite3_file* f,int n) { auto* r=file(f)->real; return r->pMethods->xUnlock(r,n); }
    static int reserved(sqlite3_file* f,int* n) { auto* r=file(f)->real; return r->pMethods->xCheckReservedLock(r,n); }
    static int control(sqlite3_file* f,int n,void* p) { auto* r=file(f)->real; return r->pMethods->xFileControl(r,n,p); }
    static int sector(sqlite3_file* f) { auto* r=file(f)->real; return r->pMethods->xSectorSize(r); }
    static int device(sqlite3_file* f) { auto* r=file(f)->real; return r->pMethods->xDeviceCharacteristics(r); }
    static int map(sqlite3_file* f,int a,int b,int c,void volatile** p) { auto* r=file(f)->real; return r->pMethods->xShmMap(r,a,b,c,p); }
    static int shm_lock(sqlite3_file* f,int a,int b,int c) { auto* r=file(f)->real; return r->pMethods->xShmLock(r,a,b,c); }
    static void barrier(sqlite3_file* f) { auto* r=file(f)->real; r->pMethods->xShmBarrier(r); }
    static int unmap(sqlite3_file* f,int n) { auto* r=file(f)->real; return r->pMethods->xShmUnmap(r,n); }
    static int fetch(sqlite3_file* f,sqlite3_int64 o,int n,void** p) { auto* r=file(f)->real; if (!r->pMethods->xFetch) { *p=nullptr; return SQLITE_OK; } return r->pMethods->xFetch(r,o,n,p); }
    static int unfetch(sqlite3_file* f,sqlite3_int64 o,void* p) { auto* r=file(f)->real; return r->pMethods->xUnfetch?r->pMethods->xUnfetch(r,o,p):SQLITE_OK; }
    inline static const sqlite3_io_methods methods_{3,close,read,write,truncate,sync,size,lock,unlock,reserved,control,sector,device,map,shm_lock,barrier,unmap,fetch,unfetch};
    static int open(sqlite3_vfs* v,const char* name,sqlite3_file* out,int flags,int* actual) {
        auto* original=static_cast<sqlite3_vfs*>(v->pAppData); auto* f=reinterpret_cast<File*>(out);
        f->base.pMethods=nullptr; f->real=reinterpret_cast<sqlite3_file*>(reinterpret_cast<std::byte*>(out)+sizeof(File));
        const auto code=original->xOpen(original,name,f->real,flags,actual);
        if(code==SQLITE_OK) { f->base.pMethods=&methods_; }
        return code;
    }
public:
    FaultVfs() : original_{sqlite3_vfs_find(nullptr)} {
        wrapped_=*original_; wrapped_.zName="fluxcrawl-certification-faults";
        wrapped_.pAppData=original_; wrapped_.szOsFile+=static_cast<int>(sizeof(File)); wrapped_.xOpen=open;
        if(sqlite3_vfs_register(&wrapped_,1)!=SQLITE_OK) { throw std::runtime_error("VFS registration failed"); }
    }
    ~FaultVfs() { plan_.remaining=0; sqlite3_vfs_register(original_,1); sqlite3_vfs_unregister(&wrapped_); }
    void arm(int error,std::size_t nth=1) { error_=error; plan_.arm(nth); }
};
}
