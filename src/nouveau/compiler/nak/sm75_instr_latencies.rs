#![allow(non_camel_case_types)]

use crate::ir::HasRegFile;
use crate::ir::Dst;
use crate::ir::Op;
use crate::ir::IsUniform;
use crate::ir::DstsAsSlice;
use crate::ir::SrcsAsSlice;
use crate::ir::RegFile;

// This contains the register scheduling information provided by NVIDIA under NDA.
// This file is for Turing only.

// Coupled instructions are ones with fixed latencies, they need delays but not scoreboards.
// Decoupled instructions are ones with variable latencies, need scoreboards but not delays.
// There are also redirected instructions which depending on the SM, can be
// coupled or decoupled so both delays and scoreboards needs to be provided.
//

#[allow(dead_code)]
#[derive(Debug)]
enum RegLatencySM75 {
    CoupledDisp64,
    CoupledDisp,
    CoupledAlu,
    CoupledFMA,
    IMADLo,
    IMADWideAB, // readers only
    IMADWideLower,
    IMADWideUpper,
    RedirectedFP64,
    RedirectedFP16,
    RedirectedHMMA_884_F16,
    RedirectedHMMA_884_F32,
    RedirectedHMMA_1688,
    RedirectedHMMA_16816,
    IMMA,
    Decoupled,
    DecoupledOther, //reads only
    BMov,
    GuardPredicate,
}

macro_rules! pred {
    ($has_pred: expr, $b: literal, $p: literal) => {
        if $has_pred {
            $b + $p
        } else {
            $p
        }
    }
}

impl RegLatencySM75 {
    fn op_category(op: &Op, reader: bool, op_reg_idx: usize) -> RegLatencySM75 {
        match op {
            // this will need updating if imad grows support for input predicates
            Op::IMad(_) | Op::IMul(_) => RegLatencySM75::IMADLo,
            Op::IMad64(_) => if reader {
                match op_reg_idx {
                    0 | 1 => RegLatencySM75::IMADWideAB,
                    2 => RegLatencySM75::IMADWideLower, // vs upper C operand - work it out
                    _ => { panic!("Illegal field in imadwide") }
                }
            } else {
                RegLatencySM75::IMADWideUpper // as above this needs more work
            }

            Op::PopC(_) => RegLatencySM75::Decoupled,
            Op::IAdd3(_)
            | Op::IAdd3X(_) => RegLatencySM75::CoupledAlu,

            Op::BMsk(_) => RegLatencySM75::CoupledAlu,
            // Sgxt => RegLatencySM75::CoupledAlu,
            Op::Lop3(_) => RegLatencySM75::CoupledAlu,
            Op::Flo(_) => RegLatencySM75::Decoupled,
            Op::ISetP(_) => RegLatencySM75::CoupledAlu,
            Op::IAbs(_) => RegLatencySM75::CoupledAlu,
            Op::Lea(_) => RegLatencySM75::CoupledAlu,
            Op::LeaX(_) => RegLatencySM75::CoupledAlu,
            Op::IMnMx(_) => RegLatencySM75::CoupledAlu,
            Op::I2I(_) => RegLatencySM75::CoupledAlu,
            // I2IP => RegLatencySM75::CoupledAlu
            Op::Shf(_) =>  RegLatencySM75::CoupledAlu,

            Op::FFma(_) => RegLatencySM75::CoupledFMA,
            Op::FAdd(_) => RegLatencySM75::CoupledFMA,
            Op::FMul(_) => RegLatencySM75::CoupledFMA,
            Op::FMnMx(_) => RegLatencySM75::CoupledAlu,
            Op::FSwzAdd(_) => RegLatencySM75::CoupledFMA,
            Op::FSet(_) => RegLatencySM75::CoupledAlu,
            // FSel => RegLatencySM75::CoupledAlu,
            Op::FSetP(_) => RegLatencySM75::CoupledAlu,
            // FChk => RegLatencySM75::Decoupled,

            Op::DAdd(_)
            | Op::DFma(_)
            | Op::DMul(_)
            | Op::DSetP(_) => RegLatencySM75::RedirectedFP64,

            Op::DMnMx(_) => RegLatencySM75::RedirectedFP64, // not in docs

            Op::HAdd2(_)
            | Op::HFma2(_)
            | Op::HMul2(_)
            | Op::HSet2(_)
            | Op::HSetP2(_) => RegLatencySM75::RedirectedFP16,

            Op::HMnMx2(_) => RegLatencySM75::RedirectedFP16, // not in docs
            // let in for documentation purposes
//            Op::Hmma(h) => {
//              match h.mat_size {
//                  HmmaSize::M16N8K4 => match h.dst_type {
//                      FloatType::F16 => RegLatencySM75::RedirectedHMMA_884_F16,
//                      _ => RegLatencySM75::RedirectedHMMA_884_F32
//                  }
//                  HmmaSize::M16N8K8 => RegLatencySM75::RedirectedHMMA_1688,
//                  HmmaSize::M16N8K16 => RegLatencySM75::RedirectedHMMA_16816,
//                }
//           }

            Op::Ipa(_) => RegLatencySM75::Decoupled,
            Op::MuFu(_) => RegLatencySM75::Decoupled,

            // Conversion functions all decoupled
            Op::F2F(_) => RegLatencySM75::Decoupled,
            Op::F2I(_) => RegLatencySM75::Decoupled,
            Op::I2F(_) => RegLatencySM75::Decoupled,
            Op::FRnd(_) => RegLatencySM75::Decoupled,
            Op::AL2P(_) => RegLatencySM75::Decoupled,

            Op::Mov(_) => RegLatencySM75::CoupledAlu,
            Op::Sel(_) => RegLatencySM75::CoupledAlu,
            Op::BRev(_) => RegLatencySM75::Decoupled,
            // P2R => RegLatencySM75::CoupledAlu,
            // R2P => RegLatencySM75::CoupledAlu,
            Op::PLop3(_) => RegLatencySM75::CoupledAlu,
            Op::Prmt(_) => RegLatencySM75::CoupledAlu,
            Op::Nop(_) => RegLatencySM75::CoupledDisp,
            Op::Vote(_) => RegLatencySM75::CoupledDisp,
            Op::S2R(_) => RegLatencySM75::Decoupled,
            // S2UR  => RegLatencySM75::Decoupled,
            Op::R2UR(_) => { if reader { RegLatencySM75::Decoupled } else { panic!("Illegal R2UR"); } }
            Op::CS2R(cs2r) => if cs2r.dst.as_reg().unwrap().comps() == 2 { RegLatencySM75::CoupledDisp64 } else { RegLatencySM75::CoupledAlu },
            // B2R => RegLatencySM75::Decoupled,
            // LEPC => RegLatencySM75::CoupledDisp64
            Op::BMov(bmov) => match bmov.dst {
                Dst::Reg(reg) => if reg.is_gpr() { RegLatencySM75::BMov } else { RegLatencySM75::Decoupled },
                _ => RegLatencySM75::Decoupled
            },
            // RPCMOV.32 => RegLatencySM75::CoupledAlu,
            // RPCMOV.64 => RegLatencySM75::CoupledDisp64
            // PMTRIG => RegLatencySM75::CoupledDisp64
            // CSMTEST =>  RegLatencySM75::CoupledAlu,
            Op::Bar(_) => RegLatencySM75::Decoupled,
            // Remove when Imma added
            //Op::Imma(_) => RegLatencySM75::IMMA,

            Op::IDp4(_) => RegLatencySM75::CoupledFMA,
            Op::BClear(_) => RegLatencySM75::Decoupled,
            Op::Bra(_) => RegLatencySM75::Decoupled,
            Op::BSSy(_) => RegLatencySM75::Decoupled,
            Op::Kill(_) => RegLatencySM75::Decoupled,
            Op::Exit(_) => RegLatencySM75::Decoupled,
            Op::BSync(_) => RegLatencySM75::Decoupled,
            Op::Tex(_) => RegLatencySM75::Decoupled,
            Op::Tld(_) => RegLatencySM75::Decoupled,
            Op::Tld4(_) => RegLatencySM75::Decoupled,
            Op::Tmml(_) => RegLatencySM75::Decoupled,
            Op::Txd(_) => RegLatencySM75::Decoupled,
            Op::Txq(_) => RegLatencySM75::Decoupled,
            Op::Ldc(_) => RegLatencySM75::Decoupled,
            Op::ALd(_) => RegLatencySM75::Decoupled,
            Op::ASt(_) => RegLatencySM75::Decoupled,
            Op::Out(_) => RegLatencySM75::Decoupled,
            Op::OutFinal(_) => RegLatencySM75::Decoupled,
            Op::Ld(_) => RegLatencySM75::Decoupled,
            Op::St(_) => RegLatencySM75::Decoupled,
            Op::Atom(_) => RegLatencySM75::Decoupled,
            //CCtl.i,c are coupled
            Op::CCtl(_) => RegLatencySM75::DecoupledOther,
            Op::MemBar(_) => RegLatencySM75::Decoupled,
            Op::SuLd(_) => RegLatencySM75::Decoupled,
            Op::SuSt(_) => RegLatencySM75::Decoupled,
            Op::SuAtom(_) => RegLatencySM75::Decoupled,
            Op::PixLd(_) => RegLatencySM75::Decoupled,
            Op::Isberd(_) => RegLatencySM75::Decoupled,
            Op::LdTram(_) => RegLatencySM75::Decoupled,
            Op::Shfl(_) => RegLatencySM75::Decoupled,
            //Op::LdSm(_) => RegLatencySM75::Decoupled
            x => { panic!("Illegal instuction in reg category {}", x); }
        }
    }

    pub fn read_after_write(writer: RegLatencySM75,
                            reader: RegLatencySM75) -> u32 {
        match writer {
            RegLatencySM75::IMADWideAB |
            RegLatencySM75::DecoupledOther => {
                panic!("Illegal IMADWideAB for writer");
            },
            _ => {}
        }

        match reader {
            RegLatencySM75::CoupledDisp64 |
            RegLatencySM75::CoupledDisp |
            RegLatencySM75::CoupledAlu => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 6,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 4,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 5,
                    RegLatencySM75::IMADWideLower => 3,
                    RegLatencySM75::IMADWideUpper => 5,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10,
                    _ => 1
                }
            },
            RegLatencySM75::CoupledFMA |
            RegLatencySM75::IMADLo => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 6,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 5,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 4,
                    RegLatencySM75::IMADWideLower => 2,
                    RegLatencySM75::IMADWideUpper => 4,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10,
                    _ => 1
                }
            }
            RegLatencySM75::IMADWideAB => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 6,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 5,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 4,
                    RegLatencySM75::IMADWideLower => 4,
                    RegLatencySM75::IMADWideUpper => 6,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10,
                    _ => 1
                }
            }
            RegLatencySM75::IMADWideLower |
            RegLatencySM75::IMADWideUpper => {
                match reader {
                    RegLatencySM75::IMADWideLower => {
                        match writer {
                            RegLatencySM75::CoupledDisp64 => 6,
                            RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 5,
                            RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 4,
                            RegLatencySM75::IMADWideLower => 2,
                            RegLatencySM75::IMADWideUpper => 2,
                            RegLatencySM75::RedirectedFP64 => 9,
                            RegLatencySM75::RedirectedFP16 => 8,
                            RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                            RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                            RegLatencySM75::RedirectedHMMA_1688 => 14,
                            RegLatencySM75::RedirectedHMMA_16816 => 22,
                            RegLatencySM75::IMMA => 10,
                            _ => 1
                        }
                    }
                    RegLatencySM75::IMADWideUpper => {
                        match writer {
                            RegLatencySM75::CoupledDisp64 => 4,
                            RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 3,
                            RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 2,
                            RegLatencySM75::IMADWideLower => 2,
                            RegLatencySM75::IMADWideUpper => 2,
                            RegLatencySM75::RedirectedFP64 => 7,
                            RegLatencySM75::RedirectedFP16 => 6,
                            RegLatencySM75::RedirectedHMMA_884_F16 => 11,
                            RegLatencySM75::RedirectedHMMA_884_F32 => 8,
                            RegLatencySM75::RedirectedHMMA_1688 => 12,
                            RegLatencySM75::RedirectedHMMA_16816 => 20,
                            RegLatencySM75::IMMA => 8,
                            _ => 1
                        }
                    }
                    _ => { panic!("Illegal IMAD field"); }
                }
            }
            RegLatencySM75::RedirectedFP64 => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 6,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 6,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 6,
                    RegLatencySM75::IMADWideLower => 6,
                    RegLatencySM75::IMADWideUpper => 6,
                    RegLatencySM75::RedirectedFP64 => 8,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10,
                    _ => 1
                }
            }
            RegLatencySM75::RedirectedFP16 => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 6,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 6,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 6,
                    RegLatencySM75::IMADWideLower => 6,
                    RegLatencySM75::IMADWideUpper => 6,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 6,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10,
                    _ => 1
                }
            }
            RegLatencySM75::RedirectedHMMA_884_F16 |
            RegLatencySM75::RedirectedHMMA_884_F32 |
            RegLatencySM75::RedirectedHMMA_1688    |
            RegLatencySM75::RedirectedHMMA_16816 |
            RegLatencySM75::Decoupled => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 6,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 6,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 6,
                    RegLatencySM75::IMADWideLower => 6,
                    RegLatencySM75::IMADWideUpper => 6,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,//4 for back to back FMA for 884
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,//4 for back o back FMA for 884
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10,
                    _ => 1
                }
            }
            RegLatencySM75::IMMA |
            RegLatencySM75::DecoupledOther => {
                match writer {
                    RegLatencySM75::CoupledDisp64 => 8,
                    RegLatencySM75::CoupledAlu | RegLatencySM75::CoupledDisp => 8,
                    RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => 8,
                    RegLatencySM75::IMADWideLower => 8,
                    RegLatencySM75::IMADWideUpper => 8,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 13,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 10,
                    RegLatencySM75::RedirectedHMMA_1688 => 14,
                    RegLatencySM75::RedirectedHMMA_16816 => 22,
                    RegLatencySM75::IMMA => 10, // 4 for back to back IMMA
                    _ => 1
                }
            }
            RegLatencySM75::BMov |
            RegLatencySM75::GuardPredicate => {
                panic!("Not a RAW category")
            }
        }
    }

    fn write_after_write(writer1: RegLatencySM75,
                         writer2: RegLatencySM75,
                         has_pred: bool) -> u32 {
        match writer1 {
            RegLatencySM75::IMADWideAB |
            RegLatencySM75::DecoupledOther => {
                panic!("Illegal reg latency for writer");
            },
            _ => {}
        }
        match writer2 {
            RegLatencySM75::CoupledDisp64 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 1,
                    RegLatencySM75::RedirectedFP64 => 4,
                    RegLatencySM75::RedirectedFP16 => 3,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 8,
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedHMMA_1688 => 9,
                    RegLatencySM75::RedirectedHMMA_16816 => 17,
                    RegLatencySM75::IMMA => 5,
                    _ => 1,
                }
            },
            RegLatencySM75::CoupledDisp |
            RegLatencySM75::CoupledAlu => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 => 2,
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 1,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 1),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 1),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 8, 1),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 5, 1),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 9, 1),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 17, 1),
                    RegLatencySM75::IMMA => pred!(has_pred, 5, 1),
                    _ => 1,
                }
            },
            RegLatencySM75::CoupledFMA | RegLatencySM75::IMADLo => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 => 2,
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower => 1,
                    RegLatencySM75::IMADWideUpper => pred!(has_pred, 1, 1),
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 1),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 1),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 8, 1),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 5, 1),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 9, 1),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 17, 1),
                    RegLatencySM75::IMMA => pred!(has_pred, 5, 1),
                    _ => 1,
                }
            }
            RegLatencySM75::IMADWideLower => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 => pred!(has_pred, 2, 2),
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu => pred!(has_pred, 2, 1),
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo => pred!(has_pred, 1, 1),
                    RegLatencySM75::IMADWideLower => 1,
                    RegLatencySM75::IMADWideUpper => 1,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 3),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 3),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 8, 3),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 5, 3),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 9, 3),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 17, 3),
                    RegLatencySM75::IMMA => pred!(has_pred, 5, 3),
                    _ => 1,
                }
            },
            RegLatencySM75::IMADWideUpper => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 => pred!(has_pred, 1, 1),
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 1,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 1),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 1),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 8, 1),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 5, 1),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 9, 1),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 17, 1),
                    RegLatencySM75::IMMA => pred!(has_pred, 5, 1),
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedFP64 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 2,
                    RegLatencySM75::RedirectedFP64 => 1,
                    RegLatencySM75::RedirectedFP16 => 2,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 5,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 2,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::IMMA => 2,
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedFP16 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 2,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 1, 1),
                    RegLatencySM75::RedirectedFP16 => 1,
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 6, 1),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 3, 1),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 7, 1),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 15, 1),
                    RegLatencySM75::IMMA => pred!(has_pred, 3, 1),
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedHMMA_884_F16 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 2,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 3, 2),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedHMMA_884_F16 => 1,
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 2, 4),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 6, 4),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 16, 2),
                    RegLatencySM75::IMMA => pred!(has_pred, 2, 4),
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedHMMA_884_F32 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => 2,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 3, 2),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 4, 5),
                    RegLatencySM75::RedirectedHMMA_884_F32 => 1,
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 6, 4),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 16, 2),
                    RegLatencySM75::IMMA => pred!(has_pred, 2, 4),
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedHMMA_1688 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::RedirectedFP64 |
                    RegLatencySM75::RedirectedFP16 => 2,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 4,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 2,
                    RegLatencySM75::RedirectedHMMA_1688 => 1,
                    RegLatencySM75::RedirectedHMMA_16816 => 16,
                    RegLatencySM75::IMMA => 2,
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedHMMA_16816 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::RedirectedFP64 |
                    RegLatencySM75::RedirectedFP16 => 2,
                    RegLatencySM75::RedirectedHMMA_884_F16 => 4,
                    RegLatencySM75::RedirectedHMMA_884_F32 => 2,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 1,
                    RegLatencySM75::IMMA => 2,
                    _ => 1,
                }
            },
            RegLatencySM75::IMMA => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 2, 3),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedHMMA_884_F16 => pred!(has_pred, 2, 7),
                    RegLatencySM75::RedirectedHMMA_884_F32 => pred!(has_pred, 2, 4),
                    RegLatencySM75::RedirectedHMMA_1688 => pred!(has_pred, 6, 4),
                    RegLatencySM75::RedirectedHMMA_16816 => pred!(has_pred, 14, 4),
                    RegLatencySM75::IMMA => 1,
                    _ => 1,
                }
            },
            RegLatencySM75::Decoupled => {
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::RedirectedFP64 |
                    RegLatencySM75::RedirectedFP16 |
                    RegLatencySM75::RedirectedHMMA_884_F16 |
                    RegLatencySM75::RedirectedHMMA_884_F32 |
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::IMMA => 2,
                    _ => 1,
                }
            },
            RegLatencySM75::BMov => {// BMOV Writing to RF?
                match writer1 {
                    RegLatencySM75::CoupledDisp64 |
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::RedirectedFP64 |
                    RegLatencySM75::RedirectedFP16 |
                    RegLatencySM75::RedirectedHMMA_884_F16 |
                    RegLatencySM75::RedirectedHMMA_884_F32 |
                    RegLatencySM75::RedirectedHMMA_1688 => 9,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::IMMA => 9,
                    _ => 1,
                }
            },
            RegLatencySM75::IMADWideAB |
            RegLatencySM75::DecoupledOther | RegLatencySM75::GuardPredicate => {
                panic!("Not a WAW category")
            }
        }
    }

    fn write_after_read(reader: RegLatencySM75,
                        writer: RegLatencySM75) -> u32 {
        match writer {
            RegLatencySM75::CoupledDisp64 |
            RegLatencySM75::CoupledDisp |
            RegLatencySM75::CoupledAlu |
            RegLatencySM75::CoupledFMA |
            RegLatencySM75::IMADLo |
            RegLatencySM75::IMADWideLower |
            RegLatencySM75::IMADWideUpper => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 5,
                    RegLatencySM75::RedirectedHMMA_16816 => 13,
                    _ => 1,
                }
            },
            RegLatencySM75::RedirectedFP64 => {
                match reader {
                    RegLatencySM75::RedirectedFP64 => 1,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::RedirectedFP16 => {
                match reader {
                    RegLatencySM75::RedirectedFP16 => 1,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::RedirectedHMMA_884_F16 => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_884_F16 => 1,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::RedirectedHMMA_884_F32 => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_884_F32 => 1,
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::RedirectedHMMA_1688 => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 1,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::RedirectedHMMA_16816 => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 1,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::IMMA => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 6,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::IMMA => 1,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::Decoupled => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 2,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 2,
                }
            },
            RegLatencySM75::BMov => {
                match reader {
                    RegLatencySM75::RedirectedHMMA_1688 => 9,
                    RegLatencySM75::RedirectedHMMA_16816 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => 9,
                }
            },
            RegLatencySM75::IMADWideAB |
            RegLatencySM75::DecoupledOther | RegLatencySM75::GuardPredicate => {
                panic!("Illegal in WAR");
            }
        }
    }

    fn pred_read_after_write(writer: RegLatencySM75,
                             reader: RegLatencySM75) -> u32 {
        match reader {
            RegLatencySM75::CoupledDisp => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 12,
                    RegLatencySM75::RedirectedFP64 => 15,
                    RegLatencySM75::RedirectedFP16 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::CoupledAlu => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu => 4,
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 5,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::CoupledFMA |
            RegLatencySM75::IMADLo => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu => 5,
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 4,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::IMADWideUpper |
            RegLatencySM75::IMADWideLower => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu => 5,
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo => 4,
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 2,
                    RegLatencySM75::RedirectedFP64 => 9,
                    RegLatencySM75::RedirectedFP16 => 8,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::RedirectedFP64 => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 12,
                    RegLatencySM75::RedirectedFP64 => 8,
                    RegLatencySM75::RedirectedFP16 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::RedirectedFP16 => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 12,
                    RegLatencySM75::RedirectedFP64 => 15,
                    RegLatencySM75::RedirectedFP16 => 6,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::Decoupled |
            RegLatencySM75::GuardPredicate => {
                match writer {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 12,
                    RegLatencySM75::RedirectedFP64 => 15,
                    RegLatencySM75::RedirectedFP16 => 14,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            _ => { panic!("Illegal reader in reg predicate"); }
        }
    }

    fn pred_write_after_write(writer1: RegLatencySM75,
                              writer2: RegLatencySM75,
                              has_pred: bool) -> u32 {
        match writer2 {
            RegLatencySM75::CoupledDisp |
            RegLatencySM75::CoupledAlu |
            RegLatencySM75::CoupledFMA |
            RegLatencySM75::IMADLo => {
                match writer1 {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 1,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 1),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 1),
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::IMADWideUpper |
            RegLatencySM75::IMADWideLower => {
                match writer1 {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu => pred!(has_pred, 1, 2),
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo => pred!(has_pred, 1, 1),
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => 1,
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 4, 3),
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 3, 3),
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::RedirectedFP64 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => pred!(has_pred, 2, 2),
                    RegLatencySM75::RedirectedFP64 => 1,
                    RegLatencySM75::RedirectedFP16 => pred!(has_pred, 2, 4),
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::RedirectedFP16 => {
                match writer1 {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower => pred!(has_pred, 2, 4),
                    RegLatencySM75::RedirectedFP64 => pred!(has_pred, 2, 7),
                    RegLatencySM75::RedirectedFP16 => 1,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            RegLatencySM75::Decoupled => {
                match writer1 {
                    RegLatencySM75::CoupledDisp |
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::RedirectedFP64 |
                    RegLatencySM75::RedirectedFP16 => 2,
                    RegLatencySM75::Decoupled => 1,
                    _ => { panic!("Illegal RAW in Predicate"); }
                }
            }
            _ => {
                panic!("Illegal WAR category in Predicates");
            }
        }
    }

    fn pred_write_after_read(reader: RegLatencySM75,
                             writer: RegLatencySM75) -> u32 {
        match writer {
            RegLatencySM75::CoupledDisp |
            RegLatencySM75::CoupledAlu |
            RegLatencySM75::CoupledFMA |
            RegLatencySM75::IMADLo |
            RegLatencySM75::IMADWideUpper |
            RegLatencySM75::IMADWideLower => { 1 },
            RegLatencySM75::RedirectedFP64 => {
                match reader {
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::RedirectedFP16 => 2,
                    _ => 1,
                }
            }
            RegLatencySM75::RedirectedFP16 => {
                match reader {
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::RedirectedFP64 => 2,
                    _ => 1,
                }
            }
            RegLatencySM75::Decoupled => {
                match reader {
                    RegLatencySM75::CoupledAlu |
                    RegLatencySM75::CoupledFMA |
                    RegLatencySM75::IMADLo |
                    RegLatencySM75::IMADWideUpper |
                    RegLatencySM75::IMADWideLower |
                    RegLatencySM75::RedirectedFP16 |
                    RegLatencySM75::RedirectedFP64 => 2,
                    _ => 1,
                }
            }
            _ => {
                panic!("Illegal WAR category in Predicates");
            }
        }
    }
}

#[allow(non_camel_case_types)]
#[allow(dead_code)]
#[derive(Debug)]
enum URegLatencySM75 {
    Udp,
    VectorCoupled,
    VectorDecoupled,
    Uldc,
    Umov,
    VectorCoupledBindless,
    VectorDecoupledBindless,
    VoteU,
    GuardPredicate,
    R2UR,
}

impl URegLatencySM75 {
    fn op_category(op: &Op, reader: bool, op_reg_idx: usize) -> URegLatencySM75 {
        // is this using a bindless cbuf as a src register.
        // this decides between the category types for readers.
        let bindless = reader && op.srcs_as_slice()[op_reg_idx].is_bindless_cbuf();

        let vcoupled = if bindless { URegLatencySM75::VectorCoupledBindless } else { URegLatencySM75::VectorCoupled };
        let vdecoupled = if bindless { URegLatencySM75::VectorDecoupledBindless } else { URegLatencySM75::VectorDecoupled };

        // if this is a reader from a ureg, it could be a U* instruction or a regular instruction.
        let uniform_op = op.is_uniform();

        let vcoupled = if uniform_op { URegLatencySM75::Udp } else { vcoupled };
        let vdecoupled = if uniform_op { URegLatencySM75::Udp } else { vdecoupled };

        match op {
            Op::BMsk(_) => vcoupled,
            Op::BRev(_) => vcoupled,
            // uclea?
            Op::Flo(_) => vdecoupled,
            Op::IAdd3(_) |
            Op::IAdd3X(_) => vcoupled,
            Op::IAbs(_) => vcoupled,
            Op::IMnMx(_) => vcoupled,
            Op::IMad(_) => vcoupled,

            Op::IMad64(_) => vcoupled,
            Op::ISetP(_) => vcoupled,
            Op::Ldc(_) => if uniform_op { URegLatencySM75::Uldc } else { vdecoupled },
            Op::Lea(_) => vcoupled,
            Op::LeaX(_) => vcoupled,
            Op::Lop2(_) |
            Op::Lop3(_) => vcoupled,

            Op::MuFu(_) => vdecoupled,
            Op::Mov(_) => if uniform_op { URegLatencySM75::Umov } else { vcoupled },

            // mov32i => URegLatency::Uldc,
            // p2ur => URegLatencySM75::Udp,
            Op::PLop3(_) => vcoupled,
            Op::PopC(_) => vdecoupled,
            Op::Prmt(_) => vcoupled,
            Op::PSetP(_) => vcoupled,
            // UR2UP
            Op::Sel(_) => vcoupled,
            // SGXT
            Op::Shf(_) => vcoupled,
            Op::Shfl(_) => vdecoupled,

            Op::I2F(_) => vdecoupled,
            Op::F2I(_) => vdecoupled,
            Op::F2F(_) => vdecoupled,
            Op::R2UR(_) => if !reader { URegLatencySM75::R2UR } else { panic!("Illegal R2UR in ureg"); }
            Op::Vote(_) => URegLatencySM75::VoteU,

            Op::FRnd(_) => vdecoupled,
            Op::FAdd(_) |
            Op::FMul(_) |
            Op::FFma(_) |
            Op::FSetP(_) |
            Op::FMnMx(_) |
            Op::HAdd2(_) |
            Op::HMul2(_) |
            Op::HSet2(_) |
            Op::HFma2(_) |
            Op::HSetP2(_) => vcoupled,
            Op::DMul(_) |
            Op::DFma(_) |
            Op::DAdd(_) |
            Op::DSetP(_) => vdecoupled,
            _ => { panic!("Illegal instuction in ureg category {}", op); }
        }
    }

    fn read_after_write(writer: URegLatencySM75,
                        reader: URegLatencySM75) -> u32 {
        match reader {
            URegLatencySM75::Udp => {
                match writer {
                    URegLatencySM75::Udp => 4,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 2,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM75::VectorCoupled => {
                match writer {
                    URegLatencySM75::Udp => 6,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 2,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM75::VectorDecoupled => {
                match writer {
                    URegLatencySM75::Udp => 9,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 2,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM75::Uldc |
            URegLatencySM75::VectorCoupledBindless |
            URegLatencySM75::VectorDecoupledBindless => {
                match writer {
                    URegLatencySM75::Udp => 12,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 5,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM75::Umov => {
                match writer {
                    URegLatencySM75::Udp => 7,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 2,
                    _ => { panic!("Illegal writer in raw ureg latency") },
                }
            }
            _ => { panic!("Illegal read in ureg raw latency") },
        }
    }

    fn write_after_write(writer1: URegLatencySM75,
                         writer2: URegLatencySM75,
                         has_pred: bool) -> u32 {
        match writer2 {
            URegLatencySM75::Udp => {
                match writer1 {
                    URegLatencySM75::Udp => 1,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 1,
                    _ => { panic!("Illegal writer in ureg waw latency") },
                }
            },
            URegLatencySM75::R2UR => {
                match writer1 {
                    URegLatencySM75::Udp => pred!(has_pred, 4, 6),
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 4,
                    _ => { panic!("Illegal writer in ureg waw latency") },
                }
            },
            URegLatencySM75::Uldc |
            URegLatencySM75::VoteU |
            URegLatencySM75::Umov => {
                match writer1 {
                    URegLatencySM75::Udp => 7,
                    URegLatencySM75::R2UR => 2,
                    URegLatencySM75::Uldc |
                    URegLatencySM75::VoteU |
                    URegLatencySM75::Umov => 1,
                    _ => { panic!("Illegal writer in ureg waw latency") },
                }
            }
            _ => { panic!("Illegal writer in ureg waw latency") },
        }
    }

    fn write_after_read(reader: URegLatencySM75,
                        writer: URegLatencySM75) -> u32 {
        match writer {
            URegLatencySM75::Udp => 1,
            URegLatencySM75::R2UR => 1,
            URegLatencySM75::Uldc |
            URegLatencySM75::VoteU |
            URegLatencySM75::Umov => {
                match reader {
                    URegLatencySM75::Udp => 3,
                    _ => 1,
                }
            }
            _ => { panic!("Illegal writer in ureg war latency") }
        }
    }

    fn pred_read_after_write(writer: URegLatencySM75,
                             reader: URegLatencySM75) -> u32 {
        match reader {
            URegLatencySM75::Udp => {
                match writer {
                    URegLatencySM75::Udp => 4,
                    URegLatencySM75::VoteU => 1,
                    _ => { panic!("Illegal writer in upred raw latency") }
                }
            }
            URegLatencySM75::VectorCoupled => {
                match writer {
                    URegLatencySM75::Udp => 6,
                    URegLatencySM75::VoteU => 1,
                    _ => { panic!("Illegal writer in upred raw latency") }
                }
            }
            URegLatencySM75::GuardPredicate => {
                match writer {
                    URegLatencySM75::Udp => 11,
                    URegLatencySM75::VoteU => 5,
                    _ => { panic!("Illegal writer in upred raw latency") }
                }
            }
            _ => { panic!("Illegal reader in upred raw latency") }
        }
    }

    fn pred_write_after_write(writer1: URegLatencySM75,
                              writer2: URegLatencySM75) -> u32 {
        match writer2 {
            URegLatencySM75::Udp => 1,
            URegLatencySM75::VoteU => {
                match writer1 {
                    URegLatencySM75::Udp => 7,
                    URegLatencySM75::VoteU => 1,
                    _ => { panic!("Illegal writer1 in upred raw latency") }
                }
            }
            _ => { panic!("Illegal writer2 in upred raw latency") }
        }
    }

    fn pred_write_after_read(reader: URegLatencySM75,
                             writer: URegLatencySM75) -> u32 {
        match writer {
            URegLatencySM75::Udp => 1,
            URegLatencySM75::VoteU => {
                match reader {
                    URegLatencySM75::Udp => 2,
                    _ => 1,
                }
            }
            _ => { panic!("Illegal writer2 in upred raw latency") }
        }
    }
}

pub struct SM75Latency {}

impl SM75Latency {
    pub fn needs_scoreboards(op: &Op) -> bool {
        if op.is_uniform() {
            match URegLatencySM75::op_category(op, false, 0) {
                URegLatencySM75::R2UR => true,
                _ => false,
            }
        } else {
            match RegLatencySM75::op_category(op, true, 0) {
                RegLatencySM75::RedirectedFP64 |
                // We don't think fp16 needs scoreboarding on any known hw
                // Put this back if we figure out it does.
                //RegLatencySM75::RedirectedFP16 |
                RegLatencySM75::RedirectedHMMA_884_F16 |
                RegLatencySM75::RedirectedHMMA_884_F32 |
                RegLatencySM75::RedirectedHMMA_1688 |
                RegLatencySM75::RedirectedHMMA_16816 |
                RegLatencySM75::IMMA |
                RegLatencySM75::Decoupled => true,
                _ => false
            }
        }
    }

    pub fn raw(write: &Op, dst_idx: usize,
               read: &Op, src_idx: usize) -> u32 {
        let dst_file = match write.dsts_as_slice()[dst_idx] {
            Dst::None => return 0,
            Dst::SSA(vec) => vec.file().unwrap(),
            Dst::Reg(reg) => reg.file(),
        };

        match dst_file {
            RegFile::GPR => {
                let write_latency = RegLatencySM75::op_category(write, false, dst_idx);
                let read_latency = RegLatencySM75::op_category(read, true, src_idx);
                return RegLatencySM75::read_after_write(write_latency,
                                                        read_latency);
            },
            RegFile::UGPR => {
                let write_latency = URegLatencySM75::op_category(write, false, dst_idx);
                let read_latency = URegLatencySM75::op_category(read, true, src_idx);
                return URegLatencySM75::read_after_write(write_latency,
                                                         read_latency);
            },
            RegFile::Pred => {
                let write_latency = RegLatencySM75::op_category(write, false, dst_idx);
                let read_latency = RegLatencySM75::op_category(read, true, src_idx);
                return RegLatencySM75::pred_read_after_write(write_latency,
                                                             read_latency);
            },
            RegFile::UPred => {
                let write_latency = URegLatencySM75::op_category(write, false, dst_idx);
                let read_latency = URegLatencySM75::op_category(read, true, src_idx);
                return URegLatencySM75::pred_read_after_write(write_latency,
                                                              read_latency);
            },
            RegFile::Carry => 6,
            _ => panic!("Not a register"),
        }
    }

    pub fn war(read: &Op, src_idx: usize,
               write: &Op, dst_idx: usize) -> u32 {
        let dst_file = match write.dsts_as_slice()[dst_idx] {
            Dst::None => return 0,
            Dst::SSA(vec) => vec.file().unwrap(),
            Dst::Reg(reg) => reg.file(),
        };

        match dst_file {
            RegFile::GPR => {
                let write_latency = RegLatencySM75::op_category(write, false, dst_idx);
                let read_latency = RegLatencySM75::op_category(read, true, src_idx);
                return RegLatencySM75::write_after_read(read_latency,
                                                        write_latency);
            },
            RegFile::UGPR => {
                let write_latency = URegLatencySM75::op_category(write, false, dst_idx);
                let read_latency = URegLatencySM75::op_category(read, true, src_idx);
                return URegLatencySM75::write_after_read(read_latency,
                                                         write_latency);
            },
            RegFile::Pred => {
                let write_latency = RegLatencySM75::op_category(write, false, dst_idx);
                let read_latency = RegLatencySM75::op_category(read, false, src_idx);
                return RegLatencySM75::pred_write_after_read(read_latency,
                                                             write_latency);
            },
            RegFile::UPred => {
                let write_latency = URegLatencySM75::op_category(write, false, dst_idx);
                let read_latency = URegLatencySM75::op_category(read, true, src_idx);
                return URegLatencySM75::pred_write_after_read(read_latency,
                                                              write_latency);
            },
            RegFile::Carry => 6,
            _ => panic!("Not a register"),
        }
    }

    pub fn waw(a: &Op, a_dst_idx: usize,
               b: &Op, b_dst_idx: usize,
               a_op_pred: bool) -> u32 {
        let dst_file = match a.dsts_as_slice()[a_dst_idx] {
            Dst::None => return 0,
            Dst::SSA(vec) => vec.file().unwrap(),
            Dst::Reg(reg) => reg.file(),
        };

        match dst_file {
            RegFile::GPR => {
                let write1_latency = RegLatencySM75::op_category(a, false, a_dst_idx);
                let write2_latency = RegLatencySM75::op_category(b, false, b_dst_idx);
                return RegLatencySM75::write_after_write(write1_latency,
                                                         write2_latency, a_op_pred);
            },
            RegFile::UGPR => {
                let write1_latency = URegLatencySM75::op_category(a, false, a_dst_idx);
                let write2_latency = URegLatencySM75::op_category(b, false, b_dst_idx);
                return URegLatencySM75::write_after_write(write1_latency,
                                                          write2_latency, a_op_pred);
            },
            RegFile::Pred => {
                let write1_latency = RegLatencySM75::op_category(a, false, a_dst_idx);
                let write2_latency = RegLatencySM75::op_category(b, false, b_dst_idx);
                return RegLatencySM75::pred_write_after_write(write1_latency,
                                                              write2_latency, a_op_pred);
            },
            RegFile::UPred => {
                let write1_latency = URegLatencySM75::op_category(a, false, a_dst_idx);
                let write2_latency = URegLatencySM75::op_category(b, false, b_dst_idx);
                return URegLatencySM75::pred_write_after_write(write1_latency,
                                                               write2_latency);
            },
            RegFile::Carry => 6,
            _ => panic!("Not a register"),
        }
    }

    pub fn paw(write: &Op, dst_idx: usize) -> u32 {
        let dst_file = match write.dsts_as_slice()[dst_idx] {
            Dst::None => return 0,
            Dst::SSA(vec) => vec.file().unwrap(),
            Dst::Reg(reg) => reg.file(),
        };

        match dst_file {
            RegFile::Pred => {
                let write_latency = RegLatencySM75::op_category(write, false, dst_idx);
                return RegLatencySM75::pred_read_after_write(write_latency,
                                                             RegLatencySM75::GuardPredicate);
            },
            RegFile::UPred => {
                let write_latency = URegLatencySM75::op_category(write, false, dst_idx);
                return URegLatencySM75::pred_read_after_write(write_latency,
                                                              URegLatencySM75::GuardPredicate);
            }
            _ => { panic!("Incorrect register file in paw_latencny") }
        }
    }
}
