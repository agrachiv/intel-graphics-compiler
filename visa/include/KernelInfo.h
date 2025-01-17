/*========================== begin_copyright_notice ============================

Copyright (C) 2021 Intel Corporation

SPDX-License-Identifier: MIT

============================= end_copyright_notice ===========================*/

#ifndef KERNELINFO_
#define KERNELINFO_


#include <string>
#include <map>

class VarInfo
{
public:
    enum AddressModel {
        GLOBAL = 0,
        LOCAL = 1
    };

    enum MemAccess {
        NONE = 0,
        BLOCKED = 1,
        STATEFUL = 2,
        STATELESS = 3,
        ATOMIC = 4
    };

    int lineNb;
    const char* srcFilename;
    int size;
    short type;
    AddressModel addrModel;
    MemAccess memoryAccess;
    bool isSpill;
    bool isUniform;
    bool isConst;
    bool promoted2GRF;

    // BankConflictInfo
    int bc_count;
    int bc_sameBank;
    int bc_twoSrc;
};

class KERNEL_INFO
{
public:
    std::map<int, VarInfo*> variables;

    KERNEL_INFO() { }
    ~KERNEL_INFO()
    {
        for (auto i = variables.begin(); i != variables.end(); ++i)
        {
            delete i->second;
        }
        variables.clear();
    }
};

#endif
