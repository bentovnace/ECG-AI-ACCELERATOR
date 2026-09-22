// ECG_COV_WRITE(): ghi tep coverage cua Verilator, va la KHONG-LAM-GI khi khong do.
//
// VI SAO MOT MACRO CHU KHONG MOT SHIM. Verilator chi THU coverage khi dich voi
// `--coverage`; viec GHI ra tep thi testbench phai tu goi. Ban dau toi lam mot
// tep `cov_shim.cpp` ghi trong ham huy cua mot doi tuong static, de khong phai
// sua 16 tep tb. NO KHONG CHAY: tep ra 22 byte, 0 diem. Do duoc nguyen nhan --
// bo dem coverage nam TRONG the hien model, va moi tb deu `delete dut` truoc khi
// thoat, nen den luc ham huy static chay thi du lieu da bien. Cung phep do sau
// khi doi cho ghi len TRUOC `delete`: 188.258 byte, 1.592 diem.
//
// BAI HOC: mot phep do bang 0 va mot phep do KHONG CHAY DUOC trong giong nhau
// tu ben ngoai. Neu toi tin cai shim thi bang coverage se bao 0 % cho moi khoi
// va do la mot con so SAI ma trong nhu mot ket qua. `tools/do_coverage.sh` co
// mot chot cho dung dieu do: tep coverage phai co it nhat mot dong diem.
//
// PHAI la `#if`, KHONG `#ifdef`. Verilator truyen `-DVM_COVERAGE=$(VM_COVERAGE)`
// tu tep `.mk` sinh ra, va o ban khong do coverage gia tri do la **0** -- tuc
// macro CO duoc dinh nghia, chi la bang 0. Nen `#ifdef` dung o CA HAI ban, va
// duong chay binh thuong goi `VerilatedCov::write` trong khi thu vien coverage
// khong duoc lien ket:
//     undefined reference to `VerilatedCov::threadCovp()'
// Do la cach phep sua nay lam hong CA 15 tb trong mot lan, va no hong o buoc
// LIEN KET chu khong bien dich -- nen thong bao khong nhac gi den ecg_cov.h.
#ifndef ECG_COV_H_
#define ECG_COV_H_

#if defined(VM_COVERAGE) && VM_COVERAGE
#include "verilated_cov.h"

#include <cstdlib>
#include <string>

// Ghi nhieu lan la vo hai: moi lan la mot ban dump DAY DU, va lan sau de len.
#define ECG_COV_WRITE()                                                       \
    do {                                                                      \
        const char* ecg_cov_p_ = std::getenv("ECG_COV_OUT");                  \
        VerilatedCov::write((ecg_cov_p_ && *ecg_cov_p_)                        \
                                ? std::string(ecg_cov_p_)                     \
                                : std::string("coverage.dat"));               \
    } while (0)
#else
#define ECG_COV_WRITE() do { } while (0)
#endif

#endif  // ECG_COV_H_
