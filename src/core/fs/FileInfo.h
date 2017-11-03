#pragma once

#include "ff/ff.h"

namespace fs {

class FileInfo {
public:
    const char *name() { return _info.fname; }

    size_t size() const { return _info.fsize; }

private:
    FILINFO _info;

    friend class Directory;
};

} // namespace fs
