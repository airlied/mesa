#![allow(non_camel_case_types)]

use crate::ir::HasRegFile;
use crate::ir::Dst;
use crate::ir::Op;
use crate::ir::IsUniform;
use crate::ir::DstsAsSlice;
use crate::ir::SrcsAsSlice;
use crate::ir::RegFile;
use crate::ir::HmmaSize;
use crate::ir::FloatType;

// This contains the register scheduling information provided by NVIDIA.

// Coupled instructions are ones with fixed latencies, they need delays but not scoreboards.
// Decoupled instructions are ones with variable latencies, need scoreboards but not delays.
// There are also redirected instructions which depending on the SM, can be
// coupled or decoupled so both delays and scoreboards needs to be provided.
//

macro_rules! pred {
    ($has_pred: expr, $b: literal, $p: literal) => {
        if $has_pred {
            $b + $p
        } else {
            $p
        }
    }
}

#[derive(Debug)]
enum RegLatencySM80 {
    CoupledAlu,
    CoupledDisp64,
    CoupledFMA,
    IMADWideReadAB,
    IMADWideReadCL,
    IMADWideReadCH,
    IMADWideWriteDL,
    IMADWideWriteDH,
    FP16,
    FP16_Alu,
    FP16_F32,
    HFMA2_MMA,
    RedirectedFP64,
    Clmad,
    IMMA_88,
    MMA_1x_collect,
    MMA_2x_collect,
    DMMA,
    Cbu,
    Decoupled,
    DecoupledAgu,
}

#[derive(Debug)]
enum PredLatencySM80 {
    Disp_Alu,
    Coupled,
    FMA,
    FP16,
    HFMA2_MMA,
    RedirectedFP64,
    Decoupled,
    Guard,
}

#[derive(Debug)]
enum URegLatencySM80 {
    Coupled,
    Decoupled,
    Cbu,
    CoupledBindless,
    DecoupledBindless,
    ToUr,
    Rpcmov_64,
    Udp,
    Uldc,
    Umov,
    VoteU,
}

#[derive(Debug)]
enum UPredLatencySM80 {
    Coupled,
    Udp,
    VoteU,
    UGuard,
    Bra_Jmp,
    Uldc_Mma,
}

impl RegLatencySM80 {

    fn op_category(op: &Op, reader: bool, op_reg_idx: usize) -> RegLatencySM80 {
        match op {
            // this will need updating if imad grows support for input predicates
            Op::IMad(_) | Op::IMul(_) => RegLatencySM80::CoupledFMA,
            Op::IMad64(_) => if reader {
                match op_reg_idx {
                    0 | 1 => RegLatencySM80::IMADWideReadAB,
                    2 => RegLatencySM80::IMADWideReadCL, // vs upper C operand - work it out
                    _ => { panic!("Illegal field in imadwide") }
                }
            } else {
                RegLatencySM80::IMADWideWriteDH // as above this needs more work
            }

            Op::PopC(_) => RegLatencySM80::Decoupled,
            Op::IAdd3(_)
            | Op::IAdd3X(_) => RegLatencySM80::CoupledAlu,

            Op::BMsk(_) => RegLatencySM80::CoupledAlu,
            // Sgxt => RegLatencySM80::CoupledAlu,
            Op::Lop3(_) => RegLatencySM80::CoupledAlu,
            Op::Flo(_) => RegLatencySM80::Decoupled,
            Op::ISetP(_) => RegLatencySM80::CoupledAlu,
            Op::IAbs(_) => RegLatencySM80::CoupledAlu,
            Op::Lea(_) => RegLatencySM80::CoupledAlu,
            Op::LeaX(_) => RegLatencySM80::CoupledAlu,
            Op::IMnMx(_) => RegLatencySM80::CoupledAlu,
            Op::I2I(_) => RegLatencySM80::CoupledAlu,
            // I2IP => RegLatencySM80::CoupledAlu
            Op::Shf(_) =>  RegLatencySM80::CoupledAlu,

            Op::F2FP(_) => RegLatencySM80::CoupledAlu,
            Op::FFma(_) => RegLatencySM80::CoupledFMA,
            Op::FAdd(_) => RegLatencySM80::CoupledFMA,
            Op::FMul(_) => RegLatencySM80::CoupledFMA,
            Op::FMnMx(_) => RegLatencySM80::CoupledAlu,
            Op::FSwzAdd(_) => RegLatencySM80::CoupledFMA,
            Op::FSet(_) => RegLatencySM80::CoupledAlu,
            // FSel => RegLatencySM80::CoupledAlu,
            Op::FSetP(_) => RegLatencySM80::CoupledAlu,
            // FChk => RegLatencySM80::Decoupled,

            Op::DAdd(_)
            | Op::DFma(_)
            | Op::DMul(_)
            | Op::DSetP(_) => RegLatencySM80::RedirectedFP64,

            Op::DMnMx(_) => RegLatencySM80::RedirectedFP64, // not in docs

            Op::HAdd2(hadd2) => if hadd2.f32 { RegLatencySM80::FP16_F32 } else { RegLatencySM80::FP16 },
            Op::HFma2(_)
            | Op::HMul2(_) => RegLatencySM80::FP16,

            Op::HSet2(_)
            | Op::HSetP2(_)
            | Op::HMnMx2(_) => RegLatencySM80::FP16_Alu,
            // let in for documentation purposes
            Op::Hmma(h) => {
            match h.mat_size {
                    HmmaSize::M16N8K4 => match h.dst_type {
                        FloatType::F16 => RegLatencySM80::MMA_1x_collect,
                        _ => RegLatencySM80::MMA_2x_collect,
                    }
                    HmmaSize::M16N8K8 => RegLatencySM80::MMA_1x_collect,
                    HmmaSize::M16N8K16 => RegLatencySM80::MMA_2x_collect,
                }
            }
            Op::Ipa(_) => RegLatencySM80::DecoupledAgu,
            Op::MuFu(_) => RegLatencySM80::Decoupled,

            // Conversion functions all decoupled
            Op::F2F(_) => RegLatencySM80::Decoupled,
            Op::F2I(_) => RegLatencySM80::Decoupled,
            Op::I2F(_) => RegLatencySM80::Decoupled,
            Op::FRnd(_) => RegLatencySM80::Decoupled,
            Op::AL2P(_) => RegLatencySM80::Decoupled,

            Op::Mov(_) => RegLatencySM80::CoupledAlu,
            Op::Sel(_) => RegLatencySM80::CoupledAlu,
            Op::BRev(_) => RegLatencySM80::Decoupled,
            // P2R => RegLatencySM80::CoupledAlu,
            // R2P => RegLatencySM80::CoupledAlu,
            Op::PLop3(_) => RegLatencySM80::CoupledAlu,
            Op::Prmt(_) => RegLatencySM80::CoupledAlu,
            Op::Nop(_) => RegLatencySM80::CoupledDisp64,
            Op::Vote(_) => RegLatencySM80::CoupledAlu,
            Op::S2R(_) => RegLatencySM80::Decoupled,
            // S2UR  => RegLatencySM80::Decoupled,
            Op::R2UR(_) => { if reader { RegLatencySM80::Decoupled } else { panic!("Illegal R2UR"); } }
            Op::CS2R(cs2r) => if cs2r.dst.as_reg().unwrap().comps() == 2 { RegLatencySM80::CoupledDisp64 } else { RegLatencySM80::CoupledAlu },
            // B2R => RegLatencySM80::DecoupledAgu,
            // LEPC => RegLatencySM80::CoupledDisp64
            Op::BMov(bmov) => match bmov.dst {
                Dst::Reg(reg) => RegLatencySM80::Cbu,
                _ => RegLatencySM80::Cbu
            },
            // RPCMOV.32 => RegLatencySM80::CoupledAlu,
            // RPCMOV.64 => RegLatencySM80::CoupledDisp64
            // PMTRIG => RegLatencySM80::CoupledDisp64
            // CSMTEST =>  RegLatencySM80::CoupledAlu,
            Op::Bar(_) => RegLatencySM80::DecoupledAgu,
            // Remove when Imma added
            //Op::Imma(_) => RegLatencySM80::IMMA,

            Op::IDp4(_) => RegLatencySM80::CoupledFMA,
            Op::BClear(_) => RegLatencySM80::Decoupled,
            Op::Bra(_) => RegLatencySM80::Decoupled,
            Op::BSSy(_) => RegLatencySM80::Decoupled,
            Op::Kill(_) => RegLatencySM80::Decoupled,
            Op::Exit(_) => RegLatencySM80::Decoupled,
            Op::BSync(_) => RegLatencySM80::Decoupled,
            Op::Tex(_) => RegLatencySM80::Decoupled,
            Op::Tld(_) => RegLatencySM80::Decoupled,
            Op::Tld4(_) => RegLatencySM80::Decoupled,
            Op::Tmml(_) => RegLatencySM80::Decoupled,
            Op::Txd(_) => RegLatencySM80::Decoupled,
            Op::Txq(_) => RegLatencySM80::Decoupled,
            Op::Ldc(_) => RegLatencySM80::Decoupled,
            Op::ALd(_) => RegLatencySM80::DecoupledAgu,
            Op::ASt(_) => RegLatencySM80::DecoupledAgu,
            Op::Out(_) => RegLatencySM80::DecoupledAgu,
            Op::OutFinal(_) => RegLatencySM80::DecoupledAgu,
            Op::Ld(_) => RegLatencySM80::DecoupledAgu,
            Op::St(_) => RegLatencySM80::DecoupledAgu,
            Op::Atom(_) => RegLatencySM80::DecoupledAgu,
            //CCtl.i,c are coupled
            Op::CCtl(_) => RegLatencySM80::DecoupledAgu,
            Op::MemBar(_) => RegLatencySM80::Decoupled,
            Op::SuLd(_) => RegLatencySM80::Decoupled,
            Op::SuSt(_) => RegLatencySM80::Decoupled,
            Op::SuAtom(_) => RegLatencySM80::Decoupled,
            Op::PixLd(_) => RegLatencySM80::DecoupledAgu,
            Op::Isberd(_) => RegLatencySM80::DecoupledAgu,
            Op::LdTram(_) => RegLatencySM80::DecoupledAgu,
            Op::Shfl(_) => RegLatencySM80::DecoupledAgu,
            //Op::LdSm(_) => RegLatencySM80::DecoupledAgu
            x => { panic!("Illegal instuction in reg category {}", x); }
        }
    }

    pub fn read_after_write(writer: RegLatencySM80,
                            reader: RegLatencySM80) -> u32 {
        match reader {
            RegLatencySM80::CoupledAlu => {
                match writer {
                    RegLatencySM80::CoupledAlu => 4,
                    RegLatencySM80::CoupledDisp64 => 6,
                    RegLatencySM80::CoupledFMA => 5,
                    RegLatencySM80::IMADWideWriteDL => 3,
                    RegLatencySM80::IMADWideWriteDH => 5,
                    RegLatencySM80::FP16 => 5,
                    RegLatencySM80::FP16_Alu => 5,
                    RegLatencySM80::FP16_F32 => 5,
                    RegLatencySM80::HFMA2_MMA => 10,
                    RegLatencySM80::RedirectedFP64 => 10,
                    RegLatencySM80::Clmad => 12,
                    RegLatencySM80::IMMA_88 => 13,
                    RegLatencySM80::MMA_1x_collect => 13,
                    RegLatencySM80::MMA_2x_collect => 17,
                    RegLatencySM80::DMMA => 25,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }
            }
            RegLatencySM80::CoupledFMA |
            RegLatencySM80::IMADWideReadCL => {
                match writer {
                    RegLatencySM80::CoupledAlu => 5,
                    RegLatencySM80::CoupledDisp64 => 6,
                    RegLatencySM80::CoupledFMA => 4,
                    RegLatencySM80::IMADWideWriteDL => 2,
                    RegLatencySM80::IMADWideWriteDH => 4,
                    RegLatencySM80::FP16 => 5,
                    RegLatencySM80::FP16_Alu => 5,
                    RegLatencySM80::FP16_F32 => 5,
                    RegLatencySM80::HFMA2_MMA => 10,
                    RegLatencySM80::RedirectedFP64 => 10,
                    RegLatencySM80::Clmad => 12,
                    RegLatencySM80::IMMA_88 => 13,
                    RegLatencySM80::MMA_1x_collect => 13,
                    RegLatencySM80::MMA_2x_collect => 17,
                    RegLatencySM80::DMMA => 25,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }
            },
            RegLatencySM80::IMADWideReadAB => {
                match writer {
                    RegLatencySM80::CoupledAlu => 5,
                    RegLatencySM80::CoupledDisp64 => 6,
                    RegLatencySM80::CoupledFMA => 4,
                    RegLatencySM80::IMADWideWriteDL => 4,
                    RegLatencySM80::IMADWideWriteDH => 6,
                    RegLatencySM80::FP16 => 5,
                    RegLatencySM80::FP16_Alu => 5,
                    RegLatencySM80::FP16_F32 => 5,
                    RegLatencySM80::HFMA2_MMA => 10,
                    RegLatencySM80::RedirectedFP64 => 10,
                    RegLatencySM80::Clmad => 12,
                    RegLatencySM80::IMMA_88 => 13,
                    RegLatencySM80::MMA_1x_collect => 13,
                    RegLatencySM80::MMA_2x_collect => 17,
                    RegLatencySM80::DMMA => 25,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }
            }
            RegLatencySM80::IMADWideReadCH => {
                match writer {
                    RegLatencySM80::CoupledAlu => 3,
                    RegLatencySM80::CoupledDisp64 => 4,
                    RegLatencySM80::CoupledFMA => 2,
                    RegLatencySM80::IMADWideWriteDL => 2,
                    RegLatencySM80::IMADWideWriteDH => 2,
                    RegLatencySM80::FP16 => 3,
                    RegLatencySM80::FP16_Alu => 3,
                    RegLatencySM80::FP16_F32 => 3,
                    RegLatencySM80::HFMA2_MMA => 8,
                    RegLatencySM80::RedirectedFP64 => 8,
                    RegLatencySM80::Clmad => 10,
                    RegLatencySM80::IMMA_88 => 11,
                    RegLatencySM80::MMA_1x_collect => 11,
                    RegLatencySM80::MMA_2x_collect => 15,
                    RegLatencySM80::DMMA => 23,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }
            }
            RegLatencySM80::FP16 |
            RegLatencySM80::FP16_Alu => {
                match writer {
                    RegLatencySM80::CoupledAlu => 5,
                    RegLatencySM80::CoupledDisp64 => 6,
                    RegLatencySM80::CoupledFMA => 5,
                    RegLatencySM80::IMADWideWriteDL => 3,
                    RegLatencySM80::IMADWideWriteDH => 5,
                    RegLatencySM80::FP16 => 4,
                    RegLatencySM80::FP16_Alu => 4,
                    RegLatencySM80::FP16_F32 => 5,
                    RegLatencySM80::HFMA2_MMA => 10,
                    RegLatencySM80::RedirectedFP64 => 10,
                    RegLatencySM80::Clmad => 12,
                    RegLatencySM80::IMMA_88 => 13,
                    RegLatencySM80::MMA_1x_collect => 13,
                    RegLatencySM80::MMA_2x_collect => 17,
                    RegLatencySM80::DMMA => 25,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }
            }
            RegLatencySM80::FP16_F32 => {
                match writer {
                    RegLatencySM80::CoupledAlu => 5,
                    RegLatencySM80::CoupledDisp64 => 6,
                    RegLatencySM80::CoupledFMA => 5,
                    RegLatencySM80::IMADWideWriteDL => 3,
                    RegLatencySM80::IMADWideWriteDH => 5,
                    RegLatencySM80::FP16 => 5,
                    RegLatencySM80::FP16_Alu => 5,
                    RegLatencySM80::FP16_F32 => 5,
                    RegLatencySM80::HFMA2_MMA => 10,
                    RegLatencySM80::RedirectedFP64 => 10,
                    RegLatencySM80::Clmad => 12,
                    RegLatencySM80::IMMA_88 => 13,
                    RegLatencySM80::MMA_1x_collect => 13,
                    RegLatencySM80::MMA_2x_collect => 17,
                    RegLatencySM80::DMMA => 25,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }
            }
            RegLatencySM80::HFMA2_MMA |
            RegLatencySM80::RedirectedFP64 => {
                match writer {
                    RegLatencySM80::CoupledAlu => 6,
                    RegLatencySM80::CoupledDisp64 => 6,
                    RegLatencySM80::CoupledFMA => 6,
                    RegLatencySM80::IMADWideWriteDL => 6,
                    RegLatencySM80::IMADWideWriteDH => 6,
                    RegLatencySM80::FP16 => 6,
                    RegLatencySM80::FP16_Alu => 6,
                    RegLatencySM80::FP16_F32 => 6,
                    RegLatencySM80::HFMA2_MMA => 6,
                    RegLatencySM80::RedirectedFP64 => 6,
                    RegLatencySM80::Clmad => 12,
                    RegLatencySM80::IMMA_88 => 13,
                    RegLatencySM80::MMA_1x_collect => 13,
                    RegLatencySM80::MMA_2x_collect => 17,
                    RegLatencySM80::DMMA => 25,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }
            }
            RegLatencySM80::Clmad => {
                match writer {
                    RegLatencySM80::CoupledAlu => 6,
                    RegLatencySM80::CoupledDisp64 => 6,
                    RegLatencySM80::CoupledFMA => 6,
                    RegLatencySM80::IMADWideWriteDL => 6,
                    RegLatencySM80::IMADWideWriteDH => 6,
                    RegLatencySM80::FP16 => 6,
                    RegLatencySM80::FP16_Alu => 6,
                    RegLatencySM80::FP16_F32 => 6,
                    RegLatencySM80::HFMA2_MMA => 10,
                    RegLatencySM80::RedirectedFP64 => 10,
                    RegLatencySM80::Clmad => 8,
                    RegLatencySM80::IMMA_88 => 13,
                    RegLatencySM80::MMA_1x_collect => 13,
                    RegLatencySM80::MMA_2x_collect => 17,
                    RegLatencySM80::DMMA => 25,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }
            }
            RegLatencySM80::IMMA_88 |
            RegLatencySM80::MMA_1x_collect => {
                match writer {
                    RegLatencySM80::CoupledAlu => 7,
                    RegLatencySM80::CoupledDisp64 => 7,
                    RegLatencySM80::CoupledFMA => 7,
                    RegLatencySM80::IMADWideWriteDL => 7,
                    RegLatencySM80::IMADWideWriteDH => 7,
                    RegLatencySM80::FP16 => 7,
                    RegLatencySM80::FP16_Alu => 7,
                    RegLatencySM80::FP16_F32 => 7,
                    RegLatencySM80::HFMA2_MMA => 11,
                    RegLatencySM80::RedirectedFP64 => 11,
                    RegLatencySM80::Clmad => 13,
                    RegLatencySM80::IMMA_88 => 14,//6??
                    RegLatencySM80::MMA_1x_collect => 14,//6??
                    RegLatencySM80::MMA_2x_collect => 18,
                    RegLatencySM80::DMMA => 26,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }
            }
            RegLatencySM80::MMA_2x_collect => {
                match writer {
                    RegLatencySM80::CoupledAlu => 7,
                    RegLatencySM80::CoupledDisp64 => 7,
                    RegLatencySM80::CoupledFMA => 7,
                    RegLatencySM80::IMADWideWriteDL => 7,
                    RegLatencySM80::IMADWideWriteDH => 7,
                    RegLatencySM80::FP16 => 7,
                    RegLatencySM80::FP16_Alu => 7,
                    RegLatencySM80::FP16_F32 => 7,
                    RegLatencySM80::HFMA2_MMA => 11,
                    RegLatencySM80::RedirectedFP64 => 11,
                    RegLatencySM80::Clmad => 13,
                    RegLatencySM80::IMMA_88 => 14,
                    RegLatencySM80::MMA_1x_collect => 14,
                    RegLatencySM80::MMA_2x_collect => 18, //10??
                    RegLatencySM80::DMMA => 26,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }
            }
            RegLatencySM80::DMMA => {
                match writer {
                    RegLatencySM80::CoupledAlu => 7,
                    RegLatencySM80::CoupledDisp64 => 7,
                    RegLatencySM80::CoupledFMA => 7,
                    RegLatencySM80::IMADWideWriteDL => 7,
                    RegLatencySM80::IMADWideWriteDH => 7,
                    RegLatencySM80::FP16 => 7,
                    RegLatencySM80::FP16_Alu => 7,
                    RegLatencySM80::FP16_F32 => 7,
                    RegLatencySM80::HFMA2_MMA => 11,
                    RegLatencySM80::RedirectedFP64 => 11,
                    RegLatencySM80::Clmad => 13,
                    RegLatencySM80::IMMA_88 => 14,
                    RegLatencySM80::MMA_1x_collect => 14,
                    RegLatencySM80::MMA_2x_collect => 18,
                    RegLatencySM80::DMMA => 26,//18??
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }

            }
            RegLatencySM80::Cbu |
            RegLatencySM80::Decoupled => {
                match writer {
                    RegLatencySM80::CoupledAlu => 4,
                    RegLatencySM80::CoupledDisp64 => 4,
                    RegLatencySM80::CoupledFMA => 4,
                    RegLatencySM80::IMADWideWriteDL => 4,
                    RegLatencySM80::IMADWideWriteDH => 4,
                    RegLatencySM80::FP16 => 4,
                    RegLatencySM80::FP16_Alu => 4,
                    RegLatencySM80::FP16_F32 => 4,
                    RegLatencySM80::HFMA2_MMA => 6,
                    RegLatencySM80::RedirectedFP64 => 6,
                    RegLatencySM80::Clmad => 8,
                    RegLatencySM80::IMMA_88 => 11,
                    RegLatencySM80::MMA_1x_collect => 11,
                    RegLatencySM80::MMA_2x_collect => 15,
                    RegLatencySM80::DMMA => 23,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }

                }
            }
            RegLatencySM80::DecoupledAgu => {
                match writer {
                    RegLatencySM80::CoupledAlu => 5,
                    RegLatencySM80::CoupledDisp64 => 5,
                    RegLatencySM80::CoupledFMA => 5,
                    RegLatencySM80::IMADWideWriteDL => 5,
                    RegLatencySM80::IMADWideWriteDH => 5,
                    RegLatencySM80::FP16 => 5,
                    RegLatencySM80::FP16_Alu => 5,
                    RegLatencySM80::FP16_F32 => 5,
                    RegLatencySM80::HFMA2_MMA => 7,
                    RegLatencySM80::RedirectedFP64 => 7,
                    RegLatencySM80::Clmad => 9,
                    RegLatencySM80::IMMA_88 => 12,
                    RegLatencySM80::MMA_1x_collect => 12,
                    RegLatencySM80::MMA_2x_collect => 16,
                    RegLatencySM80::DMMA => 24,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 raw"); }
                }
            }
            RegLatencySM80::CoupledDisp64 |
            RegLatencySM80::IMADWideWriteDL |
            RegLatencySM80::IMADWideWriteDH => { panic!("Illegal reader in sm80 raw"); },
        }
    }

    fn write_after_write(writer1: RegLatencySM80,
                         writer2: RegLatencySM80,
                         has_pred: bool) -> u32 {
        match writer2 {
            RegLatencySM80::CoupledAlu => {
                match writer1 {
                    RegLatencySM80::CoupledDisp64 => pred!(has_pred, 1, 1),
                    RegLatencySM80::CoupledAlu|
                    RegLatencySM80::CoupledFMA |
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::IMADWideWriteDH |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 => 1,
                    RegLatencySM80::HFMA2_MMA |
                    RegLatencySM80::RedirectedFP64 => pred!(has_pred, 3, 3),
                    RegLatencySM80::Clmad => pred!(has_pred, 5, 3),
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => pred!(has_pred, 8, 1),
                    RegLatencySM80::MMA_2x_collect => pred!(has_pred, 12, 1),
                    RegLatencySM80::DMMA => pred!(has_pred, 20, 1),
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            RegLatencySM80::CoupledDisp64 => {
                match writer1 {
                    RegLatencySM80::CoupledAlu |
                    RegLatencySM80::CoupledDisp64 |
                    RegLatencySM80::CoupledFMA |
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::IMADWideWriteDH |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 => 1,
                    RegLatencySM80::HFMA2_MMA |
                    RegLatencySM80::RedirectedFP64 => pred!(has_pred, 3, 1),
                    RegLatencySM80::Clmad => pred!(has_pred, 5, 1),
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => 8,
                    RegLatencySM80::MMA_2x_collect => 12,
                    RegLatencySM80::DMMA => 20,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            RegLatencySM80::CoupledFMA => {
                match writer1 {
                    RegLatencySM80::CoupledDisp64 => pred!(has_pred, 1, 1),
                    RegLatencySM80::CoupledAlu |
                    RegLatencySM80::CoupledFMA |
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 => 1,
                    RegLatencySM80::IMADWideWriteDH => pred!(has_pred, 1, 1),
                    RegLatencySM80::HFMA2_MMA |
                    RegLatencySM80::RedirectedFP64 => pred!(has_pred, 3, 3),
                    RegLatencySM80::Clmad => pred!(has_pred, 5, 3),
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => pred!(has_pred, 8, 1),
                    RegLatencySM80::MMA_2x_collect => pred!(has_pred, 12, 1),
                    RegLatencySM80::DMMA => pred!(has_pred, 20, 1),
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            RegLatencySM80::IMADWideWriteDL => {
                match writer1 {
                    RegLatencySM80::CoupledAlu => pred!(has_pred, 1, 2),
                    RegLatencySM80::CoupledDisp64 => pred!(has_pred, 1, 3),
                    RegLatencySM80::CoupledFMA => pred!(has_pred, 1, 1),
                    RegLatencySM80::IMADWideWriteDL => 1,
                    RegLatencySM80::IMADWideWriteDH => pred!(has_pred, 1, 1),
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 => pred!(has_pred, 1, 2),
                    RegLatencySM80::HFMA2_MMA |
                    RegLatencySM80::RedirectedFP64 => pred!(has_pred, 5, 3),
                    RegLatencySM80::Clmad => pred!(has_pred, 5, 5),
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => pred!(has_pred, 8, 3),
                    RegLatencySM80::MMA_2x_collect => pred!(has_pred, 12, 3),
                    RegLatencySM80::DMMA => pred!(has_pred, 20, 3),
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            RegLatencySM80::IMADWideWriteDH => {
                match writer1 {
                    RegLatencySM80::CoupledAlu => 1,
                    RegLatencySM80::CoupledDisp64 => pred!(has_pred, 1, 1),
                    RegLatencySM80::CoupledFMA => 1,
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::IMADWideWriteDH |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 => 1,
                    RegLatencySM80::HFMA2_MMA |
                    RegLatencySM80::RedirectedFP64 => pred!(has_pred, 5, 1),
                    RegLatencySM80::Clmad => pred!(has_pred, 5, 3),
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => pred!(has_pred, 8, 1),
                    RegLatencySM80::MMA_2x_collect => pred!(has_pred, 12, 1),
                    RegLatencySM80::DMMA => pred!(has_pred, 20, 1),
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            RegLatencySM80::FP16 |
            RegLatencySM80::FP16_Alu => {
                match writer1 {
                    RegLatencySM80::CoupledAlu => 1,
                    RegLatencySM80::CoupledDisp64 => pred!(has_pred, 1, 1),
                    RegLatencySM80::CoupledFMA => 1,
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::IMADWideWriteDH |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 => 1,
                    RegLatencySM80::HFMA2_MMA |
                    RegLatencySM80::RedirectedFP64 => pred!(has_pred, 3, 3),
                    RegLatencySM80::Clmad => pred!(has_pred, 5, 3),
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => pred!(has_pred, 8, 1),
                    RegLatencySM80::MMA_2x_collect => pred!(has_pred, 12, 1),
                    RegLatencySM80::DMMA => pred!(has_pred, 20, 1),
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            RegLatencySM80::FP16_F32 => {
                match writer1 {
                    RegLatencySM80::CoupledAlu |
                    RegLatencySM80::CoupledDisp64 |
                    RegLatencySM80::CoupledFMA |
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::IMADWideWriteDH |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 => 1,
                    RegLatencySM80::HFMA2_MMA |
                    RegLatencySM80::RedirectedFP64 => pred!(has_pred, 3, 2),
                    RegLatencySM80::Clmad => pred!(has_pred, 5, 2),
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => 8,
                    RegLatencySM80::MMA_2x_collect => 12,
                    RegLatencySM80::DMMA => 20,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            RegLatencySM80::HFMA2_MMA => {
                match writer1 {
                    RegLatencySM80::CoupledAlu |
                    RegLatencySM80::CoupledDisp64 |
                    RegLatencySM80::CoupledFMA |
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::IMADWideWriteDH |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 => 1,
                    RegLatencySM80::HFMA2_MMA => 2,
                    RegLatencySM80::RedirectedFP64 => 3,
                    RegLatencySM80::Clmad => pred!(has_pred, 5, 1),
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => 8,
                    RegLatencySM80::MMA_2x_collect => 12,
                    RegLatencySM80::DMMA => 20,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            RegLatencySM80::RedirectedFP64 => {
                match writer1 {
                    RegLatencySM80::CoupledAlu |
                    RegLatencySM80::CoupledDisp64 |
                    RegLatencySM80::CoupledFMA |
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::IMADWideWriteDH |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 => 1,
                    RegLatencySM80::HFMA2_MMA => 2,
                    RegLatencySM80::RedirectedFP64 => 2,
                    RegLatencySM80::Clmad => pred!(has_pred, 4, 2),
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => 7,
                    RegLatencySM80::MMA_2x_collect => 11,
                    RegLatencySM80::DMMA => 19,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            RegLatencySM80::Clmad => {
                match writer1 {
                    RegLatencySM80::CoupledAlu |
                    RegLatencySM80::CoupledDisp64 |
                    RegLatencySM80::CoupledFMA |
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::IMADWideWriteDH |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 |
                    RegLatencySM80::HFMA2_MMA |
                    RegLatencySM80::RedirectedFP64 |
                    RegLatencySM80::Clmad => 2,
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => 7,
                    RegLatencySM80::MMA_2x_collect => 11,
                    RegLatencySM80::DMMA => 19,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }

            }
            RegLatencySM80::IMMA_88 |
            RegLatencySM80::MMA_1x_collect => {
                match writer1 {
                    RegLatencySM80::CoupledAlu |
                    RegLatencySM80::CoupledDisp64 |
                    RegLatencySM80::CoupledFMA |
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::IMADWideWriteDH |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 |
                    RegLatencySM80::HFMA2_MMA |
                    RegLatencySM80::RedirectedFP64 |
                    RegLatencySM80::Clmad => 2,
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => 4,
                    RegLatencySM80::MMA_2x_collect => 8,
                    RegLatencySM80::DMMA => 17,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            RegLatencySM80::MMA_2x_collect |
            RegLatencySM80::DMMA => {
                match writer1 {
                    RegLatencySM80::CoupledAlu |
                    RegLatencySM80::CoupledDisp64 |
                    RegLatencySM80::CoupledFMA |
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::IMADWideWriteDH |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 |
                    RegLatencySM80::HFMA2_MMA |
                    RegLatencySM80::RedirectedFP64 |
                    RegLatencySM80::Clmad => 2,
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => 4,
                    RegLatencySM80::MMA_2x_collect => 8,
                    RegLatencySM80::DMMA => 16,
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            RegLatencySM80::Cbu |
            RegLatencySM80::Decoupled |
            RegLatencySM80::DecoupledAgu => {
                match writer1 {
                    RegLatencySM80::CoupledAlu |
                    RegLatencySM80::CoupledDisp64 |
                    RegLatencySM80::CoupledFMA |
                    RegLatencySM80::IMADWideWriteDL |
                    RegLatencySM80::IMADWideWriteDH |
                    RegLatencySM80::FP16 |
                    RegLatencySM80::FP16_Alu |
                    RegLatencySM80::FP16_F32 => pred!(has_pred, 1, 5),
                    RegLatencySM80::HFMA2_MMA |
                    RegLatencySM80::RedirectedFP64 => pred!(has_pred, 1, 9),
                    RegLatencySM80::Clmad => pred!(has_pred, 1, 11),
                    RegLatencySM80::IMMA_88 |
                    RegLatencySM80::MMA_1x_collect => pred!(has_pred, 7, 6),
                    RegLatencySM80::MMA_2x_collect => pred!(has_pred, 11, 6),
                    RegLatencySM80::DMMA => pred!(has_pred, 19, 6),
                    RegLatencySM80::Cbu => 1,
                    RegLatencySM80::Decoupled => 1,
                    RegLatencySM80::DecoupledAgu => 1,
                    _ => { panic!("Illegal writer in sm80 waw"); }
                }
            }
            _ => { panic!("Illegal writer in sm80 waw"); }
        }
    }

    fn write_after_read(reader: RegLatencySM80,
                        writer: RegLatencySM80) -> u32 {
        match writer {
            RegLatencySM80::CoupledAlu |
            RegLatencySM80::CoupledDisp64 |
            RegLatencySM80::CoupledFMA |
            RegLatencySM80::IMADWideWriteDL |
            RegLatencySM80::IMADWideWriteDH |
            RegLatencySM80::FP16 |
            RegLatencySM80::FP16_Alu |
            RegLatencySM80::FP16_F32 |
            RegLatencySM80::HFMA2_MMA |
            RegLatencySM80::RedirectedFP64 => 1,
            RegLatencySM80::Clmad |
            RegLatencySM80::IMMA_88 |
            RegLatencySM80::MMA_1x_collect |
            RegLatencySM80::MMA_2x_collect |
            RegLatencySM80::DMMA |
            RegLatencySM80::Cbu |
            RegLatencySM80::Decoupled |
            RegLatencySM80::DecoupledAgu => {
                match reader {
                    RegLatencySM80::CoupledAlu |
                    RegLatencySM80::CoupledDisp64 |
                    RegLatencySM80::CoupledFMA |
                    RegLatencySM80::IMADWideReadAB |
                    RegLatencySM80::IMADWideReadCL |
                    RegLatencySM80::IMADWideReadCH => 2,
                    _ => 1,
                }
            }
            _ => { panic!("Illegal writer in sm80 war"); }
        }
    }
}

impl PredLatencySM80 {

    fn op_category(op: &Op, reader: bool, op_reg_idx: usize) -> PredLatencySM80 {
        match op {

            Op::Atom(_) => PredLatencySM80::Decoupled,
            Op::DSetP(_) => PredLatencySM80::RedirectedFP64,
            Op::FMnMx(_) |
            Op::FSetP(_) => PredLatencySM80::Coupled,
            Op::HFma2(_) => PredLatencySM80::FP16,
            Op::HMnMx2(_) => PredLatencySM80::FP16,
            Op::HSetP2(_) => PredLatencySM80::FP16,
            Op::IAdd3(_) => PredLatencySM80::Coupled,
            Op::IAdd3X(_) => PredLatencySM80::Coupled,
            Op::IMad(_) => PredLatencySM80::FMA,
            Op::IMad(_) => PredLatencySM80::FMA,
            Op::IMad64(_) => PredLatencySM80::FMA,
            Op::IMnMx(_) => PredLatencySM80::Coupled,
            Op::IMul(_) => PredLatencySM80::FMA,
            Op::Ipa(_) => PredLatencySM80::Decoupled,
            Op::ISetP(_) => PredLatencySM80::Coupled,

            Op::Ld(_) => PredLatencySM80::Decoupled,

            Op::Lea(_) |
            Op::LeaX(_) => PredLatencySM80::Coupled,
            Op::PixLd(_) => PredLatencySM80::Decoupled,
            Op::PLop3(_)  => PredLatencySM80::Coupled,
            Op::PSetP(_)  => PredLatencySM80::Coupled,
            Op::R2UR(_)  => PredLatencySM80::Decoupled,
            Op::Sel(_) => PredLatencySM80::Coupled,
            Op::Shfl(_)  => PredLatencySM80::Decoupled,
            Op::SuLd(_)  => PredLatencySM80::Decoupled,
            Op::SuSt(_)  => PredLatencySM80::Decoupled,
            Op::Tex(_) => PredLatencySM80::Decoupled,
            Op::Tld(_) => PredLatencySM80::Decoupled,
            Op::Tld4(_) => PredLatencySM80::Decoupled,
            Op::Tmml(_) => PredLatencySM80::Decoupled,
            Op::Txd(_) => PredLatencySM80::Decoupled,
            Op::Txq(_) => PredLatencySM80::Decoupled,

            Op::Vote(_) => PredLatencySM80::Disp_Alu,
            _ => { panic!("Illegal op in sm80 pred latency {}", op); }
        }
    }
    fn pred_read_after_write(writer: PredLatencySM80,
                             reader: PredLatencySM80) -> u32 {
        match reader {
            PredLatencySM80::Disp_Alu => {
                match writer {
                    PredLatencySM80::Disp_Alu |
                    PredLatencySM80::Coupled |
                    PredLatencySM80::FMA |
                    PredLatencySM80::FP16 => 13,
                    PredLatencySM80::HFMA2_MMA |
                    PredLatencySM80::RedirectedFP64 => 14,
                    PredLatencySM80::Decoupled => 1,
                    _ => { panic!("Illegal writer in sm80 praw"); }
                }
            }
            PredLatencySM80::Coupled => {
                match writer {
                    PredLatencySM80::Disp_Alu |
                    PredLatencySM80::Coupled => 4,
                    PredLatencySM80::FMA |
                    PredLatencySM80::FP16 => 5,
                    PredLatencySM80::HFMA2_MMA |
                    PredLatencySM80::RedirectedFP64 => 6,
                    PredLatencySM80::Decoupled => 1,
                    _ => { panic!("Illegal writer in sm80 praw"); }
                }
            }
            PredLatencySM80::FMA => {
                match writer {
                    PredLatencySM80::Disp_Alu |
                    PredLatencySM80::Coupled => 5,
                    PredLatencySM80::FMA => 4,
                    PredLatencySM80::FP16 => 5,
                    PredLatencySM80::HFMA2_MMA |
                    PredLatencySM80::RedirectedFP64 => 6,
                    PredLatencySM80::Decoupled => 1,
                    _ => { panic!("Illegal writer in sm80 praw"); }
                }

            }
            PredLatencySM80::HFMA2_MMA |
            PredLatencySM80::RedirectedFP64 => {
                match writer {
                    PredLatencySM80::Disp_Alu |
                    PredLatencySM80::Coupled |
                    PredLatencySM80::FMA |
                    PredLatencySM80::FP16 => 13,
                    PredLatencySM80::HFMA2_MMA |
                    PredLatencySM80::RedirectedFP64 => 6,
                    PredLatencySM80::Decoupled => 1,
                    _ => { panic!("Illegal writer in sm80 praw"); }
                }
            }
            PredLatencySM80::Decoupled |
            PredLatencySM80::Guard => {
                match writer {
                    PredLatencySM80::Disp_Alu |
                    PredLatencySM80::Coupled |
                    PredLatencySM80::FMA |
                    PredLatencySM80::FP16 => 13,
                    PredLatencySM80::HFMA2_MMA |
                    PredLatencySM80::RedirectedFP64 => 14,
                    PredLatencySM80::Decoupled => 1,
                    _ => { panic!("Illegal writer in sm80 praw"); }
                }
            }
            _ => { panic!("Illegal reader in sm80 praw"); }
        }
    }

    fn pred_write_after_write(writer1: PredLatencySM80,
                              writer2: PredLatencySM80,
                              has_pred: bool) -> u32 {
        match writer2 {
            PredLatencySM80::Disp_Alu |
            PredLatencySM80::Coupled |
            PredLatencySM80::FMA => {
                match writer1 {
                    PredLatencySM80::Disp_Alu |
                    PredLatencySM80::Coupled |
                    PredLatencySM80::FMA |
                    PredLatencySM80::FP16 => 1,
                    PredLatencySM80::HFMA2_MMA |
                    PredLatencySM80::RedirectedFP64 => 2,
                    PredLatencySM80::Decoupled => 1,
                    _ => { panic!("Illegal writer in sm80 pwaw"); }
                }
            }
            PredLatencySM80::FP16 => {
                match writer1 {
                    PredLatencySM80::Disp_Alu |
                    PredLatencySM80::Coupled |
                    PredLatencySM80::FMA => pred!(has_pred, 2, 7),
                    PredLatencySM80::FP16 => 1,
                    PredLatencySM80::HFMA2_MMA |
                    PredLatencySM80::RedirectedFP64 => pred!(has_pred, 2, 8),
                    PredLatencySM80::Decoupled => 1,
                    _ => { panic!("Illegal writer in sm80 pwaw"); }
                }
            }
            PredLatencySM80::HFMA2_MMA |
            PredLatencySM80::RedirectedFP64 => {
                match writer1 {
                    PredLatencySM80::Disp_Alu |
                    PredLatencySM80::Coupled |
                    PredLatencySM80::FMA |
                    PredLatencySM80::FP16 => pred!(has_pred, 2, 5),
                    PredLatencySM80::HFMA2_MMA |
                    PredLatencySM80::RedirectedFP64 |
                    PredLatencySM80::Decoupled => 1,
                    _ => { panic!("Illegal writer in sm80 pwaw"); }
                }

            }
            PredLatencySM80::Decoupled => {
                match writer1 {
                    PredLatencySM80::Disp_Alu |
                    PredLatencySM80::Coupled |
                    PredLatencySM80::FMA => pred!(has_pred, 2, 10),
                    PredLatencySM80::FP16 => pred!(has_pred, 1, 11),
                    PredLatencySM80::HFMA2_MMA |
                    PredLatencySM80::RedirectedFP64 => pred!(has_pred, 1, 12),
                    PredLatencySM80::Decoupled => 1,
                    _ => { panic!("Illegal writer in sm80 pwaw"); }
                }
            }
            _ => { panic!("Illegal writer in sm80 pwaw"); }
        }
    }

    fn pred_write_after_read(reader: PredLatencySM80,
                             writer: PredLatencySM80) -> u32 {
        1
    }

}

impl URegLatencySM80 {
    fn op_category(op: &Op, reader: bool, op_reg_idx: usize) -> URegLatencySM80 {
        // is this using a bindless cbuf as a src register.
        // this decides between the category types for readers.
        let bindless = reader && op.srcs_as_slice()[op_reg_idx].is_bindless_cbuf();

        let vcoupled = if bindless { URegLatencySM80::CoupledBindless } else { URegLatencySM80::Coupled };
        let vdecoupled = if bindless { URegLatencySM80::DecoupledBindless } else { URegLatencySM80::Decoupled };

        // if this is a reader from a ureg, it could be a U* instruction or a regular instruction.
        let uniform_op = op.is_uniform();

        let vcoupled = if uniform_op { URegLatencySM80::Udp } else { vcoupled };
        let vdecoupled = if uniform_op { URegLatencySM80::Udp } else { vdecoupled };

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
            Op::Ldc(_) => if uniform_op { URegLatencySM80::Uldc } else { vdecoupled },
            Op::Lea(_) => vcoupled,
            Op::LeaX(_) => vcoupled,
            Op::Lop2(_) |
            Op::Lop3(_) => vcoupled,

            Op::MuFu(_) => vdecoupled,
            Op::Mov(_) => if uniform_op { URegLatencySM80::Umov } else { vcoupled },

            // mov32i => URegLatency::Uldc,
            // p2ur => URegLatencySM80::Udp,
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
            Op::R2UR(_) => if !reader { URegLatencySM80::ToUr } else { panic!("Illegal R2UR in ureg"); }
            Op::Vote(_) => URegLatencySM80::VoteU,

            Op::FRnd(_) => vdecoupled,
            Op::F2FP(_) |
            Op::FAdd(_) |
            Op::FMul(_) |
            Op::FFma(_) |
            Op::FSetP(_) |
            Op::FMnMx(_) |
            Op::HAdd2(_) |
            Op::HMul2(_) |
            Op::HSet2(_) |
            Op::HFma2(_) |
            Op::HMnMx2(_) |
            Op::HSetP2(_) => vcoupled,
            Op::DMul(_) |
            Op::DFma(_) |
            Op::DAdd(_) |
            Op::DSetP(_) => vdecoupled,
            _ => { panic!("Illegal instuction in ureg category {}", op); }
        }
    }

    fn read_after_write(writer: URegLatencySM80,
                        reader: URegLatencySM80) -> u32 {
        match reader {
            URegLatencySM80::Coupled => {
                match writer {
                    URegLatencySM80::ToUr => 1,
                    URegLatencySM80::Udp => 6,
                    URegLatencySM80::Uldc => 2,
                    URegLatencySM80::Umov => 2,
                    URegLatencySM80::VoteU => 2,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM80::Decoupled => {
                match writer {
                    URegLatencySM80::ToUr => 1,
                    URegLatencySM80::Udp => 9,
                    URegLatencySM80::Uldc => 2,
                    URegLatencySM80::Umov => 2,
                    URegLatencySM80::VoteU => 2,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM80::Cbu => {
                match writer {
                    URegLatencySM80::ToUr => 1,
                    URegLatencySM80::Udp => 10,
                    URegLatencySM80::Uldc => 3,
                    URegLatencySM80::Umov => 3,
                    URegLatencySM80::VoteU => 3,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM80::CoupledBindless |
            URegLatencySM80::DecoupledBindless |
            URegLatencySM80::Uldc => {
                match writer {
                    URegLatencySM80::ToUr => 1,
                    URegLatencySM80::Udp => 12,
                    URegLatencySM80::Uldc => 5,
                    URegLatencySM80::Umov => 5,
                    URegLatencySM80::VoteU => 5,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM80::Udp => {
                match writer {
                    URegLatencySM80::ToUr => 1,
                    URegLatencySM80::Udp => 4,
                    URegLatencySM80::Uldc => 2,
                    URegLatencySM80::Umov => 2,
                    URegLatencySM80::VoteU => 2,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            URegLatencySM80::Umov |
            URegLatencySM80::VoteU => {
                match writer {
                    URegLatencySM80::ToUr => 1,
                    URegLatencySM80::Udp => 7,
                    URegLatencySM80::Uldc => 2,
                    URegLatencySM80::Umov => 2,
                    URegLatencySM80::VoteU => 2,
                    _ => { panic!("Illegal writer in raw ureg latency {:?}", writer) },
                }
            }
            _ => { panic!("Illegal read in ureg raw latency") },
        }
    }

    fn write_after_write(writer1: URegLatencySM80,
                         writer2: URegLatencySM80,
                         has_pred: bool) -> u32 {
        match writer2 {
            URegLatencySM80::ToUr => {
                match writer1 {
                    URegLatencySM80::ToUr => 1,
                    URegLatencySM80::Udp => pred!(has_pred, 4, 7),
                    URegLatencySM80::Uldc |
                    URegLatencySM80::Umov |
                    URegLatencySM80::VoteU => 4,
                    _ => { panic!("Illegal writer in ureg waw latency") },
                }
            }
            URegLatencySM80::Udp => {
                match writer1 {
                    URegLatencySM80::ToUr |
                    URegLatencySM80::Udp |
                    URegLatencySM80::Uldc |
                    URegLatencySM80::Umov |
                    URegLatencySM80::VoteU => 1,
                    _ => { panic!("Illegal writer in ureg waw latency") },
                }
            }
            URegLatencySM80::Uldc |
            URegLatencySM80::Umov |
            URegLatencySM80::VoteU => {
                match writer1 {
                    URegLatencySM80::ToUr => 1,
                    URegLatencySM80::Udp => 7,
                    URegLatencySM80::Uldc |
                    URegLatencySM80::Umov |
                    URegLatencySM80::VoteU => 1,
                    _ => { panic!("Illegal writer in ureg waw latency") },
                }
            }
            _ => { panic!("Illegal writer in ureg waw latency") },
        }
    }

    fn write_after_read(reader: URegLatencySM80,
                        writer: URegLatencySM80) -> u32 {
        match writer {
            URegLatencySM80::ToUr |
            URegLatencySM80::Udp => {
                match reader {
                    URegLatencySM80::Coupled |
                    URegLatencySM80::Decoupled |
                    URegLatencySM80::Cbu |
                    URegLatencySM80::CoupledBindless |
                    URegLatencySM80::DecoupledBindless |
                    URegLatencySM80::Rpcmov_64 |
                    URegLatencySM80::Udp |
                    URegLatencySM80::Uldc |
                    URegLatencySM80::Umov => 1,
                    _ => { panic!("Illegal reader in ureg war latency") },
                }
            }
            URegLatencySM80::Uldc |
            URegLatencySM80::Umov |
            URegLatencySM80::VoteU => {
                match reader {
                    URegLatencySM80::Coupled |
                    URegLatencySM80::Decoupled |
                    URegLatencySM80::Cbu |
                    URegLatencySM80::CoupledBindless |
                    URegLatencySM80::DecoupledBindless |
                    URegLatencySM80::Rpcmov_64 |
                    URegLatencySM80::Uldc |
                    URegLatencySM80::Umov => 1,
                    URegLatencySM80::Udp => 3,
                    _ => { panic!("Illegal reader in ureg war latency") },
                }
            }
            _ => { panic!("Illegal writer in ureg war latency") },
        }
    }

}

impl UPredLatencySM80 {

    fn op_category(op: &Op, reader: bool, op_reg_idx: usize) -> UPredLatencySM80 {
        let uniform_op = op.is_uniform();
        match op {
            Op::BMsk(_) |
            Op::BRev(_) |
            Op::Flo(_) |
            Op::IAdd3(_) |
            Op::IAdd3X(_) |
            Op::IMad(_) |
            Op::ISetP(_) |
            Op::Lea(_) |
            Op::LeaX(_) |
            Op::Lop3(_) |
            Op::Mov(_) => UPredLatencySM80::Udp,
            Op::Ldc(_) => UPredLatencySM80::Uldc_Mma,
            Op::PLop3(_) => if uniform_op { UPredLatencySM80::Udp } else { UPredLatencySM80::Coupled },
            Op::PSetP(_) => if uniform_op { UPredLatencySM80::Udp } else { UPredLatencySM80::Coupled },
            Op::Sel(_) => if uniform_op { UPredLatencySM80::Udp } else { UPredLatencySM80::Coupled },
            Op::Vote(_) => if uniform_op { UPredLatencySM80::VoteU } else { panic!("Illegal Vote in upred"); }
            _ => { panic!("Illegal instuction in upred category {}", op); }
        }
    }

    fn pred_read_after_write(writer: UPredLatencySM80,
                             reader: UPredLatencySM80) -> u32 {
        match reader {
            UPredLatencySM80::Coupled => {
                match writer {
                    UPredLatencySM80::Udp => 6,
                    UPredLatencySM80::VoteU => 1,
                    _ => { panic!("Illegal writer in ureg praw latency") },
                }
            }
            UPredLatencySM80::Udp => {
                match writer {
                    UPredLatencySM80::Udp => 4,
                    UPredLatencySM80::VoteU => 1,
                    _ => { panic!("Illegal writer in ureg praw latency") },
                }
            }
            UPredLatencySM80::UGuard => {
                match writer {
                    UPredLatencySM80::Udp => 11,
                    UPredLatencySM80::VoteU => 5,
                    _ => { panic!("Illegal writer in ureg praw latency") },
                }
            }
            UPredLatencySM80::Bra_Jmp => {
                match writer {
                    UPredLatencySM80::Udp => 9,
                    UPredLatencySM80::VoteU => 2,
                    _ => { panic!("Illegal writer in ureg praw latency") },
                }
            }
            UPredLatencySM80::Uldc_Mma => {
                match writer {
                    UPredLatencySM80::Udp => 11,
                    UPredLatencySM80::VoteU => 5,
                    _ => { panic!("Illegal writer in ureg praw latency") },
                }
            }
            _ => { panic!("Illegal reader in ureg praw latency") },
        }
    }

    fn pred_write_after_write(writer1: UPredLatencySM80,
                              writer2: UPredLatencySM80) -> u32 {
        match writer2 {
            UPredLatencySM80::Udp => {
                match writer1 {
                    UPredLatencySM80::Udp => 1,
                    UPredLatencySM80::VoteU => 1,
                    _ => { panic!("Illegal writer in ureg waw latency") },
                }
            }
            UPredLatencySM80::VoteU => {
                match writer1 {
                    UPredLatencySM80::Udp => 7,
                    UPredLatencySM80::VoteU => 1,
                    _ => { panic!("Illegal writer in ureg waw latency") },
                }
            }
            _ => { panic!("Illegal writer in ureg waw latency") },
        }
    }

    fn pred_write_after_read(reader: UPredLatencySM80,
                             writer: UPredLatencySM80) -> u32 {
        match writer {
            UPredLatencySM80::Udp => {
                match reader {
                    UPredLatencySM80::Coupled |
                    UPredLatencySM80::Udp |
                    UPredLatencySM80::UGuard |
                    UPredLatencySM80::Bra_Jmp |
                    UPredLatencySM80::Uldc_Mma => 1,
                    _ => { panic!("Illegal reader in ureg pwar latency") },
                }
            }
            UPredLatencySM80::VoteU => {
                match reader {
                    UPredLatencySM80::Coupled |
                    UPredLatencySM80::Udp => 2,
                    UPredLatencySM80::UGuard |
                    UPredLatencySM80::Bra_Jmp |
                    UPredLatencySM80::Uldc_Mma => 1,
                    _ => { panic!("Illegal reader in ureg pwar latency") },
                }
            }
            _ => { panic!("Illegal writer in ureg pwar latency") },
        }
    }
}


pub struct SM80Latency {}

impl SM80Latency {
    pub fn needs_scoreboards(op: &Op) -> bool {
        if op.is_uniform() {
            match URegLatencySM80::op_category(op, false, 0) {
                URegLatencySM80::ToUr => true,
                _ => false,
            }
        } else {
            match RegLatencySM80::op_category(op, true, 0) {
                RegLatencySM80::RedirectedFP64 |
                RegLatencySM80::Clmad |
                RegLatencySM80::IMMA_88 |
                RegLatencySM80::MMA_1x_collect |
                RegLatencySM80::MMA_2x_collect |
                RegLatencySM80::DMMA |
                RegLatencySM80::Cbu |
                RegLatencySM80::Decoupled |
                RegLatencySM80::DecoupledAgu => true,
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
                let write_latency = RegLatencySM80::op_category(write, false, dst_idx);
                let read_latency = RegLatencySM80::op_category(read, true, src_idx);
                return RegLatencySM80::read_after_write(write_latency,
                                                        read_latency);
            },
            RegFile::UGPR => {
                let write_latency = URegLatencySM80::op_category(write, false, dst_idx);
                let read_latency = URegLatencySM80::op_category(read, true, src_idx);
                return URegLatencySM80::read_after_write(write_latency,
                                                         read_latency);
            },
            RegFile::Pred => {
                let write_latency = PredLatencySM80::op_category(write, false, dst_idx);
                let read_latency = PredLatencySM80::op_category(read, true, src_idx);
                return PredLatencySM80::pred_read_after_write(write_latency,
                                                             read_latency);
            },
            RegFile::UPred => {
                let write_latency = UPredLatencySM80::op_category(write, false, dst_idx);
                let read_latency = UPredLatencySM80::op_category(read, true, src_idx);
                return UPredLatencySM80::pred_read_after_write(write_latency,
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
                let write_latency = RegLatencySM80::op_category(write, false, dst_idx);
                let read_latency = RegLatencySM80::op_category(read, true, src_idx);
                return RegLatencySM80::write_after_read(read_latency,
                                                        write_latency);
            },
            RegFile::UGPR => {
                let write_latency = URegLatencySM80::op_category(write, false, dst_idx);
                let read_latency = URegLatencySM80::op_category(read, true, src_idx);
                return URegLatencySM80::write_after_read(read_latency,
                                                         write_latency);
            },
            RegFile::Pred => {
                let write_latency = PredLatencySM80::op_category(write, false, dst_idx);
                let read_latency = PredLatencySM80::op_category(read, false, src_idx);
                return PredLatencySM80::pred_write_after_read(read_latency,
                                                             write_latency);
            },
            RegFile::UPred => {
                let write_latency = UPredLatencySM80::op_category(write, false, dst_idx);
                let read_latency = UPredLatencySM80::op_category(read, true, src_idx);
                return UPredLatencySM80::pred_write_after_read(read_latency,
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
                let write1_latency = RegLatencySM80::op_category(a, false, a_dst_idx);
                let write2_latency = RegLatencySM80::op_category(b, false, b_dst_idx);
                return RegLatencySM80::write_after_write(write1_latency,
                                                         write2_latency, a_op_pred);
            },
            RegFile::UGPR => {
                let write1_latency = URegLatencySM80::op_category(a, false, a_dst_idx);
                let write2_latency = URegLatencySM80::op_category(b, false, b_dst_idx);
                return URegLatencySM80::write_after_write(write1_latency,
                                                          write2_latency, a_op_pred);
            },
            RegFile::Pred => {
                let write1_latency = PredLatencySM80::op_category(a, false, a_dst_idx);
                let write2_latency = PredLatencySM80::op_category(b, false, b_dst_idx);
                return PredLatencySM80::pred_write_after_write(write1_latency,
                                                              write2_latency, a_op_pred);
            },
            RegFile::UPred => {
                let write1_latency = UPredLatencySM80::op_category(a, false, a_dst_idx);
                let write2_latency = UPredLatencySM80::op_category(b, false, b_dst_idx);
                return UPredLatencySM80::pred_write_after_write(write1_latency,
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
                let write_latency = PredLatencySM80::op_category(write, false, dst_idx);
                return PredLatencySM80::pred_read_after_write(write_latency,
                                                              PredLatencySM80::Guard);
            },
            RegFile::UPred => {
                let write_latency = UPredLatencySM80::op_category(write, false, dst_idx);
                return UPredLatencySM80::pred_read_after_write(write_latency,
                                                               UPredLatencySM80::UGuard);
            }
            _ => { panic!("Incorrect register file in paw_latencny") }
        }
    }
}
