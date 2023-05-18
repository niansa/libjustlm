#ifndef DLHANDLE_H
#define DLHANDLE_H
#ifndef __WIN32
#include <string>
#include <stdexcept>
#include <utility>
#include <dlfcn.h>



class Dlhandle {
    void *chandle;

public:
    class Exception : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    Dlhandle() : chandle(nullptr) {}
    Dlhandle(const std::string& fpath, int flags = RTLD_LAZY) {
        chandle = dlopen(fpath.c_str(), flags);
        if (!chandle) {
            throw Exception("dlopen(\""+fpath+"\"): "+dlerror());
        }
    }
    Dlhandle(const Dlhandle& o) = delete;
    Dlhandle(Dlhandle&& o) : chandle(o.chandle) {
        o.chandle = nullptr;
    }
    ~Dlhandle() {
        if (chandle) dlclose(chandle);
    }

    auto operator =(Dlhandle&& o) {
        chandle = std::exchange(o.chandle, nullptr);
    }

    bool is_valid() const {
        return chandle != nullptr;
    }
    operator bool() const {
        return is_valid();
    }

    template<typename T>
    T* get(const std::string& fname) {
        auto fres = reinterpret_cast<T*>(dlsym(chandle, fname.c_str()));
        return (dlerror()==NULL)?fres:nullptr;
    }
    auto get_fnc(const std::string& fname) {
        return get<void*(...)>(fname);
    }
};
#else
#include <string>
#include <exception>
#include <libloaderapi.h>



class Dlhandle {
    HMODULE chandle;

public:
    class Exception : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    Dlhandle() : chandle(nullptr) {}
    Dlhandle(const std::string& fpath) {
        chandle = LoadLibraryA(fpath.c_str());
        if (!chandle) {
            throw Exception("dlopen(\""+fpath+"\"): Error");
        }
    }
    Dlhandle(const Dlhandle& o) = delete;
    Dlhandle(Dlhandle&& o) : chandle(o.chandle) {
        o.chandle = nullptr;
    }
    ~Dlhandle() {
        if (chandle) FreeLibrary(chandle);
    }

    bool is_valid() const {
        return chandle != nullptr;
    }

    template<typename T>
    T* get(const std::string& fname) {
        return reinterpret_cast<T*>(GetProcAddress(chandle, fname.c_str()));
    }
    auto get_fnc(const std::string& fname) {
        return get<void*(...)>(fname);
    }
};
#endif
#endif // DLHANDLE_H
